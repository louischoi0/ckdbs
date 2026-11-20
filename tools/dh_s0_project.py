#!/usr/bin/env python3
"""DH-S0: the image-volume projections of each cell from the census.

    dh_s0_project.py ARCHIVE_DIR

Reads ARCHIVE_DIR/micro-runs.jsonl, scn-runs.jsonl and ARCHIVE_DIR/census/<k>.json and
prints, per census cell, the image mix, the measured images a statement under B and L,
the census's weighted bytes an image per lever and the projected image bytes a
statement (images a statement of B x weighted bytes an image), plus the
log-bytes-a-sync against flush-wait table every micro run feeds. Units are in the
labels of each printed line (B, images, us).
"""
import json, sys

A = sys.argv[1]
M = [json.loads(l) for l in open(A + "/micro-runs.jsonl")]
S = [json.loads(l) for l in open(A + "/scn-runs.jsonl")]
LEVERS = ("L1", "lz4", "zstd1", "zstd3", "L1+lz4")
cells = {"k1": ("c1r1", "c1r2"), "k2": ("c2",), "k3": ("c3",), "k5": ("c5",), "k8": ("c8",)}


def run(i, arm):
    return [r for r in M if r["id"] == i and r["arm"] == arm]


def weighted(c):
    """The census's per-type means weighted by each type's image count: (n, mix text,
    {lever: bytes an image}, hole share, zero share, mean us an image of lz4 / zstd1 / zstd3 / zlib1 / L1+lz4)."""
    ts = c["by_type"].values()
    n = sum(e["count"] for e in ts)

    def w(f):
        return sum(e["count"] * f(e) for e in ts) / n
    mix = " ".join(f"{e['name']}={100 * e['count'] / n:.1f}%" for e in ts)
    sizes = {k: w(lambda e: e[k]["mean_bytes"]) for k in LEVERS}
    us = {k: w(lambda e: e[k + "_us"]) for k in ("lz4", "zstd1", "zstd3", "zlib1", "L1+lz4")}
    return n, mix, sizes, w(lambda e: e["hole_share_mean"]), w(lambda e: e["zero_share_mean"]), us


def show(k, mix, ib, il, sizes, hole, zero, us, unit):
    print(k, "mix:", mix)
    print(f"  images/{unit} B {ib:.3f} ({ib * 8192:.0f} B)  L "
          f"{'-' if il is None else round(il, 3)} ({'-' if il is None else round(il * 8192)} B) removes "
          f"{'-' if il is None else round(100 * (1 - il / ib), 1)}%")
    print("  weighted bytes/image", {a: round(b) for a, b in sizes.items()},
          f"\n  projected image B/{unit}", {a: round(ib * b) for a, b in sizes.items()},
          "\n  us/image", {a: round(b, 1) for a, b in us.items()})
    print("  type-blind hole share %.2f%%, zero-byte share %.1f%%" % (100 * hole, 100 * zero))


for k, ids in cells.items():
    n, mix, sizes, hole, zero, us = weighted(json.load(open(f"{A}/census/{k}.json")))
    Bs = [r for i in ids for r in run(i, "B")]
    Ls = [r for i in ids for r in run(i, "L")]
    ib = sum(r["delta"]["pool_fetch_images"] / r["statements"] for r in Bs) / len(Bs)
    il = sum(r["delta"]["pool_fetch_images"] / r["statements"] for r in Ls) / len(Ls) if Ls else None
    show(k, mix, ib, il, sizes, hole, zero, us, "stmt")
for i in ("c5", "c2"):
    ra, rb = run(i, "A0")[0], run(i, "B")[0]
    d = rb["cpu_us_per_stmt"] - ra["cpu_us_per_stmt"]
    print(i, "server cpu delta B-A0 %.1f us/stmt, per image %.1f us" % (
        d, d / (rb["delta"]["pool_fetch_images"] / rb["statements"])))
print("run arm cores dur log_B/stmt commits/sync log_B/sync wal_flush_us/stmt")
for r in M:
    d, k = r["delta"], r["statements"]
    gc, gb = d["wal_group_commits"], d["wal_group_batches"]
    cps = gc / gb if gb else 1
    print(r["id"], r["arm"], r["cores"], r.get("dur"), round(d["wal_append_lsn"] / k), round(cps, 2),
          round(d["wal_append_lsn"] / k * cps), round(r["waits_us"].get("commit.wal_flush", [0, 0])[1] / k))

# scenario 2 (8 cores, group): the census of the keep-WAL B run against the first B run's window
rb = [r for r in S if r["id"] == "s4a" and r["arm"] == "B"][0]
bookings = rb["summary"]["meta"]["outcomes"]["committed"]
n, mix, sizes, hole, zero, us = weighted(json.load(open(f"{A}/census/k4a.json")))
show(f"k4a (scenario 2, whole run's log incl. the load; n = {n})", mix,
     rb["window"]["delta"]["pool_fetch_images"] / bookings, None, sizes, hole, zero, us, "booking")
