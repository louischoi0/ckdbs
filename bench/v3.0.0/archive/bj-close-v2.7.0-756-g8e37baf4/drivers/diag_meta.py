import glob, gzip, json, statistics, sys
D = sys.argv[1]
keys = ["wal_ring_full", "wal_syncs", "wal_interval_syncs", "contention_checkpoint_runs", "contention_checkpoint_longest_us",
        "undo_pages_recycled", "undo_pages_live", "contention_syncs_writer", "contention_syncs_inline", "pool_resident",
        "contention_wal_stream_waits", "contention_wal_stream_wait_us", "contention_wal_sync_gate_waits"]
js = [json.load(open(f)) for f in sorted(glob.glob(f"{D}/diag-c1-n1000-r*.json"))]
raws = [json.load(gzip.open(f.replace(".json", ".raw.json.gz"), "rt")) for f in sorted(glob.glob(f"{D}/diag-c1-n1000-r*.json"))]
print("runs", len(js))
for k in keys:
    a = [j["meta_end_all"]["A"].get(k) for j in js]
    b = [j["meta_end_all"]["B"].get(k) for j in js]
    print(f"{k:38s} A {statistics.median(a):>10.0f} [{min(a):.0f},{max(a):.0f}]   B {statistics.median(b):>10.0f} [{min(b):.0f},{max(b):.0f}]")
for i, (j, r) in enumerate(zip(js, raws)):
    st = {L: max(r["ab"][L]["insert"]) / 1000 for L in "AB"}
    print(i, "insert max ms A %.0f B %.0f" % (st["A"], st["B"]), " ckpt_longest_us A/B", j["meta_end_all"]["A"].get("contention_checkpoint_longest_us"), j["meta_end_all"]["B"].get("contention_checkpoint_longest_us"),
          "ckpt_runs", j["meta_end_all"]["A"].get("contention_checkpoint_runs"), j["meta_end_all"]["B"].get("contention_checkpoint_runs"))
