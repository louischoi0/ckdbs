#!/usr/bin/env python3
"""Per mixed run: the WAL rolls in the window, their latched hold, each core's
roll-blocked wait, where every session ended, and the log rate."""
import glob, json, os

HERE = os.path.dirname(os.path.abspath(__file__))
print('| placement | run | rolls | roll hold | hold max | sync-gate wait | roll-blocked wait c0 / c1 / c2 / c3 | session cores at end (6 readers, walker, inserter) | log MB/s | reader schedule p99 |')
print('|---|---|---|---|---|---|---|---|---|---|')
for pl in ('segregated-no0', 'spread', 'batch0'):
    for r in (1, 2, 3):
        d = json.load(open(glob.glob(f'{HERE}/r{r}/*-{pl}-mixed-*.json')[0]))
        md = d['meta_delta']
        rb = ' / '.join(f'{md.get(f"load_c{k}_wal_roll_blocked_wait_us", 0) / 1e6:.2f} s' for k in range(4))
        end = ', '.join(str(int(c)) for c in d['cores_end'])
        print(f'| {pl} | r{r} | {int(md["wal_rolls"])} | {md["wal_roll_hold_us"] / 1e6:.2f} s | '
              f'{md["wal_roll_hold_max_us"] / 1e3:.0f} ms | {md["wal_roll_sync_gate_us"] / 1e6:.2f} s | {rb} | {end} | '
              f'{d["log_bytes_per_s"] / 1e6:.0f} | {d["read_from_schedule"]["p99_us"] / 1e3:.1f} ms |')
