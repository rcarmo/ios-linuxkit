#!/usr/bin/env python3
"""Add the checksum-pinned Alpine Go APK payload to a fresh root archive."""
import configparser
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import struct
import sys
import tarfile
import tempfile

GO_VERSION = "1.26.8"
GO_PACKAGE_VERSION = GO_VERSION + "-r0"
GO_ARCHIVE_SHA256 = "8145a38295eec475c22bd10977a06b1c85d5493b99c0f81a778fda7354bb60e3"
GO_BINARIES = ["bin/go", "bin/gofmt"] + [
    f"pkg/tool/linux_arm64/{name}" for name in
    ("asm", "cgo", "compile", "cover", "fix", "link", "preprofile", "vet")]
PROFILE = b'export CGO_ENABLED=${CGO_ENABLED:-0}\nexport GOTOOLCHAIN=${GOTOOLCHAIN:-local}\n'


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical(name):
    if name in (".", "./"):
        return "."
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name or not path.parts:
        raise ValueError("unsafe archive path")
    return str(path)


def validate_binary(stream):
    header = stream.read(64)
    if len(header) != 64 or header[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", header, 18)[0] != 183:
        raise ValueError("Go tools must be little-endian AArch64 ELF64")
    offset = struct.unpack_from("<Q", header, 32)[0]
    size, count = struct.unpack_from("<HH", header, 54)
    if size != 56 or count > 128:
        raise ValueError("invalid Go ELF program headers")
    for index in range(count):
        stream.seek(offset + index * size)
        program = stream.read(size)
        if len(program) != size:
            raise ValueError("truncated Go ELF program header")
        if struct.unpack_from("<I", program)[0] == 3:
            stream.seek(struct.unpack_from("<Q", program, 8)[0])
            if stream.read(struct.unpack_from("<Q", program, 32)[0]) != b"/lib/ld-musl-aarch64.so.1\0":
                raise ValueError("unsupported Go ELF interpreter")
    stream.seek(0)
    digest = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(chunk)
    return digest.hexdigest()


def package(rootfs, go_archive, output, go_sha256=GO_ARCHIVE_SHA256):
    if sha256(go_archive) != go_sha256:
        raise ValueError("Go APK SHA-256 mismatch")
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(rootfs, "r:gz") as base, tarfile.open(go_archive, "r:gz", ignore_zeros=True) as go:
        pkginfo = configparser.ConfigParser(strict=False, interpolation=None)
        pkginfo.read_string("[package]\n" + go.extractfile(".PKGINFO").read().decode())
        if any(pkginfo["package"].get(key) != value for key, value in
               {"pkgname": "go", "pkgver": GO_PACKAGE_VERSION, "arch": "aarch64"}.items()):
            raise ValueError("Go APK package identity mismatch")
        existing = {canonical(member.name): member for member in base.getmembers()}
        added = {}
        for member in go.getmembers():
            name = canonical(member.name)
            if name == ".PKGINFO" or name.startswith(".SIGN."):
                continue
            if name in ("usr", "usr/bin", "usr/lib"):
                if not member.isdir():
                    raise ValueError("Go APK ancestor is not a directory")
                continue
            if name not in ("usr/bin/go", "usr/bin/gofmt", "usr/lib/go") and not name.startswith("usr/lib/go/"):
                raise ValueError("Go APK member outside toolchain")
            if member.issym():
                if name not in ("usr/bin/go", "usr/bin/gofmt") or member.linkname != "/usr/lib/go/bin/" + PurePosixPath(name).name:
                    raise ValueError("unsupported Go archive member type")
            elif not member.isdir() and not member.isfile():
                raise ValueError("unsupported Go archive member type")
            if name in added:
                raise ValueError("duplicate Go archive member")
            added[name] = member
        version = added.get("usr/lib/go/VERSION")
        version_lines = go.extractfile(version).read(128).splitlines() if version and version.isfile() else []
        if not version_lines or version_lines[0] != f"go{GO_VERSION}".encode():
            raise ValueError("Go version mismatch")
        hashes = {}
        for name in GO_BINARIES:
            member = added.get("usr/lib/go/" + name)
            if not member or not member.isfile() or not member.mode & 0o111:
                raise ValueError(f"missing executable Go tool: {name}")
            with go.extractfile(member) as stream:
                hashes["/usr/lib/go/" + name] = validate_binary(stream)
        for name in ("go", "gofmt"):
            if not added.get("usr/bin/" + name, tarfile.TarInfo()).issym():
                raise ValueError("missing Go command symlink")
        reserved = {"usr/bin/go", "usr/bin/gofmt", "usr/share/linuxkit/go.json", "etc/profile.d/go.sh"}
        if any(name == "usr/lib/go" or name.startswith("usr/lib/go/") or name in reserved for name in existing):
            raise ValueError("refusing to replace existing Go files")
        ancestors = ["usr", "usr/bin", "usr/lib", "usr/share", "usr/share/linuxkit", "etc", "etc/profile.d"]
        for name in ancestors:
            if name in existing and not existing[name].isdir():
                raise ValueError("Go destination ancestor is not a directory")
        metadata = (json.dumps({"version": GO_VERSION, "packageVersion": GO_PACKAGE_VERSION,
                               "archiveSha256": go_sha256, "repository": "alpine/v3.24/community",
                               "platform": "linux-aarch64-musl", "binaries": hashes},
                              sort_keys=True, indent=2) + "\n").encode()
        with tempfile.TemporaryDirectory(prefix=".go-rootfs-", dir=output.parent) as stage:
            staged = Path(stage) / "root.tar.gz"
            with staged.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
                # GNU names retain UTF-8 bytes even when the importer uses the C locale.
                with tarfile.open(fileobj=compressed, mode="w|", format=tarfile.GNU_FORMAT) as target:
                    for member in base.getmembers():
                        target.addfile(member, base.extractfile(member) if member.isfile() else None)
                    for name in ancestors:
                        if name not in existing:
                            member = tarfile.TarInfo("./" + name)
                            member.type, member.mode = tarfile.DIRTYPE, 0o755
                            target.addfile(member)
                    for name, original in added.items():
                        member = tarfile.TarInfo("./" + name)
                        member.type, member.mode, member.size = original.type, original.mode, original.size
                        member.linkname = original.linkname
                        target.addfile(member, go.extractfile(original) if original.isfile() else None)
                    for name, content in [("usr/share/linuxkit/go.json", metadata), ("etc/profile.d/go.sh", PROFILE)]:
                        member = tarfile.TarInfo("./" + name)
                        member.mode, member.size = 0o644, len(content)
                        target.addfile(member, io.BytesIO(content))
            os.chmod(staged, 0o644)
            os.replace(staged, output)
    print(f"Packaged Alpine Go {GO_PACKAGE_VERSION}: {output}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("Usage: package-go-rootfs.py ROOT_ARCHIVE GO_APK OUTPUT")
    package(*sys.argv[1:])
