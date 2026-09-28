// Actual procfs lookup at a high PID. Count examined PID slots so O(PID)
// directory enumeration cannot pass by running on a faster host.
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/proc.h"
#include "fs/real.h"

static unsigned pid_slots;
struct task *__real_pid_get_task(dword_t);
struct task *__wrap_pid_get_task(dword_t pid) {
    pid_slots++;
    return __real_pid_get_task(pid);
}
static void lookup(const char *path, bool exists) {
    struct statbuf st;
    pid_slots = 0;
    int err = procfs.stat(NULL, path, &st);
    if (pid_slots > 4)
        fprintf(stderr, "%s: %u PID slots examined (limit 4)\n", path, pid_slots);
    assert((err == 0) == exists);
    assert(pid_slots <= 4);
}
int main(void) {
    assert(mount_root(&realfs, "/") == 0);
    assert(become_first_process() == 0);
    for (unsigned i = 2; i < 30000; i++) {
        struct task *task = task_create_(NULL);
        assert(task);
        lock(&pids_lock);
        task_destroy(task);
        unlock(&pids_lock);
    }
    struct task *target = task_create_(NULL);
    assert(target && target->pid == 30000);
    lookup("/30000/stat", true);
    lookup("/30000/cmdline", true);
    lookup("/30000/maps", true);
    lookup("/30000/statm", true);
    lookup("/1/stat", true);
    lookup("/self", true);
    lookup("/cpuinfo", true);
    lookup("/30001/stat", false);
    lookup("/0/stat", false);
    lookup("/030000/stat", false);
    lookup("/+30000/stat", false);
    lookup("/30000x/stat", false);
    lookup("/32769/stat", false);
    lookup("/18446744073709581616/stat", false);
    lock(&pids_lock);
    target->zombie = true;
    unlock(&pids_lock);
    lookup("/30000/stat", false); // retain existing live-task lookup semantics
    lock(&pids_lock);
    task_destroy(target);
    unlock(&pids_lock);
    lookup("/30000/stat", false);
    // Direct lookup must not alter root directory iteration.
    struct proc_entry root = {.meta = &proc_root}, next = {0};
    unsigned long index = 0;
    unsigned count = 0;
    bool saw_init = false;
    while (proc_dir_read(&root, &index, &next)) {
        char name[64];
        proc_entry_getname(&next, name);
        saw_init |= !strcmp(name, "1");
        count++;
        proc_entry_cleanup(&next);
    }
    assert(saw_init && count > 1);
    puts("proc-pid-lookup-ok: high PID, invalid names, exit and enumeration");
    return 0;
}
