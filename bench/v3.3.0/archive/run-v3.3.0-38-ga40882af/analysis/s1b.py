import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis")
from s1 import *


def cmp_pct(names, tag="s1-c1-g", pgtag="pg-s1"):
    s = "| phase | run | ops | p0 | p25 | p50 | p95 | p99 |\n|---|---|---|---|---|---|---|---|\n"
    for n in names:
        for lab, d in (("now", now(tag)), ("morning (old client)", mor(1, tag)), ("PostgreSQL 18.6 (morning's run)", mor(1, pgtag))):
            p = ph(d, n)
            s += (f"| {n} | {lab} | {p['ops']:,} | {p['p0_us']:,.1f} µs | {p['p25_us']:,.1f} µs | {p['p50_us']:,.1f} µs | {p['p95_us']:,.1f} µs | {p['p99_us']:,.1f} µs |\n")
    return s


def components():
    d, m, p = now("s1-c1-g"), mor(1, "s1-c1-g"), mor(1, "pg-s1")
    bl, blm, blp = ph(d, "read-bar-lookup"), ph(m, "read-bar-lookup"), ph(p, "read-bar-lookup")
    br, brm, brp = ph(d, "read-bar-range"), ph(m, "read-bar-range"), ph(p, "read-bar-range")
    sh, shm, shp = ph(d, "read-symbol-history"), ph(m, "read-symbol-history"), ph(p, "read-symbol-history")
    ds, dsm, dsp = ph(d, "read-day-slice"), ph(m, "read-day-slice"), ph(p, "read-day-slice")
    a1 = med([x["qps"] for r in N("s1-c1-g") for x in r["meta"]["write_sweep"] if x["batch"] == 1])
    a1k = med([x["qps"] for r in N("s1-c1-g") for x in r["meta"]["write_sweep"] if x["batch"] == 1000])
    pa1 = med([x["qps"] for r in Mo(1, "pg-s1") for x in r["meta"]["write_sweep"] if x["batch"] == 1])
    pa1k = med([x["qps"] for r in Mo(1, "pg-s1") for x in r["meta"]["write_sweep"] if x["batch"] == 1000])
    s = "| Component | Now | Morning (old client) | PostgreSQL (morning's run) | How derived |\n|---|---|---|---|---|\n"
    s += (f"| Fixed round trip (client, socket, parse, one pk descent) | **{bl['mean_us']:,.1f} µs** mean, p0 {bl['p0_us']:,.1f} µs | {blm['mean_us']:,.1f} µs, p0 {blm['p0_us']:,.1f} µs | "
          f"{blp['mean_us']:,.1f} µs, p0 {blp['p0_us']:,.1f} µs | `read-bar-lookup`, run 1, 200 ops |\n")
    s += (f"| Per-row cost of a returned row | **{(br['mean_us'] - bl['mean_us']) / 200:.2f} µs per row** (`read-bar-range`) and **{(sh['mean_us'] - bl['mean_us']) / 7560:.2f} µs per row** (`read-symbol-history`) | "
          f"{(brm['mean_us'] - blm['mean_us']) / 200:.2f} and {(shm['mean_us'] - blm['mean_us']) / 7560:.2f} µs | {(brp['mean_us'] - blp['mean_us']) / 200:.2f} and {(shp['mean_us'] - blp['mean_us']) / 7560:.2f} µs | "
          f"(mean - fixed) / rows returned (200; 7,560), run 1 of each |\n")
    s += (f"| Per-row cost of an examined, unreturned row | about {(ds['mean_us'] - bl['mean_us']) / 60480 * 1000:.0f} ns | about {(dsm['mean_us'] - blm['mean_us']) / 60480 * 1000:.0f} ns | "
          f"about {(dsp['mean_us'] - blp['mean_us']) / 60480 * 1000:.0f} ns | `read-day-slice` examines all 60,480 `daily_stats` rows and returns 8: (mean - fixed) / 60,480. Run 1 of each; this is the shape the host drift of section 9 touches |\n")
    s += (f"| Durability / commit | **{1e6 / a1 - 1e6 / a1k:,.0f} µs per commit** | - | {1e6 / pa1 - 1e6 / pa1k:,.0f} µs | autocommit insert {1e6 / a1:,.0f} µs per row, "
          f"a 1,000-row batch {1e6 / a1k:,.1f} µs per row; the difference is one durable commit (median of three runs) |\n")
    s += "| Lock or conflict wait | none | none | none | one connection (the concurrency sweep reads only) |\n"
    s += f"| Write statement | {1e6 / a1k:,.1f} µs per row at batch 1,000 | - | {1e6 / pa1k:,.1f} µs | the insert path with the sync amortised away |\n"
    return s


if __name__ == "__main__":
    for f in sys.argv[1:]:
        print(globals()[f]())
