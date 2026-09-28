#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

ISH_BIN="${ISH_BIN:-$PROJECT_DIR/build-arm64-linux/ish}"
ROOTFS="${ROOTFS:-$PROJECT_DIR/alpine-arm64-fakefs}"
TIMEOUT_S="${TIMEOUT_S:-120}"
REPORT_DIR="${REPORT_DIR:-/workspace/tmp}"
STAMP="$(date +%Y%m%d-%H%M%S)"
REPORT="$REPORT_DIR/ish-arm64-internal-continue-fixtures-$STAMP.md"
GUEST_WORK="/tmp/arm64-internal-continue-fixtures"
HOST_TMP="$(mktemp -d)"

PASS_COUNT=0
FAIL_COUNT=0
TOTAL_COUNT=0
REPORT_ROWS=""

cleanup() {
    rm -rf "$HOST_TMP"
}
trap cleanup EXIT

mkdir -p "$REPORT_DIR"

append_row() {
    local name="$1"
    local status="$2"
    local detail="$3"
    REPORT_ROWS+="| $name | $status | ${detail//$'\n'/<br>} |"$'\n'
}

run_audit_test() {
    local name="$1"
    local func="$2"
    local expect_text="$3"
    local out="$HOST_TMP/${name//[^A-Za-z0-9_]/_}.out"

    TOTAL_COUNT=$((TOTAL_COUNT + 1))
    printf '[internal-continue] %s ... ' "$name"

    if "$func" >"$out" 2>&1 && grep -qx "$expect_text" "$out"; then
        PASS_COUNT=$((PASS_COUNT + 1))
        echo PASS
        append_row "$name" PASS "$(sed -n '1,8p' "$out" | sed 's/|/\\|/g')"
        return
    fi

    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo FAIL
    append_row "$name" FAIL "$(sed -n '1,20p' "$out" | sed 's/|/\\|/g')"
}

run_host_test() {
    local name="$1"
    local env_spec="$2"
    local cmd="$3"
    local expect_kind="$4"
    local expect_text="$5"
    local out="$HOST_TMP/${name//[^A-Za-z0-9_]/_}.out"

    TOTAL_COUNT=$((TOTAL_COUNT + 1))
    printf '[internal-continue] %s ... ' "$name"

    local -a env_args=()
    if [ -n "$env_spec" ]; then
        # shellcheck disable=SC2206
        env_args=($env_spec)
    fi

    if env "${env_args[@]}" timeout "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /bin/sh -c "export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin; $cmd" >"$out" 2>&1; then
        case "$expect_kind" in
            exact)
                if grep -qx "$expect_text" "$out" && ! grep -q 'ARM64_FUSION_STATS' "$out"; then
                    PASS_COUNT=$((PASS_COUNT + 1))
                    echo PASS
                    append_row "$name" PASS "$(sed -n '1,8p' "$out" | sed 's/|/\\|/g')"
                    return
                fi
                ;;
            stats-zero)
                if grep -qx "$expect_text" "$out" && grep -Eq 'internal_continue=0($|[[:space:]])' "$out"; then
                    PASS_COUNT=$((PASS_COUNT + 1))
                    echo PASS
                    append_row "$name" PASS "$(sed -n '1,8p' "$out" | sed 's/|/\\|/g')"
                    return
                fi
                ;;
            stats-positive)
                if grep -qx "$expect_text" "$out" && grep -Eq 'internal_continue=[1-9][0-9]*' "$out"; then
                    PASS_COUNT=$((PASS_COUNT + 1))
                    echo PASS
                    append_row "$name" PASS "$(sed -n '1,8p' "$out" | sed 's/|/\\|/g')"
                    return
                fi
                ;;
        esac
    fi

    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo FAIL
    append_row "$name" FAIL "$(sed -n '1,20p' "$out" | sed 's/|/\\|/g')"
}

source_default_off_audit() {
    cd "$PROJECT_DIR"

    grep -Fq 'arm64_internal_continue_set_enabled_from_env(getenv("ISH_ARM64_INTERNAL_CONTINUE"))' main.c
    grep -Fq "arm64_internal_continue_enabled = env != NULL && env[0] != '\\0' && strcmp(env, \"0\") != 0;" asbestos/guest-arm64/gen.c

    if grep -R "ISH_ARM64_INTERNAL_CONTINUE\|ARM64_INTERNAL_CONTINUE" app --include='*.m' --include='*.h' --include='*.c' --include='*.mm' --include='*.xcconfig' --include='*.plist' >/dev/null 2>&1; then
        echo "internal-continue env leaked into app config/source" >&2
        return 1
    fi

    printf 'ios-default-off-audit-ok\n'
}

push_tree() {
    local src="$1"
    local dst="$2"
    tar -C "$src" -cf - . | timeout "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /bin/sh -c "rm -rf '$dst' && mkdir -p '$dst' && tar -xf - -C '$dst'"
}

write_report() {
    cat >"$REPORT" <<EOF_REPORT
# ARM64 internal-continue fixture report

- Generated: $(date -Is)
- Binary: $ISH_BIN
- Rootfs: $ROOTFS
- Timeout: ${TIMEOUT_S}s
- Total: $TOTAL_COUNT
- Passed: $PASS_COUNT
- Failed: $FAIL_COUNT

| Test | Status | Detail |
|---|---|---|
$REPORT_ROWS
EOF_REPORT
    printf '\n%s\n' "$REPORT"
}

prepare_fixture() {
    local dir="$HOST_TMP/src"
    mkdir -p "$dir"
    cat >"$dir/internal_continue_fixture.c" <<'EOF_C'
#define _GNU_SOURCE
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <ucontext.h>

uintptr_t expected_internal_fault_pc;

int ic_branch_fixture(int v);
int ic_fallthrough_fixture(int v);
int ic_cbz_taken_fixture(int v);
int ic_cbnz_taken_fixture(int v);
int ic_cbz_fallthrough_fixture(int v);
int ic_cbnz_fallthrough_fixture(int v);
int ic_tbz_taken_fixture(int v);
int ic_tbnz_taken_fixture(int v);
int ic_tbz_fallthrough_fixture(int v);
int ic_tbnz_fallthrough_fixture(int v);
int ic_call_adjacent_fixture(int v);
void ic_internal_fault_fixture(int v);
int ic_invalidation_fixture(int v);
extern uint32_t ic_invalidation_patch_site;

__asm__(
".text\n"
".align 2\n"
".global ic_branch_fixture\n"
"ic_branch_fixture:\n"
"    cmp w0, #0\n"
"    b.eq 1f\n"
"    mov w0, #22\n"
"    ret\n"
"1:  mov w0, #11\n"
"    ret\n"
".global ic_cbz_taken_fixture\n"
"ic_cbz_taken_fixture:\n"
"    cbz w0, 1f\n"
"    b 2f\n"
"1:  mov w0, #11\n"
"    ret\n"
"2:  mov w0, #22\n"
"    ret\n"
".global ic_cbnz_taken_fixture\n"
"ic_cbnz_taken_fixture:\n"
"    cbnz w0, 1f\n"
"    b 2f\n"
"1:  mov w0, #11\n"
"    ret\n"
"2:  mov w0, #22\n"
"    ret\n"
".global ic_tbz_taken_fixture\n"
"ic_tbz_taken_fixture:\n"
"    tbz w0, #0, 1f\n"
"    b 2f\n"
"1:  mov w0, #11\n"
"    ret\n"
"2:  mov w0, #22\n"
"    ret\n"
".global ic_tbnz_taken_fixture\n"
"ic_tbnz_taken_fixture:\n"
"    tbnz w0, #0, 1f\n"
"    b 2f\n"
"1:  mov w0, #11\n"
"    ret\n"
"2:  mov w0, #22\n"
"    ret\n"
".global ic_fallthrough_fixture\n"
"ic_fallthrough_fixture:\n"
"    b 3f\n"
"2:  mov w0, #22\n"
"    ret\n"
"3:  cmp w0, #0\n"
"    b.eq 2b\n"
"    mov w0, #11\n"
"    ret\n"
".global ic_cbz_fallthrough_fixture\n"
"ic_cbz_fallthrough_fixture:\n"
"    b 4f\n"
"3:  mov w0, #22\n"
"    ret\n"
"4:  cbz w0, 3b\n"
"    mov w0, #11\n"
"    ret\n"
".global ic_cbnz_fallthrough_fixture\n"
"ic_cbnz_fallthrough_fixture:\n"
"    b 6f\n"
"5:  mov w0, #22\n"
"    ret\n"
"6:  cbnz w0, 5b\n"
"    mov w0, #11\n"
"    ret\n"
".global ic_tbz_fallthrough_fixture\n"
"ic_tbz_fallthrough_fixture:\n"
"    b 8f\n"
"7:  mov w0, #22\n"
"    ret\n"
"8:  tbz w0, #0, 7b\n"
"    mov w0, #11\n"
"    ret\n"
".global ic_tbnz_fallthrough_fixture\n"
"ic_tbnz_fallthrough_fixture:\n"
"    b 12f\n"
"11: mov w0, #22\n"
"    ret\n"
"12: tbnz w0, #0, 11b\n"
"    mov w0, #11\n"
"    ret\n"
".global ic_call_adjacent_fixture\n"
"ic_call_adjacent_fixture:\n"
"    b 5f\n"
"4:  add w0, w0, #9\n"
"    ret\n"
"5:  stp x29, x30, [sp, #-16]!\n"
"    bl ic_call_helper_fixture\n"
"    ldp x29, x30, [sp], #16\n"
"    cmp w0, #7\n"
"    b.eq 4b\n"
"    add w0, w0, #5\n"
"    ret\n"
"ic_call_helper_fixture:\n"
"    add w0, w0, #1\n"
"    ret\n"
".global ic_internal_fault_fixture\n"
"ic_internal_fault_fixture:\n"
"    b 7f\n"
"6:  ret\n"
"7:  cmp w0, #0\n"
"    b.eq 6b\n"
"    adrp x10, expected_internal_fault_pc\n"
"    add x10, x10, :lo12:expected_internal_fault_pc\n"
"    adr x9, 8f\n"
"    str x9, [x10]\n"
"    mov x11, xzr\n"
"8:  ldr x12, [x11]\n"
"    mov w0, #99\n"
"    ret\n"
".global ic_invalidation_fixture\n"
".global ic_invalidation_patch_site\n"
"ic_invalidation_fixture:\n"
"    b 10f\n"
"9:  mov w0, #17\n"
"    ret\n"
"10: cmp w0, #0\n"
"    b.eq 9b\n"
"ic_invalidation_patch_site:\n"
"    mov w0, #31\n"
"    ret\n"
);

static sigjmp_buf jb;
static volatile uintptr_t observed_pc;
static volatile uintptr_t observed_fault;

static void handler(int sig, siginfo_t *si, void *uctx) {
    (void)sig;
    ucontext_t *uc = (ucontext_t *)uctx;
    observed_pc = (uintptr_t)uc->uc_mcontext.pc;
    observed_fault = (uintptr_t)si->si_addr;
    siglongjmp(jb, 1);
}

static int branch_fixture(void) {
    int fail = 0;
    for (int i = 0; i < 64; i++) {
        if (ic_branch_fixture(0) != 11)
            fail++;
        if (ic_branch_fixture(1) != 22)
            fail++;
    }
    if (fail) {
        printf("branch-fail %d\n", fail);
        return 1;
    }
    puts("branch-ok");
    return 0;
}

static int fallthrough_fixture(void) {
    int fail = 0;
    for (int i = 0; i < 64; i++) {
        if (ic_fallthrough_fixture(0) != 22)
            fail++;
        if (ic_fallthrough_fixture(1) != 11)
            fail++;
    }
    if (fail) {
        printf("fallthrough-fail %d\n", fail);
        return 1;
    }
    puts("fallthrough-ok");
    return 0;
}

static int cbz_taken_fixture(void) {
    int fail = 0;
    for (int i = 0; i < 64; i++) {
        if (ic_cbz_taken_fixture(0) != 11)
            fail++;
        if (ic_cbz_taken_fixture(1) != 22)
            fail++;
        if (ic_cbnz_taken_fixture(0) != 22)
            fail++;
        if (ic_cbnz_taken_fixture(1) != 11)
            fail++;
    }
    if (fail) {
        printf("cbz-taken-fail %d\n", fail);
        return 1;
    }
    puts("cbz-taken-ok");
    return 0;
}

static int cbz_fallthrough_fixture(void) {
    int fail = 0;
    for (int i = 0; i < 64; i++) {
        if (ic_cbz_fallthrough_fixture(0) != 22)
            fail++;
        if (ic_cbz_fallthrough_fixture(1) != 11)
            fail++;
        if (ic_cbnz_fallthrough_fixture(0) != 11)
            fail++;
        if (ic_cbnz_fallthrough_fixture(1) != 22)
            fail++;
    }
    if (fail) {
        printf("cbz-fallthrough-fail %d\n", fail);
        return 1;
    }
    puts("cbz-fallthrough-ok");
    return 0;
}

static int tbz_taken_fixture(void) {
    int fail = 0;
    for (int i = 0; i < 64; i++) {
        if (ic_tbz_taken_fixture(0) != 11)
            fail++;
        if (ic_tbz_taken_fixture(1) != 22)
            fail++;
        if (ic_tbnz_taken_fixture(0) != 22)
            fail++;
        if (ic_tbnz_taken_fixture(1) != 11)
            fail++;
    }
    if (fail) {
        printf("tbz-taken-fail %d\n", fail);
        return 1;
    }
    puts("tbz-taken-ok");
    return 0;
}

static int tbz_fallthrough_fixture(void) {
    int fail = 0;
    for (int i = 0; i < 64; i++) {
        if (ic_tbz_fallthrough_fixture(0) != 22)
            fail++;
        if (ic_tbz_fallthrough_fixture(1) != 11)
            fail++;
        if (ic_tbnz_fallthrough_fixture(0) != 11)
            fail++;
        if (ic_tbnz_fallthrough_fixture(1) != 22)
            fail++;
    }
    if (fail) {
        printf("tbz-fallthrough-fail %d\n", fail);
        return 1;
    }
    puts("tbz-fallthrough-ok");
    return 0;
}

static int call_adjacent_fixture(void) {
    int fail = 0;
    for (int i = 0; i < 64; i++) {
        if (ic_call_adjacent_fixture(6) != 16)
            fail++;
        if (ic_call_adjacent_fixture(1) != 7)
            fail++;
    }
    if (fail) {
        printf("call-adjacent-fail %d\n", fail);
        return 1;
    }
    puts("call-adjacent-ok");
    return 0;
}

static int invalidation_fixture(void) {
    int before = ic_invalidation_fixture(1);
    int taken = ic_invalidation_fixture(0);
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return 1;
    uintptr_t site = (uintptr_t)&ic_invalidation_patch_site;
    uintptr_t page = site & ~((uintptr_t)page_size - 1);
    if (mprotect((void *)page, (size_t)page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("mprotect");
        return 1;
    }
    ic_invalidation_patch_site = 0x52800560u; // mov w0, #43
    __builtin___clear_cache((char *)site, (char *)site + sizeof(ic_invalidation_patch_site));
    int after = ic_invalidation_fixture(1);
    int taken_after = ic_invalidation_fixture(0);
    if (before != 31 || taken != 17 || after != 43 || taken_after != 17) {
        printf("invalidation-fail %d %d %d %d\n", before, taken, after, taken_after);
        return 1;
    }
    puts("invalidation-ok");
    return 0;
}

static int fault_fixture(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL) != 0)
        return 1;

    if (sigsetjmp(jb, 1) == 0) {
        ic_internal_fault_fixture(1);
        puts("missing-fault");
        return 1;
    }

    if (observed_fault != 0 || observed_pc != expected_internal_fault_pc) {
        fprintf(stderr, "internal fault pc mismatch expected=%#lx observed=%#lx fault=%#lx\n",
                (unsigned long)expected_internal_fault_pc,
                (unsigned long)observed_pc,
                (unsigned long)observed_fault);
        return 1;
    }

    puts("internal-fault-pc-ok");
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s branch|fallthrough|cbz-taken|cbz-fallthrough|tbz-taken|tbz-fallthrough|call|fault|invalidation\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "branch") == 0)
        return branch_fixture();
    if (strcmp(argv[1], "fallthrough") == 0)
        return fallthrough_fixture();
    if (strcmp(argv[1], "cbz-taken") == 0)
        return cbz_taken_fixture();
    if (strcmp(argv[1], "cbz-fallthrough") == 0)
        return cbz_fallthrough_fixture();
    if (strcmp(argv[1], "tbz-taken") == 0)
        return tbz_taken_fixture();
    if (strcmp(argv[1], "tbz-fallthrough") == 0)
        return tbz_fallthrough_fixture();
    if (strcmp(argv[1], "call") == 0)
        return call_adjacent_fixture();
    if (strcmp(argv[1], "fault") == 0)
        return fault_fixture();
    if (strcmp(argv[1], "invalidation") == 0)
        return invalidation_fixture();
    return 2;
}
EOF_C
}

main() {
    [ -x "$ISH_BIN" ] || { echo "missing ish binary: $ISH_BIN" >&2; exit 1; }
    [ -d "$ROOTFS" ] || { echo "missing rootfs: $ROOTFS" >&2; exit 1; }

    run_audit_test "source/iOS default-off audit" source_default_off_audit "ios-default-off-audit-ok"

    prepare_fixture
    # Minimal rootfs lanes need no guest toolchain: compile the same fixture
    # statically on a native AArch64 host when explicitly requested.
    if [ -n "${HOST_CC:-}" ]; then
        [ "$(uname -m)" = aarch64 ] || { echo 'HOST_CC requires native AArch64' >&2; exit 1; }
        "$HOST_CC" -O0 -static -fno-pie -no-pie "$HOST_TMP/src/internal_continue_fixture.c" \
            -o "$HOST_TMP/src/internal_continue_fixture" || exit 1
    fi
    push_tree "$HOST_TMP/src" "$GUEST_WORK"

    if [ -n "${HOST_CC:-}" ]; then
        run_host_test "host-built fixture available" "" "cd '$GUEST_WORK' && test -x internal_continue_fixture && echo build-ok" exact "build-ok"
    else
        run_host_test "build fixture" "" "cd '$GUEST_WORK' && command -v gcc >/dev/null && gcc -O0 -fno-pie -no-pie internal_continue_fixture.c -o internal_continue_fixture && test -x internal_continue_fixture && echo build-ok" exact "build-ok"
    fi
    [ "$FAIL_COUNT" -eq 0 ] || { write_report; return 1; }
    run_host_test "default branch fixture stays silent" "" "cd '$GUEST_WORK' && ./internal_continue_fixture branch" exact "branch-ok"
    run_host_test "stats-only default-off fixture" "ISH_ARM64_FUSION_STATS=1" "cd '$GUEST_WORK' && ./internal_continue_fixture branch" stats-zero "branch-ok"
    run_host_test "opt-in branch taken-internal fixture" "ISH_ARM64_FUSION_STATS=1 ISH_ARM64_INTERNAL_CONTINUE=1 ISH_ARM64_INTERNAL_CONTINUE_TAKEN=1" "cd '$GUEST_WORK' && ./internal_continue_fixture branch" stats-positive "branch-ok"
    run_host_test "opt-in branch fallthrough-internal fixture" "ISH_ARM64_FUSION_STATS=1 ISH_ARM64_INTERNAL_CONTINUE=1" "cd '$GUEST_WORK' && ./internal_continue_fixture fallthrough" stats-positive "fallthrough-ok"
    run_host_test "opt-in cbz/cbnz taken-internal fixture" "ISH_ARM64_FUSION_STATS=1 ISH_ARM64_INTERNAL_CONTINUE=1 ISH_ARM64_INTERNAL_CONTINUE_TAKEN=1" "cd '$GUEST_WORK' && ./internal_continue_fixture cbz-taken" stats-positive "cbz-taken-ok"
    run_host_test "opt-in cbz/cbnz fallthrough-internal fixture" "ISH_ARM64_FUSION_STATS=1 ISH_ARM64_INTERNAL_CONTINUE=1" "cd '$GUEST_WORK' && ./internal_continue_fixture cbz-fallthrough" stats-positive "cbz-fallthrough-ok"
    run_host_test "opt-in tbz/tbnz taken-internal fixture" "ISH_ARM64_FUSION_STATS=1 ISH_ARM64_INTERNAL_CONTINUE=1 ISH_ARM64_INTERNAL_CONTINUE_TAKEN=1" "cd '$GUEST_WORK' && ./internal_continue_fixture tbz-taken" stats-positive "tbz-taken-ok"
    run_host_test "opt-in tbz/tbnz fallthrough-internal fixture" "ISH_ARM64_FUSION_STATS=1 ISH_ARM64_INTERNAL_CONTINUE=1" "cd '$GUEST_WORK' && ./internal_continue_fixture tbz-fallthrough" stats-positive "tbz-fallthrough-ok"
    run_host_test "default same-page invalidation fixture" "" "cd '$GUEST_WORK' && ./internal_continue_fixture invalidation" exact "invalidation-ok"
    run_host_test "opt-in same-page invalidation fixture" "ISH_ARM64_FUSION_STATS=1 ISH_ARM64_INTERNAL_CONTINUE=1" "cd '$GUEST_WORK' && ./internal_continue_fixture invalidation" stats-positive "invalidation-ok"
    run_host_test "opt-in call-adjacent fixture" "ISH_ARM64_FUSION_STATS=1 ISH_ARM64_INTERNAL_CONTINUE=1" "cd '$GUEST_WORK' && ./internal_continue_fixture call" stats-positive "call-adjacent-ok"
    run_host_test "opt-in internal-segment fault PC fixture" "ISH_ARM64_FUSION_STATS=1 ISH_ARM64_INTERNAL_CONTINUE=1" "cd '$GUEST_WORK' && ./internal_continue_fixture fault" stats-positive "internal-fault-pc-ok"

    write_report
    [ "$FAIL_COUNT" -eq 0 ]
}

main "$@"
