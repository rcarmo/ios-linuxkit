// Real Darwin SIGBUS/SIGSEGV contexts and statically linked Mach-O accesses.
#include "asbestos/guest-arm64/jit.c"
#include "platform/native_fault.c"
#include "platform/native_fault_app.c"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>

__thread struct fiber_frame *jit_active_frame;
__thread volatile sig_atomic_t in_jit;
__thread volatile uint64_t jit_saved_pc;
__thread volatile uint64_t jit_last_host_fault, jit_last_x7, jit_last_x10;
__thread volatile int jit_crash_count;
extern int apple_probe_call(void *, struct fiber_frame *, void *);
extern void apple_probe_read(void), apple_probe_write(void);
extern const uint32_t apple_probe_text_start[], apple_probe_text_end[];

static void previous_info(int sig, siginfo_t *info, void *context) {
    _exit((sig == SIGBUS || sig == SIGSEGV) && info && context ? 73 : 99);
}
static void previous_basic(int sig) { _exit(sig == SIGSEGV ? 74 : 99); }

static void child_check(unsigned mode, struct fiber_frame *frame, void *target) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        in_jit = mode != 0;
        frame->native_fault_host_pc = (uintptr_t)apple_probe_read + (mode == 1 ? 4 : 0);
        struct fiber_frame other = {0};
        apple_probe_call(apple_probe_read, mode == 2 ? &other : frame, target);
        _exit(99);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    if (mode == 0) assert(WIFEXITED(status) && WEXITSTATUS(status) == 73);
    else assert(WIFSIGNALED(status) && (WTERMSIG(status) == SIGBUS || WTERMSIG(status) == SIGSEGV));
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "basic")) {
        struct sigaction prior = {.sa_handler = previous_basic};
        sigemptyset(&prior.sa_mask);
        assert(sigaction(SIGSEGV, &prior, NULL) == 0);
        assert(ish_app_native_fault_install() == 0);
        raise(SIGSEGV);
        return 99;
    }
    struct sigaction prior = {.sa_sigaction = previous_info, .sa_flags = SA_SIGINFO};
    sigemptyset(&prior.sa_mask);
    assert(sigaction(SIGSEGV, &prior, NULL) == 0 && sigaction(SIGBUS, &prior, NULL) == 0);
    assert(ish_app_native_fault_install() == 0 && ish_app_native_fault_install() == 0);
    assert(jit_aot_prepare_layout() == 0);
    static struct aot_module image;
    image = (struct aot_module){.path = "apple-static-fault-probe", .abi = jit_abi(),
        .prologue_words = prologue_words, .entry_off = entry_off(), .n_pinned = n_pinned,
        .text_start = apple_probe_text_start, .text_end = apple_probe_text_end};
    ish_aot_register(&image);
    assert(jit_aot_start(1) == 0);
    assert(jit_aot_start(0) == -1);
    assert(jit_aot_start(2) == -1);
    assert(jit_units_begin() != NULL && aot_nimages == 1 && !region);
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    void *target = mmap(NULL, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(target != MAP_FAILED);
    struct fiber_frame frame = {0};
    jit_active_frame = &frame;
    for (unsigned write = 0; write < 2; write++) {
        void *access = write ? apple_probe_write : apple_probe_read;
        frame.native_fault_host_pc = (uintptr_t)access;
        frame.native_fault_guest_pc = 0x1234;
        frame.native_fault_addr = 0x4000;
        frame.native_fault_write = write;
        in_jit = 1;
        assert(apple_probe_call(access, &frame, target) == 0x100);
        in_jit = 0;
        assert(frame.cpu.pc == 0x1234 && frame.jit_saved_pc == 0x1234);
        assert(frame.cpu.segfault_addr == 0x4000 && frame.cpu.segfault_was_write == write);
        assert(!frame.native_fault_host_pc && jit_crash_count == (int)write + 1);
        assert(frame.cpu.regs[19] == 0x1357 && frame.cpu.nzcv == 0x60000000);
        for (unsigned i = 0; i < 16; i++) assert(frame.cpu.fp[0].b[i] == 0x37);
    }
    child_check(0, &frame, target); // non-native faults retain the previous handler
    child_check(1, &frame, target); // unmatched PC inside static image fails closed
    child_check(2, &frame, target); // wrong x1 cannot impersonate the trusted frame
    assert(munmap(target, page_size) == 0);
    puts("APPLE_STATIC_FAULT_OK: read/write, exact PC, SP, SIMD, NZCV, chaining and fail-stop");
}
