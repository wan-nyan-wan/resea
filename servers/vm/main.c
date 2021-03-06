#include <list.h>
#include <resea/ipc.h>
#include <resea/malloc.h>
#include <resea/printf.h>
#include <resea/task.h>
#include <string.h>
#include "elf.h"
#include "bootfs.h"
#include "pages.h"

extern char __bootfs[];
extern char __zeroed_pages[];
extern char __zeroed_pages_end[];
extern char __free_vaddr[];
extern char __free_vaddr_end[];

struct page_area {
    list_elem_t next;
    vaddr_t vaddr;
    paddr_t paddr;
    size_t num_pages;
};

#define SERVICE_NAME_LEN 32

/// Task Control Block (TCB).
struct task {
    bool in_use;
    task_t tid;
    char name[32];
    struct bootfs_file *file;
    void *file_header;
    struct elf64_ehdr *ehdr;
    struct elf64_phdr *phdrs;
    vaddr_t free_vaddr;
    list_t page_areas;
    vaddr_t ool_buf;
    size_t ool_len;
    task_t received_ool_from;
    vaddr_t received_ool_buf;
    size_t received_ool_len;
    list_t ool_sender_queue;
    list_elem_t ool_sender_next;
    struct message ool_sender_m;
    char waiting_for[SERVICE_NAME_LEN];
};

struct service {
    list_elem_t next;
    char name[SERVICE_NAME_LEN];
    task_t task;
};

static struct task tasks[CONFIG_NUM_TASKS];
static struct bootfs_file *files;
static unsigned num_files;
static list_t services;

static paddr_t alloc_pages(struct task *task, vaddr_t vaddr, size_t num_pages);

/// Look for the task in the our task table.
static struct task *get_task_by_tid(task_t tid) {
    if (tid <= 0 || tid > CONFIG_NUM_TASKS) {
        PANIC("invalid tid %d", tid);
    }

    struct task *task = &tasks[tid - 1];
    ASSERT(task->in_use);
    return task;
}

static void read_file(struct bootfs_file *file, offset_t off, void *buf, size_t len) {
    void *p =
        (void *) (((uintptr_t) __bootfs) + file->offset + off);
    memcpy(buf, p, len);
}

static void init_task_struct(struct task *task, const char *name,
    struct bootfs_file *file, void *file_header, struct elf64_ehdr *ehdr) {
    task->in_use = true;
    task->file = file;
    task->file_header = file_header;
    task->ehdr = ehdr;
    if (ehdr) {
        task->phdrs = (struct elf64_phdr *) ((uintptr_t) ehdr + ehdr->e_ehsize);
    } else {
        task->phdrs = NULL;
    }

    task->free_vaddr = (vaddr_t) __free_vaddr;
    task->ool_buf = 0;
    task->ool_len = 0;
    task->received_ool_buf = 0;
    task->received_ool_len = 0;
    task->received_ool_from = 0;
    list_init(&task->ool_sender_queue);
    list_nullify(&task->ool_sender_next);
    strncpy(task->name, name, sizeof(task->name));
    strncpy(task->waiting_for, "", sizeof(task->waiting_for));
    list_init(&task->page_areas);
}

static task_t launch_task(struct bootfs_file *file) {
    TRACE("launching %s...", file->name);

    // Look for an unused task ID.
    struct task *task = NULL;
    for (int i = 0; i < CONFIG_NUM_TASKS; i++) {
        if (!tasks[i].in_use) {
            task = &tasks[i];
            break;
        }
    }

    if (!task) {
        PANIC("too many tasks");
    }

    void *file_header = malloc(PAGE_SIZE);
    read_file(file, 0, file_header, PAGE_SIZE);

    // Ensure that it's an ELF file.
    struct elf64_ehdr *ehdr = (struct elf64_ehdr *) file_header;
    if (memcmp(ehdr->e_ident, "\x7f" "ELF", 4) != 0) {
        WARN("%s: invalid ELF magic, ignoring...", file->name);
        return ERR_NOT_ACCEPTABLE;
    }

    // Create a new task for the server.
    error_t err =
        task_create(task->tid, file->name, ehdr->e_entry, task_self(), TASK_IO);
    ASSERT_OK(err);

    init_task_struct(task, file->name, file, file_header, ehdr);
    return task->tid;
}

static error_t map_page(task_t tid, vaddr_t vaddr, paddr_t paddr,
                        unsigned flags, bool overwrite) {
    flags |= overwrite ? (MAP_DELETE | MAP_UPDATE) : MAP_UPDATE;
    while (true) {
        paddr_t kpage = pages_alloc(1);
        error_t err = task_map(tid, vaddr, paddr, kpage, flags);
        switch (err) {
            case ERR_TRY_AGAIN:
                continue;
            default:
                return err;
        }
    }
}

static paddr_t pager(struct task *task, vaddr_t vaddr, unsigned fault) {
    vaddr = ALIGN_DOWN(vaddr, PAGE_SIZE);

    if (fault & EXP_PF_PRESENT) {
        // Invalid access. For instance the user thread has tried to write to
        // readonly area.
        WARN("%s: invalid memory access at %p (perhaps segfault?)", task->name,
             vaddr);
        return 0;
    }

    LIST_FOR_EACH (area, &task->page_areas, struct page_area, next) {
        if (area->vaddr <= vaddr
            && vaddr < area->vaddr + area->num_pages * PAGE_SIZE) {
            return area->paddr + (vaddr - area->vaddr);
        }
    }

    // Zeroed pages.
    vaddr_t zeroed_pages_start = (vaddr_t) __zeroed_pages;
    vaddr_t zeroed_pages_end = (vaddr_t) __zeroed_pages_end;
    if (zeroed_pages_start <= vaddr && vaddr < zeroed_pages_end) {
        // The accessed page is zeroed one (.bss section, stack, or heap).
        paddr_t paddr = alloc_pages(task, vaddr, 1);
        ASSERT_OK(map_page(INIT_TASK, paddr, paddr, MAP_W, false));
        memset((void *) paddr, 0, PAGE_SIZE);
        return paddr;
    }

    // Look for the associated program header.
    struct elf64_phdr *phdr = NULL;
    if (task->ehdr) {
        for (unsigned i = 0; i < task->ehdr->e_phnum; i++) {
            // Ignore GNU_STACK
            if (!task->phdrs[i].p_vaddr) {
                continue;
            }

            vaddr_t start = task->phdrs[i].p_vaddr;
            vaddr_t end = start + task->phdrs[i].p_memsz;
            if (start <= vaddr && vaddr <= end) {
                phdr = &task->phdrs[i];
                break;
            }
        }

        if (phdr) {
            // Allocate a page and fill it with the file data.
            paddr_t paddr = alloc_pages(task, vaddr, 1);
            ASSERT_OK(map_page(INIT_TASK, paddr, paddr, MAP_W, false));
            size_t offset_in_segment = (vaddr - phdr->p_vaddr) + phdr->p_offset;
            read_file(task->file, offset_in_segment, (void *) paddr, PAGE_SIZE);
            return paddr;
        }
    }

    WARN("invalid memory access (addr=%p), killing %s...", vaddr, task->name);
    return 0;
}

static void kill(struct task *task) {
    task_destroy(task->tid);
    task->in_use = false;
    if (task->file_header) {
        free(task->file_header);
    }
}

/// Allocates a virtual address space by so-called the bump pointer allocation
/// algorithm.
static vaddr_t alloc_virt_pages(struct task *task, size_t num_pages) {
    vaddr_t vaddr = task->free_vaddr;
    size_t size = num_pages * PAGE_SIZE;

    if (vaddr + size >= (vaddr_t) __free_vaddr_end) {
        // Task's virtual memory space has been exhausted.
        kill(task);
        return 0;
    }

    task->free_vaddr += size;
    return vaddr;
}

static paddr_t alloc_pages(struct task *task, vaddr_t vaddr, size_t num_pages) {
    struct page_area *area = malloc(sizeof(*area));
    area->vaddr = vaddr;
    area->paddr = pages_alloc(num_pages);
    area->num_pages = num_pages;
    list_push_back(&task->page_areas, &area->next);
    return area->paddr;
}

static error_t phy_alloc_pages(struct task *task, vaddr_t *vaddr, paddr_t *paddr,
                               size_t num_pages) {
    if (*paddr && !is_mappable_paddr(*paddr)) {
        return ERR_INVALID_ARG;
    }

    *vaddr = alloc_virt_pages(task, num_pages);
    if (*paddr) {
        pages_incref(paddr2pfn(*paddr), num_pages);
    } else {
        *paddr = pages_alloc(num_pages);
    }

    struct page_area *area = malloc(sizeof(*area));
    area->vaddr = *vaddr;
    area->paddr = *paddr;
    area->num_pages = num_pages;
    list_push_back(&task->page_areas, &area->next);
    return OK;
}

static paddr_t vaddr2paddr(struct task *task, vaddr_t vaddr) {
    LIST_FOR_EACH (area, &task->page_areas, struct page_area, next) {
        if (area->vaddr <= vaddr
            && vaddr < area->vaddr + area->num_pages * PAGE_SIZE) {
            return area->paddr + (vaddr - area->vaddr);
        }
    }

    // The page is not mapped. Try filling it with pager.
    return pager(task, vaddr, EXP_PF_USER | EXP_PF_WRITE /* FIXME: strip PF_WRITE */);
}

static error_t handle_ool_send(struct message *m);

static error_t handle_ool_recv(struct message *m) {
    struct task *task = get_task_by_tid(m->src);
    ASSERT(task);

//    TRACE("accept: %s: %p %d (old=%p)",
//        task->name, m->ool_recv.addr, m->ool_recv.len, task->ool_buf);
    if (task->ool_buf) {
        return ERR_ALREADY_EXISTS;
    }

    task->ool_buf = m->ool_recv.addr;
    task->ool_len = m->ool_recv.len;

    struct task *sender = LIST_POP_FRONT(&task->ool_sender_queue, struct task,
                                         ool_sender_next);
    if (sender) {
        struct message m;
        memcpy(&m, &sender->ool_sender_m, sizeof(m));
//        TRACE("%s -> %s: src = %d / %d", task->name, sender->name,
//              sender->ool_sender_m.src, m.src);
        error_t err = handle_ool_send(&m);
        switch (err) {
            case OK:
                ipc_reply(sender->tid, &m);
                break;
            case DONT_REPLY:
                // Do nothing.
                break;
            default:
                OOPS_OK(err);
                ipc_reply_err(sender->tid, err);
        }
    }

    m->type = OOL_RECV_REPLY_MSG;
    return OK;
}

static error_t handle_ool_verify(struct message *m) {
    struct task *task = get_task_by_tid(m->src);
    ASSERT(task);

//    TRACE("verify: %s: id=%p len=%d (src=%d)", task->name,
//          m->ool_verify.id, m->ool_verify.len, m->src);
    if (m->ool_verify.src != task->received_ool_from
        || m->ool_verify.id != task->received_ool_buf
        || m->ool_verify.len != task->received_ool_len) {
        return ERR_INVALID_ARG;
    }

    m->type = OOL_VERIFY_REPLY_MSG;
    m->ool_verify_reply.received_at = task->received_ool_buf;

    task->received_ool_buf = 0;
    task->received_ool_len = 0;
    task->received_ool_from = 0;
    return OK;
}

uint8_t __src_page[PAGE_SIZE] __aligned(PAGE_SIZE);
uint8_t __dst_page[PAGE_SIZE] __aligned(PAGE_SIZE);

static error_t handle_ool_send(struct message *m) {
    struct task *src_task = get_task_by_tid(m->src);
    ASSERT(src_task);

    struct task *dst_task = get_task_by_tid(m->ool_send.dst);
    if (!dst_task) {
        return ERR_NOT_FOUND;
    }

//    TRACE("do_copy: %s -> %s: %p -> %p, len=%d",
//        src_task->name, dst_task->name,
//        m->ool_send.addr, dst_task->ool_buf,
//        m->ool_send.len);
    if (!dst_task->ool_buf) {
        memcpy(&src_task->ool_sender_m, m, sizeof(*m));
        list_push_back(&dst_task->ool_sender_queue, &src_task->ool_sender_next);
        return DONT_REPLY;
    }

    size_t len = m->ool_send.len;
    vaddr_t src_buf = m->ool_send.addr;
    vaddr_t dst_buf = dst_task->ool_buf;
    DEBUG_ASSERT(len <= dst_task->ool_len);

    size_t remaining = len;
    while (remaining > 0) {
        offset_t src_off = src_buf % PAGE_SIZE;
        offset_t dst_off = dst_buf % PAGE_SIZE;
        size_t copy_len = MIN(remaining, MIN(PAGE_SIZE - src_off, PAGE_SIZE - dst_off));

        void *src_ptr;
        if (src_task->tid == INIT_TASK) {
            src_ptr = (void *) src_buf;
        } else {
            paddr_t src_paddr = vaddr2paddr(src_task, ALIGN_DOWN(src_buf, PAGE_SIZE));
            if (!src_paddr) {
                kill(src_task);
                return DONT_REPLY;
            }

            ASSERT_OK(map_page(INIT_TASK, (vaddr_t) __src_page, src_paddr,
                               MAP_W, true));
            src_ptr = &__src_page[src_off];
        }

        void *dst_ptr;
        if (dst_task->tid == INIT_TASK) {
            dst_ptr = (void *) dst_buf;
        } else {
            paddr_t dst_paddr = vaddr2paddr(dst_task, ALIGN_DOWN(dst_buf, PAGE_SIZE));
            if (!dst_paddr) {
                kill(dst_task);
                return ERR_UNAVAILABLE;
            }

            // Temporarily map the pages into the our address space.
            ASSERT_OK(map_page(INIT_TASK, (vaddr_t) __dst_page, dst_paddr,
                               MAP_W, true));
            dst_ptr = &__dst_page[dst_off];
        }

        // Copy between the tasks.
        memcpy(dst_ptr, src_ptr, copy_len);
        remaining -= copy_len;
        dst_buf += copy_len;
        src_buf += copy_len;
    }

    dst_task->received_ool_buf = dst_task->ool_buf;
    dst_task->received_ool_len = m->ool_send.len;
    dst_task->received_ool_from = src_task->tid;
    dst_task->ool_buf = 0;
    dst_task->ool_len = 0;

    m->type = OOL_SEND_REPLY_MSG;
    m->ool_send_reply.id = dst_task->received_ool_buf;
    return OK;
}

error_t call_self(struct message *m) {
    m->src = INIT_TASK;
    error_t err;
    switch (m->type) {
        case OOL_RECV_MSG:
            err = handle_ool_recv(m);
            break;
        case OOL_VERIFY_MSG:
            err = handle_ool_verify(m);
            break;
        case OOL_SEND_MSG:
            err = handle_ool_send(m);
            break;
        default:
            UNREACHABLE();
    }

    if (err != OK) {
        PANIC("call_self failed (%s)", err2str(err));
    }

    return err;
}

static void handle_message(const struct message *m) {
    struct message r;
    bzero(&r, sizeof(r));

    switch (m->type) {
        case OOL_RECV_MSG: {
            memcpy(&r, m, sizeof(r));
            error_t err = handle_ool_recv(&r);
            switch (err) {
                case DONT_REPLY:
                    break;
                case OK:
                    ipc_reply(m->src, &r);
                    break;
                default:
                    ipc_reply_err(m->src, err);
            }
            break;
        }
        case OOL_VERIFY_MSG: {
            memcpy(&r, m, sizeof(r));
            error_t err = handle_ool_verify(&r);
            switch (err) {
                case DONT_REPLY:
                    break;
                case OK:
                    ipc_reply(m->src, &r);
                    break;
                default:
                    ipc_reply_err(m->src, err);
            }
            break;
        }
        case OOL_SEND_MSG: {
            memcpy(&r, m, sizeof(r));
            error_t err = handle_ool_send(&r);
            switch (err) {
                case DONT_REPLY:
                    break;
                case OK:
                    ipc_reply(m->src, &r);
                    break;
                default:
                    ipc_reply_err(m->src, err);
            }
            break;
        }
        case NOP_MSG:
            r.type = NOP_REPLY_MSG;
            r.nop_reply.value = m->nop.value * 7;
            ipc_reply(m->src, &r);
            break;
        case NOP_WITH_OOL_MSG:
            free((void *) m->nop_with_ool.data);
            r.type = NOP_WITH_OOL_REPLY_MSG;
            r.nop_with_ool_reply.data = "reply!";
            r.nop_with_ool_reply.data_len = 7;
            ipc_reply(m->src, &r);
            break;
        case EXCEPTION_MSG: {
            if (m->src != KERNEL_TASK) {
                WARN("forged exception message from #%d, ignoring...",
                     m->src);
                break;
            }

            struct task *task = get_task_by_tid(m->exception.task);
            ASSERT(task);
            ASSERT(m->exception.task == task->tid);

            if (m->exception.exception == EXP_GRACE_EXIT) {
                INFO("%s: terminated its execution", task->name);
            } else {
                WARN("%s: exception occurred, killing the task...",
                     task->name);
            }

            kill(task);
            break;
        }
        case PAGE_FAULT_MSG: {
            if (m->src != KERNEL_TASK) {
                WARN("forged page fault message from #%d, ignoring...",
                     m->src);
                break;
            }

            struct task *task = get_task_by_tid(m->page_fault.task);
            ASSERT(task);
            ASSERT(m->page_fault.task == task->tid);

            paddr_t paddr =
                pager(task, m->page_fault.vaddr, m->page_fault.fault);
            if (!paddr) {
                ipc_reply_err(m->src, ERR_NOT_FOUND);
                break;
            }

            vaddr_t aligned_vaddr = ALIGN_DOWN(m->page_fault.vaddr, PAGE_SIZE);
            ASSERT_OK(map_page(task->tid, aligned_vaddr, paddr, MAP_W, false));
            r.type = PAGE_FAULT_REPLY_MSG;

            ipc_reply(task->tid, &r);
            break;
        }
        case SERVE_MSG: {
            struct service *service = malloc(sizeof(*service));
            service->task = m->src;
            strncpy(service->name, m->serve.name, sizeof(service->name));
            list_nullify(&service->next);
            list_push_back(&services, &service->next);

            r.type = SERVE_REPLY_MSG;
            ipc_reply(m->src, &r);

            for (int i = 0; i < CONFIG_NUM_TASKS; i++) {
                struct task *task = &tasks[i];
                if (!strcmp(task->waiting_for, service->name)) {
                    bzero(&r, sizeof(r));
                    r.type = LOOKUP_REPLY_MSG;
                    r.lookup_reply.task = service->task;
                    ipc_reply(task->tid, &r);

                    // The task no longer wait for the service. Clear the field.
                    strncpy(task->waiting_for, "", sizeof(task->waiting_for));
                }
            }

            free((void *) m->serve.name);
            break;
        }
        case LOOKUP_MSG: {
            struct service *service = NULL;
            LIST_FOR_EACH (s, &services, struct service, next) {
                if (!strcmp(s->name, m->lookup.name)) {
                    service = s;
                    break;
                }
            }

            if (service) {
                r.type = LOOKUP_REPLY_MSG;
                r.lookup_reply.task = service->task;
                ipc_reply(m->src, &r);
                free((void *) m->lookup.name);
                break;
            }

            // The service is not yet available. Block the caller task until the
            // server is registered by `ipc_serve()`.
            struct task *task = get_task_by_tid(m->src);
            if (!task) {
                ipc_reply_err(m->src, ERR_NOT_FOUND);
                free((void *) m->lookup.name);
                break;
            }

            strncpy(task->waiting_for, m->lookup.name, sizeof(task->waiting_for));
            free((void *) m->lookup.name);
            break;
        }
        case ALLOC_PAGES_MSG: {
            struct task *task = get_task_by_tid(m->src);
            ASSERT(task);

            vaddr_t vaddr;
            paddr_t paddr = m->alloc_pages.paddr;
            error_t err =
                phy_alloc_pages(task, &vaddr, &paddr, m->alloc_pages.num_pages);
            if (err != OK) {
                ipc_reply_err(m->src, err);
                break;
            }

            r.type = ALLOC_PAGES_REPLY_MSG;
            r.alloc_pages_reply.vaddr = vaddr;
            r.alloc_pages_reply.paddr = paddr;
            ipc_reply(m->src, &r);
            break;
        }
        case LAUNCH_TASK_MSG: {
            // Look for the program in the apps directory.
            char *name = (char *) m->launch_task.name;
            struct bootfs_file *file = NULL;
            for (uint32_t i = 0; i < num_files; i++) {
                if (!strcmp(files[i].name, name)) {
                    file = &files[i];
                    break;
                }
            }

            free(name);
            if (!file) {
                ipc_reply_err(m->src, ERR_NOT_FOUND);
                break;
            }

            launch_task(file);
            r.type = LAUNCH_TASK_REPLY_MSG;
            ipc_reply(m->src, &r);
            break;
        }
        default:
            WARN("unknown message type (type=%d)", m->type);
            // FIXME: Free ool payloads.
    }
}

void main(void) {
    TRACE("starting...");
    struct bootfs_header *header = (struct bootfs_header *) __bootfs;
    num_files = header->num_files;
    files =
        (struct bootfs_file *) (((uintptr_t) &__bootfs) + header->files_off);
    pages_init();
    list_init(&services);

    for (int i = 0; i < CONFIG_NUM_TASKS; i++) {
        tasks[i].in_use = false;
        tasks[i].tid = i + 1;
    }

    // Initialize a task struct for myself.
    init_task_struct(&tasks[INIT_TASK - 1], "vm", NULL, NULL, NULL);

    // Launch servers in bootfs.
    int num_launched = 0;
    for (uint32_t i = 0; i < num_files; i++) {
        struct bootfs_file *file = &files[i];

        // Autostart server names (separated by whitespace).
        char *startups = AUTOSTARTS;

        // Execute the file if it is listed in the autostarts.
        while (*startups != '\0') {
            size_t len = strlen(file->name);
            if (!strncmp(file->name, startups, len)
                && (startups[len] == '\0' || startups[len] == ' ')) {
                launch_task(file);
                num_launched++;
                break;
            }

            while (*startups != '\0' && *startups != ' ') {
                startups++;
            }

            if (*startups == ' ') {
                startups++;
            }
        }
    }

    if (!num_launched) {
        WARN("no servers to launch");
    }

    // The mainloop: receive and handle messages.
    INFO("ready");
    while (true) {
        struct message m;
        error_t err = ipc_recv(IPC_ANY, &m);
        ASSERT_OK(err);
        handle_message(&m);
    }
}
