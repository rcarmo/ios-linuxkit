#define _POSIX_C_SOURCE 200809L
#include "kernel/memory_pressure_linux.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[4096], proc[4096];
static unsigned checks;
static const char *quiet = "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"
        "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n";
static struct memory_pressure_linux_options options;
static struct memory_pressure_linux_snapshot result;

static void path(char *out, size_t cap, const char *name) {
    int n = snprintf(out, cap, "%s/%s", root, name);
    assert(n > 0 && (size_t) n < cap);
}
static void put(const char *name, const char *text) {
    char p[8192]; path(p, sizeof(p), name);
    FILE *f = fopen(p, "w"); assert(f);
    assert(fputs(text, f) >= 0); assert(fclose(f) == 0);
}
static void remove_file(const char *name) {
    char p[8192]; path(p, sizeof(p), name); assert(unlink(p) == 0);
}
static void mkdir_fixture(const char *name) {
    char p[8192]; path(p, sizeof(p), name); assert(mkdir(p, 0700) == 0);
}
static void controls(const char *dir, const char *current, const char *max, const char *high) {
    char p[8192];
    snprintf(p, sizeof(p), "%s/memory.current", dir); put(p, current);
    snprintf(p, sizeof(p), "%s/memory.max", dir); put(p, max);
    snprintf(p, sizeof(p), "%s/memory.high", dir); put(p, high);
    snprintf(p, sizeof(p), "%s/memory.pressure", dir); put(p, quiet);
}
static void reset(void) {
    memory_pressure_linux_options_init(&options);
    options.root_path = root; options.proc_path = proc;
    options.safety_reserve_bytes = 100;
    put("proc/self/cgroup", "0::/parent/leaf\n");
    put("proc/self/mountinfo", "31 26 0:27 / /cg rw shared:8 - cgroup2 cgroup2 rw\n");
    put("proc/meminfo", "MemTotal: 100 kB\nMemAvailable: 50 kB\n");
    put("proc/pressure/memory", quiet);
    controls("cg", "0\n", "max\n", "max\n");
    controls("cg/parent", "800\n", "1000\n", "max\n");
    controls("cg/parent/leaf", "10\n", "max\n", "max\n");
}
static void success(uint64_t available, uint64_t allowance,
        enum memory_pressure_linux_source source, const char *source_path) {
    int rc = memory_pressure_linux_sample(&options, &result);
    if (rc) fprintf(stderr, "check %u failed: %s\n", checks + 1, strerror(errno));
    assert(rc == 0);
    assert(result.available_bytes == available); assert(result.allowance_bytes == allowance);
    assert(result.source == source); assert(!strcmp(result.source_path, source_path));
    assert(result.finish_ns >= result.capture_ns); checks++;
}
static void failure(void) {
    memset(&result, 0xa5, sizeof(result));
    int rc = memory_pressure_linux_sample(&options, &result);
    if (rc != -1) fprintf(stderr, "check %u unexpectedly succeeded\n", checks + 1);
    assert(rc == -1);
    struct memory_pressure_linux_snapshot zero = {0};
    assert(!memcmp(&result, &zero, sizeof(result))); checks++;
}
struct injected { uint64_t time; unsigned ticks, mode, hooks; };
static int clock_fixture(void *context, uint64_t *ns) {
    struct injected *i = context;
    *ns = i->time + i->ticks++;
    if (i->mode == 5 && i->ticks == 3) *ns = 0;
    if (i->mode == 6 && i->ticks == 3) *ns += 1000000000;
    return 0;
}
static void mutate(void *context, unsigned attempt) {
    struct injected *i = context; i->hooks++;
    if (i->mode == 1) put("cg/parent/memory.max", attempt % 2 ? "1000\n" : "1100\n");
    if (i->mode == 2 && !attempt) put("cg/parent/memory.max", "1100\n");
    if (i->mode == 3) put("cg/parent/memory.current", "950\n");
    if (i->mode == 4) put("proc/self/cgroup", attempt % 2 ? "0::/parent/leaf\n" : "0::/parent\n");
    if (i->mode == 7) put("proc/self/mountinfo", attempt % 2 == 0 ?
            "31 26 0:27 / /cg rw - cgroup2 cgroup2 rw\n" :
            "31 26 0:27 / /cg rw shared:8 - cgroup2 cgroup2 rw\n");
    if (i->mode == 8) put("cg/parent/memory.high", attempt % 2 ? "max\n" : "900\n");
}
static void inject(struct injected *i, unsigned mode) {
    *i = (struct injected) {.time = 1000, .mode = mode};
    options.now_ns = clock_fixture; options.between_reads = mutate; options.context = i;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--live")) {
        memory_pressure_linux_options_init(&options);
        options.safety_reserve_bytes = 64 * 1024 * 1024;
        if (memory_pressure_linux_sample(&options, &result)) {
            fprintf(stderr, "live sample rejected (no fallback): %s\n", strerror(errno));
            return 1;
        }
        printf("live: available=%llu allowance=%llu source=%d path=%s psi=%u/%u critical=%d duration_ns=%llu\n",
                (unsigned long long) result.available_bytes, (unsigned long long) result.allowance_bytes,
                result.source, result.source_path, result.psi_some_millipercent,
                result.psi_full_millipercent, result.critical,
                (unsigned long long) (result.finish_ns - result.capture_ns));
        return 0;
    }
    assert(argc == 2); assert(strlen(argv[1]) < sizeof(root)); strcpy(root, argv[1]);
    int n = snprintf(proc, sizeof(proc), "%s/proc", root); assert(n > 0 && (size_t) n < sizeof(proc));
    mkdir_fixture("proc"); mkdir_fixture("proc/self"); mkdir_fixture("proc/pressure");
    mkdir_fixture("cg"); mkdir_fixture("cg/parent"); mkdir_fixture("cg/parent/leaf");
    reset(); success(100, 900, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent");
    // Ancestor usage includes siblings; unlimited leaf never supplies its own current.
    put("cg/parent/memory.current", "950\n"); success(0, 900, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent");
    assert(result.critical);
    reset(); put("cg/parent/memory.high", "900\n"); success(0, 800, MEMORY_PRESSURE_LINUX_CGROUP_HIGH, "/parent");
    reset(); put("cg/parent/leaf/memory.max", "150\n"); success(40, 50, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent/leaf");
    reset(); put("cg/memory.max", "150\n"); success(50, 50, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/");
    reset(); put("cg/parent/memory.max", "max\n"); success(51100, 102300, MEMORY_PRESSURE_LINUX_HOST, "/proc/meminfo");
    put("cg/parent/memory.high", "900\n"); success(0, 800, MEMORY_PRESSURE_LINUX_CGROUP_HIGH, "/parent");
    reset(); put("proc/meminfo", "MemTotal: 100 kB\nMemAvailable: 0 kB\n");
    success(0, 102300, MEMORY_PRESSURE_LINUX_HOST, "/proc/meminfo");
    reset(); options.safety_reserve_bytes = UINT64_MAX; success(0, 0, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent");
    const char *invalid[] = {"", "1000", "-1\n", "+1\n", " 1\n", "1x\n", "maxx\n",
        "18446744073709551616\n", "100\n200\n", "\n", "1\n\n"};
    for (size_t j = 0; j < sizeof(invalid) / sizeof(*invalid); j++) {
        reset(); put("cg/parent/memory.max", invalid[j]); failure();
    }
    reset(); put("cg/parent/memory.current", "max\n"); failure();
    reset(); remove_file("cg/parent/memory.high"); failure();
    reset(); remove_file("cg/parent/memory.current"); failure();
    reset(); remove_file("cg/memory.current"); failure(); // Partial root is invalid.
    remove_file("cg/memory.max"); remove_file("cg/memory.high");
    success(100, 900, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent"); // Real Linux root shape.
    reset(); put("proc/meminfo", "MemTotal: 1 kB\nMemAvailable: 2 kB\n"); failure();
    put("proc/meminfo", "MemTotal: 1 kB\n"); failure();
    put("proc/meminfo", "MemTotal: 18446744073709551615 kB\nMemAvailable: 1 kB\n"); failure();
    put("proc/meminfo", "MemTotal: 100 kB\nMemAvailable: 1 kB\nMemAvailable: 1 kB\n"); failure();
    reset(); put("cg/parent/memory.pressure", "some avg10=1.25 avg60=0.00 avg300=0.00 total=1\n"
        "full avg10=0.20 avg60=0.00 avg300=0.00 total=1\n");
    success(100, 900, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent");
    assert(result.psi && result.critical && result.psi_some_millipercent == 1250 && result.psi_full_millipercent == 200);
    options.psi_some_millipercent = options.psi_full_millipercent = 0;
    success(100, 900, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent"); assert(!result.psi && !result.critical);
    reset(); put("proc/pressure/memory", "some avg10=100.01 avg60=0.00 avg300=0.00 total=0\n"
        "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"); failure();
    reset(); remove_file("cg/parent/memory.pressure"); failure();
    reset(); put("cg/parent/memory.pressure", "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"); failure();
    const char *memberships[] = {"0::/parent/../leaf\n", "0::/parent//leaf\n", "0::/parent/leaf/\n",
        "0::/parent/leaf\n0::/parent\n", "0:memory:/parent/leaf\n", "1:memory:/parent/leaf\n", "0::relative\n"};
    for (size_t j = 0; j < sizeof(memberships) / sizeof(*memberships); j++) {
        reset(); put("proc/self/cgroup", memberships[j]); failure();
    }
    reset(); put("proc/self/mountinfo", "31 26 0:27 /parent /cg/parent rw - cgroup2 cgroup2 rw\n");
    success(100, 900, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent");
    put("proc/self/mountinfo", "31 26 0:27 /parents /cg/parent rw - cgroup2 cgroup2 rw\n"); failure();
    reset(); put("proc/self/mountinfo", "31 26 0:27 / /cg rw - cgroup2 cgroup2 rw\n"
        "32 26 0:28 /parent /cg/parent rw - cgroup2 cgroup2 rw\n"); failure();
    put("proc/self/mountinfo", "31 26 0:27 / /cg/../cg rw - cgroup2 cgroup2 rw\n"); failure();
    put("proc/self/mountinfo", "x 26 0:27 / /cg rw - cgroup2 cgroup2 rw\n"); failure();
    put("proc/self/mountinfo", "31 26 0:27 / /cg\\057escape rw - cgroup2 cgroup2 rw\n"); failure();
    mkdir_fixture("cg space"); controls("cg space", "800\n", "1000\n", "max\n");
    reset(); put("proc/self/cgroup", "0::/parent\n");
    put("proc/self/mountinfo", "31 26 0:27 /parent /cg\\040space rw - cgroup2 cgroup2 rw\n");
    success(100, 900, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent");
    reset(); char linkpath[8192]; path(linkpath, sizeof(linkpath), "escape"); assert(symlink("cg", linkpath) == 0);
    put("proc/self/mountinfo", "31 26 0:27 / /escape rw - cgroup2 cgroup2 rw\n"); failure();
    reset(); char huge[300000]; memset(huge, '1', sizeof(huge) - 2); huge[sizeof(huge) - 2] = '\n'; huge[sizeof(huge) - 1] = 0;
    put("proc/self/mountinfo", huge); failure();
    reset(); put("cg/parent/memory.max", huge); failure();
    reset(); char embedded[] = "1000\0\n"; char p[8192]; path(p, sizeof(p), "cg/parent/memory.max");
    FILE *f = fopen(p, "w"); assert(f); assert(fwrite(embedded, 1, sizeof(embedded) - 1, f) == sizeof(embedded) - 1); assert(!fclose(f)); failure();
    struct injected i;
    reset(); inject(&i, 1); failure(); assert(i.hooks == 2 && errno == EAGAIN);
    reset(); inject(&i, 2); success(200, 1000, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent"); assert(i.hooks == 2);
    reset(); inject(&i, 3); success(0, 900, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent");
    reset(); inject(&i, 4); failure(); assert(i.hooks == 2);
    reset(); inject(&i, 5); failure(); assert(errno == EAGAIN);
    reset(); inject(&i, 6); failure(); assert(errno == EAGAIN);
    reset(); inject(&i, 7); failure();
    reset(); inject(&i, 8); failure();
    reset(); options.max_attempts = 4; failure();
    reset(); options.max_sample_ns = 0; failure();
    // Positive host headroom tighter than every visible cgroup bound.
    reset(); put("cg/parent/memory.max", "100000\n");
    success(51100, 102300, MEMORY_PRESSURE_LINUX_HOST, "/proc/meminfo");
    reset(); put("cg/parent/memory.high", "2000\n");
    success(100, 900, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent");
    // PSI from a non-limiting leaf still influences the critical indicator.
    reset(); put("cg/parent/leaf/memory.pressure", "some avg10=2.00 avg60=0.00 avg300=0.00 total=0\n"
        "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
    success(100, 900, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent"); assert(result.psi);
    reset(); put("proc/pressure/memory", "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"
        "full avg10=0.30 avg60=0.00 avg300=0.00 total=0\n");
    success(100, 900, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent"); assert(result.psi);
    reset(); put("cg/parent/memory.high", "1.0\n"); failure();
    reset(); put("cg/parent/memory.pressure", "some avg10=0.00 avg60=0.00 avg300=0.00 total=18446744073709551616\n"
        "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"); failure();
    // The visible mountpoint itself can be /; no duplicate/trailing slash.
    reset(); controls("", "0\n", "max\n", "max\n");
    put("proc/self/cgroup", "0::/parent\n");
    put("proc/self/mountinfo", "31 26 0:27 /parent / rw - cgroup2 cgroup2 rw\n");
    success(51100, 102300, MEMORY_PRESSURE_LINUX_HOST, "/proc/meminfo");
    // Delegated roots must have controls; hierarchy-root omission is not valid.
    reset(); put("proc/self/mountinfo", "31 26 0:27 /parent /cg/parent rw - cgroup2 cgroup2 rw\n");
    remove_file("cg/parent/memory.current"); remove_file("cg/parent/memory.max"); remove_file("cg/parent/memory.high"); failure();
    reset(); success(100, 900, MEMORY_PRESSURE_LINUX_CGROUP_MAX, "/parent");
    printf("memory-pressure-linux: %u fixture checks passed\n", checks);
    return 0;
}
