#!/usr/bin/env python3
"""Move runs that meet a discard rule (analyze_bk.reasons) from <dir> to <dir>/discarded/, with the reasons beside them.
usage: triage.py <dir> <cores> [--new-is-old] [--rows 200,1000,10000] [--dry]
"""
import argparse, glob, json, os, shutil, statistics, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_bk as ab  # noqa


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("cores", type=int)
    ap.add_argument("--rows", default="200,1000,10000")
    ap.add_argument("--new-is-old", action="store_true")
    ap.add_argument("--flipped", action="store_true")
    ap.add_argument("--null-new", action="store_true")
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args()
    moved = 0
    for rows in [int(x) for x in a.rows.split(",")]:
        allr = ab.load(a.dir, a.cores, rows, 0, 10 ** 6)
        if not allr:
            continue
        ref = statistics.median(j["arms"][L]["ping"]["p50_us"] for j in allr for L in "AB")
        for j in allr:
            rs = ab.reasons(j, ref, ('flip' if a.flipped else ('nullnew' if a.null_new else ('null' if a.new_is_old else 'main'))))
            if not rs:
                continue
            tag = j["tag"]
            print(f"{tag}: {'; '.join(rs)[:200]}")
            moved += 1
            if a.dry:
                continue
            dd = os.path.join(a.dir, "discarded")
            os.makedirs(dd, exist_ok=True)
            for ext in (".json", ".raw.json.gz"):
                shutil.move(os.path.join(a.dir, tag + ext), os.path.join(dd, tag + ext))
            with open(os.path.join(dd, tag + ".reasons.txt"), "w") as f:
                f.write("\n".join(rs) + "\n")
    print(f"{moved} runs {'would be ' if a.dry else ''}moved")


if __name__ == "__main__":
    sys.exit(main())
