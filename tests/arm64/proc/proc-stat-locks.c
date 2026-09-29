// Actual proc-stat callback must not acquire sighand under group/general locks.
// Signal-frame writes hold sighand and can grow the stack via rlimit(group),
// producing an ABBA deadlock if proc stat uses the reverse nesting.
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/init.h"
#include "kernel/fs.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "fs/proc.h"
#include "fs/fd.h"
#include "fs/real.h"

static struct task *target;
static unsigned checked;
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (target && mutex == &target->sighand->lock.m) {
        // No competing threads: EBUSY proves this same callback nested locks.
        int g = pthread_mutex_trylock(&target->group->lock.m);
        int t = pthread_mutex_trylock(&target->general_lock.m);
        fprintf(stderr, "proc-stat-lock-order group=%d general=%d\n", g, t);
        assert(g == 0 && t == 0);
        pthread_mutex_unlock(&target->general_lock.m);
        pthread_mutex_unlock(&target->group->lock.m);
        checked++;
    }
    return __real_pthread_mutex_lock(mutex);
}
int main(void) {
    assert(mount_root(&realfs, "/") == 0);
    assert(become_first_process() == 0);
    assert(do_mount(&procfs, "", "/proc", "", 0) == 0);
    current->pending = 1ull << (SIGUSR1_ - 1);
    current->blocked = 1ull << (SIGUSR2_ - 1);
    current->sighand->action[SIGUSR1_].handler = SIG_IGN_;
    current->sighand->action[SIGUSR2_].handler = 0x1234;
    strcpy(current->comm, "stat-fixture");
    struct fd *fd = generic_open("/proc/1/stat", O_RDONLY_, 0);
    assert(!IS_ERR(fd));
    target = current;
    char text[2048] = {0};
    int n = fd->ops->pread(fd, text, sizeof(text)-1, 0);
    target = NULL;
    assert(n > 0 && checked == 1);
    assert(strstr(text, "1 (stat-fixture) R ") == text);
    // Fields 3..39 follow the command; verify the signal snapshot (31..34).
    char *p = strchr(text, ')') + 2, *save;
    unsigned field=3;
    for(char *s=strtok_r(p," \n",&save);s;s=strtok_r(NULL," \n",&save),field++) {
        if(field==31) assert(strtoul(s,NULL,10)==(1ul << (SIGUSR1_-1)));
        if(field==32) assert(strtoul(s,NULL,10)==(1ul << (SIGUSR2_-1)));
        if(field==33) assert(strtoul(s,NULL,10)==(1ul << SIGUSR1_));
        if(field==34) assert(strtoul(s,NULL,10)==(1ul << SIGUSR2_));
    }
    assert(field == 40);
    fd_close(fd);
    puts("proc-stat-locks-ok: no group/general-to-sighand nesting; signal fields preserved");
}
