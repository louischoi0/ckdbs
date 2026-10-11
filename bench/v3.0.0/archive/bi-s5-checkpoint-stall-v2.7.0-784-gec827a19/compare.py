"""BI-S5: A (BI-S1's cells) against B (the close's), per shape, three runs
each, with BI-Q10's readings."""
import json
import math
import sys

sys.path.insert(0, '/home/cdkbs/ckdbs/.claude/worktrees/bi-checkpoint-off-the-reactor/tools')
import bi_checkpoint_stall as b  # noqa: E402

A_DIR, B_DIR = sys.argv[1], sys.argv[2]


def pct(xs, p):
    xs = sorted(xs)
    return xs[max(0, math.ceil(round(p * len(xs) / 100, 9)) - 1)] if xs else None


def f(v, d=1):
    return '-' if v is None else f'{v:.{d}f}'


def cell_stats(path, is_b):
    c = json.load(open(path))
    lo = c['window'][0]
    hi = c.get('load_stop', c['window'][1])
    runs = [r for r in c['runs'] if r['end'] >= lo and r['start'] <= hi]
    out = {'runs': len(runs)}
    walls = [r['wall_us'] / 1000 for r in runs]
    out['run_p50'] = pct(walls, 50)
    out['run_max'] = max(walls, default=None)
    # The checkpoint's own holds. A: every run is one hold, its wall time.
    # B: the system group's polls, and the run record's single-call maxima.
    if is_b:
        sys_long = max(((c['meta'][k] or {}).get('sched_system_longest_poll_us', 0) for k in c['meta']),
                       default=0) / 1000
        over10 = sum(((c['meta'][k] or {}).get('sched_system_polls_over_10ms') or 0) for k in c['meta'])
        over100 = sum(((c['meta'][k] or {}).get('sched_system_polls_over_100ms') or 0) for k in c['meta'])
    else:
        sys_long = out['run_max']
        over10 = sum(1 for w in walls if w > 10)
        over100 = sum(1 for w in walls if w > 100)
    out['own_longest'] = sys_long
    out['own_over10'] = over10
    out['own_over100'] = over100
    # The probe inside/outside runs, per core, pooled; ratio of p99s.
    sin, sout = [], []
    for core in c['meta']:
        spans = [(r['start'], r['end']) for r in runs if r['core'] == int(core)]
        for t, ms in c['samples'].get(f'stmt:{core}', []):
            if ms < 0 or t > hi:
                continue
            (sin if any(s <= t <= e for s, e in spans) else sout).append(ms)
    out['stmt_in_p50'], out['stmt_out_p50'] = pct(sin, 50), pct(sout, 50)
    out['stmt_in_p99'], out['stmt_out_p99'] = pct(sin, 99), pct(sout, 99)
    rows, _ = b.summarize_cell(c)
    out['hold_in'] = max(r['hold_in_run_longest_ms'] for r in rows)
    out['hold_out'] = max(r['hold_out_run_longest_ms'] for r in rows)
    out['gap'] = max((r['anchor_gap_s_max'] or 0) for r in rows)
    # Idle-core runs (B and the A-idle cells): runs starting after the load.
    idle = [r['wall_us'] / 1000 for r in c['runs'] if 'load_stop' in c and r['start'] >= c['load_stop']]
    out['idle_runs'] = idle
    # Core 0's loss window: the conservative interval, from the last reading
    # before a move to the reading that saw the next move, inside its runs.
    samples = c.get('core0_interval_syncs', [])
    spans0 = [(r['start'], r['end']) for r in runs if r['core'] == 0]
    worst = None
    last_move_prev = None
    for i in range(1, len(samples)):
        t, n = samples[i]
        tp, np_ = samples[i - 1]
        if n != np_ and n >= 0:
            if last_move_prev is not None and any(s <= t and last_move_prev <= e for s, e in spans0):
                gap = (t - last_move_prev) * 1000
                worst = gap if worst is None or gap > worst else worst
            last_move_prev = tp
    out['loss_window_ms'] = worst
    return out


shapes = ['c1-relaxed', 'c4-relaxed', 'c1-group', 'c4-group']
print("| shape | run | runs A / B | run p50 A / B | the checkpoint's longest own hold A / B | its holds > 10 ms A / B | > 100 ms A / B | longest iteration in a run A / B | statement probe p50 in a run A / B | p99 in a run A / B | p99 out of a run A / B | worst anchor gap A / B |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
for sh in shapes:
    for r in (1, 2, 3):
        a = cell_stats(f'{A_DIR}/{sh}-r{r}.json', False)
        bb = cell_stats(f'{B_DIR}/B-{sh}-r{r}.json', True)
        print(f"| {sh} | r{r} | {a['runs']} / {bb['runs']} runs | {f(a['run_p50'],0)} ms / {f(bb['run_p50'],0)} ms | "
              f"{f(a['own_longest'],0)} ms / {f(bb['own_longest'],0)} ms | {a['own_over10']} / {bb['own_over10']} | "
              f"{a['own_over100']} / {bb['own_over100']} | {f(a['hold_in'],0)} ms / {f(bb['hold_in'],0)} ms | "
              f"{f(a['stmt_in_p50'],1)} ms / {f(bb['stmt_in_p50'],1)} ms | {f(a['stmt_in_p99'],0)} ms / {f(bb['stmt_in_p99'],0)} ms | "
              f"{f(a['stmt_out_p99'],0)} ms / {f(bb['stmt_out_p99'],0)} ms | {a['gap']:.1f} s / {bb['gap']:.1f} s |")
print()
print("| B cell | idle-core runs | idle run wall p50 / max | core 0's loss window, longest, in its runs |")
print("|---|---|---|---|")
for sh in shapes:
    for r in (1, 2, 3):
        bb = cell_stats(f'{B_DIR}/B-{sh}-r{r}.json', True)
        print(f"| B-{sh}-r{r} | {len(bb['idle_runs'])} runs | {f(pct(bb['idle_runs'],50),0)} ms / {f(max(bb['idle_runs'], default=None),0)} ms | {f(bb['loss_window_ms'],0)} ms |")
for tag in ('A-c4-relaxed-idle', 'A-c4-group-idle'):
    try:
        aa = cell_stats(f'{B_DIR}/{tag}.json', False)
        print(f"| {tag} | {len(aa['idle_runs'])} runs | {f(pct(aa['idle_runs'],50),0)} ms / {f(max(aa['idle_runs'], default=None),0)} ms | {f(aa['loss_window_ms'],0)} ms |")
    except FileNotFoundError:
        print(f"| {tag} | not yet |")
