#ifndef __TYPES_H__
#define __TYPES_H__

typedef char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned uint32_t;
typedef unsigned long long uint64_t;

typedef int task_t;
typedef long handle_t;

typedef unsigned msec_t;
#define MSEC_MAX 0xffffffff

#if __LP64__
typedef unsigned long long offset_t;
#else
typedef unsigned long offset_t;
#endif

typedef char bool;
#define true 1
#define false 0
#define NULL ((void *) 0)

typedef __builtin_va_list va_list;

#define offsetof(type, field)    __builtin_offsetof(type, field)
#define is_constant(expr)        __builtin_constant_p(expr)
#define va_start(ap, param)      __builtin_va_start(ap, param)
#define va_end(ap)               __builtin_va_end(ap)
#define va_arg(ap, type)         __builtin_va_arg(ap, type)
#define UNUSED                   __attribute__((unused))
#define PACKED                   __attribute__((packed))
#define NORETURN                 __attribute__((noreturn))
#define WEAK                     __attribute__((weak))
#define MUSTUSE                  __attribute__((warn_unused_result))
#define ALIGN_DOWN(value, align) ((value) & ~((align) -1))
#define ALIGN_UP(value, align)   ALIGN_DOWN((value) + (align) -1, align)
#define IS_ALIGNED(value, align) (((value) & ((align) -1)) == 0)
#define STATIC_ASSERT(expr)      _Static_assert(expr, #expr);
#define MAX(a, b)                                                              \
    ({                                                                         \
        __typeof__(a) __a = (a);                                               \
        __typeof__(b) __b = (b);                                               \
        (__a > __b) ? __a : __b;                                               \
    })
#define MIN(a, b)                                                              \
    ({                                                                         \
        __typeof__(a) __a = (a);                                               \
        __typeof__(b) __b = (b);                                               \
        (__a < __b) ? __a : __b;                                               \
    })

typedef int error_t;
#define IS_ERROR(err) ((err) < 0)
#define IS_OK(err)    ((err) >= 0)

// Error values. Don't forget to update `error_names` as well!
#define OK                 (0)
#define ERR_NO_MEMORY      (-1)
#define ERR_NOT_PERMITTED  (-2)
#define ERR_WOULD_BLOCK    (-3)
#define ERR_ABORTED        (-4)
#define ERR_TOO_LARGE      (-5)
#define ERR_TOO_SMALL      (-6)
#define ERR_NOT_FOUND      (-7)
#define ERR_INVALID_ARG    (-8)
#define ERR_ALREADY_EXISTS (-9)
#define ERR_UNAVAILABLE    (-10)
#define ERR_NOT_ACCEPTABLE (-11)
#define ERR_EMPTY          (-12)
#define DONT_REPLY         (-13)
#define ERR_IN_USE         (-14)
#define ERR_END            (-15)

#define SYS_SPAWN      1
#define SYS_KILL       2
#define SYS_SETATTRS   3
#define SYS_IPC        4
#define SYS_LISTENIRQ  5
#define SYS_WRITELOG   6
#define SYS_READLOG    7
#define SYS_KDEBUG     8
#define SYS_MAP     9

// Task flags.
#define TASK_IO      (1 << 0)
#define TASK_ABI_EMU (1 << 1)

// IPC source task IDs.
#define IPC_ANY     0  /* So-called "open receive". */
#define IPC_DENY    -1 /* Blocked in the IPC send phase. Internally used by kernel. */

// IPC options.
#define IPC_SEND    (1 << 0)
#define IPC_RECV    (1 << 1)
#define IPC_CALL    (IPC_SEND | IPC_RECV)
#define IPC_NOBLOCK (1 << 2)
#define IPC_NOTIFY  (1 << 3)
#define IPC_BULK    (1 << 4)
#define IPC_KERNEL  (1 << 5) /* Internally used by kernel. */

// Flags in the message type (m->type).
#define MSG_STR  (1 << 30)
#define MSG_BULK (1 << 29)
#define MSG_ID(type) ((type) & 0xffff)

// klogctl operations.
#define KLOGCTL_READ     1
#define KLOGCTL_WRITE    2
#define KLOGCTL_LISTEN   3
#define KLOGCTL_UNLISTEN 4

typedef uint64_t notifications_t;
#define NOTIFY_TIMER    (1ULL << 0)
#define NOTIFY_IRQ      (1ULL << 1)
#define NOTIFY_ABORTED  (1ULL << 2)
#define NOTIFY_NEW_DATA (1ULL << 3)

// TODO: Migrate into error_t
enum exception_type {
    EXP_GRACE_EXIT,
    EXP_NO_KERNEL_MEMORY,
    EXP_INVALID_MSG_FROM_PAGER,
    EXP_INVALID_MEMORY_ACCESS,
    EXP_INVALID_OP,
    EXP_ABORTED_KERNEL_IPC,
};

/// The kernel sends messages (e.g. EXCEPTION_MSG and PAGE_FAULT_MSG) as this
/// task ID.
#define KERNEL_TASK_TID 0
/// The initial task ID.
#define INIT_TASK_TID 1

#include <arch_types.h>

#endif
