// Exercise actual generic_open/procfs fstat. No filesystem callback may run
// under inodes_lock: proc fstat takes pids_lock; proc readers can wait on mm,
// while an unmap holding mm waits for inode release (three-lock deadlock).
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/real.h"
#include "fs/fd.h"
#include "fs/inode.h"
static unsigned checked;
static int checked_fstat(struct fd *fd, struct statbuf *st) {
    int err = pthread_mutex_trylock(&inodes_lock.m);
    if (err) fprintf(stderr, "proc fstat invoked while inodes_lock is held: %d\n", err);
    assert(err == 0);
    pthread_mutex_unlock(&inodes_lock.m);
    checked++;
    return procfs.fstat(fd, st);
}
int main(void) {
    assert(mount_root(&realfs, "/") == 0);
    assert(become_first_process() == 0);
    struct fs_ops probe_procfs = procfs;
    probe_procfs.fstat = checked_fstat;
    assert(do_mount(&probe_procfs, "", "/proc", "", 0) == 0);
    const char *paths[] = {"/proc", "/proc/1/stat", "/proc/1/cmdline", "/proc/1/maps"};
    for (unsigned i = 0; i < sizeof(paths)/sizeof(*paths); i++) {
        struct fd *fd = generic_open(paths[i], O_RDONLY_, 0);
        assert(!IS_ERR(fd));
        fd_close(fd);
    }
    assert(checked == 4);
    puts("proc-open-locks-ok: filesystem fstat outside inode lock");
}
