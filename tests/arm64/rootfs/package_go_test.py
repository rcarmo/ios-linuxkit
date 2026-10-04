import importlib.util
import io
import os
from pathlib import Path
import struct
import subprocess
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("package_go", Path(__file__).resolve().parents[3] / "scripts/package-go-rootfs.py")
package_go = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package_go)


class GoPackagingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.base, self.go, self.output = [self.root / name for name in ("base.tar.gz", "go.tar.gz", "output.tar.gz")]
        with tarfile.open(self.base, "w:gz") as archive:
            member = tarfile.TarInfo(".")
            member.type = tarfile.DIRTYPE
            archive.addfile(member)
            member = tarfile.TarInfo("./usr/local/bin/bun")
            member.size, member.uid, member.mode = 3, 42, 0o755
            archive.addfile(member, io.BytesIO(b"bun"))
        self.write_go()

    def write_go(self, machine=183, extra=None, version=None, missing=None, interpreter=False):
        elf = bytearray(64 if not interpreter else 120)
        elf[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<H", elf, 18, machine)
        struct.pack_into("<Q", elf, 32, 64)
        struct.pack_into("<HH", elf, 54, 56, int(interpreter))
        if interpreter:
            struct.pack_into("<I", elf, 64, 3)
        with tarfile.open(self.go, "w:gz") as archive:
            files = {".PKGINFO": f"pkgname = go\npkgver = {package_go.GO_PACKAGE_VERSION}\narch = aarch64\n".encode(),
                     "usr/lib/go/VERSION": (version or "go" + package_go.GO_VERSION).encode() + b"\n",
                     "usr/lib/go/src/runtime/runtime2.go": b"package runtime\n"}
            files.update({"usr/lib/go/" + name: elf for name in package_go.GO_BINARIES if name != missing})
            for name, content in files.items():
                member = tarfile.TarInfo(name)
                member.mode, member.size = 0o755, len(content)
                archive.addfile(member, io.BytesIO(content))
            for name in ("go", "gofmt"):
                member = tarfile.TarInfo("usr/bin/" + name)
                member.type, member.linkname = tarfile.SYMTYPE, "/usr/lib/go/bin/" + name
                archive.addfile(member)
            if extra:
                archive.addfile(extra)

    def package(self, checksum=None):
        package_go.package(self.base, self.go, self.output, checksum or package_go.sha256(self.go))

    def rejected(self, message):
        self.output.write_bytes(b"previous")
        with self.assertRaisesRegex(ValueError, message):
            self.package()
        self.assertEqual(self.output.read_bytes(), b"previous")

    def test_preserves_base_and_installs_complete_toolchain(self):
        self.package()
        with tarfile.open(self.output) as archive:
            self.assertEqual(archive.extractfile("./usr/local/bin/bun").read(), b"bun")
            self.assertEqual(archive.getmember("./usr/local/bin/bun").uid, 42)
            for name in package_go.GO_BINARIES:
                self.assertEqual(archive.getmember("./usr/lib/go/" + name).mode, 0o755)
            self.assertEqual(archive.getmember("./usr/bin/go").linkname, "/usr/lib/go/bin/go")
            self.assertIn(package_go.GO_VERSION.encode(), archive.extractfile("./usr/share/linuxkit/go.json").read())
            self.assertIn(b"CGO_ENABLED", archive.extractfile("./etc/profile.d/go.sh").read())

    def test_bad_checksum_keeps_previous_output(self):
        self.output.write_bytes(b"previous")
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            self.package("0" * 64)
        self.assertEqual(self.output.read_bytes(), b"previous")

    def test_wrong_architecture(self):
        self.write_go(machine=62)
        self.rejected("AArch64")

    def test_dynamic_tool_rejected(self):
        self.write_go(interpreter=True)
        self.rejected("unsupported Go ELF interpreter")

    def test_wrong_version(self):
        self.write_go(version="go0.1")
        self.rejected("version mismatch")

    def test_missing_compiler(self):
        self.write_go(missing="pkg/tool/linux_arm64/compile")
        self.rejected("missing executable")

    def test_path_traversal(self):
        self.write_go(extra=tarfile.TarInfo("usr/lib/go/../outside"))
        self.rejected("unsafe")

    def test_symlink_rejected(self):
        member = tarfile.TarInfo("usr/lib/go/link")
        member.type, member.linkname = tarfile.SYMTYPE, "/outside"
        self.write_go(extra=member)
        self.rejected("member type")

    def test_duplicate_rejected(self):
        self.write_go(extra=tarfile.TarInfo("usr/lib/go/VERSION"))
        self.rejected("duplicate")

    def test_existing_go_not_overwritten(self):
        self.package()
        self.base = self.output
        self.output = self.root / "rejected.tar.gz"
        self.rejected("existing Go")

    @unittest.skipUnless(os.environ.get("FAKEFSIFY_BIN"), "set FAKEFSIFY_BIN to check the real importer")
    def test_unicode_and_long_names_import_without_locale_configuration(self):
        name = "usr/lib/go/src/cmd/go/testdata/" + "long-directory/" * 8 + "\u03bb.go"
        self.write_go(extra=tarfile.TarInfo(name))
        self.package()
        guest = self.root / "guest"
        imported = subprocess.run([os.environ["FAKEFSIFY_BIN"], str(self.output), str(guest)],
                                  capture_output=True, timeout=30, env={**os.environ, "LC_ALL": "C"})
        self.assertEqual(imported.returncode, 0, imported.stderr.decode())
        self.assertTrue((guest / "data" / name).is_file())


if __name__ == "__main__":
    unittest.main()
