#!/usr/bin/env python3
"""Select module-relative block offsets for gen.py --keep.

Profiles contain '<module path> <offset hex> <insns> <count>'. Each workload
has equal weight after normalization. Rank dispatch-profiled guest instructions
per native byte, then cover --coverage of the *recorded* profiled instructions.
This is a selection heuristic, not a timing or speedup measurement. Report
unrecorded coverage separately. ISH_BLOCK_PROF_FILE works in gadget-only builds
without JIT support. Exact counts require gadgets (ISH_JIT=0 if compiled with
JIT support) and DISABLE_BLOCK_CHAINING + DISABLE_RET_CACHE at compile time;
otherwise these are dispatch samples. ISH_NO_CHAIN/ISH_NO_RETCACHE are not
supported runtime switches. This checkout's exact no-chain runs also need
ISH_ARM64_EAGER_PRECHAIN=0 and ISH_ARM64_EAGER_PRECHAIN_INCOMING=0: eager
prepatching otherwise turns fake targets into host pointers despite no-chain.
"""
import argparse
import json
import math
import sys


def load(recording, profiles):
    sizes = {}
    module = None
    with open(recording) as f:
        for line in f:
            row = json.loads(line)
            if 'header' in row:
                if row['header'].get('region_exhausted') or row['header'].get('region_full'):
                    raise ValueError('incomplete recording: code arena exhausted')
                continue
            if module is not None and module != row['mod']:
                raise ValueError('expected one module')
            module = row['mod']
            # --keep selects offsets, including all variants at that offset.
            sizes[row['off']] = sizes.get(row['off'], 0) + 4 * sum(len(s['words']) for s in row['segs'])
    if not sizes:
        raise ValueError('no translations')
    worth = {}
    for profile in profiles:
        one = {}
        with open(profile) as f:
            for line in f:
                fields = line.rsplit(None, 3)
                if len(fields) != 4 or fields[0] != module:
                    continue
                off, insns, count = int(fields[1], 16), int(fields[2]), int(fields[3])
                if off < 0 or insns < 0 or count < 0:
                    raise ValueError('negative profile value')
                one[off] = one.get(off, 0) + insns * count
        total = sum(one.values())
        if not total:
            raise ValueError(f'{profile}: no executed instructions of {module}')
        for off, value in one.items():
            worth[off] = worth.get(off, 0) + value / total
    return module, sizes, worth


def select(sizes, worth, coverage):
    if not math.isfinite(coverage) or not 0 < coverage <= 1:
        raise ValueError('coverage must be in (0, 1]')
    covered = sum(worth.get(off, 0) for off in sizes)
    if not covered:
        raise ValueError('recording and profiles have no executed blocks in common')
    order = sorted(sizes, key=lambda off: (-worth.get(off, 0) / max(sizes[off], 1), off))
    keep, acc = [], 0
    for off in order:
        if not worth.get(off, 0):
            continue
        keep.append(off)
        acc += worth[off]
        if acc >= coverage * covered - 1e-12:
            break
    return sorted(keep), covered


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('recording')
    ap.add_argument('profile', nargs='+')
    ap.add_argument('--coverage', type=float, default=0.999)
    ap.add_argument('--keep', help='output offsets for gen.py --keep')
    args = ap.parse_args()
    try:
        module, sizes, worth = load(args.recording, args.profile)
        keep, covered = select(sizes, worth, args.coverage)
    except (ValueError, KeyError, OSError) as error:
        sys.exit(str(error))
    print(f'{module}: {len(sizes)} recorded offsets; recording covers {covered / sum(worth.values()):.3%} of dispatch-profiled instructions')
    print(f'keep {len(keep)} offsets, {sum(sizes[o] for o in keep)} native bytes; target {args.coverage:.3%} of recorded profiled instructions (not timing)')
    if args.keep:
        with open(args.keep, 'w') as f:
            f.write(''.join(f'{off:#x}\n' for off in keep))


if __name__ == '__main__':
    main()
