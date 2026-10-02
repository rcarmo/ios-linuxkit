// Shared synchronous fault core. OS register access stays in host_context.
#include <stddef.h>
#include <signal.h>
#include "platform/native_fault.h"
#include "platform/host_context_aarch64.h"
#include "asbestos/frame.h"
#ifdef ISH_JIT
#include "asbestos/guest-arm64/jit.h"
#endif
extern __thread volatile sig_atomic_t in_jit;
extern __thread volatile uint64_t jit_saved_pc;
extern __thread volatile uint64_t jit_last_host_fault, jit_last_x7, jit_last_x10;
extern __thread volatile int jit_crash_count;
extern void jit_crash_trampoline(void);
#include "cpu-offsets.h"
_Static_assert(offsetof(struct fiber_frame,cpu)==0,"fault ABI: frame base");
_Static_assert(offsetof(struct cpu_state,pc)==CPU_pc,"fault ABI: pc");
_Static_assert(offsetof(struct cpu_state,segfault_addr)==CPU_segfault_addr,"fault ABI: addr");
_Static_assert(offsetof(struct cpu_state,segfault_was_write)==CPU_segfault_was_write,"fault ABI: write");
_Static_assert(offsetof(struct fiber_frame,jit_exit_sp)==LOCAL_jit_exit_sp,"fault ABI: sp");
_Static_assert(offsetof(struct fiber_frame,jit_saved_pc)==LOCAL_jit_saved_pc,"fault ABI: retry");
enum ish_fault_result ish_native_fault_recover(int sig,const siginfo_t *info,
        void *context,int allow_gadget_replay) {
#if defined(__aarch64__)
    if((sig!=SIGSEGV && sig!=SIGBUS) || !in_jit || !context || !info)
        return ISH_FAULT_UNHANDLED;
    ucontext_t *uc=context;
    int native=0;
#ifdef ISH_JIT
    native=jit_crash_recover(uc);
    if(native<0) return ISH_FAULT_FATAL;
#endif
    if(!native && !allow_gadget_replay) return ISH_FAULT_UNHANDLED;
    uintptr_t cpu_ptr=host_ctx_aarch64_reg(uc,1);
    uint64_t x7=host_ctx_aarch64_reg(uc,7), x10=host_ctx_aarch64_reg(uc,10);
    // Native recovery validated x1 against dispatch's trusted frame. The CLI
    // gadget fallback deliberately retains its existing pinned-register ABI.
    struct fiber_frame *frame=(struct fiber_frame *)cpu_ptr;
    if(!frame) return ISH_FAULT_FATAL;
    if(!native) {
        frame->cpu.segfault_addr=(x7-x10)&0xffffffffffffULL;
        frame->cpu.segfault_was_write=host_ctx_aarch64_fault_was_write(uc,info)!=0;
        frame->cpu.pc=frame->jit_saved_pc ? frame->jit_saved_pc : (uint64_t)jit_saved_pc;
    }
    jit_last_host_fault=(uintptr_t)info->si_addr;
    jit_last_x7=x7; jit_last_x10=x10; jit_crash_count++;
    host_ctx_aarch64_set_sp(uc,frame->jit_exit_sp);
    host_ctx_aarch64_set_pc(uc,(uintptr_t)jit_crash_trampoline);
    return ISH_FAULT_REDIRECTED;
#else
    (void)sig; (void)info; (void)context; (void)allow_gadget_replay;
    return ISH_FAULT_UNHANDLED;
#endif
}
