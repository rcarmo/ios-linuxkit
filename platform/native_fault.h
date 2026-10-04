#ifndef ISH_NATIVE_FAULT_H
#define ISH_NATIVE_FAULT_H
#include <signal.h>
// Called only from an installed SA_SIGINFO adapter, on the faulting thread.
// No handler installation, allocation, locks or signal-mask changes here.
enum ish_fault_result { ISH_FAULT_UNHANDLED, ISH_FAULT_REDIRECTED, ISH_FAULT_FATAL };
enum ish_fault_result ish_native_fault_recover(int sig, const siginfo_t *info,
        void *context, int allow_gadget_replay);
// App adapter: exact native checkpoints only; does not install itself.
// Caller must fail-stop on FATAL and preserve its previous non-native policy.
enum ish_fault_result ish_app_native_fault_recover(int sig,
        const siginfo_t *info, void *context);
// Install once before starting guest threads. Preserve the previous policy for
// non-native faults; unmatched native faults terminate instead of replaying.
int ish_app_native_fault_install(void);
#endif
