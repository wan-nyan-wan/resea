#include <arch.h>
#include <main.h>
#include <printk.h>
#include <task.h>
#include "serial.h"
#include "task.h"
#include "trap.h"

static void gdt_init(void) {
    uint64_t tss_addr = (uint64_t) &ARCH_CPUVAR->tss;
    struct gdt *gdt = &ARCH_CPUVAR->gdt;
    gdt->null = 0x0000000000000000;
    gdt->kernel_cs = 0x00af9a000000ffff;
    gdt->kernel_ds = 0x00af92000000ffff;
    gdt->user_cs32 = 0x0000000000000000;
    gdt->user_cs64 = 0x00affa000000ffff;
    gdt->user_ds = 0x008ff2000000ffff;
    gdt->tss_low =
        0x0000890000000000 | sizeof(struct tss) | ((tss_addr & 0xffff) << 16)
        | (((tss_addr >> 16) & 0xff) << 32) | (((tss_addr >> 24) & 0xff) << 56);
    gdt->tss_high = tss_addr >> 32;

    // Update GDTR
    struct gdtr gdtr;
    gdtr.laddr = (uint64_t) gdt;
    gdtr.len = sizeof(*gdt) - 1;
    asm_lgdt((uint64_t) &gdtr);
}

static void idt_init(void) {
    struct idt *idt = &ARCH_CPUVAR->idt;

    // Initialize IDT entries.
    for (int i = 0; i < IDT_DESC_NUM; i++) {
        uint64_t handler = (uint64_t) &interrupt_handlers[i];
        idt->descs[i].offset1 = handler & 0xffff;
        idt->descs[i].seg = KERNEL_CS;
        idt->descs[i].ist = IST_RSP0;
        idt->descs[i].info = IDT_INT_HANDLER;
        idt->descs[i].offset2 = (handler >> 16) & 0xffff;
        idt->descs[i].offset3 = (handler >> 32) & 0xffffffff;
        idt->descs[i].reserved = 0;
    }

    struct idtr idtr;
    idtr.laddr = (uint64_t) idt;
    idtr.len = sizeof(*idt) - 1;
    asm_lidt((uint64_t) &idtr);
}

// Disables PIC. We use IO APIC instead.
static void pic_init(void) {
    asm_out8(0xa1, 0xff);
    asm_out8(0x21, 0xff);
    asm_out8(0x20, 0x11);
    asm_out8(0xa0, 0x11);
    asm_out8(0x21, 0x20);
    asm_out8(0xa1, 0x28);
    asm_out8(0x21, 0x04);
    asm_out8(0xa1, 0x02);
    asm_out8(0x21, 0x01);
    asm_out8(0xa1, 0x01);
    asm_out8(0xa1, 0xff);
    asm_out8(0x21, 0xff);
}

static void tss_init(void) {
    struct tss *tss = &ARCH_CPUVAR->tss;
    tss->rsp0 = 0;
    tss->iomap_offset = offsetof(struct tss, iomap);
    tss->iomap_last_byte = 0xff;
    asm_ltr(TSS_SEG);
}

static void syscall_init(void) {
    asm_wrmsr(MSR_STAR,
              ((uint64_t) USER_CS32 << 48) | ((uint64_t) KERNEL_CS << 32));
    asm_wrmsr(MSR_LSTAR, (uint64_t) syscall_entry);
    asm_wrmsr(MSR_FMASK, SYSCALL_RFLAGS_MASK);
    // RIP for compatibility mode. We don't support it for now.
    asm_wrmsr(MSR_CSTAR, 0);
    // Enable SYSCALL/SYSRET.
    asm_wrmsr(MSR_EFER, asm_rdmsr(MSR_EFER) | EFER_SCE);
}

static void calibrate_apic_timer(void) {
    // TODO: Calibrate the timer automatically.
    write_apic(APIC_REG_TIMER_INITCNT, CONFIG_LAPIC_TIMER_1MS_COUNT);
}

static void apic_timer_init(void) {
    write_apic(APIC_REG_TIMER_INITCNT, 0xffffffff);
    write_apic(APIC_REG_LVT_TIMER, (VECTOR_IRQ_BASE + TIMER_IRQ) | 0x20000);
    write_apic(APIC_REG_TIMER_DIV, APIC_TIMER_DIV);
    calibrate_apic_timer();
}

static void apic_init(void) {
    asm_wrmsr(MSR_APIC_BASE, (asm_rdmsr(MSR_APIC_BASE) & 0xfffff100) | 0x0800);
    write_apic(APIC_REG_SPURIOUS_INT, 1 << 8);
    write_apic(APIC_REG_TPR, 0);
    write_apic(APIC_REG_LOGICAL_DEST, 0x01000000);
    write_apic(APIC_REG_DEST_FORMAT, 0xffffffff);
    write_apic(APIC_REG_LVT_TIMER, 1 << 16 /* masked */);
    write_apic(APIC_REG_LVT_ERROR, 1 << 16 /* masked */);
}

static void common_setup(void) {
    STATIC_ASSERT(sizeof(struct cpuvar) <= CPUVAR_SIZE_MAX);
    STATIC_ASSERT(IS_ALIGNED(CPUVAR_SIZE_MAX, PAGE_SIZE));

    // Enable some CPU features.
    asm_write_cr0((asm_read_cr0() | CR0_MP) & (~CR0_EM) & (~CR0_TS));
    asm_write_cr4(asm_read_cr4() | CR4_FSGSBASE | CR4_OSXSAVE | CR4_OSFXSR
                  | CR4_OSXMMEXCPT);
    asm_xsetbv(0, asm_xgetbv(0) | XCR0_SSE | XCR0_AVX);

    // Set RDGSBASE to enable the CPUVAR macro.
    struct gsbase *gsbase =
        from_paddr((paddr_t) __cpuvar_base + mp_self() * CPUVAR_SIZE_MAX);
    asm_wrgsbase((uint64_t) gsbase);

    apic_init();
    gdt_init();
    tss_init();
    idt_init();
    apic_timer_init();
    syscall_init();
}

void init(void) {
    lock();
    serial_init();
    pic_init();
    common_setup();
    serial_enable_interrupt();
    kmain();
}

void mpinit(void) {
    lock();
    INFO("Booting CPU #%d...", mp_self());
    common_setup();
    mpmain();
}

void arch_idle(void) {
    task_switch();
    while (true) {
        unlock();
        asm_stihlt();
        asm_cli();
        lock();
    }
}

void arch_semihosting_halt(void) {
    // QEMU
    __asm__ __volatile__("outw %0, %1" ::
        "a"((uint16_t) 0x2000), "Nd"((uint16_t) 0x604));
}
