#!/usr/bin/env python3
"""Symbolize and fold a `tools/kds_sampler.c` dump (BN-S1).

    kds_sampler_report.py DUMP [--top N] [--parts PARTS.json] [--json OUT]

Prints the top functions by self samples (the interrupted frame) and by
inclusive samples (anywhere on the chain, counted once per sample). With
`--parts`, every sample is also assigned to the first part, in file order,
whose regular expression matches any function on its chain - so a part
list reads "this sample is in the parser if any frame of it is", and an
earlier part wins a sample two parts would claim; list the narrow parts
(the parser, the codec) before the broad ones (the dispatcher).
PARTS.json is a list of [name, regex] pairs; a sample no part claims is
"other".

Symbolization is `addr2line -f -C` per mapped file, one process per file,
on the file-relative address (`pc - map_start + map_offset`), which is the
link-time address for a PIE text segment and for the shared libraries this
host ships.
"""
import argparse, collections, json, re, subprocess, sys


def load(path):
    maps, samples = [], []
    with open(path) as f:
        for line in f:
            if line.startswith("M "):
                p = line[2:].split()
                if len(p) >= 6 and "x" in p[1]:
                    lo, hi = (int(x, 16) for x in p[0].split("-"))
                    maps.append((lo, hi, int(p[2], 16), p[5]))
            elif line.startswith("S"):
                samples.append([int(x, 16) for x in line[1:].split()])
    return maps, samples


def symbolize(maps, addrs):
    by_file = collections.defaultdict(list)
    out = {}
    for a in addrs:
        for lo, hi, off, path in maps:
            if lo <= a < hi:
                by_file[path].append((a, a - lo + off))
                break
        else:
            out[a] = "[unmapped]"
    for path, pairs in by_file.items():
        try:
            r = subprocess.run(["addr2line", "-f", "-C", "-e", path] +
                               ["%x" % rel for _, rel in pairs],
                               capture_output=True, text=True, check=True)
            lines = r.stdout.splitlines()
        except (OSError, subprocess.CalledProcessError):
            lines = []
        short = path.rsplit("/", 1)[-1]
        for i, (a, rel) in enumerate(pairs):
            name = lines[2 * i] if 2 * i < len(lines) else "??"
            out[a] = name if name != "??" else "%s+0x%x" % (short, rel)
    return out


def trim(name):
    # Drop argument lists and template noise so one function is one row.
    depth, cut = 0, []
    for ch in name:
        if ch in "(<":
            depth += 1
        elif ch in ")>":
            depth -= 1
        elif depth == 0:
            cut.append(ch)
    return "".join(cut).strip() or name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--parts")
    ap.add_argument("--json")
    a = ap.parse_args()
    maps, samples = load(a.dump)
    names = symbolize(maps, {pc for s in samples for pc in s})
    total = len(samples)
    self_c, incl_c = collections.Counter(), collections.Counter()
    chains = []
    for s in samples:
        fn = [trim(names[pc]) for pc in s]
        chains.append(fn)
        self_c[fn[0]] += 1
        for n in set(fn):
            incl_c[n] += 1
    print("samples", total)
    print("\n-- self --")
    for n, c in self_c.most_common(a.top):
        print("%6.2f%% %6d  %s" % (100.0 * c / total, c, n))
    print("\n-- inclusive --")
    for n, c in incl_c.most_common(a.top):
        print("%6.2f%% %6d  %s" % (100.0 * c / total, c, n))
    result = {"samples": total}
    if a.parts:
        parts = [(p, re.compile(rx)) for p, rx in json.load(open(a.parts))]
        part_c = collections.Counter()
        for fn in chains:
            got = "other"
            for p, rx in parts:
                if any(rx.search(n) for n in fn):
                    got = p
                    break
            part_c[got] += 1
        print("\n-- parts --")
        for p, _ in parts + [("other", None)]:
            print("%6.2f%% %6d  %s" % (100.0 * part_c[p] / max(1, total), part_c[p], p))
        result["parts"] = dict(part_c)
    if a.json:
        result["self"] = self_c.most_common(a.top)
        result["inclusive"] = incl_c.most_common(a.top)
        json.dump(result, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    sys.exit(main())
