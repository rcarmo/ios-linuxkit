# Security model

`ios-linuxkit` runs Linux-compatible guest code inside one application process. The outer iOS sandbox is the security boundary; guest users, permissions and memory mappings do not isolate hostile code from the host application.

The runtime contains a large C and AArch64 assembly codebase. Memory corruption, permission-check gaps and thread-safety defects are correctness bugs unless they cross the outer sandbox or trigger host-side effects without user consent. Do not use the runtime as a secure container or expose it directly to untrusted workloads.

Host integrations widen guest access:

- realfs and fakefs bind mounts expose selected host-sandbox paths;
- native offload handlers receive guest-controlled arguments and execute as host code;
- spawned native mappings execute host programs outside instruction emulation.

Validate paths, arguments and data at each integration boundary. Cooperative
offload's retained guest VFS and exclusive-output example improve correctness,
but do not confine hostile host code. Fakefs guest mode bits do not create a
separate host-user security boundary. Partial output may remain after failure or
cancellation; callers must not treat it as a committed result. Read
[offload contracts](docs/NATIVE_OFFLOAD.md) before registering a handler. AOT artifact
manifests detect file changes but provide no signature or archive sandbox. Use
trusted archives, verify separately supplied checksums and run native import/build
tools with normal user privileges.

Report ordinary crashes, illegal instructions, syscall defects and compatibility failures through the repository's [GitHub issues](https://github.com/rcarmo/ios-linuxkit/issues). Include the source revision, host or iOS version, rootfs, reproduction command, exit status and relevant diagnostics.

This fork does not currently enable GitHub private vulnerability reporting. For a report that cannot safely be public, email [rui@carmo.io](mailto:rui@carmo.io) and request a private channel. Do not attach exploit details, credentials or private user data to a public issue.

Use the contact above for this fork. Upstream iSH maintains its own policy and contacts.
