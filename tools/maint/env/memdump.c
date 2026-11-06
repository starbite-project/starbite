/*
 * Starbite Maintenance: memdump PHYS LEN > file
 *
 * Copies LEN bytes of physical memory at PHYS to stdout through an mmap of /dev/mem (read only).
 * `dd if=/dev/mem` cannot read the DTB's no-map reserved regions (the kernel has no linear mapping
 * of them, so read() fails); an mmap maps them as device memory and works. Used at boot to keep
 * what the previous boot left in RAM (docs/MAINTENANCE.md 6): the ramoops rings and the boot-next
 * mailbox (0xFED10000, 32 KiB), the loader's log page (0x91FFF000) and S-Boot's sec_log
 * (0xC0000000, 512 KiB). Built static by tools/maint/env/mk-maint.sh. Reads 8 bytes at a time
 * (device memory: no unaligned or wider accesses).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: memdump PHYS LEN  (hex or decimal; PHYS and LEN multiples of 8)\n");
        return 2;
    }
    uint64_t phys = strtoull(argv[1], 0, 0), len = strtoull(argv[2], 0, 0);
    if ((phys | len) & 7 || len == 0 || len > (64u << 20)) {
        fprintf(stderr, "memdump: PHYS and LEN must be multiples of 8, LEN 8 .. 64 MiB\n");
        return 2;
    }
    long pg = sysconf(_SC_PAGESIZE);
    uint64_t base = phys & ~(uint64_t)(pg - 1), off = phys - base, maplen = off + len;
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "memdump: /dev/mem: %s\n", strerror(errno));
        return 1;
    }
    void *m = mmap(0, maplen, PROT_READ, MAP_SHARED, fd, (off_t)base);
    if (m == MAP_FAILED) {
        fprintf(stderr, "memdump: mmap %#llx: %s\n", (unsigned long long)base, strerror(errno));
        return 1;
    }
    const volatile uint64_t *src = (const volatile uint64_t *)((char *)m + off);
    uint64_t buf[512];
    for (uint64_t done = 0; done < len;) {
        unsigned n = 0;
        while (n < 512 && done + 8 * n < len) {
            buf[n] = src[done / 8 + n];
            ++n;
        }
        if (fwrite(buf, 8, n, stdout) != n) {
            fprintf(stderr, "memdump: write: %s\n", strerror(errno));
            return 1;
        }
        done += 8 * (uint64_t)n;
    }
    munmap(m, maplen);
    close(fd);
    return 0;
}
