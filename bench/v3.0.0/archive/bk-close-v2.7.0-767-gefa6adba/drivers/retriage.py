#!/usr/bin/env python3
"""Restore runs from <dir>/discarded/ that are clean under the corrected rules (analyze_bk.py as it now stands).
A tag with a kept run is left alone; otherwise the earliest clean attempt is restored as <tag>.json.
usage: retriage.py <dir> <cores> [--new-is-old] [--dry]
"""
import argparse, glob, json, os, re, shutil, statistics, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_bk as ab  # noqa


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("cores", type=int)
    ap.add_argument("--new-is-old", action="store_true")
    ap.add_argument("--flipped", action="store_true")
    ap.add_argument("--null-new", action="store_true")
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args()
    dd = os.path.join(a.dir, "discarded")
    restored = 0
    for rows in (200, 1000, 10000):
        kept = ab.load(a.dir, a.cores, rows, 0, 10 ** 6)
        disc = []
        for f in glob.glob(f"{dd}/c{a.cores}-n{rows}-r*.json"):
            b = os.path.basename(f)
            if b.endswith(".reasons.txt"):
                continue
            m = re.match(rf"(c{a.cores}-n{rows}-r(\d+))(?:-a(\d+))?\.json$", b)
            if not m:
                continue
            j = json.load(open(f))
            j["_raw"] = f[:-len(".json")] + ".raw.json.gz"
            disc.append((m.group(1), int(m.group(3) or -1), f, j))
        allj = [j for _, _, _, j in disc] + kept
        if not allj:
            continue
        ref = statistics.median(j["arms"][L]["ping"]["p50_us"] for j in allj for L in "AB")
        have = {j["tag"] for j in kept}
        for tag, att, f, j in sorted(disc, key=lambda x: (x[0], x[1])):
            if tag in have:
                continue
            if ab.reasons(j, ref, ('flip' if a.flipped else ('nullnew' if a.null_new else ('null' if a.new_is_old else 'main')))):
                continue
            print(f"restore {os.path.basename(f)} -> {tag}.json")
            have.add(tag)
            restored += 1
            if not a.dry:
                shutil.move(f, os.path.join(a.dir, tag + ".json"))
                shutil.move(f[:-len(".json")] + ".raw.json.gz", os.path.join(a.dir, tag + ".raw.json.gz"))
                rs = f[:-len(".json")] + ".reasons.txt"
                if os.path.exists(rs):
                    shutil.move(rs, os.path.join(dd, "restored-" + os.path.basename(rs)))
    print(f"{restored} runs {'would be ' if a.dry else ''}restored")


if __name__ == "__main__":
    sys.exit(main())
