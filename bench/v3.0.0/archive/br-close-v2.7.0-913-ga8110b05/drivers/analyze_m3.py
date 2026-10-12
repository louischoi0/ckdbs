#!/usr/bin/env python3
"""M3 tables. usage: analyze_m3.py <out/m3 dir>"""
import glob, json, statistics as st, sys

for f in sorted(glob.glob(f"{sys.argv[1]}/*.json")):
    d = json.load(open(f))
    m = d["manifest"]
    start, stop = int(m["start_lsn"]), int(m["stop_lsn"])
    print(f"\n### {d['label']} (cores {d['cores']}, archive {d['archive']}, target {d['target']})")
    print(f"backup {m['backup_id']}: path {m['path']}, start {start}, stop {stop}, window {(stop - start) / (1 << 20):.0f} MiB of log, "
          f"segments {m['start_segment']}..{m['stop_segment']}, backup size {d['backup_bytes'] / (1 << 20):.0f} MiB; verify {d['verify']['s']} s")
    print("| rep | backup files | total | layout (copy + verify) | recovery (mount, replay, exit) |")
    print("|---|---|---|---|---|")
    for r in d["restores"]:
        print(f"| {r['rep']} | {'cold (fadvise DONTNEED)' if r['cold_backup_files'] else 'warm'} | {r['total_s']} s | {r['layout_s']} s | {r['recovery_s']} s |")
    s = d["serve"]
    mt = s["meta"] or {}
    end = mt.get("wal_durable_lsn")
    redo_start = mt.get("recovery_redo_start", mt.get("recovery_restore_start"))
    rec = {k: mt.get(k) for k in ("recovery_records", "recovery_committed", "recovery_redo_applied", "recovery_analysis_us", "recovery_redo_us",
                                  "recovery_undo_us", "recovery_checkpoint_us", "recovery_high_water_us")}
    print("serve run's recovery block:", rec)
    if mt.get("recovery_analysis_us") is not None:
        scan_s = (mt["recovery_analysis_us"] + mt["recovery_redo_us"]) / 1e6
        total_us = sum(mt.get(k) or 0 for k in ("recovery_analysis_us", "recovery_redo_us", "recovery_high_water_us", "recovery_undo_us", "recovery_checkpoint_us"))
        print(f"phases sum {total_us / 1e6:.2f} s of the recovery wall time {st.median([r['recovery_s'] for r in d['restores']]):.2f} s")
    print("layout lines:", s["lines"][:2])
