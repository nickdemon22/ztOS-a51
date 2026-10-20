#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <errno.h>

struct ion_allocation_data_modern {
    uint64_t len;
    uint32_t heap_id_mask;
    uint32_t flags;
    uint32_t fd;
    uint32_t unused;
};

struct ion_allocation_data_legacy {
    size_t len;
    size_t align;
    unsigned int heap_id_mask;
    unsigned int flags;
    int handle;
};

struct ion_fd_data {
    int handle;
    int fd;
};

#define ION_IOC_MAGIC 'I'
#define ION_IOC_ALLOC_MODERN _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data_modern)
#define ION_IOC_ALLOC_LEGACY _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data_legacy)
#define ION_IOC_SHARE _IOWR(ION_IOC_MAGIC, 4, struct ion_fd_data)

int main(void) {
    int ion_fd = open("/dev/ion", O_RDWR);
    if (ion_fd < 0) {
        perror("open /dev/ion");
        return 1;
    }
    struct ion_allocation_data_modern m = {0};
    m.len = 1080 * 2400 * 4;
    m.heap_id_mask = 0xffffffff;
    if (ioctl(ion_fd, ION_IOC_ALLOC_MODERN, &m) == 0) {
        printf("MODERN_ION dmabuf fd: %d\n", m.fd);
        void *p = mmap(NULL, m.len, PROT_READ | PROT_WRITE, MAP_SHARED, m.fd, 0);
        printf("mmap: %p\n", p);
        if (p != MAP_FAILED) munmap(p, m.len);
        close(m.fd);
        close(ion_fd);
        return 0;
    }
    printf("Modern ion failed (%d), trying legacy...\n", errno);
    struct ion_allocation_data_legacy leg = {0};
    leg.len = 1080 * 2400 * 4;
    leg.align = 4096;
    leg.heap_id_mask = 0xffffffff;
    if (ioctl(ion_fd, ION_IOC_ALLOC_LEGACY, &leg) == 0) {
        printf("LEGACY_ION handle: %d\n", leg.handle);
        struct ion_fd_data s = { .handle = leg.handle };
        if (ioctl(ion_fd, ION_IOC_SHARE, &s) == 0) {
            printf("LEGACY_ION dmabuf fd: %d\n", s.fd);
            void *p = mmap(NULL, leg.len, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd, 0);
            printf("mmap: %p\n", p);
            if (p != MAP_FAILED) munmap(p, leg.len);
            close(s.fd);
        } else {
            perror("ION_IOC_SHARE");
        }
        close(ion_fd);
        return 0;
    }
    perror("LEGACY_ION");
    close(ion_fd);
    return 2;
}
