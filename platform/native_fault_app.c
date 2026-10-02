// Prepared app adapter, intentionally not installed by any existing scheme.
#include "platform/native_fault.h"
enum ish_fault_result ish_app_native_fault_recover(int sig,
        const siginfo_t *info,void *context) {
#ifdef ISH_JIT
    return ish_native_fault_recover(sig,info,context,0);
#else
    (void)sig; (void)info; (void)context;
    return ISH_FAULT_UNHANDLED;
#endif
}
