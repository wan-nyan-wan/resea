OUTPUT_FORMAT("elf64-x86-64");
ENTRY(boot);

LMA_OFFSET = 0xffff800000000000;
KERNEL_BOOT_ADDR = 0x100000;

PHDRS {
	boot PT_LOAD FLAGS(5); /* flags = rx */
	text PT_LOAD FLAGS(5); /* flags = rx */
	data PT_LOAD FLAGS(6); /* flags = rw */
}

SECTIONS {
    , = 0x5000;
    __mp_boot_trampoine = .;
    . += 0x1000;
    __mp_boot_trampoine_end = .;
    __mp_boot_gdtr = .;

    . = KERNEL_BOOT_ADDR;
    __kernel_image = .; /* paddr_t */

    .boot : AT(KERNEL_BOOT_ADDR) {
        KEEP(*(.boot));
    } :boot

        /* The kernel page table (paddr_t). */
        . = ALIGN(4096);
        __kernel_pml4 = .;
        . += 0x1000;
        __kernel_pdpt = .;
        . += 0x1000;
        __kernel_pd = .;
        . += 4 * 512 * 8; /* (# of PDPT entries) * (# of entries in PD) *
                             (size of entry) */

        /* CPU variables and boot stacks (paddr_t). */
        . = ALIGN(4096);
        __cpuvar_base = .;
        . += 0x4000 * 16; /* CPUVAR_SIZE_MAX * CPU_NUM_MAX */
        __boot_stack_base = .;
        . += 0x1000 * 16; /* PAGE_SIZE * CPU_NUM_MAX */


    /* Move to higher address (0xffff8000_00000000-). */
    . += LMA_OFFSET;

    .text : AT(ADDR(.text) - LMA_OFFSET) ALIGN(8) {
        *(.text);
        *(.text.*);
    } :text

    .rodata : AT(ADDR(.rodata) - LMA_OFFSET) ALIGN(8) {
        *(.rodata);
        *(.rodata.*);
    } :text

    .data : AT(ADDR(.data) - LMA_OFFSET) ALIGN(8) {
        *(.data);
        *(.data.*);
    } :data

    .bss : AT(ADDR(.bss) - LMA_OFFSET) ALIGN(8) {
        __bss = .;
        *(.bss);
        *(.bss.*);
        __bss_end = .;
    } :data

    . = ALIGN(4096);
    __kernel_image_end = . - LMA_OFFSET; /* paddr_t */

    /DISCARD/ :{
        *(*.eh_frame);
    }
}
