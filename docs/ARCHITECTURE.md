# ARM64 runtime architecture

`ios-linuxkit` executes an AArch64 Linux guest with a userspace kernel and a threaded-code interpreter. The current Meson configuration accepts only `guest_arch=arm64` and `engine=asbestos`.

## Execution path

```text
AArch64 ELF program
        |
        v
userspace Linux kernel and syscall table
        |
        v
ARM64 decoder (`asbestos/guest-arm64/gen.c`)
        |
        v
gadget program: host function pointers plus inline operands
        |
        v
precompiled AArch64 gadgets (`gadgets-aarch64/*.S`)
        |
        v
Linux or Darwin host APIs
```

The decoder builds a `fiber_block` for each guest basic block. Its code array contains addresses of precompiled gadget functions and their operands. Each gadget performs part of a guest instruction and branches to the next gadget. All executable host instructions come from the built application. Translated programs are data arrays and need no `MAP_JIT` memory.

The word `jit` survives in internal identifiers such as `jit_saved_pc` and `jit_crash_trampoline`. In these names it means translated-block execution or fault recovery.

## Decoder and gadgets

| Path | Responsibility |
|---|---|
| `asbestos/guest-arm64/gen.c` | Decode AArch64 instructions and emit gadget words. |
| `asbestos/guest-arm64/gadgets-aarch64/entry.S` | Enter and leave gadget execution; crash trampoline. |
| `asbestos/guest-arm64/gadgets-aarch64/memory.S` | Loads, stores, TLB fast paths and fault exits. |
| `asbestos/guest-arm64/gadgets-aarch64/control.S` | Branches and control flow. |
| `asbestos/guest-arm64/gadgets-aarch64/math.S` | Integer, floating-point and AdvSIMD operations. |
| `asbestos/guest-arm64/gadgets-aarch64/crypto.S` | Implemented cryptographic instructions. |
| `asbestos/asbestos.c` | Block cache, chaining, invalidation and executor diagnostics. |

Instruction coverage is incomplete. A decoder match and a native-looking gadget do not establish architectural correctness on their own; each added instruction needs an executable guest fixture, including width, aliasing, alignment, flags and exceptional cases that apply.

Floating-point conversion gadgets that depend on the guest control or status registers save the host thread's `FPCR` and `FPSR`, load `cpu_state`'s guest values, execute the native instruction, copy cumulative exceptions back to the guest `FPSR`, then restore the host values. The current implementation applies this hand-off to `FCVTN`/`FCVTN2`, `FCVTL`/`FCVTL2` and `FCVTXN`/`FCVTXN2`. [`tests/arm64/fp/`](../tests/arm64/fp/) checks rounding, exception flags, vector halves, source/destination aliasing and decoder masks.

## Address space and memory

`kernel/memory.h` defines a four-level page table with 512 entries per level and 4 KiB pages. This gives a 48-bit guest address space. Default stacks and ordinary anonymous mappings stay below 4 GiB for shorter, hotter page-table paths; explicit high mappings and lazy `MAP_NORESERVE` reservations can use the wider range.

Large lazy reservations record ranges and permissions without allocating every page-table entry. Guest faults materialise pages when required. Mapping changes increment a generation used by the TLB to reject stale host pointers.

`emu/tlb.h` defines an 8,192-entry TLB and a 4,096-entry persistent block cache. Guest stores mark translated pages dirty. Page invalidation discards stale blocks before later execution, which is required for guest self-modifying code.

## Fault recovery

A host `SIGSEGV` or `SIGBUS` can occur inside a memory gadget when a guest access needs copy-on-write, stack growth or page materialisation. Faultable operations save their guest instruction address in `fiber_frame::jit_saved_pc`. The AArch64 host signal adapter in `platform/host_context_aarch64.h` redirects execution to `jit_crash_trampoline`; the dispatch loop resolves the guest fault and retries that instruction.

The precise saved address prevents earlier instructions in the same block from executing twice. Normal-register unsigned-immediate `LDR X` saves that address inside `load64_imm_fast`, consuming `[operands][guest PC]` together instead of dispatching a separate PC-save gadget. Its LDR+CBZ/CBNZ fusion already saves the LDR PC internally; other memory forms retain their existing saves. Operand-stream producers and consumers must change together.

A separate broad synthetic read-fault fallback is compiled only with `ENABLE_ARM64_READ_FAULT_RECOVERY` and is disabled in ordinary builds. This does **not** disable all compatibility recovery: `kernel/calls.c` still demand-maps readable zeros for an unmapped read page with a mapped neighbour within 16 pages, and retains targeted V8 recovery paths. These can suppress guest faults that native Linux would deliver. See [limitations](LIMITATIONS.md#memory-and-code-protection) and the [load-PC evidence](reports/benchmarks/ARM_LINUX_LOAD_PC_2026-09-05.md).

## Userspace kernel

The `kernel/` and `fs/` trees implement Linux-facing process, memory, signal, file, socket and polling interfaces in user space. `kernel/arch/arm64/calls.c` maps AArch64 syscall numbers to those implementations. Unimplemented calls return the configured stub result, usually `ENOSYS`.

The principal filesystem choices are:

- **realfs**, which exposes a host directory directly;
- **fakefs**, which stores Linux metadata in SQLite while keeping file contents under a host directory;
- in-memory and synthetic filesystems for `/tmp`, `/proc`, devices and pseudo-terminals.

This userspace compatibility layer reproduces enough Linux behaviour for the tested userland. Kernel modules, namespaces, cgroups and device passthrough are unavailable.

## Host boundaries

`platform/platform.h` separates common code from Linux and Darwin implementations for host statistics, random data, paths, thread names and memory-pressure hooks. `platform/host_context_aarch64.h` handles the incompatible Darwin and Linux AArch64 `ucontext_t` layouts used by fault recovery.

Some host differences remain at their call sites:

- native offload in `kernel/native_offload.c` has platform-specific execution paths;
- sockets and polling map Linux guest behaviour to different host facilities;
- synchronisation uses host-specific timed-wait and lock operations.

## Guest compatibility settings

`kernel/exec.c` supplies defaults when the guest environment does not already define them:

```text
GODEBUG=asyncpreemptoff=1
GOMAXPROCS=2
JSC_numberOfGCMarkers=1
JSC_useConcurrentGC=0
```

The Go settings avoid asynchronous pre-emption paths whose interrupted guest PC cannot always be represented precisely. The JavaScriptCore settings avoid multi-marker suspension and concurrent-GC hangs. They trade concurrency for reliable execution and are part of the current compatibility contract.

The initial process path in `xX_main_Xx.h` adds `--jitless`, `--no-lazy`, `--no-expose-wasm` and a 512 MiB old-space limit when it launches Node directly. Later Node `execve` calls add `--jitless`, `--no-lazy` and the same old-space limit in `kernel/exec.c`; optional rootfs polyfills handle selected WebAssembly-dependent packages.

## iOS application boundary

The `iSH-ARM64` target links the userspace kernel and emulator libraries into an iOS application. Its build downloads the AArch64 Alpine rootfs declared by `app/GuestARM64.xcconfig`. `app/download-root.sh` extracts `bin/busybox`, checks that it is AArch64, and rejects a rootfs for another architecture.

Host integration includes fakefs bind mounts through `fakefs_bind_mount()` and optional native command handlers through `native_offload_add_handler()`. These interfaces run inside the app's iOS sandbox and inherit its trust boundary.
