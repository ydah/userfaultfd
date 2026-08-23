#ifndef USERFAULTFD_COMPAT_H
#define USERFAULTFD_COMPAT_H

#ifdef __linux__
# include <linux/userfaultfd.h>
# include <sys/ioctl.h>

# ifndef UFFD_USER_MODE_ONLY
#  define UFFD_USER_MODE_ONLY 1
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
