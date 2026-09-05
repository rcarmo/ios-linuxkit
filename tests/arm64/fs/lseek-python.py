"""Real-runtime integration for the ARM64 syscall return-width regression."""
import tempfile

with tempfile.TemporaryFile() as f:
    for offset in (0xFFFFF000, 0xFFFFF001, 0xFFFFFFEA, 0xFFFFFFFF, 0x100000000):
        assert f.seek(offset) == offset
        assert f.tell() == offset
        assert f.seek(-1, 1) == offset - 1
        assert f.tell() == offset - 1
    assert f.seek(0, 2) == 0  # no data written, so no multi-GB allocation
print("lseek-python-ok")
