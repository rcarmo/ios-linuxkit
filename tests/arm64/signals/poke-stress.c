#define _GNU_SOURCE
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>

/* Parent sends one signal at a time while the child executes arithmetic with
 * no syscalls. Every handler acknowledgement must be observed, not a timeout
 * safety valve. Shared lock-free atomics are also async-signal-safe here. */
static _Atomic unsigned *shared;
static void handler(int sig) {
    (void)sig;
    atomic_fetch_add_explicit(&shared[1], 1, memory_order_release);
}
int main(void) {
    _Static_assert(ATOMIC_INT_LOCK_FREE == 2, "lock-free signal acknowledgement");
    shared = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) { perror("mmap"); return 2; }
    struct sigaction sa = {.sa_handler = handler};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL)) { perror("sigaction"); return 2; }
    alarm(25);
    for (unsigned round = 0; round < 8; round++) {
        atomic_store(&shared[0], 0);
        atomic_store(&shared[1], 0);
        pid_t child = fork();
        if (child < 0) { perror("fork"); return 2; }
        if (child == 0) {
            alarm(20);
            atomic_store_explicit(&shared[0], 1, memory_order_release);
            volatile uint64_t x = 1;
            while (atomic_load_explicit(&shared[1], memory_order_acquire) < 128) {
                for (unsigned i=0; i<256; i++) x = x * 6364136223846793005ULL + 1;
            }
            _exit(0);
        }
        while (!atomic_load_explicit(&shared[0], memory_order_acquire)) sched_yield();
        for (unsigned n=1; n<=128; n++) {
            if (kill(child, SIGUSR1)) { perror("kill"); return 2; }
            while (atomic_load_explicit(&shared[1], memory_order_acquire) != n) sched_yield();
        }
        int status;
        if (waitpid(child, &status, 0) != child || status != 0) return 1;
    }
    alarm(0);
    if (munmap(shared, 4096)) return 2;
    puts("poke-stress-ok");
    return 0;
}
