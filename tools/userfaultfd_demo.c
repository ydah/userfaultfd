#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../ext/userfaultfd/compat.h"

static void *
zero_one_fault(void *argument)
{
    int fd = *(int *)argument;
    struct uffd_msg message;
    struct uffdio_zeropage zero;

    if (read(fd, &message, sizeof(message)) != sizeof(message)) {
        perror("read(userfaultfd)");
        return (void *)1;
    }
    if (message.event != UFFD_EVENT_PAGEFAULT) {
        fputs("unexpected userfaultfd event\n", stderr);
        return (void *)1;
    }
    memset(&zero, 0, sizeof(zero));
    zero.range.start = message.arg.pagefault.address & ~(sysconf(_SC_PAGESIZE) - 1);
    zero.range.len = (unsigned long)sysconf(_SC_PAGESIZE);
    if (ioctl(fd, UFFDIO_ZEROPAGE, &zero) < 0) {
        perror("UFFDIO_ZEROPAGE");
        return (void *)1;
    }
    return NULL;
}

int
main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *region = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    struct uffdio_api api = {.api = UFFD_API};
    struct uffdio_register registration;
    pthread_t handler;
    int fd;

    if (region == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
    if (fd < 0) {
        perror("userfaultfd");
        return 1;
    }
    if (ioctl(fd, UFFDIO_API, &api) < 0) {
        perror("UFFDIO_API");
        return 1;
    }
    memset(&registration, 0, sizeof(registration));
    registration.range.start = (unsigned long)region;
    registration.range.len = (unsigned long)page_size;
    registration.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(fd, UFFDIO_REGISTER, &registration) < 0) {
        perror("UFFDIO_REGISTER");
        return 1;
    }
    if (pthread_create(&handler, NULL, zero_one_fault, &fd) != 0) {
        perror("pthread_create");
        return 1;
    }

    if (*(volatile unsigned char *)region != 0) return 1;
    if (pthread_join(handler, NULL) != 0) return 1;
    puts("zero-fill fault handled");
    close(fd);
    munmap(region, (size_t)page_size);
    return 0;
}
