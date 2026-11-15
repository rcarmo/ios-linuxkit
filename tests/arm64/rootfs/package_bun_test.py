import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import struct
import tarfile
import tempfile
import unittest
import zipfile
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("package_bun", Path(__file__).resolve().parents[3] / "scripts/package-bun-rootfs.py")
package_bun = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package_bun)


class BunPackagingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.archive = self.root / "alpine.tar.gz"
        with tarfile.open(self.archive, "w:gz") as archive:
            info = tarfile.TarInfo("./bin/busybox")
            info.mode = 0o755
            info.uid = 42
            info.size = 7
            archive.addfile(info, io.BytesIO(b"busybox"))
            info = tarfile.TarInfo("./bin/sh")
            info.type = tarfile.SYMTYPE
            info.linkname = "busybox"
            archive.addfile(info)
        self.zip = self.root / "bun.zip"
        self.write_bun(183)
        self.output = self.root / "output.tar.gz"

    def write_bun(self, machine):
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<H", elf, 18, machine)
        struct.pack_into("<H", elf, 54, 56)
        with zipfile.ZipFile(self.zip, "w") as archive:
            archive.writestr(package_bun.BUN_MEMBER, elf)

    def package(self, bun_hash=None):
        package_bun.package(self.archive, self.zip, self.output, package_bun.sha256(self.archive),
                            bun_hash or package_bun.sha256(self.zip))

    def test_release_pin_download_url_and_strict_recorder_gates_agree(self):
        self.assertEqual(package_bun.BUN_VERSION, "1.4.4")
        self.assertEqual(package_bun.BUN_ARCHIVE_SHA256,
                         "57f01b02ac7113c61c935f28f38c2eab849e3c970dab21f8b640a87b1ef65009")
        root = Path(__file__).resolve().parents[3]
        download = (root / "app/download-aot-root.sh").read_text()
        urls = re.findall(r"https://github\.com/oven-sh/bun/releases/download/\S+", download)
        self.assertEqual(urls, [f"https://github.com/oven-sh/bun/releases/download/"
                                f"bun-v{package_bun.BUN_VERSION}/bun-linux-aarch64-musl.zip"])
        self.assertIn('python3 "$SRCROOT/scripts/package-bun-rootfs.py"', download)
        targeted = (root / "tools/jit_aot/targeted.ts").read_text()
        self.assertIn(f"if (bun.stdout.trim() !== '{package_bun.BUN_VERSION}') "
                      f"throw new Error('Bun training requires exact version {package_bun.BUN_VERSION}');",
                      targeted)
        apple = (root / "tools/jit_aot/record-apple.ts").read_text()
        self.assertIn(f'test "$(/usr/local/bin/bun --version)" = {package_bun.BUN_VERSION}\n', apple)

    def test_default_checksum_rejects_unpinned_archive(self):
        self.output.write_bytes(b"previous archive")
        with self.assertRaisesRegex(ValueError, "Bun archive SHA"):
            package_bun.package(self.archive, self.zip, self.output, package_bun.sha256(self.archive))
        self.assertEqual(self.output.read_bytes(), b"previous archive")

    def test_preserves_bytes_modes_owners_and_links(self):
        self.package()
        with tarfile.open(self.output) as archive:
            self.assertEqual(archive.extractfile("./bin/busybox").read(), b"busybox")
            self.assertEqual(archive.getmember("./bin/busybox").uid, 42)
            self.assertEqual(archive.getmember("./bin/sh").linkname, "busybox")
            self.assertEqual(archive.getmember("./usr/local/bin/bun").mode, 0o755)
            metadata = json.load(archive.extractfile("./usr/share/linuxkit/bun.json"))
            self.assertEqual(metadata, {
                "version": "1.4.4",
                "archiveSha256": package_bun.sha256(self.zip),
                "binarySha256": hashlib.sha256(
                    archive.extractfile("./usr/local/bin/bun").read()).hexdigest(),
                "platform": "linux-aarch64-musl",
                "runtimePackages": [],
            })

    def test_failed_checksum_preserves_previous_archive(self):
        self.output.write_bytes(b"previous archive")
        with self.assertRaisesRegex(ValueError, "Bun archive SHA"):
            self.package("0" * 64)
        self.assertEqual(self.output.read_bytes(), b"previous archive")

    def test_wrong_architecture_preserves_previous_archive(self):
        self.output.write_bytes(b"previous archive")
        self.write_bun(62)
        with self.assertRaisesRegex(ValueError, "AArch64"):
            self.package()
        self.assertEqual(self.output.read_bytes(), b"previous archive")

    def test_does_not_replace_existing_guest_bun(self):
        self.package()
        self.archive = self.output
        with self.assertRaisesRegex(ValueError, "existing Bun"):
            self.package()

    def test_runtime_libraries_and_symlink_are_preserved(self):
        paths = []
        packages = []
        for index, metadata in enumerate(package_bun.RUNTIME_PACKAGES):
            path = self.root / f"runtime-{index}.apk"
            with tarfile.open(path, "w:gz") as archive:
                for name in metadata["files"]:
                    member = tarfile.TarInfo(name)
                    member.mode = 0o755
                    if name.endswith("libstdc++.so.6"):
                        member.type = tarfile.SYMTYPE
                        member.linkname = "libstdc++.so.6.0.34"
                        archive.addfile(member)
                    else:
                        member.size = 7
                        archive.addfile(member, io.BytesIO(b"library"))
            paths.append(path)
            packages.append({**metadata, "sha256": package_bun.sha256(path)})
        with patch.object(package_bun, "RUNTIME_PACKAGES", packages):
            package_bun.package(self.archive, self.zip, self.output, package_bun.sha256(self.archive),
                                package_bun.sha256(self.zip), paths)
        with tarfile.open(self.output) as archive:
            self.assertEqual(archive.extractfile("usr/lib/libgcc_s.so.1").read(), b"library")
            self.assertEqual(archive.getmember("usr/lib/libstdc++.so.6").linkname, "libstdc++.so.6.0.34")
            metadata = json.load(archive.extractfile("./usr/share/linuxkit/bun.json"))
            self.assertEqual(metadata["runtimePackages"], packages)
            self.assertEqual(metadata["version"], package_bun.BUN_VERSION)

    def test_runtime_checksum_failure_preserves_previous_archive(self):
        self.output.write_bytes(b"previous archive")
        with self.assertRaisesRegex(ValueError, "libgcc archive SHA"):
            package_bun.package(self.archive, self.zip, self.output, package_bun.sha256(self.archive),
                                package_bun.sha256(self.zip), [self.zip, self.zip])
        self.assertEqual(self.output.read_bytes(), b"previous archive")


if __name__ == "__main__":
    unittest.main()
