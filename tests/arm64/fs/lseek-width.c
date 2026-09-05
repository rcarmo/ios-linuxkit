#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Observe x0 directly, before libc decodes Linux's [-4095, -1] errno range. */
static int64_t raw_seek(int fd, int64_t off, int whence) {
    register uint64_t x0 __asm__("x0") = fd;
    register uint64_t x1 __asm__("x1") = off;
    register uint64_t x2 __asm__("x2") = whence;
    register uint64_t x8 __asm__("x8") = 62;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory", "cc");
    return (int64_t) x0;
}

static void check(const char *what, int64_t got, int64_t want) {
    if (got != want) {
        fprintf(stderr, "%s: got=%#" PRIx64 " expected=%#" PRIx64 "\n", what,
                (uint64_t) got, (uint64_t) want);
        exit(1);
    }
}

int main(void) {
    char path[] = "/tmp/lseek-width-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0 || unlink(path)) { perror("temporary file"); return 2; }
    const int64_t offsets[] = {
        0, 4096, INT32_MAX, INT64_C(0x80000000), INT64_C(0xfffff000),
        INT64_C(0xfffff001), INT64_C(0xffffffea), INT64_C(0xffffffff),
        INT64_C(0x100000000), INT64_C(0x1ffffffff), INT64_C(1) << 40,
    };
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        int64_t off = offsets[i];
        check("raw SET", raw_seek(fd, off, SEEK_SET), off);
        check("raw CUR", raw_seek(fd, 0, SEEK_CUR), off);
        errno = 0;
        check("libc SET", lseek(fd, off, SEEK_SET), off);
        check("libc errno", errno, 0);
        check("CUR delta", raw_seek(fd, -1, SEEK_CUR), off ? off - 1 : -EINVAL);
        check("invalid origin", raw_seek(fd, 0, 99), -EINVAL);
        check("invalid keeps position", raw_seek(fd, 0, SEEK_CUR), off ? off - 1 : 0);
    }
    check("empty END", raw_seek(fd, 0, SEEK_END), 0);
    check("invalid fd", raw_seek(-1, 0, SEEK_SET), -EBADF);
    int pipefd[2];
    if (pipe(pipefd)) { perror("pipe"); return 2; }
    check("pipe seek", raw_seek(pipefd[0], 0, SEEK_SET), -ESPIPE);
    close(pipefd[0]); close(pipefd[1]); close(fd);
    puts("lseek-width-ok");
    return 0;
}
