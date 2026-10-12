"""Per arm, the SHOW META snapshots of one run. usage: snaps_run.py <out-dir>/<cell> <rows> <run>"""
import json, sys

d, rows, run = sys.argv[1:4]
s = json.load(open(f"{d}/n{rows}-r{run}.json"))
print("order_first", s["order_first"], "mount_ckpt_us", s["mount_ckpt_us"], "wait_s", s["wait_s"])
for srv in "AB":
    prev = None
    print(srv, "max_us per arm:", {a: round(v["max_us"] / 1000, 1) for a, v in s["arms"][srv].items()})
    for arm, sn in s["snaps"][srv].items():
        row = {k: sn[k] for k in ("checkpoint_runs", "contention_checkpoint_longest_us", "sched_hold_in_run_longest_us", "sched_hold_out_run_longest_us", "pool_resident", "pool_slots", "wal_syncs", "wal_interval_syncs")}
        print(f"   {arm:13s}", row)
    print("  end:", {k: v for k, v in s["meta_end"][srv].items() if k in ("checkpoint_last_wall_us", "checkpoint_last_steps", "checkpoint_last_pages", "checkpoint_last_dirty_at_start", "checkpoint_last_step_max_us", "checkpoint_last_page_write_max_us", "checkpoint_last_data_sync_max_us", "checkpoint_last_publish_sync_max_us", "wal_segments_removed")})
