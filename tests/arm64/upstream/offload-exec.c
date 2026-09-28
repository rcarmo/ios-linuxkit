// Actual sys_execve integration on Linux with only the Apple offload entry
// points wrapped. Real guest memory, generic_open and shebang guard are used.
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/memory.h"
#include "kernel/native_offload_policy.h"
#include "fs/real.h"
static unsigned offloads;
const char *__wrap_native_offload_lookup_exec(const char *path, const char *env, bool *generic) {
    *generic = true;
    return native_offload_path_allowed("ffmpeg", path) &&
        !native_offload_env_disabled("ffmpeg", env) ? "[test]" : NULL;
}
int __wrap_native_offload_exec(const char *native, const char *file,
        size_t argc, const char *argv, const char *env) {
    (void)native; (void)file; (void)argc; (void)argv; (void)env;
    offloads++;
    return 0; // Don't execute native code in the fixture.
}
static void probe(const char *path, bool expected) {
    addr_t args[] = {0x11000, 0};
    assert(user_write(0x11000, path, strlen(path) + 1) == 0);
    assert(user_write(0x12000, args, sizeof(args)) == 0);
    unsigned before = offloads;
    sys_execve(0x11000, 0x12000, 0);
    assert(offloads == before + expected);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(mount_root(&realfs, argv[1]) == 0);
    assert(become_first_process() == 0);
    assert(pt_map_nothing(current->mem, 0x10, 4, P_READ | P_WRITE) == 0);
    probe("/bin/ffmpeg", false); // a shebang wrapper, interpreter deliberately absent
    probe("/usr/bin/ffmpeg", true); // absent built-in command
    probe("/tmp/ffmpeg", false); // private binary must never be hijacked
    puts("offload-exec-shebang-ok");
    return 0;
}
