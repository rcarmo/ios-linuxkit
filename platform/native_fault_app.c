#include "platform/native_fault.h"
#include <errno.h>
#include <pthread.h>
#include <string.h>
enum ish_fault_result ish_app_native_fault_recover(int sig,
        const siginfo_t *info,void *context) {
#ifdef ISH_JIT
    return ish_native_fault_recover(sig,info,context,0);
#else
    (void)sig; (void)info; (void)context;
    return ISH_FAULT_UNHANDLED;
#endif
}

#if defined(ISH_JIT) && defined(__aarch64__)
static struct sigaction previous_segv, previous_bus;
static pthread_once_t install_once = PTHREAD_ONCE_INIT;
static int install_error;

static void default_fault(int sig, const siginfo_t *info) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(sig, &action, NULL);
    // Hardware faults re-enter the untouched faulting instruction under the
    // default disposition, preserving its original PC/address in diagnostics.
    // An asynchronous signal needs to be raised again instead.
    if (!info || info->si_code <= 0)
        raise(sig);
}

static void app_fault_handler(int sig, siginfo_t *info, void *context) {
    enum ish_fault_result result = ish_app_native_fault_recover(sig, info, context);
    if (result == ISH_FAULT_REDIRECTED)
        return;
    const struct sigaction *previous = sig == SIGBUS ? &previous_bus : &previous_segv;
    if (result == ISH_FAULT_FATAL || previous->sa_handler == SIG_DFL) {
        default_fault(sig, info);
        return;
    }
    if (previous->sa_handler == SIG_IGN) {
        // Ignoring an asynchronous signal is valid; ignoring a hardware fault
        // only re-enters the same failing instruction.
        if (info && info->si_code > 0)
            default_fault(sig, info);
        return;
    }
    if (previous->sa_flags & SA_RESETHAND) {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(sig, &action, NULL);
    }
    if (previous->sa_flags & SA_SIGINFO)
        previous->sa_sigaction(sig, info, context);
    else
        previous->sa_handler(sig);
}

static void install_handler(void) {
    if (sigaction(SIGSEGV, NULL, &previous_segv) < 0 || sigaction(SIGBUS, NULL, &previous_bus) < 0) {
        install_error = errno;
        return;
    }
    struct sigaction action = previous_segv;
    action.sa_sigaction = app_fault_handler;
    action.sa_flags = (action.sa_flags & (SA_ONSTACK | SA_NODEFER | SA_RESTART)) | SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, NULL) < 0) {
        install_error = errno;
        return;
    }
    action = previous_bus;
    action.sa_sigaction = app_fault_handler;
    action.sa_flags = (action.sa_flags & (SA_ONSTACK | SA_NODEFER | SA_RESTART)) | SA_SIGINFO;
    if (sigaction(SIGBUS, &action, NULL) < 0) {
        install_error = errno;
        sigaction(SIGSEGV, &previous_segv, NULL);
    }
}
#endif

int ish_app_native_fault_install(void) {
#if defined(ISH_JIT) && defined(__aarch64__)
    int error = pthread_once(&install_once, install_handler);
    if (error || install_error) {
        errno = error ? error : install_error;
        return -1;
    }
    return 0;
#else
    errno = ENOTSUP;
    return -1;
#endif
}
