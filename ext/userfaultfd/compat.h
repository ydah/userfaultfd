#ifndef USERFAULTFD_COMPAT_H
#define USERFAULTFD_COMPAT_H

#ifdef __linux__
# include <stdint.h>
# include <sys/ioctl.h>

# ifdef HAVE_LINUX_USERFAULTFD_H
#  include <linux/userfaultfd.h>
# else
/* Linux 4.11 baseline UAPI for build hosts without userfaultfd.h. */
#  define UFFD_API UINT64_C(0xAA)

#  define _UFFDIO_REGISTER 0x00
#  define _UFFDIO_UNREGISTER 0x01
#  define _UFFDIO_WAKE 0x02
#  define _UFFDIO_COPY 0x03
#  define _UFFDIO_ZEROPAGE 0x04
#  define _UFFDIO_WRITEPROTECT 0x06
#  define _UFFDIO_CONTINUE 0x07
#  define _UFFDIO_API 0x3F
#  define UFFDIO 0xAA

struct uffd_msg {
    uint8_t event;
    uint8_t reserved1;
    uint16_t reserved2;
    uint32_t reserved3;
    union {
        struct {
            uint64_t flags;
            uint64_t address;
            union { uint32_t ptid; } feat;
        } pagefault;
        struct { uint32_t ufd; } fork;
        struct { uint64_t from, to, len; } remap;
        struct { uint64_t start, end; } remove;
        struct { uint64_t reserved1, reserved2, reserved3; } reserved;
    } arg;
} __attribute__((packed));

#  define UFFD_EVENT_PAGEFAULT 0x12
#  define UFFD_EVENT_FORK 0x13
#  define UFFD_EVENT_REMAP 0x14
#  define UFFD_EVENT_REMOVE 0x15
#  define UFFD_EVENT_UNMAP 0x16

#  define UFFD_PAGEFAULT_FLAG_WRITE (1 << 0)
#  define UFFD_PAGEFAULT_FLAG_WP (1 << 1)
#  define UFFD_PAGEFAULT_FLAG_MINOR (1 << 2)

struct uffdio_api { uint64_t api, features, ioctls; };
#  define UFFD_FEATURE_PAGEFAULT_FLAG_WP (UINT64_C(1) << 0)
#  define UFFD_FEATURE_EVENT_FORK (UINT64_C(1) << 1)
#  define UFFD_FEATURE_EVENT_REMAP (UINT64_C(1) << 2)
#  define UFFD_FEATURE_EVENT_REMOVE (UINT64_C(1) << 3)
#  define UFFD_FEATURE_MISSING_HUGETLBFS (UINT64_C(1) << 4)
#  define UFFD_FEATURE_MISSING_SHMEM (UINT64_C(1) << 5)
#  define UFFD_FEATURE_EVENT_UNMAP (UINT64_C(1) << 6)
#  define UFFD_FEATURE_SIGBUS (UINT64_C(1) << 7)
#  define UFFD_FEATURE_THREAD_ID (UINT64_C(1) << 8)

struct uffdio_range { uint64_t start, len; };
struct uffdio_register {
    struct uffdio_range range;
    uint64_t mode, ioctls;
};
#  define UFFDIO_REGISTER_MODE_MISSING (UINT64_C(1) << 0)
#  define UFFDIO_REGISTER_MODE_WP (UINT64_C(1) << 1)
#  define UFFDIO_REGISTER_MODE_MINOR (UINT64_C(1) << 2)

struct uffdio_copy {
    uint64_t dst, src, len, mode;
    int64_t copy;
};
#  define UFFDIO_COPY_MODE_DONTWAKE (UINT64_C(1) << 0)

struct uffdio_zeropage {
    struct uffdio_range range;
    uint64_t mode;
    int64_t zeropage;
};
#  define UFFDIO_ZEROPAGE_MODE_DONTWAKE (UINT64_C(1) << 0)

struct uffdio_writeprotect {
    struct uffdio_range range;
    uint64_t mode;
};
#  define UFFDIO_WRITEPROTECT_MODE_WP (UINT64_C(1) << 0)

struct uffdio_continue {
    struct uffdio_range range;
    uint64_t mode;
    int64_t mapped;
};
#  define UFFDIO_CONTINUE_MODE_DONTWAKE (UINT64_C(1) << 0)

#  define UFFDIO_API _IOWR(UFFDIO, _UFFDIO_API, struct uffdio_api)
#  define UFFDIO_REGISTER _IOWR(UFFDIO, _UFFDIO_REGISTER, struct uffdio_register)
#  define UFFDIO_UNREGISTER _IOR(UFFDIO, _UFFDIO_UNREGISTER, struct uffdio_range)
#  define UFFDIO_WAKE _IOR(UFFDIO, _UFFDIO_WAKE, struct uffdio_range)
#  define UFFDIO_COPY _IOWR(UFFDIO, _UFFDIO_COPY, struct uffdio_copy)
#  define UFFDIO_ZEROPAGE _IOWR(UFFDIO, _UFFDIO_ZEROPAGE, struct uffdio_zeropage)
#  define UFFDIO_WRITEPROTECT _IOWR(UFFDIO, _UFFDIO_WRITEPROTECT, struct uffdio_writeprotect)
#  define UFFDIO_CONTINUE _IOWR(UFFDIO, _UFFDIO_CONTINUE, struct uffdio_continue)
# endif

# ifndef UFFD_USER_MODE_ONLY
#  define UFFD_USER_MODE_ONLY 1
# endif

/* Keep old or absent build headers feature-complete; the kernel still probes support. */
# ifndef UFFD_PAGEFAULT_FLAG_WP
#  define UFFD_PAGEFAULT_FLAG_WP (1 << 1)
# endif
# ifndef UFFD_PAGEFAULT_FLAG_MINOR
#  define UFFD_PAGEFAULT_FLAG_MINOR (1 << 2)
# endif
# ifndef UFFD_FEATURE_PAGEFAULT_FLAG_WP
#  define UFFD_FEATURE_PAGEFAULT_FLAG_WP (UINT64_C(1) << 0)
# endif
# ifndef UFFD_FEATURE_EVENT_FORK
#  define UFFD_FEATURE_EVENT_FORK (UINT64_C(1) << 1)
# endif
# ifndef UFFD_FEATURE_EVENT_REMAP
#  define UFFD_FEATURE_EVENT_REMAP (UINT64_C(1) << 2)
# endif
# ifndef UFFD_FEATURE_EVENT_REMOVE
#  define UFFD_FEATURE_EVENT_REMOVE (UINT64_C(1) << 3)
# endif
# ifndef UFFD_FEATURE_MISSING_HUGETLBFS
#  define UFFD_FEATURE_MISSING_HUGETLBFS (UINT64_C(1) << 4)
# endif
# ifndef UFFD_FEATURE_MISSING_SHMEM
#  define UFFD_FEATURE_MISSING_SHMEM (UINT64_C(1) << 5)
# endif
# ifndef UFFD_FEATURE_EVENT_UNMAP
#  define UFFD_FEATURE_EVENT_UNMAP (UINT64_C(1) << 6)
# endif
# ifndef UFFD_FEATURE_SIGBUS
#  define UFFD_FEATURE_SIGBUS (UINT64_C(1) << 7)
# endif
# ifndef UFFD_FEATURE_THREAD_ID
#  define UFFD_FEATURE_THREAD_ID (UINT64_C(1) << 8)
# endif
# ifndef UFFD_FEATURE_MINOR_HUGETLBFS
#  define UFFD_FEATURE_MINOR_HUGETLBFS (UINT64_C(1) << 9)
# endif
# ifndef UFFD_FEATURE_MINOR_SHMEM
#  define UFFD_FEATURE_MINOR_SHMEM (UINT64_C(1) << 10)
# endif
# ifndef UFFD_FEATURE_EXACT_ADDRESS
#  define UFFD_FEATURE_EXACT_ADDRESS (UINT64_C(1) << 11)
# endif
# ifndef UFFD_FEATURE_WP_HUGETLBFS_SHMEM
#  define UFFD_FEATURE_WP_HUGETLBFS_SHMEM (UINT64_C(1) << 12)
# endif
# ifndef UFFD_FEATURE_WP_UNPOPULATED
#  define UFFD_FEATURE_WP_UNPOPULATED (UINT64_C(1) << 13)
# endif
# ifndef UFFD_FEATURE_POISON
#  define UFFD_FEATURE_POISON (UINT64_C(1) << 14)
# endif
# ifndef UFFD_FEATURE_WP_ASYNC
#  define UFFD_FEATURE_WP_ASYNC (UINT64_C(1) << 15)
# endif
# ifndef UFFD_FEATURE_MOVE
#  define UFFD_FEATURE_MOVE (UINT64_C(1) << 16)
# endif

# ifndef UFFDIO_REGISTER_MODE_WP
#  define UFFDIO_REGISTER_MODE_WP (UINT64_C(1) << 1)
# endif
# ifndef UFFDIO_REGISTER_MODE_MINOR
#  define UFFDIO_REGISTER_MODE_MINOR (UINT64_C(1) << 2)
# endif

# ifndef UFFDIO_WRITEPROTECT
#  ifndef _UFFDIO_WRITEPROTECT
#   define _UFFDIO_WRITEPROTECT 0x06
#  endif
struct uffdio_writeprotect {
    struct uffdio_range range;
    uint64_t mode;
};
#  define UFFDIO_WRITEPROTECT_MODE_WP (UINT64_C(1) << 0)
#  define UFFDIO_WRITEPROTECT _IOWR(UFFDIO, _UFFDIO_WRITEPROTECT, struct uffdio_writeprotect)
# endif

# ifndef UFFDIO_CONTINUE
#  ifndef _UFFDIO_CONTINUE
#   define _UFFDIO_CONTINUE 0x07
#  endif
struct uffdio_continue {
    struct uffdio_range range;
    uint64_t mode;
    int64_t mapped;
};
#  define UFFDIO_CONTINUE_MODE_DONTWAKE (UINT64_C(1) << 0)
#  define UFFDIO_CONTINUE _IOWR(UFFDIO, _UFFDIO_CONTINUE, struct uffdio_continue)
# endif

# ifndef UFFDIO_POISON
#  ifndef _UFFDIO_POISON
#   define _UFFDIO_POISON 0x08
#  endif
struct uffdio_poison {
    struct uffdio_range range;
    uint64_t mode;
    int64_t updated;
};
#  define UFFDIO_POISON_MODE_DONTWAKE (UINT64_C(1) << 0)
#  define UFFDIO_POISON _IOWR(UFFDIO, _UFFDIO_POISON, struct uffdio_poison)
# endif

# ifndef UFFDIO_MOVE
#  ifndef _UFFDIO_MOVE
#   define _UFFDIO_MOVE 0x05
#  endif
struct uffdio_move {
    uint64_t dst, src, len, mode;
    int64_t move;
};
#  define UFFDIO_MOVE_MODE_DONTWAKE (UINT64_C(1) << 0)
#  define UFFDIO_MOVE _IOWR(UFFDIO, _UFFDIO_MOVE, struct uffdio_move)
# endif

/* Added in Linux 5.19. Old headers still build and use the syscall path. */
# ifndef USERFAULTFD_IOC
#  define USERFAULTFD_IOC 0xAA
# endif
# ifndef USERFAULTFD_IOC_NEW
#  define USERFAULTFD_IOC_NEW _IO(USERFAULTFD_IOC, 0x00)
# endif
#endif

#endif
