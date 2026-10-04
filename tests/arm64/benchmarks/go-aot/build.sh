#!/bin/sh
set -eu
fixture=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=$(mktemp -d /tmp/go-aot.XXXXXX)
trap 'rm -rf "$work"' EXIT
trap 'exit 1' INT TERM
export PATH=/usr/lib/go/bin:/usr/local/bin:/usr/bin:/bin
export CGO_ENABLED=0 GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GO111MODULE=off
export GOMAXPROCS=2 GODEBUG=asyncpreemptoff=1
export GOCACHE=${GO_AOT_CACHE:-$work/cache}
export GOFLAGS=-p=1
test "$(go version)" = 'go version go1.26.8 linux/arm64'
cp "$fixture/main.go" "$fixture/probe.go" "$fixture/probe.s" "$work/"
cd "$work"
gofmt -w main.go probe.go
go tool compile -p probe -o probe.a probe.go
go tool asm -p probe -o probe.o probe.s
go build -o hello main.go
test "$(./hello)" = 'GO_AOT_RUN_OK 45'
go vet main.go
printf 'package invalid\nfunc broken(\n' > invalid.go
if go tool compile -o invalid.a invalid.go > invalid.log 2>&1; then
    echo 'Go compiler accepted invalid source' >&2
    exit 1
fi
grep -q 'syntax error' invalid.log
printf 'GO_AOT_BUILD_OK\n'
