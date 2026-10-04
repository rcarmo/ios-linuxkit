#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fs/real.h"
#include "kernel/errno.h"

int main(int argc, char **argv) {
    assert(argc == 2);
    struct mount mount = { .root_fd = open(argv[1], O_RDONLY | O_DIRECTORY) };
    assert(mount.root_fd >= 0);
    assert(mkdirat(mount.root_fd, "empty", 0700) == 0);
    assert(mkdirat(mount.root_fd, "populated", 0700) == 0);
    int fd = openat(mount.root_fd, "populated/file", O_CREAT | O_RDWR, 0600);
    assert(fd >= 0); close(fd);
    assert(realfs_unlink(&mount, "/empty") == _EISDIR);
    assert(realfs_unlink(&mount, "/populated") == _EISDIR);
    assert(symlinkat("empty", mount.root_fd, "link") == 0);
    assert(realfs_unlink(&mount, "/link") == 0);
    assert(realfs_unlink(&mount, "/populated/file") == 0);
    assert(realfs_unlink(&mount, "/missing") == _ENOENT);
#ifdef __APPLE__
    fd = openat(mount.root_fd, "protected", O_CREAT | O_RDWR, 0600);
    assert(fd >= 0);
    assert(fchflags(fd, UF_IMMUTABLE) == 0);
    int result = realfs_unlink(&mount, "/protected");
    assert(fchflags(fd, 0) == 0);
    close(fd);
    assert(result == _EPERM);
    assert(realfs_unlink(&mount, "/protected") == 0);
#endif
    assert(realfs_rmdir(&mount, "/empty") == 0);
    assert(realfs_rmdir(&mount, "/populated") == 0);
    close(mount.root_fd);
    puts("REALFS_UNLINK_OK");
}
