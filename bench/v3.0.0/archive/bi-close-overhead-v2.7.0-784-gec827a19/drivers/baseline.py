#!/usr/bin/env python3
"""This run's relaxed A and B p50 (median over runs of per-run p50) against the BA close's B', derived as BA's A p50 plus
BA's median B - A delta (results-ba-close-overhead-v2.7.0-736-ge5a7e1b7.md section 4, reported passes overheadq-c1 and overheadq3-c2).
usage: baseline.py <out-dir>"""
import glob, json, re, statistics as st, sys

OUT = sys.argv[1]
SIZES = (200, 1000, 10000)
# (BA A p50, BA delta B - A) per size, from section 4 of the BA close file
BA = {
    1: {"insert": [(49.1, 0.00), (49.0, 0.05), (48.9, 0.00)], "select-pk": [(64.8, 0.50), (66.3, 0.60), (64.0, 0.50)],
        "update-pk": [(50.9, 0.55), (51.3, 0.20), (51.4, 0.65)], "delete-pk": [(48.8, 0.10), (48.8, 0.20), (49.0, 0.10)],
        "bulk20": [(93.9, 0.40), (94.5, -0.10), (94.6, -0.10)], "range100": [(85.7, 1.40), (143.9, 4.75), (783.4, 39.65)],
        "iso": [(37.7, 0.00), (37.6, 0.00), (37.6, 0.00)]},
    2: {"insert": [(49.5, 0.15), (49.4, 0.10), (49.5, 0.10)], "select-pk": [(66.4, 0.95), (68.7, 0.85), (65.7, 0.85)],
        "update-pk": [(52.1, 0.85), (52.3, 0.75), (52.7, 1.15)], "delete-pk": [(49.4, 0.30), (49.4, 0.25), (49.4, 0.20)],
        "bulk20": [(102.1, 1.00), (101.2, 1.25), (102.5, 0.95)], "range100": [(87.7, 1.35), (146.4, 3.35), (798.5, 33.25)],
        "iso": [(38.0, 0.00), (38.2, 0.00), (38.1, 0.00)]},
}
DESC = {"insert": "INSERT, pk omitted", "select-pk": "SELECT by pk", "update-pk": "UPDATE by pk", "delete-pk": "DELETE by pk",
        "bulk20": "bulk INSERT, 20 rows", "range100": "whole-relation walk", "iso": "SET ISOLATION LEVEL (control)"}


def load(cell, rows):
    fs = glob.glob(f"{OUT}/{cell}/n{rows}-r*.json")
    return [json.load(open(f)) for f in fs]


for c in (1, 2):
    print(f"\n#### cores = {c}, relaxed: p50 of the BA close's B' (`e5a7e1b7`, v2.7.0-736, derived), of this run's A (`55ee8370`, v2.7.0-779) and B (`ec827a19`, v2.7.0-784)\n")
    print("| shape | rows | BA close B' | this run, A | this run, B | A - BA B' | B - BA B' |")
    print("|---|---|---|---|---|---|---|")
    for a in DESC:
        for i, r in enumerate(SIZES):
            summ = load(f"relaxed-c{c}", r)
            if not summ:
                continue
            ba_a, ba_d = BA[c][a][i]
            bp = ba_a + ba_d
            pa = st.median(s["arms"]["A"][a]["p50_us"] for s in summ)
            pb = st.median(s["arms"]["B"][a]["p50_us"] for s in summ)
            print(f"| {DESC[a]} | {r:,} | {bp:.2f} µs | {pa:.1f} µs | {pb:.1f} µs | {pa - bp:+.2f} µs | {pb - bp:+.2f} µs |")
