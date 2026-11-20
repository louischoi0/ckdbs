#!/usr/bin/env python3
"""Tables for the DH close measurement from dh_close.py's runs.jsonl.

    dh_close_report.py RUNS.jsonl

Prints: the A-against-B table with the floor, the attribution table (A, B3, B, A0), every run
with its percentiles / waits / log / images / checkpoints, the scenario runs and the recovery
runs. Every cell carries its unit. Image bytes are the engine's `pool_fetch_image_bytes` for
B and B3 and count x 8,224 for A and A0 (A has no byte counter: an image record is a 32-byte
header and an 8,192-byte page).
"""
import json, sys
from collections import defaultdict

rs = [json.loads(l) for l in open(sys.argv[1])]
micro = [r for r in rs if r["kind"] == "micro"]
scn = [r for r in rs if r["kind"] == "scenario"]
rec = [r for r in rs if r["kind"] == "recovery"]


def n(x, unit, d=0):
    return "-" if x is None else f"{x:,.{d}f} {unit}"


def mean(x):
    return sum(x) / len(x)


def spread(x):
    return 100 * (max(x) - min(x)) / mean(x) if len(x) > 1 else None


def cellkey(r):
    return r["id"].rsplit("r", 1)[0] if r["id"][-2] == "r" else r["id"]


cells = defaultdict(lambda: defaultdict(list))
for r in micro:
    cells[cellkey(r)][r["arm"]].append(r)
for r in scn:
    cells[cellkey(r)][r["arm"]].append(r)


def tput(r):
    return r["stmt_per_s"] if r["kind"] == "micro" else r["summary"]["meta"]["tps"]


unit = {"micro": "stmt/s", "scenario": "tps"}
LAB = {"m1": "update 100K, 4 cores x 4 clients, group", "m2": "update 100K, 8 cores x 8 clients, group",
       "m3": "update 1M, pool 2,048 frames, 1 core x 4, group", "m4": "update 100K, 1 core x 4, group",
       "k1": "select-pk 10K, 1 core x 4", "k2": "select-pk 10K, 8 cores x 8", "k3": "update 10K, 1 core x 4",
       "k4": "insert, 4 cores x 4", "s4a": "scenario 2, 8 cores x 8 bookers, group",
       "s4b": "scenario 2, 1 core x 8 bookers, group", "m7": "update 1M, pool 2,048 frames, 4 cores x 4, group",
       "m8": "update 100K, 4 cores x 4, strict"}

print("### A against B\n")
print("| cell | A | B | B vs A | floor (larger of A's and B's spread) | outside the floor? |")
print("|---|---|---|---|---|---|")
for c, arms in cells.items():
    if "A" not in arms or "B" not in arms:
        continue
    u = unit[arms["A"][0]["kind"]]
    a = [tput(r) for r in arms["A"]]
    b = [tput(r) for r in arms["B"]]
    d = 100 * (mean(b) / mean(a) - 1)
    sa, sb = spread(a), spread(b)
    fl = max(x for x in (sa, sb) if x is not None) if (sa is not None or sb is not None) else None
    if fl is None:
        out = "single run, no floor"
    else:
        out = ("yes, B faster" if d > fl else "yes, B slower" if d < -fl else "no")
    print(f"| {c}: {LAB.get(c, '')} | {mean(a):,.0f} {u} ({'; '.join(f'{x:,.0f}' for x in a)}) | "
          f"{mean(b):,.0f} {u} ({'; '.join(f'{x:,.0f}' for x in b)}) | {d:+.1f} % | "
          f"{'-' if fl is None else f'{fl:.1f} %'} | {out} |")

print("\n### Attribution (cells with all four arms)\n")
print("| cell | A | B3 (L3 alone) | B (L3 + L2) | A0 (bound) | gap A0 vs A | gap recovered by B | by L3 (B3) | by L2 on top (B - B3) | floor |")
print("|---|---|---|---|---|---|---|---|---|---|")
for c, arms in cells.items():
    if not all(k in arms for k in ("A", "B", "B3", "A0")):
        continue
    m = {k: mean([tput(r) for r in arms[k]]) for k in arms}
    gap = m["A0"] - m["A"]
    fl = max([spread([tput(r) for r in arms[k]]) or 0 for k in arms])
    u = unit[arms["A"][0]["kind"]]
    print(f"| {c}: {LAB.get(c, '')} | {m['A']:,.0f} {u} | {m['B3']:,.0f} {u} | {m['B']:,.0f} {u} | {m['A0']:,.0f} {u} | "
          f"{100 * (m['A0'] / m['A'] - 1):+.1f} % ({gap:+,.0f} {u}) | "
          f"{100 * (m['B'] - m['A']) / gap:.0f} % of the gap | {100 * (m['B3'] - m['A']) / gap:.0f} % of the gap | "
          f"{100 * (m['B'] - m['B3']) / gap:.0f} % of the gap | {fl:.1f} % |")

print("\n### Every micro run\n")
print("| run | arm | throughput | p0 | p25 | p50 | p90 | p99 | server CPU | log written | images | image bytes | ckpt runs (counter) | ckpt runs by core | ckpt pages written | pages drained | wal_flush wait | run_queue wait | on_cpu | page_read wait | commits per sync | bad | hold / attempt |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for r in micro:
    d, k, l, w = r["delta"], r["statements"], r["lat_us"], r["waits_us"]
    imgs = d["pool_fetch_images"]
    ib = d.get("pool_fetch_image_bytes")
    if ib is None:
        ib = imgs * 8224
    gc, gb = d.get("wal_group_commits"), d.get("wal_group_batches")

    def wu(ev):
        return n(w.get(ev, [0, 0])[1] / k, "us/stmt", 1)
    print(f"| {r['id']} | {r['arm']} | {n(r['stmt_per_s'], 'stmt/s')} | {n(l['0'], 'us')} | {n(l['25'], 'us')} | {n(l['50'], 'us')} | "
          f"{n(l['90'], 'us')} | {n(l['99'], 'us')} | {n(r['cpu_us_per_stmt'], 'us/stmt', 1)} | {n(d['wal_append_lsn'] / k, 'B/stmt')} | "
          f"{n(imgs / k, 'img/stmt', 3)} | {n(ib / k, 'B/stmt')}{' (count x 8,224)' if d.get('pool_fetch_image_bytes') is None else ''} | "
          f"{n(d['contention_checkpoint_runs'], 'runs')} | {', '.join(f'c{c}:{v}' for c, v in sorted(r.get('ckpt_by_core', {}).items())) or '-'} | "
          f"{n(d['contention_checkpoint_pages_written'], 'pages')} | {n(d['pool_dirty_drained'], 'pages')} | {wu('commit.wal_flush')} | "
          f"{wu('sched.run_queue')} | {wu('cpu.on_cpu')} | {wu('io.page_read')} | {n(gc / gb, 'commits/sync', 1) if gb else '-'} | "
          f"{n(r['bad'], 'stmts')} | {n(r.get('hold_s', 0), 's')} / attempt {r.get('attempt')} |")

print("\n### Scenario 2 runs\n")
print("| run | arm | cores x bookers | throughput | booking p0 | p25 | p50 | p95 | p99 | commit p0 | p25 | p50 | p95 | p99 | log written | images | image bytes | ckpt runs (counter) | verify failures | committed | hold / attempt |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for r in scn:
    s, w = r.get("summary"), r.get("window")
    if not s or not w:
        print(f"| {r['id']} | {r['arm']} | no result |")
        continue
    ph = {p["phase"]: p for p in s["phases"]}
    b, c, m = ph["booking"], ph["commit"], s["meta"]
    k = m["outcomes"]["committed"]
    d = w["delta"]
    ib = d.get("pool_fetch_image_bytes")
    if ib is None:
        ib = d["pool_fetch_images"] * 8224
    v = m.get("verify", {})
    print(f"| {r['id']} | {r['arm']} | {r['cores']} cores x {r['clients']} bookers | {n(m['tps'], 'tps')} | {n(b['p0_us'], 'us')} | {n(b['p25_us'], 'us')} | "
          f"{n(b['p50_us'], 'us')} | {n(b['p95_us'], 'us')} | {n(b['p99_us'], 'us')} | {n(c['p0_us'], 'us')} | {n(c['p25_us'], 'us')} | "
          f"{n(c['p50_us'], 'us')} | {n(c['p95_us'], 'us')} | {n(c['p99_us'], 'us')} | {n(d['wal_append_lsn'] / k, 'B/booking')} | "
          f"{n(d['pool_fetch_images'] / k, 'img/booking', 3)} | {n(ib / k, 'B/booking')} | {n(d['contention_checkpoint_runs'], 'runs')} | "
          f"{v.get('failures')} of {v.get('checks')} checks | {n(k, 'bookings')} | {n(r.get('hold_s', 0), 's')} / attempt {r.get('attempt')} |")

print("\n### Recovery and log kept (8 cores x 8 clients, update 100K, group, 50 s then kill -9)\n")
print("| run | arm | throughput | log written in the window | images | image bytes | ckpt runs (counter) | ckpt rows by core | WAL files at the kill | WAL directory at the kill | first segment | mount (exec to first connection) | rows after | recovery records | redo applied | pages healed | recovery analysis / redo / undo | hold / attempt |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for r in rec:
    f = r["recovery_fields"]
    ib = r["image_bytes"] if r["image_bytes"] is not None else r["images"] * 8224
    print(f"| {r['id']} | {r['arm']} | {n(r['stmt_per_s'], 'stmt/s')} | {n(r['log_bytes_written'] / 1e6, 'MB', 1)} | {n(r['images'], 'images')} | "
          f"{n(ib / 1e6, 'MB', 1)} | {n(r['ckpt_runs_counter'], 'runs')} | {', '.join(f'c{c}:{v}' for c, v in sorted(r['ckpt_rows_by_core'].items()))} | "
          f"{n(r['wal_segments_at_kill'], 'files')} | {n(r['wal_dir_bytes_at_kill'] / 1e6, 'MB', 1)} | {r.get('wal_first_segment')} | "
          f"{n(r['mount_s'], 's', 2)} | {r['count_after']} rows | {f.get('recovery_records')} records | {f.get('recovery_redo_applied')} pages | "
          f"{f.get('recovery_pages_healed')} pages | {f.get('recovery_analysis_us')} / {f.get('recovery_redo_us')} / {f.get('recovery_undo_us')} us | "
          f"{n(r.get('hold_s', 0), 's')} / attempt {r.get('attempt')} |")
