import json, glob, os, sys
D = sys.argv[1]
ARMS = "ABC"
cells = {}
for f in sorted(glob.glob(os.path.join(D, "dt-*-r*.json"))):
    d = json.load(open(f))
    cells[(d["cell"], d["arm"], d["run"])] = d


def pc(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))] if xs else float("nan")


def f1(x):
    return f"{x:,.1f}"


def hot_table(cellnames):
    print("| cell | arm | run | n | reads/s | p0 | p25 | p50 | p95 | p99 | p99.9 | max |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for c in cellnames:
        for a in ARMS:
            for r in (0, 1):
                d = cells.get((c, a, r))
                if not d:
                    continue
                h = d["hot_service"]
                print(f"| {c} | {a} | r{r} | {h['n']:,} | {h['n']/d['seconds']:,.1f} /s | " + " | ".join(
                    f"{f1(h[k])} µs" for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us", "p99.9_us", "max_us")) + " |")


def walker_table(cellnames):
    print("| cell | arm | run | walks finished (in window) | walks > 1 s | p0 | p25 | p50 | p95 | p99 | max |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for c in cellnames:
        for a in ARMS:
            for r in (0, 1):
                d = cells.get((c, a, r))
                if not d:
                    continue
                w = d["walk_seconds"]
                ms = lambda x: f"{x*1000:,.0f} ms"
                print(f"| {c} | {a} | r{r} | {len(w)} ({d['walks_in_window']}) | {sum(1 for x in w if x > 1.0)} | "
                      f"{ms(min(w))} | {ms(pc(w,.25))} | {ms(pc(w,.5))} | {ms(pc(w,.95))} | {ms(pc(w,.99))} | {ms(max(w))} |")


def wal_table(cellnames):
    print("| cell | arm | run | window | `on_cpu` | `sched.run_queue` (sum) | `io.page_read` | system polls | fg polls | longest hold | holds > 100 ms | WAL appended | page images (MB) | WAL syncs | sync-gate waits (count / total) | checkpoint runs | ckpt pages written | ring full | commits |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for c in cellnames:
        for a in ARMS:
            for r in (0, 1):
                d = cells.get((c, a, r))
                if not d:
                    continue
                m = d["meta_delta"]
                w = d["waits_delta"]
                span = d["window_s"] * 1e6
                sh = lambda e: f"{w.get(e,{}).get('total_us',0)/span*100:.1f} %"
                print(f"| {c} | {a} | r{r} | {d['window_s']:.1f} s | {sh('cpu.on_cpu')} | {sh('sched.run_queue')} | {sh('io.page_read')} | "
                      f"{m.get('sched_system_polls',0):,.0f} | {m.get('sched_foreground_polls',0):,.0f} | {m.get('sched_hold_in_run_longest_us',0)/1000:,.1f} ms | "
                      f"{m.get('sched_hold_in_run_over_100ms',0):,.0f} | {m.get('wal_append_lsn',0)/1e6:,.1f} MB | "
                      f"{m.get('pool_fetch_images',0):,.0f} ({m.get('pool_fetch_image_bytes',0)/1e6:,.0f}) | {m.get('wal_syncs',0):,.0f} | "
                      f"{m.get('contention_wal_sync_gate_waits',0):,.0f} / {m.get('contention_wal_sync_gate_wait_us',0)/1e6:.2f} s | "
                      f"{m.get('contention_checkpoint_runs',0):,.0f} | {m.get('contention_checkpoint_pages_written',0):,.0f} | {m.get('wal_ring_full',0):,.0f} | "
                      f"{d.get('commits', '')} |")


for title, fn, cn in (("HOT", hot_table, ["ivw", "ivu", "ivr", "i"]),
                      ("WALKER with hot", walker_table, ["ivw", "ivu", "ivr"]),
                      ("WALKER alone", walker_table, ["aw", "au"]),
                      ("WAIT/WAL", wal_table, ["ivw", "ivu", "ivr", "i", "aw", "au"])):
    print("###", title)
    fn(cn)
    print()
print("### misc")
for k, d in sorted(cells.items()):
    print(k, d["errors"], d["first_error"], d["walk_replies"][:2], "host", d["host_before"]["loadavg"], "|", d["host_after"]["loadavg"], d["bin_sha256"][:12])
