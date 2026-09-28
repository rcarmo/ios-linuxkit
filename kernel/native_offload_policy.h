// Adapted from OpenMinis 72dd6aaa. Header-only so Meson and Xcode use the
// same policy without adding a platform-specific compilation unit.
#ifndef NATIVE_OFFLOAD_POLICY_H
#define NATIVE_OFFLOAD_POLICY_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline bool native_offload_name_is_generic(const char *name) {
    return name != NULL && (!strcmp(name, "ffmpeg") || !strcmp(name, "ffprobe"));
}

// Do not replace arbitrary user binaries by basename. Exact paths only:
// relative paths, dot components and duplicate slashes safely fall through.
// Apple-only synthetic commands retain their existing path behaviour.
static inline bool native_offload_path_allowed(const char *name, const char *path) {
    if (name == NULL || path == NULL)
        return false;
    if (!native_offload_name_is_generic(name))
        return true;
    const char *const dirs[] = {"/bin/", "/usr/bin/", "/usr/local/bin/"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(*dirs); i++) {
        size_t n = strlen(dirs[i]);
        if (strlen(path) == n + strlen(name) && !strncmp(path, dirs[i], n) &&
                !strcmp(path + n, name))
            return true;
    }
    return false;
}

// Packed, double-NUL-terminated exec environment. Keep upstream's override
// for compatibility, and expose the neutral NO_OFFLOAD spelling as well.
static inline bool native_offload_env_disabled(const char *name, const char *envp) {
    if (!native_offload_name_is_generic(name) || envp == NULL)
        return false;
    for (const char *p = envp; *p; p += strlen(p) + 1)
        if (!strcmp(p, "MINIS_NO_FFMPEG_OFFLOAD=1") || !strcmp(p, "NO_OFFLOAD=1"))
            return true;
    return false;
}
#endif
