// Live, actual-archive policy bridge integration. Run ONLY via the companion
// runner in a new delegated unit. No controls outside that unit are written.
#define _GNU_SOURCE
#include "kernel/memory_policy.h"
#include "kernel/memory_pressure_linux.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MIB (UINT64_C(1024) * 1024)
static char base[4096], hierarchy[4096];
static unsigned checks;
static struct memory_pressure_linux_options options;
static uint64_t started;
static pid_t sibling = -1;
static _Noreturn void die(const char *message) {
    fprintf(stderr, "FAIL check=%u %s errno=%d (%s)\n", checks + 1,
            message, errno, strerror(errno));
    exit(1); // systemd KillMode=control-group also cleans up the sibling
}
#define REQUIRE(x) do { if (!(x)) die(#x); } while (0)
static void pass(const char *name) { printf("PASS %u %s\n", ++checks, name); }
static uint64_t now_ns(void) {
    struct timespec t; REQUIRE(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void path(char *out, size_t cap, const char *suffix) {
    int n = snprintf(out, cap, "%s/%s", base, suffix);
    REQUIRE(n > 0 && (size_t)n < cap);
}
static void write_text(const char *file, const char *text) {
    int fd = open(file, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(fd >= 0);
    size_t size = strlen(text);
    REQUIRE(write(fd, text, size) == (ssize_t)size); REQUIRE(!close(fd));
}
static void control(const char *name, const char *text) {
    char p[8192]; path(p, sizeof(p), name); write_text(p, text);
}
static void read_text(const char *file, char *out, size_t cap) {
    int fd = open(file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW); REQUIRE(fd >= 0);
    ssize_t n = read(fd, out, cap - 1); REQUIRE(n > 0 && (size_t)n < cap - 1);
    out[n] = 0; REQUIRE(!close(fd));
}
static uint64_t current_bytes(const char *name) {
    char p[8192], text[80], *end; path(p, sizeof(p), name);
    read_text(p, text, sizeof(text)); errno = 0;
    uint64_t n = strtoull(text, &end, 10); REQUIRE(!errno && !strcmp(end, "\n"));
    return n;
}
static void move_to(const char *dir) {
    char suffix[128], pid[64];
    REQUIRE(snprintf(suffix, sizeof(suffix), "%s/cgroup.procs", dir) > 0);
    snprintf(pid, sizeof(pid), "%ld\n", (long)getpid()); control(suffix, pid);
}
static struct memory_pressure_linux_snapshot feed(const char *stage,
        enum memory_pressure_linux_source source, uint64_t capacity) {
    struct memory_pressure_linux_snapshot s;
    REQUIRE(!memory_pressure_linux_feed(&options, &s));
    REQUIRE(s.source == source && !strcmp(s.source_path, hierarchy));
    REQUIRE(s.allowance_bytes == capacity - options.safety_reserve_bytes);
    struct ish_memory_sample policy; bool braked;
    REQUIRE(ish_memory_policy_snapshot(&policy, &braked));
    REQUIRE(policy.available_bytes == s.available_bytes &&
            policy.allowance_bytes == s.allowance_bytes && policy.critical == s.critical);
    printf("SNAPSHOT stage=%s source=%d path=%s current=%" PRIu64
            " leaf_current=%" PRIu64 " allowance=%" PRIu64 " headroom=%" PRIu64
            " reserve=%" PRIu64 " braked=%d critical=%d psi=%u/%u sample_ns=%" PRIu64 "\n",
            stage, s.source, s.source_path, current_bytes("memory.current"),
            current_bytes("leaf/memory.current"), s.allowance_bytes, s.available_bytes,
            options.safety_reserve_bytes, braked, s.critical,
            s.psi_some_millipercent, s.psi_full_millipercent, s.finish_ns - s.capture_ns);
    return s;
}
static bool claim(uint64_t bytes) {
    struct ish_memory_ticket t = {0};
    bool ok = ish_memory_reserve_ticket(0, bytes, &t);
    if (ok) REQUIRE(!anon_pages_finish_ticket(&t, false));
    return ok;
}
static void byte_write(int fd) { REQUIRE(write(fd, "x", 1) == 1); }
static void byte_read(int fd) { char c; REQUIRE(read(fd, &c, 1) == 1); }
static void fixture_file(const char *dir, const char *name, const char *text) {
    char p[8192]; int n = snprintf(p, sizeof(p), "%s/%s", dir, name);
    REQUIRE(n > 0 && (size_t)n < sizeof(p));
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    REQUIRE(fd >= 0); REQUIRE(!close(fd)); write_text(p, text);
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0); started = now_ns(); alarm(85);
    REQUIRE(argc == 4 && !strcmp(argv[1], "--delegated"));
    char *end; unsigned long reserve = strtoul(argv[3], &end, 10);
    REQUIRE(*argv[3] && !*end && reserve <= 8);
    memory_pressure_linux_options_init(&options);
    options.safety_reserve_bytes = reserve * MIB;
    // Isolate capacity/debt tests from unrelated host PSI. PSI is still read
    // and recorded; deterministic PSI semantics belong to sampler unit tests.
    options.psi_some_millipercent = options.psi_full_millipercent = 0;
    char text[4096]; read_text("/proc/meminfo", text, sizeof(text));
    char *available = strstr(text, "MemAvailable:"); unsigned long long available_kib;
    REQUIRE(available && sscanf(available, "MemAvailable: %llu kB", &available_kib) == 1);
    if (available_kib < 512 * 1024) {
        fprintf(stderr, "BLOCKER: host MemAvailable below 512MiB safety floor\n");
        return 77;
    }
    printf("SAFETY host_available_kib=%llu physical_cap_mib=160 task_alarm_s=85\n", available_kib);
    read_text("/proc/self/cgroup", text, sizeof(text));
    REQUIRE(!strncmp(text, "0::/", 4));
    char *newline = strchr(text, '\n'); REQUIRE(newline && !newline[1]); *newline = 0;
    REQUIRE(strlen(text + 3) < sizeof(hierarchy)); strcpy(hierarchy, text + 3);
    const char *unit = strrchr(hierarchy, '/');
    REQUIRE(unit && !strcmp(unit + 1, argv[2]) &&
            !strncmp(argv[2], "ish-memory-pressure-cgroup-", 26) &&
            strstr(argv[2], ".service") && !strchr(argv[2], '/'));
    REQUIRE(snprintf(base, sizeof(base), "/sys/fs/cgroup%s", hierarchy) < (int)sizeof(base));
    // Refuse to touch a shared/populated unit, even if incorrectly invoked.
    char p[8192], pid[64]; path(p, sizeof(p), "cgroup.procs");
    read_text(p, text, sizeof(text)); snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
    REQUIRE(!strcmp(text, pid));
    REQUIRE(current_bytes("memory.max") == 192 * MIB);
    REQUIRE(current_bytes("memory.high") == 160 * MIB);
    REQUIRE(current_bytes("memory.swap.max") == 0);
    for (unsigned i = 0; i < 3; i++) {
        const char *dirs[] = {"manager", "leaf", "sibling"};
        path(p, sizeof(p), dirs[i]); REQUIRE(!mkdir(p, 0700));
    }
    move_to("manager"); control("cgroup.subtree_control", "+memory\n");
    int ready[2], release[2]; REQUIRE(!pipe(ready) && !pipe(release));
    sibling = fork(); REQUIRE(sibling >= 0);
    if (!sibling) {
        close(ready[0]); close(release[1]); move_to("sibling");
        byte_write(ready[1]); byte_read(release[0]);
        // Exactly 96MiB resident pressure; no retrying or unbounded growth.
        size_t size = 96 * MIB;
        volatile unsigned char *memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        REQUIRE(memory != MAP_FAILED);
        long page_size = sysconf(_SC_PAGESIZE); REQUIRE(page_size > 0);
        for (size_t off = 0; off < size; off += (size_t)page_size) memory[off] = 1;
        byte_write(ready[1]); byte_read(release[0]);
        REQUIRE(!munmap((void *)memory, size)); byte_write(ready[1]);
        close(ready[1]); close(release[0]); _exit(0);
    }
    close(ready[1]); close(release[0]); byte_read(ready[0]); move_to("leaf");
    path(p, sizeof(p), "leaf/memory.max"); read_text(p, text, sizeof(text)); REQUIRE(!strcmp(text, "max\n"));
    path(p, sizeof(p), "leaf/memory.high"); read_text(p, text, sizeof(text)); REQUIRE(!strcmp(text, "max\n"));
    REQUIRE(ish_memory_policy_enable(true)); REQUIRE(!claim(MIB));
    pass("enabled-without-sample-refuses");
    struct memory_pressure_linux_snapshot before = feed("ancestor-unlimited-leaf", MEMORY_PRESSURE_LINUX_CGROUP_HIGH, 160 * MIB);
    REQUIRE(before.available_bytes >= 128 * MIB);
    REQUIRE(claim(MIB)); pass("finite-ancestor-unlimited-leaf-admits");
    // Exercise actual max and high controls, only on this newly-created unit.
    control("memory.high", "max\n");
    feed("max192", MEMORY_PRESSURE_LINUX_CGROUP_MAX, 192 * MIB);
    control("memory.max", "184549376\n"); // 176MiB, well above bounded usage
    feed("max176", MEMORY_PRESSURE_LINUX_CGROUP_MAX, 176 * MIB);
    control("memory.max", "201326592\n"); control("memory.high", "167772160\n");
    pass("live-max-limit-change");
    byte_write(release[1]); byte_read(ready[0]);
    struct memory_pressure_linux_snapshot loaded = feed("sibling-loaded", MEMORY_PRESSURE_LINUX_CGROUP_HIGH, 160 * MIB);
    REQUIRE(current_bytes("sibling/memory.current") >= 90 * MIB);
    REQUIRE(before.available_bytes > loaded.available_bytes + 80 * MIB);
    REQUIRE(claim(MIB)); pass("sibling-current-reduces-ancestor-headroom");
    struct ish_memory_ticket ticket = {0};
    REQUIRE(ish_memory_reserve_ticket(0, 16 * MIB, &ticket));
    uint64_t debt = anon_pages_finish_ticket(&ticket, true); REQUIRE(debt == 16 * MIB);
    loaded = feed("retained-ticket", MEMORY_PRESSURE_LINUX_CGROUP_HIGH, 160 * MIB);
    REQUIRE(ish_memory_policy_budget().retained_bytes == debt);
    REQUIRE(loaded.available_bytes > debt && !claim(loaded.available_bytes - debt + MIB));
    ish_memory_policy_release(debt); REQUIRE(claim(MIB));
    pass("fresh-feed-preserves-debt-and-bounded-ticket-refusal");
    control("memory.high", "109051904\n"); // 104MiB; do not approach memory.max
    feed("constrained", MEMORY_PRESSURE_LINUX_CGROUP_HIGH, 104 * MIB);
    bool braked; REQUIRE(ish_memory_policy_snapshot(NULL, &braked) && braked);
    REQUIRE(!claim(MIB)); pass("constrained-high-brakes-and-refuses");
    byte_write(release[1]); byte_read(ready[0]);
    int status; REQUIRE(waitpid(sibling, &status, 0) == sibling && WIFEXITED(status) && !WEXITSTATUS(status));
    sibling = -1; close(ready[0]); close(release[1]);
    feed("pressure-released", MEMORY_PRESSURE_LINUX_CGROUP_HIGH, 104 * MIB);
    REQUIRE(ish_memory_policy_snapshot(NULL, &braked) && !braked && claim(MIB));
    pass("pressure-release-recovers-admission");
    // Malformed proc fixture affects this feed only, never the host's procfs.
    char fixture[] = "/tmp/ish-pressure-proc-XXXXXX"; REQUIRE(mkdtemp(fixture));
    snprintf(p, sizeof(p), "%s/self", fixture); REQUIRE(!mkdir(p, 0700));
    fixture_file(fixture, "self/cgroup", "0::relative\n");
    fixture_file(fixture, "self/mountinfo", "1 0 0:1 / /sys/fs/cgroup rw - cgroup2 cgroup2 rw\n");
    options.proc_path = fixture;
    struct memory_pressure_linux_snapshot invalid;
    REQUIRE(memory_pressure_linux_feed(&options, &invalid) == -1 && errno == EINVAL);
    REQUIRE(ish_memory_policy_snapshot(NULL, &braked) && braked && !claim(MIB));
    pass("sampling-failure-invalidates-previous-good-policy");
    options.proc_path = "/proc";
    feed("failure-recovered", MEMORY_PRESSURE_LINUX_CGROUP_HIGH, 104 * MIB);
    REQUIRE(claim(MIB)); pass("fresh-feed-after-failure-recovers");
    snprintf(p, sizeof(p), "%s/self/cgroup", fixture); REQUIRE(!unlink(p));
    snprintf(p, sizeof(p), "%s/self/mountinfo", fixture); REQUIRE(!unlink(p));
    snprintf(p, sizeof(p), "%s/self", fixture); REQUIRE(!rmdir(p)); REQUIRE(!rmdir(fixture));
    struct ish_memory_budget budget = ish_memory_policy_budget();
    REQUIRE(!budget.pending_bytes && !budget.retained_bytes && !budget.transactions);
    REQUIRE(ish_memory_policy_enable(false));
    // The optional process-lifetime worker has explicit ownership and can be
    // joined even if a ticket temporarily prevents turning admission off.
    REQUIRE(memory_pressure_linux_start() == 0);
    REQUIRE(memory_pressure_linux_start() == -1 && errno == EALREADY);
    struct ish_memory_ticket active = {0};
    REQUIRE(ish_memory_reserve_ticket(0, 0, &active));
    REQUIRE(memory_pressure_linux_stop() == -1 && errno == EBUSY);
    anon_pages_finish_ticket(&active, false);
    REQUIRE(memory_pressure_linux_stop() == 0);
    REQUIRE(!ish_memory_policy_snapshot(NULL, NULL));
    pass("worker-start-stop-ownership-and-active-ticket");
    path(p, sizeof(p), "memory.events"); read_text(p, text, sizeof(text));
    printf("MEMORY_EVENTS\n%s", text);
    char *oom = strstr(text, "oom "), *kill = strstr(text, "oom_kill ");
    REQUIRE(oom && kill && strtoull(oom + 4, NULL, 10) == 0 && strtoull(kill + 9, NULL, 10) == 0);
    pass("clean-budget-no-cgroup-oom");
    move_to("manager");
    path(p, sizeof(p), "leaf"); REQUIRE(!rmdir(p));
    path(p, sizeof(p), "sibling"); REQUIRE(!rmdir(p));
    struct rusage self, children; REQUIRE(!getrusage(RUSAGE_SELF, &self) && !getrusage(RUSAGE_CHILDREN, &children));
    printf("RECEIPT wall_ms=%" PRIu64 " self_minflt=%ld self_majflt=%ld self_maxrss_kib=%ld"
            " children_minflt=%ld children_majflt=%ld children_maxrss_kib=%ld physical_pressure_mib=96\n",
            (now_ns() - started) / 1000000, self.ru_minflt, self.ru_majflt, self.ru_maxrss,
            children.ru_minflt, children.ru_majflt, children.ru_maxrss);
    REQUIRE(self.ru_maxrss + children.ru_maxrss <= 160 * 1024);
    puts("LIMITATIONS: synchronous observations are not atomic; PSI thresholds disabled for capacity isolation; CLI opt-in not exercised here (parent responsibility)");
    printf("memory-pressure-cgroup-actual-bridge-ok tests=%u\n", checks);
    return 0;
}
