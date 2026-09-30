// Opt-in host interface binding: raw Linux sockopt number 25 is guest ABI.
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 1;
    const char missing[] = "ish_no_such_if";
    if (setsockopt(s, SOL_SOCKET, 25, missing, sizeof(missing)) != -1 || errno != ENODEV) {
        puts("BIND_DEVICE_FAIL missing"); return 1;
    }
    struct if_nameindex *interfaces = if_nameindex();
    if (!interfaces || !interfaces[0].if_name) return 1;
    const char *name = interfaces[0].if_name;
    unsigned len = 0;
    while (name[len]) len++;
    if (setsockopt(s, SOL_SOCKET, 25, name, len + 1) != 0) {
        puts("BIND_DEVICE_FAIL known"); return 1;
    }
    char empty = 0;
    // Linux may allow the first bind without CAP_NET_RAW but require the
    // capability for rebinding/unbinding. Preserve that host policy.
    if (setsockopt(s, SOL_SOCKET, 25, &empty, 1) != 0 && errno != EPERM) {
        puts("BIND_DEVICE_FAIL unbind"); return 1;
    }
    if_freenameindex(interfaces);
    close(s);
    puts("BIND_DEVICE_PASS");
}
