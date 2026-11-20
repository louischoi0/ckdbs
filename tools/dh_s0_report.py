#!/usr/bin/env python3
"""Turns dh_s0.py's JSON lines into the markdown tables of DH-S0's results file.

    dh_s0_report.py MICRO.jsonl SCN.jsonl

Prints to stdout; no judgement is made here (the results file carries it). Every
cell carries its unit (bench/README.md). Checkpoint runs are the server's own
counter `contention_checkpoint_runs` (the `sys.checkpoints` rows an older
`dh_s0.py` kept were filtered by a maximum `seq`, which counts per core, and are
not used here).
"""
import json, sys

PAGE = 8192


def load(p):
    return [json.loads(l) for l in open(p)] if p else []


def n(x, unit, d=0):
    """One cell: the number and its unit, or '-' where there is none."""
    return "-" if x is None else f"{x:,.{d}f} {unit}"


def micro_rows(rs):
    print("| run | arm | cell | cores x clients | durability | throughput | p0 | p25 | p50 | p90 | p99 | server CPU | log written | images | image bytes (count x 8,192) | ckpt runs (counter) | ckpt pages written | pages drained | commits per sync | ring full | wal_flush wait | run_queue wait | on_cpu | page_read wait | bad |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in rs:
        d, k = r["delta"], r["statements"]
        w = r["waits_us"]

        def wu(ev):
            return n(w.get(ev, [0, 0])[1] / k, "us/stmt", 1)
        l = r["lat_us"]
        gc, gb = d.get("wal_group_commits"), d.get("wal_group_batches")
        cell = f"{r['shape']}:{int(r['rows']):,} rows" + (f", pool {int(r['frames']):,} frames" if r.get("frames") else "")
        print(f"| {r['id']} | {r['arm']} | {cell} | {r['cores']} cores x {r['clients']} clients | {r.get('dur','group')} | "
              f"{n(r['stmt_per_s'], 'stmt/s')} | {n(l['0'], 'us')} | {n(l['25'], 'us')} | {n(l['50'], 'us')} | "
              f"{n(l['90'], 'us')} | {n(l['99'], 'us')} | {n(r['cpu_us_per_stmt'], 'us/stmt', 1)} | "
              f"{n(d['wal_append_lsn'] / k, 'B/stmt')} | {n(d['pool_fetch_images'] / k, 'img/stmt', 3)} | "
              f"{n(d['pool_fetch_images'] * PAGE / k, 'B/stmt')} | {n(d['contention_checkpoint_runs'], 'runs')} | "
              f"{n(d['contention_checkpoint_pages_written'], 'pages')} | {n(d['pool_dirty_drained'], 'pages')} | "
              f"{n(gc / gb, 'commits/sync', 1) if gb else '-'} | {n(d['wal_ring_full'], 'events')} | {wu('commit.wal_flush')} | "
              f"{wu('sched.run_queue')} | {wu('cpu.on_cpu')} | {wu('io.page_read')} | {n(r['bad'], 'stmts')} |")


def scn_rows(rs):
    print("| run | arm | cores x bookers | durability | throughput | booking p0 | p25 | p50 | p95 | p99 | commit p0 | p25 | p50 | p95 | p99 | log written | images | image bytes (count x 8,192) | ckpt runs (counter) | ckpt pages written | syncs | commits per sync | ring full | window | verify failures | committed |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in rs:
        s = r.get("summary")
        w = r.get("window")
        if not s or not w:
            print(f"| {r['id']} | {r['arm']} | - | - | no result |")
            continue
        ph = {p["phase"]: p for p in s["phases"]}
        b, c = ph["booking"], ph["commit"]
        m = s["meta"]
        k = m["outcomes"]["committed"]
        d = w["delta"]
        v = m.get("verify", {})
        gc, gb = d.get("wal_group_commits"), d.get("wal_group_batches")
        print(f"| {r['id']} | {r['arm']} | {r['cores']} cores x {r['clients']} bookers | {r.get('dur')} | {n(m['tps'], 'tps')} | "
              f"{n(b['p0_us'], 'us')} | {n(b['p25_us'], 'us')} | {n(b['p50_us'], 'us')} | {n(b['p95_us'], 'us')} | {n(b['p99_us'], 'us')} | "
              f"{n(c['p0_us'], 'us')} | {n(c['p25_us'], 'us')} | {n(c['p50_us'], 'us')} | {n(c['p95_us'], 'us')} | {n(c['p99_us'], 'us')} | "
              f"{n(d['wal_append_lsn'] / k, 'B/booking')} | {n(d['pool_fetch_images'] / k, 'img/booking', 3)} | "
              f"{n(d['pool_fetch_images'] * PAGE / k, 'B/booking')} | {n(d['contention_checkpoint_runs'], 'runs')} | "
              f"{n(d['contention_checkpoint_pages_written'], 'pages')} | {n(d['wal_syncs'], 'syncs')} | "
              f"{n(gc / gb, 'commits/sync', 1) if gb else '-'} | {n(d['wal_ring_full'], 'events')} | {n(w['span_s'], 's', 1)} | "
              f"{v.get('failures')} of {v.get('checks')} checks | {n(k, 'bookings')} |")


def main():
    micro = load(sys.argv[1])
    scn = load(sys.argv[2]) if len(sys.argv) > 2 else []
    print("### micro runs\n")
    micro_rows(micro)
    print("\n### scenario 2 runs\n")
    scn_rows(scn)


if __name__ == "__main__":
    main()
