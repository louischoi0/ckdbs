#!/usr/bin/env python3
"""M1b tables. usage: analyze_m1b.py <out/m1b dir> [cell ...]  (cell = m1b-c1, m1b-c1-offcpu, m1b-c4)"""
import json, statistics as st, sys

OUT = sys.argv[1]
CELLS = sys.argv[2:] or ["m1b-c1", "m1b-c1-offcpu", "m1b-c4"]


def rng(v, d=1):
    return f"{st.median(v):.{d}f} [{min(v):.{d}f} to {max(v):.{d}f}]"


for cell in CELLS:
    d = json.load(open(f"{OUT}/{cell}.json"))
    W = {w["label"]: w for w in d["windows"]}
    reps = len(d["backups"])
    print(f"\n### {cell}: ballast {d['ballast_rows']:,} rows, loaded in {d['ballast_load_s']} s; {reps} backups")

    def rate_excl(w):
        a = w["all"]
        return (w["stmts"] - a["slow_gt_1ms_n"]) / max(1e-9, w["elapsed_s"] - a["slow_gt_1ms_s"])

    def row(kind):
        ws = [W[f"{kind}-{i}"] for i in range(reps)]
        return ws
    print("| window | n | duration (s) | statements/s, raw | statements/s excluding statements over 1 ms | share of the window inside statements over 1 ms | p50 | p99 | p99.9 | WAL bytes per statement |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for kind, title in (("base", "baseline (no backup)"), ("copy", "BACKUP COPY running"), ("stop", "BACKUP STOP finishing (data sync, log copy, manifest)")):
        ws = row(kind)
        print(f"| {title} | {len(ws)} windows | {rng([w['elapsed_s'] for w in ws])} s | {rng([w['stmt_per_s'] for w in ws], 0)} /s | "
              f"{rng([rate_excl(w) for w in ws], 0)} /s | {rng([100 * w['all']['slow_gt_1ms_s'] / w['elapsed_s'] for w in ws], 0)} % | "
              f"{rng([w['all']['p50_us'] for w in ws])} µs | {rng([w['all']['p99_us'] for w in ws])} µs | {rng([w['all']['p999_us'] for w in ws])} µs | "
              f"{rng([w['wal_bytes_per_stmt'] for w in ws], 0)} B |")
    # paired
    dp50 = [W[f"copy-{i}"]["all"]["p50_us"] - W[f"base-{i}"]["all"]["p50_us"] for i in range(reps)]
    dp99 = [W[f"copy-{i}"]["all"]["p99_us"] - W[f"base-{i}"]["all"]["p99_us"] for i in range(reps)]
    drate = [rate_excl(W[f"copy-{i}"]) - rate_excl(W[f"base-{i}"]) for i in range(reps)]
    drel = [100 * (rate_excl(W[f"copy-{i}"]) / rate_excl(W[f"base-{i}"]) - 1) for i in range(reps)]
    print(f"\n{reps} backups, each copy window minus the baseline window just before it: p50 {rng(dp50)} µs; p99 {rng(dp99)} µs; "
          f"service rate excluding statements over 1 ms {rng(drate, 0)} /s, {rng(drel)} %.")
    print("\n| statement kind | baseline p50 | copy p50 | baseline p99 | copy p99 |")
    print("|---|---|---|---|---|")
    for k in ("ins-hot", "upd-hot", "upd-ballast", "sel-hot"):
        b = [W[f"base-{i}"]["kinds"][k] for i in range(reps)]
        c = [W[f"copy-{i}"]["kinds"][k] for i in range(reps)]
        print(f"| {k} | {rng([x['p50_us'] for x in b])} µs | {rng([x['p50_us'] for x in c])} µs | {rng([x['p99_us'] for x in b])} µs | {rng([x['p99_us'] for x in c])} µs |")
    cw = [W[f"copy-{i}"] for i in range(reps)]
    sw = [W[f"stop-{i}"] for i in range(reps)]
    print(f"\nThe copy: {rng([w['copy_s'] for w in cw], 2)} s at {rng([w['copy_mib_s'] for w in cw], 1)} MiB/s over {rng([w['data_pages'] for w in cw], 0)} pages. "
          f"STOP's finish: {rng([w['stop_finish_s'] for w in sw], 2)} s. Writes that waited for the fence, in total at the end: {d['backups'][-1]['fence_writes_waited_total']:.0f} writes. "
          f"Log copied per backup: {', '.join(str(int(b['log_bytes_copied']) // (1 << 20)) for b in d['backups'])} MiB. "
          f"Backup size: {', '.join(str(b['du_bytes'] // (1 << 20)) for b in d['backups'])} MiB. "
          f"Log span of the window (stop LSN - start LSN): {', '.join(str((int(b['stop_lsn']) - int(b['start_lsn'])) // (1 << 20)) for b in d['backups'])} MiB.")
