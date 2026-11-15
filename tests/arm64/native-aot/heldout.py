# Evaluation only; not used to generate any AOT recording.
import hashlib
import json
import pathlib
import sys
import zlib

case = sys.argv[1]
root = pathlib.Path(sys.argv[2])
if case == "json":
    raw = (root / "records.json").read_bytes()
    total = 0
    for _ in range(8):
        rows = json.loads(raw)
        chosen = sorted((r for r in rows if r["active"]), key=lambda r: (r["group"], r["id"]))
        total += sum(r["value"] * (r["group"] + 1) for r in chosen)
        assert json.loads(json.dumps(chosen, sort_keys=True)) == chosen
    print("HELDOUT_JSON", len(chosen), total, hashlib.sha256(raw).hexdigest())
elif case == "codec":
    raw = (root / "payload.bin").read_bytes()
    for level in (1, 6, 9):
        for _ in range(4):
            enc = zlib.compressobj(level=level, wbits=-15)
            packed = enc.compress(raw) + enc.flush()
            dec = zlib.decompressobj(wbits=-15)
            parts = [dec.decompress(packed[p:p + 8191]) for p in range(0, len(packed), 8191)]
            restored = b"".join(parts) + dec.flush()
            assert dec.eof and restored == raw
    print("HELDOUT_CODEC", len(raw), hashlib.sha256(raw).hexdigest())
else:
    raise ValueError(case)
