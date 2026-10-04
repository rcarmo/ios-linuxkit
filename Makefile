# Convenience targets for local ARM64 Linux bring-up.
# Meson remains the source of truth for actual builds; this Makefile captures
# the repeatable build/test flows used during runtime coverage work.

MESON ?= meson
NINJA ?= ninja
CC ?= clang

RELEASE_BUILD_DIR ?= build-arm64-linux
DEBUG_BUILD_DIR ?= build-arm64-linux-debug
NATIVE_BUILD_DIR ?= build-arm64-native-release
AOT_RECORD_DIR ?= /workspace/tmp/ish-aot-targeted
MESON_SETUP_ARGS ?=
ROOTFS_DIR ?= $(CURDIR)/alpine-arm64-fakefs
DEBIAN_ROOTFS_DIR ?= $(CURDIR)/debian-arm64-fakefs
DEBIAN_SUITE ?= trixie
NODE_VERSION ?= 24.14.1
BUN_VERSION ?= 1.3.13
ROOTFS_LANES ?= alpine=$(ROOTFS_DIR) debian=$(DEBIAN_ROOTFS_DIR)
CLI_PACKAGE_MANAGERS ?= npm bun pip
REPORT_DIR ?= /workspace/tmp
TIMEOUT_S ?= 120
INSTALL_TIMEOUT_S ?= 1200
PERF_RUNS ?= 21
PERF_CPU ?= 11
HEAVY_TIMEOUT_S ?= 120

.PHONY: help
help:
	@echo "iSH ARM64 local targets:"
	@echo "  make build-arm64-linux              Build release Linux host binary"
	@echo "  make build-arm64-linux-debug        Build debug Linux host binary"
	@echo "  make build-arm64-linux-all          Build release + debug"
	@echo "  make check-docs                     Check local Markdown links"
	@echo "  make test-arm64-runtime-coverage    Run staged C/Go/Bun/Node/Python/Lua/Java/Clojure/PyPy/Swift/Rust/Erlang/Zig coverage"
	@echo "  make test-arm64-runtime-coverage-debug"
	@echo "                                      Run coverage against debug binary"
	@echo "  make test-arm64-fcvt-vector        Run focused AdvSIMD FP widen/narrow conversions"
	@echo "  make test-arm64-proc-mem-seek      Run /proc/<pid>/mem native seek-semantics regression"
	@echo "  make test-arm64-proc-exit-race     Run full procfs/exit stress with verified worker shutdown"
	@echo "  make test-arm64-load64-fault-pc    Run precise-PC load/retry regression gate"
	@echo "  make test-arm64-upstream          Run imported correctness, lifetime and failure-injection gates"
	@echo "  make test-arm64-internal-continue-fixtures"
	@echo "                                      Run opt-in ARM64 internal-continue first-call-site fixtures"
	@echo "  make test-arm64-cli-corner-smoke   Run optional CLI/TUI/network/container corner-case smoke tests"
	@echo "  make debian-arm64-fakefs           Build minimal Debian ARM64 fakefs lane"
	@echo "  make test-arm64-npm-cli-runtime-coverage"
	@echo "                                      Run npm-only CLI package coverage across ROOTFS_LANES"
	@echo "  make test-arm64-cli-package-runtime-coverage"
	@echo "                                      Run second-stage npm/Bun/pip CLI package coverage"
	@echo "  make test-arm64-cli-package-runtime-coverage-debug"
	@echo "                                      Run CLI package coverage against debug binary"
	@echo "  make perf-bench                      Pinned multi-run benchmark (p5/p50/p95)"
	@echo ""
	@echo "Knobs: ROOTFS_DIR=$(ROOTFS_DIR) DEBIAN_ROOTFS_DIR=$(DEBIAN_ROOTFS_DIR) ROOTFS_LANES=$(ROOTFS_LANES) CLI_PACKAGE_MANAGERS=$(CLI_PACKAGE_MANAGERS) REPORT_DIR=$(REPORT_DIR) TIMEOUT_S=$(TIMEOUT_S) INSTALL_TIMEOUT_S=$(INSTALL_TIMEOUT_S) PERF_RUNS=$(PERF_RUNS) PERF_CPU=$(PERF_CPU)"

.PHONY: check-docs check-docs-style test-docs-style
check-docs:
	bun scripts/check-markdown-links.ts

check-docs-style:
	bun scripts/check-docs-style.ts

test-docs-style:
	bun test scripts/check-docs-style.test.ts

.PHONY: test-rootfs-download
test-rootfs-download:
	sh tests/arm64/rootfs/download-root.sh

.PHONY: build-arm64-linux
build-arm64-linux:
	@test -d "$(RELEASE_BUILD_DIR)" || CC="$(CC)" $(MESON) setup "$(RELEASE_BUILD_DIR)" -Dguest_arch=arm64 --buildtype=release $(MESON_SETUP_ARGS)
	$(NINJA) -C "$(RELEASE_BUILD_DIR)"

.PHONY: build-arm64-linux-debug
build-arm64-linux-debug:
	@test -d "$(DEBUG_BUILD_DIR)" || CC="$(CC)" $(MESON) setup "$(DEBUG_BUILD_DIR)" -Dguest_arch=arm64 --buildtype=debug $(MESON_SETUP_ARGS)
	$(NINJA) -C "$(DEBUG_BUILD_DIR)"

.PHONY: build-arm64-native test-arm64-native-emitter test-aot-generator record-arm64-aot test-arm64-linked-aot
build-arm64-native:
	$(MAKE) build-arm64-linux RELEASE_BUILD_DIR="$(NATIVE_BUILD_DIR)" CC=clang MESON_SETUP_ARGS='-Djit=true'

test-arm64-native-emitter: build-arm64-native
	BUILD_DIR="$(abspath $(NATIVE_BUILD_DIR))" bun tests/arm64/native-aot/run.ts

test-aot-generator:
	bun test tests/arm64/native-aot/generator.test.ts

.PHONY: test-aot-kit test-xcode-gadget-guard test-darwin-feature-macros
test-darwin-feature-macros:
	bun test --timeout 60000 tests/host/darwin-feature-macros.test.ts

test-aot-kit:
	bun test tests/arm64/native-aot/kit.test.ts

test-xcode-gadget-guard:
	bun test tests/arm64/native-aot/xcode-guard.test.ts

test-arm64-linked-aot:
	ISH_BIN="$(abspath $(RELEASE_BUILD_DIR))/ish" ROOTFS="$(ROOTFS_DIR)" AOT_RECORD_DIR="$(AOT_RECORD_DIR)" bash tests/arm64/native-aot/run-linked.sh

record-arm64-aot: build-arm64-native
	bun tools/jit_aot/targeted.ts "$(abspath $(NATIVE_BUILD_DIR))/ish" "$(ROOTFS_DIR)" "$(AOT_RECORD_DIR)" elf

.PHONY: build-arm64-linux-all
build-arm64-linux-all: build-arm64-linux build-arm64-linux-debug

$(DEBIAN_ROOTFS_DIR): | build-arm64-linux
	@command -v debootstrap >/dev/null || { echo "missing debootstrap; install it first (sudo apt install debootstrap)" >&2; exit 1; }
	@set -eu; \
	WORK="$(REPORT_DIR)/debian-arm64-rootfs"; \
	TAR="$(REPORT_DIR)/debian-arm64-minimal.tar"; \
	sudo rm -rf "$$WORK" "$@"; \
	sudo debootstrap --arch=arm64 --variant=minbase \
	  --include=ca-certificates,curl,wget,busybox,file,tar,gzip,xz-utils,sed,grep,findutils,bash,python3,python3-venv,python3-pip \
	  "$(DEBIAN_SUITE)" "$$WORK" http://deb.debian.org/debian; \
	mkdir -p "$(REPORT_DIR)/node-v$(NODE_VERSION)-linux-arm64" "$(REPORT_DIR)/bun-v$(BUN_VERSION)-linux-aarch64"; \
	cd "$(REPORT_DIR)/node-v$(NODE_VERSION)-linux-arm64"; \
	if [ ! -d node-v$(NODE_VERSION)-linux-arm64 ]; then curl -L --fail -O https://nodejs.org/dist/v$(NODE_VERSION)/node-v$(NODE_VERSION)-linux-arm64.tar.xz; tar -xf node-v$(NODE_VERSION)-linux-arm64.tar.xz; fi; \
	sudo rm -rf "$$WORK/opt/node-v$(NODE_VERSION)-linux-arm64"; \
	sudo cp -a "$(REPORT_DIR)/node-v$(NODE_VERSION)-linux-arm64/node-v$(NODE_VERSION)-linux-arm64" "$$WORK/opt/"; \
	sudo ln -sf /opt/node-v$(NODE_VERSION)-linux-arm64/bin/node "$$WORK/usr/local/bin/node"; \
	sudo ln -sf /opt/node-v$(NODE_VERSION)-linux-arm64/bin/npm "$$WORK/usr/local/bin/npm"; \
	sudo ln -sf /opt/node-v$(NODE_VERSION)-linux-arm64/bin/npx "$$WORK/usr/local/bin/npx"; \
	cd "$(REPORT_DIR)/bun-v$(BUN_VERSION)-linux-aarch64"; \
	if [ ! -d bun-linux-aarch64 ]; then curl -L --fail -o bun-linux-aarch64.zip https://github.com/oven-sh/bun/releases/download/bun-v$(BUN_VERSION)/bun-linux-aarch64.zip; unzip -q bun-linux-aarch64.zip; fi; \
	sudo install -d "$$WORK/usr/local/bin"; \
	sudo install -m 0755 "$(REPORT_DIR)/bun-v$(BUN_VERSION)-linux-aarch64/bun-linux-aarch64/bun" "$$WORK/usr/local/bin/bun"; \
	printf 'nameserver 1.1.1.1\n' | sudo tee "$$WORK/etc/resolv.conf" >/dev/null; \
	printf 'deb http://deb.debian.org/debian $(DEBIAN_SUITE) main\n' | sudo tee "$$WORK/etc/apt/sources.list" >/dev/null; \
	sudo chroot "$$WORK" /bin/sh -lc 'apt-get clean; rm -rf /var/lib/apt/lists/* /var/cache/apt/* /tmp/* /var/tmp/* /usr/share/doc/* /usr/share/man/* /usr/share/info/* /usr/share/lintian /usr/share/linda'; \
	sudo tar --numeric-owner -C "$$WORK" -cf "$$TAR" .; \
	"$(CURDIR)/$(RELEASE_BUILD_DIR)/tools/fakefsify" "$$TAR" "$@"

.PHONY: debian-arm64-fakefs
debian-arm64-fakefs: $(DEBIAN_ROOTFS_DIR)

.PHONY: test-arm64-runtime-coverage
test-arm64-runtime-coverage: build-arm64-linux $(DEBIAN_ROOTFS_DIR)
	ISH_BIN="$(CURDIR)/$(RELEASE_BUILD_DIR)/ish" \
	ROOTFS="$(ROOTFS_DIR)" \
	ROOTFS_LANES="$(ROOTFS_LANES)" \
	REPORT_DIR="$(REPORT_DIR)" \
	TIMEOUT_S="$(TIMEOUT_S)" \
	INSTALL_TIMEOUT_S="$(INSTALL_TIMEOUT_S)" \
	./tests/arm64/runtime-coverage.sh

.PHONY: test-arm64-runtime-coverage-debug
test-arm64-runtime-coverage-debug: build-arm64-linux-debug $(DEBIAN_ROOTFS_DIR)
	ISH_BIN="$(CURDIR)/$(DEBUG_BUILD_DIR)/ish" \
	ROOTFS="$(ROOTFS_DIR)" \
	ROOTFS_LANES="$(ROOTFS_LANES)" \
	REPORT_DIR="$(REPORT_DIR)" \
	TIMEOUT_S="$(TIMEOUT_S)" \
	INSTALL_TIMEOUT_S="$(INSTALL_TIMEOUT_S)" \
	./tests/arm64/runtime-coverage.sh

.PHONY: test-arm64-node-bun-perf
test-arm64-node-bun-perf: build-arm64-linux
	ISH_BIN="$(CURDIR)/$(RELEASE_BUILD_DIR)/ish" \
	ROOTFS="$(ROOTFS_DIR)" \
	ROOTFS_LANES="$(ROOTFS_LANES)" \
	REPORT_DIR="$(REPORT_DIR)" \
	TIMEOUT_S="$(TIMEOUT_S)" \
	./tests/arm64/node-bun-perf-table.sh

.PHONY: perf-bench
perf-bench: build-arm64-linux
	ISH_BIN="$(CURDIR)/$(RELEASE_BUILD_DIR)/ish" \
	ROOTFS="$(ROOTFS_DIR)" \
	PERF_RUNS="$(PERF_RUNS)" \
	PERF_CPU="$(PERF_CPU)" \
	TIMEOUT_S="$(TIMEOUT_S)" \
	HEAVY_TIMEOUT_S="$(HEAVY_TIMEOUT_S)" \
	REPORT_DIR="$(REPORT_DIR)" \
	./tests/arm64/perf-bench.sh

.PHONY: test-arm64-scalar-saturation test-arm64-gzip
test-arm64-scalar-saturation: build-arm64-linux
	ISH_BIN="$(CURDIR)/$(RELEASE_BUILD_DIR)/ish" ROOTFS="$(ROOTFS_DIR)" \
	CC="$(CC)" bash tests/arm64/simd-saturating/run.sh

test-arm64-gzip: build-arm64-linux
	bun tests/arm64/simd-saturating/gzip.ts "$(CURDIR)/$(RELEASE_BUILD_DIR)/ish" \
	"$(ROOTFS_DIR)" "$(REPORT_DIR)/gzip-$$(date +%Y%m%d-%H%M%S)"

.PHONY: test-arm64-fcvt-vector
test-arm64-fcvt-vector: build-arm64-linux $(DEBIAN_ROOTFS_DIR)
	CC="$(CC)" \
	ISH_BIN="$(CURDIR)/$(RELEASE_BUILD_DIR)/ish" \
	ROOTFS="$(DEBIAN_ROOTFS_DIR)" \
	TIMEOUT_S="$(TIMEOUT_S)" \
	./tests/arm64/fp/run-fcvt-vector.sh

.PHONY: test-arm64-proc-mem-seek
test-arm64-proc-mem-seek: build-arm64-linux $(DEBIAN_ROOTFS_DIR)
	CC="$(CC)" \
	ISH_BIN="$(CURDIR)/$(RELEASE_BUILD_DIR)/ish" \
	ROOTFS="$(DEBIAN_ROOTFS_DIR)" \
	TIMEOUT_S="$(TIMEOUT_S)" \
	./tests/arm64/proc/run-proc-mem-seek.sh

.PHONY: test-arm64-poll-regular
test-arm64-poll-regular: build-arm64-linux
	CC="$(CC)" ISH_BIN="$(abspath $(RELEASE_BUILD_DIR))/ish" \
	ROOTFS="$(ROOTFS_DIR)" bash tests/arm64/fs/run-poll-regular.sh

.PHONY: test-arm64-lseek-width
test-arm64-lseek-width: build-arm64-linux $(DEBIAN_ROOTFS_DIR)
	CC="$(CC)" ISH_BIN="$(abspath $(RELEASE_BUILD_DIR))/ish" \
	ROOTFS="$(DEBIAN_ROOTFS_DIR)" TIMEOUT_S="$(TIMEOUT_S)" \
	./tests/arm64/fs/run-lseek-width.sh

.PHONY: test-arm64-poke-stress
test-arm64-poke-stress: build-arm64-linux $(DEBIAN_ROOTFS_DIR)
	CC="$(CC)" ISH_BIN="$(abspath $(RELEASE_BUILD_DIR))/ish" \
	ROOTFS="$(DEBIAN_ROOTFS_DIR)" \
	./tests/arm64/signals/run-poke-stress.sh

.PHONY: test-arm64-load64-fault-pc
test-arm64-load64-fault-pc: build-arm64-linux
	ISH_BIN="$(CURDIR)/$(RELEASE_BUILD_DIR)/ish" \
	ROOTFS="$(DEBIAN_ROOTFS_DIR)" \
	./tests/arm64/loadstore/run-load64-fault-pc.sh

.PHONY: test-arm64-proc-exit-race
test-arm64-proc-exit-race: build-arm64-linux
	ISH_BIN="$(CURDIR)/$(RELEASE_BUILD_DIR)/ish" \
	ROOTFS="$(DEBIAN_ROOTFS_DIR)" \
	./tests/arm64/proc/run-proc-exit-race.sh

.PHONY: test-arm64-netlink
test-arm64-netlink: build-arm64-linux
	@test -n "$(NETLINK_REALFS)" || { echo 'set NETLINK_REALFS to a disposable realfs root' >&2; exit 2; }
	CC_GUEST="$(if $(CC_GUEST),$(CC_GUEST),aarch64-linux-musl-gcc)" \
	./tests/regress/run_netlink.sh "$(abspath $(RELEASE_BUILD_DIR))/ish" "$(NETLINK_REALFS)"

.PHONY: test-arm64-upstream
test-arm64-upstream: build-arm64-linux
	ISH_BIN="$(CURDIR)/$(RELEASE_BUILD_DIR)/ish" \
	ROOTFS="$(DEBIAN_ROOTFS_DIR)" \
	./tests/arm64/upstream/run.sh

.PHONY: test-arm64-offload-setup test-arm64-native-fs test-arm64-offload-context test-arm64-offload-io test-arm64-offload-local-copy
test-arm64-offload-local-copy: build-arm64-linux
	BUILD_DIR="$(abspath $(RELEASE_BUILD_DIR))" CC="$(CC)" \
	bash tests/arm64/upstream/run-offload-local-copy.sh

test-arm64-offload-setup: build-arm64-linux
	BUILD_DIR="$(abspath $(RELEASE_BUILD_DIR))" CC="$(CC)" \
	bash tests/arm64/upstream/run-offload-setup.sh

test-arm64-offload-io: build-arm64-linux
	BUILD_DIR="$(abspath $(RELEASE_BUILD_DIR))" CC="$(CC)" \
	bash tests/arm64/upstream/run-offload-io.sh

test-arm64-offload-context: build-arm64-linux
	BUILD_DIR="$(abspath $(RELEASE_BUILD_DIR))" CC="$(CC)" \
	bash tests/arm64/upstream/run-offload-context.sh

test-arm64-native-fs: build-arm64-linux
	BUILD_DIR="$(abspath $(RELEASE_BUILD_DIR))" CC="$(CC)" \
	bash tests/arm64/upstream/run-native-fs.sh

.PHONY: test-arm64-internal-continue-fixtures
test-arm64-internal-continue-fixtures: build-arm64-linux
	ISH_BIN="$(CURDIR)/$(RELEASE_BUILD_DIR)/ish" \
	ROOTFS="$(ROOTFS_DIR)" \
	REPORT_DIR="$(REPORT_DIR)" \
	TIMEOUT_S="$(TIMEOUT_S)" \
	./tests/arm64/internal-continue-fixtures.sh

.PHONY: test-arm64-cli-corner-smoke
test-arm64-cli-corner-smoke: build-arm64-linux
	ISH_BIN="$(CURDIR)/$(RELEASE_BUILD_DIR)/ish" \
	ROOTFS="$(ROOTFS_DIR)" \
	ROOTFS_LANES="$(ROOTFS_LANES)" \
	REPORT_DIR="$(REPORT_DIR)" \
	TIMEOUT_S="$(TIMEOUT_S)" \
	INSTALL_TIMEOUT_S="$(INSTALL_TIMEOUT_S)" \
	./tests/arm64/cli-corner-smoke.sh

.PHONY: test-arm64-cli-package-runtime-coverage
test-arm64-cli-package-runtime-coverage: build-arm64-linux $(DEBIAN_ROOTFS_DIR)
	ISH_BIN="$(CURDIR)/$(RELEASE_BUILD_DIR)/ish" \
	ROOTFS="$(ROOTFS_DIR)" \
	ROOTFS_LANES="$(ROOTFS_LANES)" \
	CLI_PACKAGE_MANAGERS="$(CLI_PACKAGE_MANAGERS)" \
	REPORT_DIR="$(REPORT_DIR)" \
	TIMEOUT_S="$(TIMEOUT_S)" \
	INSTALL_TIMEOUT_S="$(INSTALL_TIMEOUT_S)" \
	./tests/arm64/cli-package-runtime-coverage.sh

.PHONY: test-arm64-npm-cli-runtime-coverage
test-arm64-npm-cli-runtime-coverage: build-arm64-linux $(DEBIAN_ROOTFS_DIR)
	ISH_BIN="$(CURDIR)/$(RELEASE_BUILD_DIR)/ish" \
	ROOTFS="$(ROOTFS_DIR)" \
	ROOTFS_LANES="$(ROOTFS_LANES)" \
	CLI_PACKAGE_MANAGERS="npm" \
	REPORT_DIR="$(REPORT_DIR)" \
	TIMEOUT_S="$(TIMEOUT_S)" \
	INSTALL_TIMEOUT_S="$(INSTALL_TIMEOUT_S)" \
	./tests/arm64/cli-package-runtime-coverage.sh

.PHONY: test-arm64-cli-package-runtime-coverage-debug
test-arm64-cli-package-runtime-coverage-debug: build-arm64-linux-debug $(DEBIAN_ROOTFS_DIR)
	ISH_BIN="$(CURDIR)/$(DEBUG_BUILD_DIR)/ish" \
	ROOTFS="$(ROOTFS_DIR)" \
	ROOTFS_LANES="$(ROOTFS_LANES)" \
	CLI_PACKAGE_MANAGERS="$(CLI_PACKAGE_MANAGERS)" \
	REPORT_DIR="$(REPORT_DIR)" \
	TIMEOUT_S="$(TIMEOUT_S)" \
	INSTALL_TIMEOUT_S="$(INSTALL_TIMEOUT_S)" \
	./tests/arm64/cli-package-runtime-coverage.sh
