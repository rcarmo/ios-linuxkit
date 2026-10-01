// Actual signal delivery and stack materialization, no memory-policy API.
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/signal.h"
#include "fs/real.h"
static jmp_buf exited;
static int injection;
static unsigned failures;
static addr_t initial_sp;
static long initial_pages;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n) {
    if (injection == 2 && n == sizeof(struct data)) {
        injection = 0; failures++; errno = ENOMEM; return NULL;
    }
    return __real_malloc(n);
}
void *__real_mmap64(void *, size_t, int, int, int, off_t);
void *__wrap_mmap64(void *a, size_t n, int prot, int flags, int fd, off_t off) {
    if (injection == 1 && n == PAGE_SIZE) {
        injection = 0; failures++; errno = ENOMEM; return MAP_FAILED;
    }
    return __real_mmap64(a, n, prot, flags, fd, off);
}
noreturn void __wrap_do_exit_group(int status) {
    assert(status == SIGSEGV_ && current->cpu.sp < initial_sp);
    assert(pthread_mutex_trylock(&current->sighand->lock.m) == 0);
    pthread_mutex_unlock(&current->sighand->lock.m);
    assert(atomic_load(&anon_page_count) == initial_pages && failures == 1);
    longjmp(exited, 1);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    int kind = !strcmp(argv[1], "mmap") ? 1 : !strcmp(argv[1], "data") ? 2 : 0;
    assert(kind || !strcmp(argv[1], "recover"));
    assert(mount_root(&realfs, "/") == 0 && become_first_process() == 0);
    write_wrlock(&current->mem->lock);
    assert(pt_unmap_always(current->mem, 0, MEM_PAGES) == 0);
    assert(pt_map_nothing(current->mem, STACK_INIT_PAGE, 1, P_READ | P_WRITE | P_GROWSDOWN) == 0);
    write_wrunlock(&current->mem->lock);
    // Actual ARM64 frame includes the extended context and may span pages.
    current->cpu.sp = initial_sp = (STACK_INIT_PAGE << PAGE_BITS) + 512;
    current->cpu.pc = 0x400000;
    current->sighand->action[SIGUSR1_] = (struct sigaction_) {.handler = 0x500000, .restorer = 0x500100};
    // A SIGSEGV handler cannot repair a stack on which no frame can be built.
    current->sighand->action[SIGSEGV_] = (struct sigaction_) {.handler = 0x600000, .restorer = 0x600100};
    initial_pages = atomic_load(&anon_page_count);
    send_signal(current, SIGUSR1_, SIGINFO_NIL);
    injection = kind;
    if (setjmp(exited) == 0) {
        receive_signals();
        assert(!kind && current->cpu.pc == 0x500000 && current->cpu.sp < initial_sp);
        long new_pages = STACK_INIT_PAGE - (current->cpu.sp >> PAGE_BITS);
        assert(new_pages > 0 && atomic_load(&anon_page_count) == initial_pages + new_pages);
        assert(!failures);
        puts("signal-frame-recovery-ok");
    } else {
        assert(kind); puts("signal-frame-refusal-ok");
    }
    write_wrlock(&current->mem->lock);
    assert(pt_unmap_always(current->mem, 0, MEM_PAGES) == 0);
    write_wrunlock(&current->mem->lock);
    assert(atomic_load(&anon_page_count) == 0);
}
