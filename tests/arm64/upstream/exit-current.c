// Actual normal do_exit path must surrender its TLS task pointer before host
// pthread cleanup can run: the parent may already have reaped/freed the task.
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "fs/real.h"

extern _Noreturn void __real_pthread_exit(void *);
_Noreturn void __wrap_pthread_exit(void *value) {
    assert(current == NULL); // deterministic baseline failure, no racing free needed
    __real_pthread_exit(value);
}
extern int do_wait(int type, pid_t_ id, struct siginfo_ *, struct rusage_ *, int options);
static void *child_exit(void *arg) {
    current=arg;
    do_exit(0);
}
int main(void) {
    assert(mount_root(&realfs,"/")==0);
    assert(become_first_process()==0);
    struct task *parent=current;
    for(unsigned i=0;i<100;i++) {
        assert(become_new_init_child()==0);
        struct task *child=current;pid_t_ pid=child->pid;current=parent;
        pthread_t thread;assert(!pthread_create(&thread,NULL,child_exit,child));
        assert(!pthread_join(thread,NULL));
        struct siginfo_ info={0};struct rusage_ usage={0};
        assert(do_wait(1,pid,&info,&usage,4)==0); // P_PID, WEXITED
        assert(info.child.pid==pid&&info.child.status==0);
    }
    puts("exit-current-ok: 100 normal leader exits clear TLS before pthread cleanup and reap");
}
