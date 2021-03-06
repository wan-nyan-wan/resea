#ifndef __SYSCALL_H__
#define __SYSCALL_H__

#include <types.h>
#include <message.h>

/// Checks if the current task is allowed to control (dangerous operations) the
/// `target` task. This macro is true if any following statements are hold:
///
///  - The current task is the init task.
///  - The current task is the pager task for `target`.
///
#define SYSCALL_AUTH(target) \
    (!CURRENT->pager || CURRENT == (target)->pager)

/// A pointer given by the user. Don't reference it directly; access it through
/// safe functions such as memcpy_from_user and memcpy_to_user!
typedef vaddr_t userptr_t;

void memcpy_from_user(void *dst, userptr_t src, size_t len);
void memcpy_to_user(userptr_t dst, const void *src, size_t len);
long handle_syscall(int n, long a1, long a2, long a3, long a4, long a5);

#ifdef CONFIG_ABI_EMU
void abi_emu_hook(trap_frame_t *frame, enum abi_hook_type type);
#endif

// Implemented in arch.
struct task;
void arch_memcpy_from_user(void *dst, userptr_t src, size_t len);
void arch_memcpy_to_user(userptr_t dst, const void *src, size_t len);

#endif
