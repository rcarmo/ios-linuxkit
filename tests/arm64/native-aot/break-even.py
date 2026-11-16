# Evaluation only: first 64 fixed records; never a training/recording producer.
import hashlib
import json
import pathlib
import sys
import time
import platform

raw_path, repeat_arg, warmup_arg = sys.argv[1:]
if not repeat_arg.isdecimal() or not warmup_arg.isdecimal():
    raise ValueError("integer work counts required")
repeats, warmup = int(repeat_arg), int(warmup_arg)
if not 1 <= repeats <= 64 or not 0 <= warmup <= 8:
    raise ValueError("unbounded work count")
raw = pathlib.Path(raw_path).read_bytes()
rows = json.loads(raw)
expected = [dict(id=i, group=i % 17, active=i % 3 != 0,
                 value=(101 + 7919 * i) % 65521, label="record-" + format(i, "x"))
            for i in range(64)]
if not isinstance(rows, list) or rows[:64] != expected:
    raise ValueError("fixed 64-record corpus mismatch")
corpus_hash = hashlib.sha256(json.dumps(expected, separators=(",", ":")).encode()).hexdigest()

def work():
    chosen = sorted((r for r in json.loads(raw)[:64] if r["active"]),
                    key=lambda r: (r["group"], r["id"]))
    if json.loads(json.dumps(chosen, sort_keys=True)) != chosen:
        raise ValueError("roundtrip mismatch")
    return sum(r["value"] * (r["group"] + 1) for r in chosen)

def phase(name, count):
    start = time.perf_counter_ns()
    total = sum(work() for _ in range(count))
    end = time.perf_counter_ns()
    print("BREAK_EVEN_PHASE " + json.dumps(dict(schema=1, runtime="python",
          version=platform.python_version(), clock="monotonic", phase=name,
          workCount=count, startNs=str(start), endNs=str(end),
          seconds=(end-start)/1e9), separators=(",", ":")), file=sys.stderr)
    return total

phase("warmup", warmup)
total = phase("steady-state", repeats)
print("BREAK_EVEN_JSON", 64, 42, repeats, total, corpus_hash)
