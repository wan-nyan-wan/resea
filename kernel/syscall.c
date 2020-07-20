#include <arch.h>
#include <list.h>
#include <string.h>
#include <types.h>
#include "ipc.h"
#include "kdebug.h"
#include "printk.h"
#include "syscall.h"
#include "task.h"

/// Copies bytes from the userspace. If the user's pointer is invalid, this
/// function or the page fault handler kills the current task.
void memcpy_from_user(void *dst, userptr_t src, size_t len) {
    if (is_kernel_addr_range(src, len)) {
        task_exit(EXP_INVALID_MEMORY_ACCESS);
    }

    arch_memcpy_from_user(dst, src, len);
}

/// Copies bytes into the userspace. If the user's pointer is invalid, this
/// function or the page fault handler kills the current task.
void memcpy_to_user(userptr_t dst, const void *src, size_t len) {
    if (is_kernel_addr_range(dst, len)) {
        task_exit(EXP_INVALID_MEMORY_ACCESS);
    }

    arch_memcpy_to_user(dst, src, len);
}

/// Copy a string terminated by NUL from the userspace. If the user's pointer is
/// invalid, this function or the page fault handler kills the current task.
static void strncpy_from_user(char *dst, userptr_t src, size_t max_len) {
    if (is_kernel_addr_range(src, max_len)) {
        task_exit(EXP_INVALID_MEMORY_ACCESS);
    }

    arch_strncpy_from_user(dst, src, max_len);
}


/// Initializes and starts a task.
static error_t sys_spawn(task_t tid, userptr_t name, vaddr_t ip, task_t pager,
                         unsigned flags) {
    struct task *task = task_lookup_unchecked(tid);
    if (!task || task == CURRENT) {
        return ERR_INVALID_ARG;
    }

    struct task *pager_task = task_lookup(pager);
    if (!pager_task) {
        return ERR_INVALID_ARG;
    }

    // Create a task.
    char namebuf[CONFIG_TASK_NAME_LEN];
    strncpy_from_user(namebuf, name, sizeof(namebuf));
    return task_create(task, namebuf, ip, pager_task, flags);
}

/// Kills a task.
static error_t sys_kill(task_t tid) {
    if (!tid) {
        task_exit(EXP_GRACE_EXIT);
        UNREACHABLE();
    }

    struct task *task = task_lookup(tid);
    if (!task || task == CURRENT) {
        return ERR_INVALID_ARG;
    }

    return task_destroy(task);
}

/// Sets task attributes.
static task_t sys_setattrs(userptr_t bulk_ptr, size_t bulk_len,
                           msec_t timeout) {
    if (bulk_ptr) {
        if (bulk_len < CONFIG_BULK_BUFFER_LEN) {
            return ERR_TOO_SMALL;
        }
    }

    if (timeout) {
        CURRENT->timeout = timeout;
    }

    return CURRENT->tid;
}

/// Send/receive IPC messages and notifications.
static error_t sys_ipc(task_t dst, task_t src, userptr_t m, unsigned flags) {
    if (flags & IPC_KERNEL) {
        return ERR_INVALID_ARG;
    }

    if (src < 0 || src > CONFIG_NUM_TASKS) {
        return ERR_INVALID_ARG;
    }

    struct task *dst_task = NULL;
    if (flags & (IPC_SEND | IPC_NOTIFY)) {
        dst_task = task_lookup(dst);
        if (!dst_task) {
            return ERR_INVALID_ARG;
        }

        if (flags & IPC_NOTIFY) {
            notify(dst_task, m);
            return OK;
        }
    }

    return ipc(dst_task, src, (struct message *) m, flags);
}

/// Registers a interrupt listener task.
static error_t sys_listenirq(unsigned irq, task_t listener) {
    if (listener) {
        struct task *task = task_lookup(listener);
        if (!task) {
            return ERR_INVALID_ARG;
        }

        return task_listen_irq(task, irq);
    } else {
        return task_unlisten_irq(irq);
    }
}

/// Writes log messages into the kernel log buffer.
static int sys_writelog(userptr_t buf, size_t buf_len) {
    char kbuf[256];
    int remaining = buf_len;
    while (remaining > 0) {
        int copy_len = MIN(remaining, (int) sizeof(kbuf));
        memcpy_from_user(kbuf, buf, copy_len);
        for (int i = 0; i < copy_len; i++) {
            printk("%c", kbuf[i]);
        }
        remaining -= copy_len;
    }

    return OK;
}

/// Read log messages from the kernel log buffer.
static error_t sys_readlog(userptr_t buf, size_t buf_len, bool listen) {
    char kbuf[256];
    int remaining = buf_len;
    while (remaining > 0) {
        int max_len = MIN(remaining, (int) sizeof(kbuf));
        int read_len = klog_read(kbuf, max_len);
        if (!read_len) {
            break;
        }

        memcpy_to_user(buf, kbuf, read_len);
        buf += read_len;
        remaining -= read_len;
    }

    if (listen) {
        klog_listen(CURRENT);
    } else {
        klog_unlisten();
    }

    return buf_len - remaining;
}

static error_t sys_kdebug(userptr_t cmdline) {
    char input[128];
    strncpy_from_user(input, cmdline, sizeof(input));
    return kdebug_run(input);
}

static paddr_t resolve_paddr(vaddr_t vaddr) {
    if (CURRENT->tid == INIT_TASK_TID) {
        if (is_kernel_paddr(vaddr)) {
            return 0;
        }
        return vaddr;
    } else {
        paddr_t paddr = vm_resolve(&CURRENT->vm, vaddr);
        if (!paddr) {
            return ERR_NOT_FOUND;
        }
        return paddr;
    }
}

static error_t sys_map(task_t tid, vaddr_t vaddr, vaddr_t src, vaddr_t kpage,
                       unsigned flags) {
    TRACE("v=%p, src=%p, kp=%p", vaddr,src,kpage);
    if (!IS_ALIGNED(vaddr, PAGE_SIZE) || !IS_ALIGNED(vaddr, PAGE_SIZE)
        || !IS_ALIGNED(kpage, PAGE_SIZE)) {
        return ERR_INVALID_ARG;
    }

    // TODO: Check if kpage is mapped in the kernel's address space.

    struct task *task = task_lookup(tid);
    if (!task) {
        return ERR_INVALID_ARG;
    }

    // Resolve paddrs.
    paddr_t paddr = resolve_paddr(src);
    paddr_t kpage_paddr = resolve_paddr(kpage);
    if (!paddr || !kpage_paddr) {
        return ERR_NOT_FOUND;
    }

    // TODO: Use flags
    // TODO: pages[pfn]
    return vm_link(&task->vm, vaddr, paddr, kpage, PAGE_USER | PAGE_WRITABLE);
}

/// The system call handler.
long handle_syscall(int n, long a1, long a2, long a3, long a4, long a5) {
    stack_check();

    long ret;
    switch (n) {
        case SYS_SPAWN:
            ret = sys_spawn(a1, a2, a3, a4, a5);
            break;
        case SYS_KILL:
            ret = sys_kill(a1);
            break;
        case SYS_SETATTRS:
            ret = sys_setattrs(a1, a2, a3);
            break;
        case SYS_IPC:
            ret = sys_ipc(a1, a2, a3, a4);
            break;
        case SYS_MAP:
            ret = sys_map(a1, a2, a3, a4, a5);
            break;
        case SYS_LISTENIRQ:
            ret = sys_listenirq(a1, a2);
            break;
        case SYS_WRITELOG:
            ret = sys_writelog(a1, a2);
            break;
        case SYS_READLOG:
            ret = sys_readlog(a1, a2, a3);
            break;
        case SYS_KDEBUG:
            ret = sys_kdebug(a1);
            break;
        default:
            ret = ERR_INVALID_ARG;
    }

    stack_check();
    return ret;
}

#ifdef CONFIG_ABI_EMU
/// The system call handler for ABI emulation.
void abi_emu_hook(trap_frame_t *frame, enum abi_hook_type type) {
    struct message m;
    m.type = ABI_HOOK_MSG;
    m.abi_hook.type = type;
    m.abi_hook.task = CURRENT->tid;
    memcpy(&m.abi_hook.frame, frame, sizeof(m.abi_hook.frame));

    error_t err = ipc(CURRENT->pager, CURRENT->pager->tid, &m,
                      IPC_CALL | IPC_KERNEL);
    if (IS_ERROR(err)) {
        WARN("%s: aborted kernel ipc", CURRENT->name);
        task_exit(EXP_ABORTED_KERNEL_IPC);
    }

    // Check if the reply is valid.
    if (m.type != ABI_HOOK_REPLY_MSG) {
        WARN("%s: invalid abi hook reply (type=%d)",
             CURRENT->name, m.type);
        task_exit(EXP_INVALID_MSG_FROM_PAGER);
    }

    memcpy(frame, &m.abi_hook_reply.frame, sizeof(*frame));
}
#endif
