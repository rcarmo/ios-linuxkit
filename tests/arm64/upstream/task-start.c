// Link against the actual kernel archives with --wrap=pthread_create.
// Verify failed-start rollback for fork, vfork, threads and app-created tasks.
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/mm.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/real.h"

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
        void *(*entry)(void *), void *arg) {
    (void)thread; (void)attr; (void)entry; (void)arg;
    return EAGAIN;
}

int main(void) {
    assert(mount_root(&realfs, "/") == 0);
    assert(become_first_process() == 0);
    struct task *parent = current;
    // A running process has an executable; mm_copy retains that fd.
    parent->mm->exefile = generic_open("/dev/null", O_RDONLY_, 0);
    assert(!IS_ERR(parent->mm->exefile));
    unsigned mm_refs = atomic_load(&parent->mm->refcount);
    unsigned fd_refs = atomic_load(&parent->files->refcount);
    unsigned fs_refs = atomic_load(&parent->fs->refcount);
    unsigned sig_refs = atomic_load(&parent->sighand->refcount);
    // fork, vfork+VM, pthread-style sharing
    const unsigned flags[] = {17, 17 | 0x100 | 0x4000,
        0x100 | 0x200 | 0x400 | 0x800 | 0x10000};
    for (unsigned round = 0; round < 100; round++) {
        for (unsigned i = 0; i < sizeof(flags) / sizeof(*flags); i++) {
            assert((int_t)sys_clone(flags[i], 0, 0, 0, 0) == _EAGAIN);
            assert(list_empty(&parent->children));
            assert(list_size(&parent->group->threads) == 1);
            assert(atomic_load(&parent->mm->refcount) == mm_refs);
            assert(atomic_load(&parent->files->refcount) == fd_refs);
            assert(atomic_load(&parent->fs->refcount) == fs_refs);
            assert(atomic_load(&parent->sighand->refcount) == sig_refs);
        }
        assert(become_new_init_child() == 0);
        assert(task_start(current) == _EAGAIN);
        task_discard_unstarted(current);
        current = parent;
        assert(list_empty(&parent->children));
    }
    lock(&pids_lock);
    for (unsigned pid = 2; pid <= MAX_PID; pid++)
        assert(pid_get_task(pid) == NULL);
    unlock(&pids_lock);
    puts("task-start-rollback-ok: 300 failed clones, 100 failed app tasks");
    return 0;
}
