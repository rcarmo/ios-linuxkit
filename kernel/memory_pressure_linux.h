#ifndef MEMORY_PRESSURE_LINUX_H
#define MEMORY_PRESSURE_LINUX_H

#include <stdbool.h>
#include <stdint.h>

// Opt-in, synchronous Linux-only sampler. No emulator dependencies, global
// state, worker, policy integration or host/cgroup writes. Link this .c only
// where wanted. options_init supplies live defaults; root_path prefixes the
// absolute mountpoint from mountinfo, proc_path names the proc tree directly.
// Both are explicit trusted paths (fixture roots work without a real mount).
// Descendants of root_path may not be symlinks; proc/self is intentionally
// followed. Unified membership must map to exactly one cgroup2 mount root;
// ambiguous mounts, noncanonical paths and escapes are refused.
//
// Every VISIBLE ancestor, including the mount root, is inspected with its own
// memory.current and both memory.max/high. Only a true hierarchy root (mount
// root "/") may omit ALL three controls, as Linux normally does. PSI is
// required at every ancestor and in proc/pressure/memory. Hidden ancestors
// outside a delegated mount/namespace cannot be discovered or guaranteed.
// MemTotal and MemAvailable are required. Numeric input is strict and bounded.
//
// Two observations recheck membership, mountinfo, directory identities and
// limits. Changing controls/membership retry at most max_attempts (1..3).
// Currents/host availability use the conservative minimum headroom of both
// observations; PSI uses the maximum avg10. This is NOT an atomic snapshot or
// reservation: ABA changes and usage after reads remain possible. Consumers
// must enforce their own freshness and policy. Time bounds reject slow or
// backwards observations, but synchronous file I/O is not deadline-safe.
//
// On success available_bytes = saturating(headroom - safety_reserve_bytes).
// allowance_bytes = saturating(limiting capacity - safety_reserve_bytes), from
// the SAME source: MemTotal for host, numeric max/high for cgroup. Thus fractions
// have a meaningful denominator; allowance is NOT additional free memory.
// Ties retain host, then leaf-to-root max before high. critical means exhausted
// headroom after reserve or a configured PSI avg10 threshold was met; PSI does
// not silently scale available_bytes. Thresholds use milli-percent (100000 =
// 100%); zero disables that threshold. Source path is a cgroup hierarchy path.
//
// Returns 0 on success, -1 with errno on failure. Failure zeroes the entire
// snapshot (source INVALID); callers MUST NOT treat this as a usable sample.
// EAGAIN = observed churn/time violation; EINVAL = configuration/input syntax;
// EOVERFLOW = bounds/overflow; other errors include missing/unreadable files.
// Optional callbacks are for deterministic tests; caller owns their context,
// must not mutate options, and must supply monotonic nanoseconds for now_ns.
// between_reads runs once per attempt before the second full observation.
#define MEMORY_PRESSURE_LINUX_PATH_MAX 4096

enum memory_pressure_linux_source {
    MEMORY_PRESSURE_LINUX_INVALID = 0,
    MEMORY_PRESSURE_LINUX_HOST,
    MEMORY_PRESSURE_LINUX_CGROUP_MAX,
    MEMORY_PRESSURE_LINUX_CGROUP_HIGH,
};

struct memory_pressure_linux_options {
    const char *root_path;
    const char *proc_path;
    uint64_t safety_reserve_bytes;
    uint64_t max_sample_ns;
    unsigned max_attempts;
    unsigned psi_some_millipercent;
    unsigned psi_full_millipercent;
    int (*now_ns)(void *context, uint64_t *ns);
    void (*between_reads)(void *context, unsigned attempt);
    void *context;
};

struct memory_pressure_linux_snapshot {
    uint64_t capture_ns;
    uint64_t finish_ns;
    uint64_t available_bytes;
    uint64_t allowance_bytes;
    unsigned psi_some_millipercent;
    unsigned psi_full_millipercent;
    bool psi;
    bool critical;
    enum memory_pressure_linux_source source;
    char source_path[MEMORY_PRESSURE_LINUX_PATH_MAX];
};

void memory_pressure_linux_options_init(struct memory_pressure_linux_options *options);
int memory_pressure_linux_sample(const struct memory_pressure_linux_options *options,
        struct memory_pressure_linux_snapshot *snapshot);

// Optional policy bridge (memory_pressure_feed.c), separate from the standalone
// sampler. feed requires enabled policy and invalidates on any sampling failure.
int memory_pressure_linux_feed(const struct memory_pressure_linux_options *options,
        struct memory_pressure_linux_snapshot *snapshot);
// Process-lifetime CLI worker: 250ms interval, 64MiB reserve, explicit opt-in.
// stop joins the worker; EBUSY if an active ticket prevents disabling admission.
int memory_pressure_linux_start(void);
int memory_pressure_linux_stop(void);

#endif
