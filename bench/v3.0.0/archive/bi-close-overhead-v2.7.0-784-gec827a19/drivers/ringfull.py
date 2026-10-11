"""wal_ring_full (drains the appender paid) at the end of each run vs the out-of-run hold growth. usage: ringfull.py <out-dir> <cell>"""
import glob, json, statistics as st, sys

out, cell = sys.argv[1:3]
for rows in (200, 1000, 10000):
    for srv in "AB":
        rf, big, both = [], [], []
        for f in glob.glob(f"{out}/{cell}/n{rows}-r*.json"):
            s = json.load(open(f))
            rf.append(s["meta_end"][srv].get("wal_ring_full", 0))
            o1 = s["meta_end"][srv].get("sched_hold_out_run_longest_us", 0) / 1000
            big.append(o1)
            both.append((s["meta_end"][srv].get("wal_ring_full", 0), o1))
        print(f"{cell} {rows:6d} {srv}: wal_ring_full median {st.median(rf):.0f} [{min(rf):.0f}-{max(rf):.0f}]; out-of-run longest hold median {st.median(big):.1f} ms, runs over 100 ms {sum(b > 100 for b in big)} of {len(big)}")
