#!/bin/sh
set -eu
fixture=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=$(mktemp -d /tmp/go-aot.XXXXXX)
export PATH=/usr/lib/go/bin:/usr/local/bin:/usr/bin:/bin
export CGO_ENABLED=0 GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GO111MODULE=off
export GOMAXPROCS=2 GODEBUG=asyncpreemptoff=1
export GOCACHE=${GO_AOT_CACHE:-$work/cache}
export GOFLAGS=-p=1
run_step() {
    step=$1
    shift
    if [ "${GO_AOT_TIMING:-0}" = 1 ]; then
        started=$(date +%s)
        printf 'GO_AOT_STEP_BEGIN %s %s\n' "$step" "$started" >&2
        "$@"
        finished=$(date +%s)
        printf 'GO_AOT_STEP_END %s %s seconds=%s\n' "$step" "$finished" "$((finished - started))" >&2
    else
        "$@"
    fi
}
trap 'run_step cleanup rm -rf "$work"' EXIT
trap 'exit 1' INT TERM
test "$(go version)" = 'go version go1.26.8 linux/arm64'
cp "$fixture/main.go" "$fixture/probe.go" "$fixture/probe.s" "$fixture/probe.cfg" "$work/"
cd "$work"
# Give vet its own recording space before the other tools fill the code arena.
if [ "${GO_AOT_VET_FIRST:-0}" = 1 ]; then
    /usr/lib/go/pkg/tool/linux_arm64/vet probe.cfg
fi
run_step format gofmt -w main.go probe.go
run_step compile go tool compile -p probe -o probe.a probe.go
run_step assemble go tool asm -p probe -o probe.o probe.s
if [ "${GO_AOT_REBUILD:-0}" = 1 ]; then
    run_step build go build -a -o hello main.go
else
    run_step build go build -o hello main.go
fi
test "$(run_step run ./hello)" = 'GO_AOT_RUN_OK 45'
run_step vet go vet main.go
# Exercise the analyzer itself even when go vet reuses a cached result.
run_step analyze go tool vet probe.cfg
printf 'package invalid\nfunc broken(\n' > invalid.go
if go tool compile -o invalid.a invalid.go > invalid.log 2>&1; then
    echo 'Go compiler accepted invalid source' >&2
    exit 1
fi
grep -q 'syntax error' invalid.log
printf 'GO_AOT_BUILD_OK\n'
