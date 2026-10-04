#!/usr/bin/env python3
"""Add checksum-pinned Linux ARM64 musl Bun to a fresh Alpine archive."""
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tarfile
import tempfile
import zipfile

BUN_VERSION = "1.4.2"
BUN_ARCHIVE_SHA256 = "71760b6c8ea30623b81a4907cb815d48e2ea266f2e73e751534a44a0607950df"
BUN_MEMBER = "bun-linux-aarch64-musl/bun"
RUNTIME_PACKAGES = [
    {"name": "libgcc", "version": "15.2.0-r5",
     "sha256": "369aaa6e9d099a737bad6dd3e6c2fe7bb1547ca26d22b94ee0411228f709b403",
     "files": ["usr/lib/libgcc_s.so.1"]},
    {"name": "libstdc++", "version": "15.2.0-r5",
     "sha256": "2302e766d4e4926038ec166ecb85837ee884576115236ddb565e3a5fca4a11d7",
     "files": ["usr/lib/libstdc++.so.6", "usr/lib/libstdc++.so.6.0.34"]},
]


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_elf(path):
    with open(path, "rb") as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:6] != b"\x7fELF\x02\x01":
            raise ValueError("Bun must be little-endian ELF64")
        if struct.unpack_from("<H", header, 18)[0] != 183:
            raise ValueError("Bun must be AArch64")
        offset = struct.unpack_from("<Q", header, 32)[0]
        size, count = struct.unpack_from("<HH", header, 54)
        if size != 56 or count > 128:
            raise ValueError("invalid Bun ELF program headers")
        for index in range(count):
            stream.seek(offset + index * size)
            program = stream.read(size)
            if len(program) != size:
                raise ValueError("truncated Bun program header")
            if struct.unpack_from("<I", program)[0] == 3:
                stream.seek(struct.unpack_from("<Q", program, 8)[0])
                interpreter = stream.read(struct.unpack_from("<Q", program, 32)[0])
                if interpreter != b"/lib/ld-musl-aarch64.so.1\0":
                    raise ValueError("Bun requires an unsupported ELF interpreter")


def package(rootfs, bun_archive, output, rootfs_sha256, bun_sha256=BUN_ARCHIVE_SHA256, runtime_archives=()):
    if sha256(rootfs) != rootfs_sha256:
        raise ValueError("Alpine rootfs SHA-256 mismatch")
    if sha256(bun_archive) != bun_sha256:
        raise ValueError("Bun archive SHA-256 mismatch")
    if runtime_archives and len(runtime_archives) != len(RUNTIME_PACKAGES):
        raise ValueError("Bun requires both pinned runtime archives")
    runtime_files = []
    for path, metadata in zip(runtime_archives, RUNTIME_PACKAGES):
        if sha256(path) != metadata["sha256"]:
            raise ValueError(f'{metadata["name"]} archive SHA-256 mismatch')
        with tarfile.open(path, "r:gz", ignore_zeros=True) as archive:
            for name in metadata["files"]:
                member = archive.getmember(name)
                if not member.isfile() and not member.issym():
                    raise ValueError("unexpected runtime library file type")
                content = archive.extractfile(member).read() if member.isfile() else None
                runtime_files.append((member, content))
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".bun-rootfs-", dir=output.parent) as stage:
        binary = Path(stage) / "bun"
        with zipfile.ZipFile(bun_archive) as archive, archive.open(BUN_MEMBER) as source, binary.open("wb") as target:
            shutil.copyfileobj(source, target)
        validate_elf(binary)
        metadata = (json.dumps({"version": BUN_VERSION, "archiveSha256": bun_sha256,
                               "binarySha256": sha256(binary), "platform": "linux-aarch64-musl",
                               "runtimePackages": RUNTIME_PACKAGES if runtime_archives else []},
                              sort_keys=True, indent=2) + "\n").encode()
        staged_output = Path(stage) / "root.tar.gz"
        with tarfile.open(rootfs, "r:gz") as source, staged_output.open("wb") as raw:
            with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
                with tarfile.open(fileobj=compressed, mode="w|") as target:
                    members = source.getmembers()
                    existing = {member.name.removeprefix("./").rstrip("/") for member in members}
                    additions = {"usr/local/bin/bun", "usr/share/linuxkit/bun.json"}
                    additions.update(member.name for member, _ in runtime_files)
                    if existing & additions:
                        raise ValueError("refusing to replace existing Bun files")
                    for member in members:
                        target.addfile(member, source.extractfile(member) if member.isfile() else None)
                    for member, content in runtime_files:
                        target.addfile(member, io.BytesIO(content) if content is not None else None)
                    for name in ["usr/local", "usr/local/bin", "usr/share", "usr/share/linuxkit"]:
                        if name not in existing:
                            info = tarfile.TarInfo("./" + name)
                            info.type = tarfile.DIRTYPE
                            info.mode = 0o755
                            target.addfile(info)
                        elif not next(m for m in members if m.name.removeprefix("./").rstrip("/") == name).isdir():
                            raise ValueError("Bun destination ancestor is not a directory")
                    info = tarfile.TarInfo("./usr/local/bin/bun")
                    info.mode = 0o755
                    info.size = binary.stat().st_size
                    with binary.open("rb") as stream:
                        target.addfile(info, stream)
                    info = tarfile.TarInfo("./usr/share/linuxkit/bun.json")
                    info.mode = 0o644
                    info.size = len(metadata)
                    target.addfile(info, io.BytesIO(metadata))
        os.chmod(staged_output, 0o644)
        os.replace(staged_output, output)
        print(f"Packaged Bun {BUN_VERSION}: {output}")


if __name__ == "__main__":
    if len(sys.argv) != 7:
        sys.exit("Usage: package-bun-rootfs.py ALPINE_ARCHIVE BUN_ZIP OUTPUT ALPINE_SHA256 LIBGCC_APK LIBSTDCXX_APK")
    package(*sys.argv[1:5], runtime_archives=sys.argv[5:])
