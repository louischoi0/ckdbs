#!/usr/bin/env python3
"""M1a tables. usage: analyze_m1a.py <out/m1a dir> <out/m1a-diag dir>"""
import glob, json, statistics as st, sys


def load(d, cores, mode):
    return [json.load(open(f)) for f in sorted(glob.glob(f"{d}/c{cores}-p*-{mode}.json"))]


def rng(v, d=1):
    return f"{st.median(v):.{d}f} [{min(v):.{d}f} to {max(v):.{d}f}]"


ARMS = [("insert", "INSERT hot (10,000 rows)"), ("select-pk", "SELECT by pk"), ("update-pk", "UPDATE by pk"),
        ("wide-insert-50rows", "wide INSERT, 50 rows x 1.8 KB")]
print("| cores | shape | archive | n | statements/s | p50 | p99 | p99.9 | max |")
print("|---|---|---|---|---|---|---|---|---|")
for cores in (1, 4):
    off, on = load(sys.argv[1], cores, "off"), load(sys.argv[1], cores, "on")
    for arm, name in ARMS:
        for lab, rs in (("off", off), ("on", on)):
            v = [r["results"][arm] for r in rs]
            print(f"| {cores} | {name} | {lab} | {len(v)} rounds | {rng([x['qps'] for x in v], 0)} /s | {rng([x['p50_us'] for x in v])} µs | "
                  f"{rng([x['p99_us'] for x in v])} µs | {rng([x['p999_us'] / 1000 for x in v], 1)} ms | {rng([x['max_us'] / 1000 for x in v], 0)} ms |")
        # paired delta on qps
        d = [b["results"][arm]["qps"] / a["results"][arm]["qps"] * 100 - 100 for a, b in zip(off, on)]
        print(f"| {cores} | {name} | on vs off, paired | {len(d)} rounds | {rng(d)} % | | | | |")
print("\nWAL and archive counters, wide INSERT phase:")
print("| cores | archive | WAL written in the phase | WAL rate | segments archived by the end | wal_archive_lag_segments at the end of the phase | failures |")
print("|---|---|---|---|---|---|---|")
for cores in (1, 4):
    for lab in ("off", "on"):
        rs = load(sys.argv[1], cores, lab)
        w = [r["results"]["wide-insert-50rows"] for r in rs]
        print(f"| {cores} | {lab} | {rng([x['wal_bytes'] / (1 << 20) for x in w], 0)} MiB | {rng([x['wal_mib_s'] for x in w])} MiB/s | "
              f"{rng([r['meta_after']['wal_archived'] or 0 for r in rs], 0)} | {rng([r['meta_end']['wal_archive_lag_segments'] or 0 for r in rs], 0)} | {sum(r['meta_after']['wal_archive_failures'] or 0 for r in rs):.0f} |")
print("\nDiagnostic (cores = 1): archive directory on tmpfs (/dev/shm), on the data device, and off. 3 rounds, order rotated.")
print("| archive | n | wide INSERT statements/s | p50 | p99 | WAL rate |")
print("|---|---|---|---|---|---|")
for mode in ("off", "shm", "on"):
    rs = load(sys.argv[2], 1, mode)
    w = [r["results"]["wide-insert-50rows"] for r in rs]
    print(f"| {mode} | {len(w)} | {rng([x['qps'] for x in w], 1)} /s | {rng([x['p50_us'] for x in w])} µs | {rng([x['p99_us'] for x in w])} µs | {rng([x['wal_mib_s'] for x in w])} MiB/s |")
