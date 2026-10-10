#!/usr/bin/env python3
"""BA-S4's census analysis: reads every run's JSON under this directory and
prints the tables the results file quotes.

Per cell (engine, shape, durability, cores or PostgreSQL's CPU set, sessions),
the median over its runs of statements/s, p50 and p99. The client-bound mark
is recomputed here: a run is client-bound when any one client CPU - the sum of
the clients the driver pinned to it, client i on CPU i mod n - was over 80 %
busy with them (the driver's own mark tested each client alone, and 16
sessions on 4 CPUs saturate them at 25 % each).

Each BA item's share, KDS only: its latches' summed contended wait over
`cores` x the measured span (`contention_*` is summed over every core;
`sched_wall_us` is one core's, `meta_session_core`). The WAL writer thread
writes slot 0 (`contention.hpp`), so a wait it takes is in the sum too: the
share is of `cores` x span, not strictly of reactor time. Materiality is
BA-R0's: 5 % in any cell, or any refusal; P12 when a carve or a checkpoint ran
over 10 ms. `*_longest_us` is a maximum over the server's life - setup and
warm-up included - not over the measured span.
"""

import glob
import json
import os
import statistics as st
import sys
from collections import defaultdict

D = os.path.dirname(os.path.abspath(__file__))

ITEMS = {
    "P1 frame table": ["frame_table", "free_map", "free_map_flush"],
    "P2 window": ["window"],
    "P3 statistics": ["optimizer", "cabin_stats", "cabin_partition"],
    "P4 relation borrow": ["lock_partition", "lock_wait_for"],
    "P6 WAL append": ["wal_stream"],
    "P7 sync gate": ["wal_sync_gate"],
    # Defect B's snapshot ceiling is P7's (section 0); a tally, not a latch.
    "P7 snapshot ceiling": ["ceiling"],
    "P8 assertion dir": ["assertion_dir"],
    "superblock": ["superblock"],
}
LATCHES = {k for kinds in ITEMS.values() for k in kinds} - {"ceiling"}


def load():
    runs = []
    for f in glob.glob(os.path.join(D, "*", "*.json")):
        r = json.load(open(f))
        r["_set"] = os.path.basename(os.path.dirname(f))
        runs.append(r)
    return runs


def client_bound(r):
    ncpu = len(r["client_cpus"].split(","))
    per_cpu = defaultdict(float)
    for i, s in enumerate(r["client_cpu_share"]):
        per_cpu[i % ncpu] += s
    return max(per_cpu.values()) > 0.80


def cell_key(r):
    place = f"k{r['cores']}" if r["engine"] == "kds" else r["_set"].replace("pg-cpus", "cpus ")
    return (r["engine"], r["shape"], r["durability"], place, r["sessions"])


def med(xs):
    xs = [x for x in xs if x is not None]
    return st.median(xs) if xs else None


def share(r, kinds):
    span_us = r["span_s"] * 1e6 * max(1, r["cores"] or 1)
    wait = sum(r["meta_delta"].get(f"contention_{k}_wait_us", 0) for k in kinds)
    return wait / span_us if span_us else 0.0


def main():
    runs = load()
    cells = defaultdict(list)
    for r in runs:
        cells[cell_key(r)].append(r)

    print("## Throughput per cell (median of runs)\n")
    print("| engine | shape | durability | place | sessions | runs | statements/s | p50 | p99 | refusals | client-bound runs |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for key in sorted(cells, key=lambda k: (k[0], k[1], k[2], k[3], k[4])):
        rs = cells[key]
        cb = sum(client_bound(r) for r in rs)
        print(f"| {' | '.join(map(str, key[:4]))} | {key[4]} sessions | {len(rs)} runs | "
              f"{med(r['statements_per_s'] for r in rs):,.0f} stmt/s | "
              f"{med(r.get('p50_us') for r in rs):,.1f} µs | {med(r.get('p99_us') for r in rs):,.1f} µs | "
              f"{sum(r['refusals'] for r in rs)} refusals | {cb} of {len(rs)} |")

    kds = [r for r in runs if r["engine"] == "kds"]
    print("\n## Item shares, KDS (contended wait over cores x span)\n")
    print("| item | max share, all cells | cell at max | max share, clean cells (cores <= 2) | clean cell at max "
          "| median share over cells | material (>= 5 %) |")
    print("|---|---|---|---|---|---|---|")
    for item, kinds in ITEMS.items():
        by_cell = defaultdict(list)
        for r in kds:
            by_cell[cell_key(r)].append(share(r, kinds))
        meds = {k: med(v) for k, v in by_cell.items()}
        kmax = max(meds, key=meds.get)
        clean = {k: v for k, v in meds.items() if k[3] in ("k1", "k2")}
        cmax = max(clean, key=clean.get)
        print(f"| {item} | {meds[kmax] * 100:.2f} % | {' '.join(map(str, kmax[1:]))} | "
              f"{clean[cmax] * 100:.2f} % | {' '.join(map(str, cmax[1:]))} | "
              f"{med(meds.values()) * 100:.3f} % | {'yes' if meds[kmax] >= 0.05 else 'no'} |")

    print("\n## Other counters, KDS (sum over every run)\n")
    tot = defaultdict(float)
    for r in kds:
        for k, v in r["meta_delta"].items():
            if k.startswith("contention_") and not any(
                    k in (f"contention_{n}_waits", f"contention_{n}_wait_us") for n in LATCHES | {"other", "handoff"}):
                tot[k] += v
    for k in sorted(tot):
        print(f"- `{k}`: {tot[k]:,.0f}")
    longest = defaultdict(float)
    for r in kds:
        for k, v in r["meta_longest"].items():
            longest[k] = max(longest[k], v)
    events = {"contention_carve_longest_us": "contention_carves",
              "contention_checkpoint_longest_us": "contention_checkpoint_runs"}
    for k, v in longest.items():
        over = [r for r in kds if r["meta_longest"].get(k, 0) > 10_000]
        none_in_span = sum(1 for r in over if not r["meta_delta"].get(events[k], 0))
        print(f"- `{k}` (over the server's life): max {v / 1000:,.1f} ms; runs over 10 ms: "
              f"{len(over)} of {len(kds)}, {none_in_span} of them with no such event in the measured span")
    refused = defaultdict(float)
    for r in kds:
        for k, v in r["meta_delta"].items():
            if k.startswith("contention_refused_"):
                refused[(r["shape"], r["cores"])] += v
    print("\n### Refusals by shape and cores (sum over runs)\n")
    for (shape, k), v in sorted(refused.items()):
        if v:
            print(f"- {shape} k{k}: {v:,.0f}")

    print("\n## BA-R4's premise: a session's latency by its core, `cores = 2`\n")
    # Medians over clients of each client's own p50/p99; a client with few
    # statements has a p99 that is its maximum, hence the statement column.
    print("| shape | durability | core 0 p50 | core 0 p99 | peer p50 | peer p99 | clients on core 0 / peers "
          "| statements per client, core 0 / peers (median) |")
    print("|---|---|---|---|---|---|---|---|")
    for shape in ("insert1", "insertN", "trade"):
        for dur in ("group", "strict"):
            c0, peer = [], []
            for r in kds:
                if r["shape"] == shape and r["durability"] == dur and r["cores"] == 2:
                    for c in r.get("by_client", []):
                        if not c["statements"]:
                            continue
                        (c0 if c["core"] == 0 else peer).append(c)
            if c0 or peer:
                fmt = lambda xs, q: f"{med(c[q] for c in xs):,.1f} µs" if xs else "-"
                print(f"| {shape} | {dur} | {fmt(c0, 'p50_us')} | {fmt(c0, 'p99_us')} | "
                      f"{fmt(peer, 'p50_us')} | {fmt(peer, 'p99_us')} | {len(c0)} / {len(peer)} clients | "
                      f"{med(c['statements'] for c in c0):,.0f} / {med(c['statements'] for c in peer):,.0f} statements |")

    # P11's proxy: BA-R0's balanced arm was not run, so within a cell the runs
    # whose sessions landed on every core are set against those that did not.
    print("\n## P11 proxy: runs with a session on every core against the rest, same cell\n")
    gains = []
    for key, rs in sorted(cells.items()):
        if key[0] != "kds" or key[3] == "k1":
            continue
        k = int(key[3][1:])
        bal = [r for r in rs if len(set(r["session_cores"])) == k]
        unb = [r for r in rs if len(set(r["session_cores"])) != k]
        if bal and unb:
            g = med(r["statements_per_s"] for r in bal) / med(r["statements_per_s"] for r in unb) - 1
            gains.append(g)
            print(f"- {' '.join(map(str, key[1:]))}: {len(bal)} runs against {len(unb)}, {g * 100:+.1f} %")
    print(f"\n{len(gains)} cells; the every-core runs faster by over 10 % in {sum(g > 0.10 for g in gains)}, "
          f"slower by over 10 % in {sum(g < -0.10 for g in gains)}")

    print("\n## Host\n")
    loads = [float(r["host_before"]["loadavg"].split()[0]) for r in runs]
    kl = [float(r["host_before"]["loadavg"].split()[0]) for r in kds]
    # The pgrep pattern also matches a process whose command line merely
    # *names* a build: another session's long-lived `until ... pgrep -f
    # "cmake --build build"` wait loop, and that loop's own pgrep. Only a
    # process whose own program is a compiler, cmake or ctest counts; any real
    # build has one, whatever shell wrapper launched it. An entry is
    # "<pid> <argv0> <args...>".
    def real(entries):
        return [e for e in entries
                if len(e.split()) > 1 and os.path.basename(e.split()[1]) in ("cc1plus", "cmake", "ctest")]
    competing = sum(1 for r in runs if real(r["host_before"]["competing"] + r["host_after"]["competing"]))
    named = sum(1 for r in runs if r["host_before"]["competing"] or r["host_after"]["competing"])
    print(f"- runs: {len(runs)}; 1-min load before a run: median {med(loads):.2f}, max {max(loads):.2f}; "
          f"KDS runs alone: median {med(kl):.2f}, max {max(kl):.2f}")
    print(f"- runs with a competing build or ctest before or after: {competing}")
    print(f"- runs where pgrep matched only a wait loop naming a build: {named - competing}")
    print(f"- binaries: {sorted({r['bin_sha256'] for r in runs if r['bin_sha256']})}")
    print(f"- data devices: {sorted({r['data_device'] for r in runs if r['data_device']})}")


if __name__ == "__main__":
    sys.exit(main())
