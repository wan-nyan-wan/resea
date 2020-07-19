#include <arch.h>
#include <printk.h>
#include <string.h>
#include "vm.h"

static uint64_t *traverse_page_table(uint64_t pml4, vaddr_t vaddr,
                                     paddr_t page, pageattrs_t attrs) {
    ASSERT(vaddr < KERNEL_BASE_ADDR);
    ASSERT(IS_ALIGNED(vaddr, PAGE_SIZE));

    uint64_t *table = from_paddr(pml4);
    for (int level = 4; level > 1; level--) {
        int index = NTH_LEVEL_INDEX(level, vaddr);
        if (!table[index]) {
            if (!attrs) {
                return NULL;
            }

            /* The PDPT, PD or PT is not allocated. */
            if (!page) {
                return NULL;
            }

            memset(from_paddr(page), 0, PAGE_SIZE);
            table[index] = page;
            page = 0;
        }

        // Update attributes if given.
        table[index] = table[index] | attrs;

        // Go into the next level paging table.
        table = (uint64_t *) from_paddr(ENTRY_PADDR(table[index]));
    }

    return &table[NTH_LEVEL_INDEX(1, vaddr)];
}

extern char __kernel_heap[];

error_t vm_create(struct vm *vm) {
    vm->pml4 = 0;
    return OK;
}

void vm_destroy(struct vm *vm) {
}

// TODO: remove
void *alloc_page(void);

error_t vm_link(struct vm *vm, vaddr_t vaddr, paddr_t paddr,
                pageattrs_t attrs) {
    ASSERT(vaddr < KERNEL_BASE_ADDR);
    ASSERT(IS_ALIGNED(vaddr, PAGE_SIZE));
    ASSERT(IS_ALIGNED(paddr, PAGE_SIZE));

    paddr_t page = (paddr_t) alloc_page();
    if (!vm->pml4) {
        if (!page) {
            return ERR_NO_MEMORY;
        }

        uint64_t *table = from_paddr(vm->pml4);
        memcpy(table, from_paddr((paddr_t) __kernel_pml4), PAGE_SIZE);

        // The kernel no longer access a virtual address around 0x0000_0000. Unmap
        // the area to catch bugs (especially NULL pointer dereferences in the
        // kernel).
        table[0] = 0;
    }

    attrs |= PAGE_PRESENT;
    uint64_t *entry = traverse_page_table(vm->pml4, vaddr, page, attrs);
    if (!entry) {
        return ERR_NO_MEMORY;
    }

    *entry = paddr | attrs;
    asm_invlpg(vaddr);
    return OK;
}

paddr_t vm_resolve(struct vm *vm, vaddr_t vaddr) {
    uint64_t *entry = traverse_page_table(vm->pml4, vaddr, 0, 0);
    return (entry) ? ENTRY_PADDR(*entry) : 0;
}
