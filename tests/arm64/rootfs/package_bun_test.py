import importlib.util
import io
from pathlib import Path
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

    def test_preserves_bytes_modes_owners_and_links(self):
        self.package()
        with tarfile.open(self.output) as archive:
            self.assertEqual(archive.extractfile("./bin/busybox").read(), b"busybox")
            self.assertEqual(archive.getmember("./bin/busybox").uid, 42)
            self.assertEqual(archive.getmember("./bin/sh").linkname, "busybox")
            self.assertEqual(archive.getmember("./usr/local/bin/bun").mode, 0o755)
            self.assertIn(b'"version": "1.4.2"', archive.extractfile("./usr/share/linuxkit/bun.json").read())

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

    def test_runtime_checksum_failure_preserves_previous_archive(self):
        self.output.write_bytes(b"previous archive")
        with self.assertRaisesRegex(ValueError, "libgcc archive SHA"):
            package_bun.package(self.archive, self.zip, self.output, package_bun.sha256(self.archive),
                                package_bun.sha256(self.zip), [self.zip, self.zip])
        self.assertEqual(self.output.read_bytes(), b"previous archive")


if __name__ == "__main__":
    unittest.main()
