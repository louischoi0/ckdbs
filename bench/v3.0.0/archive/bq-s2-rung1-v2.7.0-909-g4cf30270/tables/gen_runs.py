import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *
import re as _re

OUT = []


def P(s=''):
    OUT.append(s)


def nonbulk(j, rollcores):
    role = 'reader' if j['readers'] else 'writer'
    cs = [c for c in j['clients'] if c['role'] == role and c['core'] not in rollcores]
    n = sum(c['ok'] + c['errors'] + c['refusals'] for c in cs)
    slow = sorted(s[1] / 1000 for c in cs for s in c['slow'])
    drop = sum(c['slow_dropped'] for c in cs)
    res = {'n': n, 'cores': sorted({int(c['core']) for c in cs}), 'drop': drop}
    for q in (99, 99.9):
        k = max(1, math.ceil(Fraction(str(q)) * n / 100))
        fast = n - len(slow)
        res[q] = None if (drop or n == 0) else (slow[k - fast - 1] if k > fast else 0.0)
    return res


def short_comp(lst):
    if not lst:
        return 'none'
    ids = []
    for l in lst:
        pid = l.split()[0]
        txt = l
        if 'br-backup-restore' in txt:
            lab = 'another session: cmake --build + ctest -j8 (br-backup-restore)'
        elif 'cmake --build' in txt and 'simsweep' in txt:
            lab = 'another session: cmake --build + sim sweep'
        elif 'cmake --build' in txt and 'ctest' in txt:
            lab = 'another session: cmake --build + ctest -j8 (br-backup-restore)'
        elif 'cmake --build' in txt and 'kds_tests' in txt:
            lab = 'another session: cmake --build kds_tests + gtest'
        else:
            lab = 'other'
        ids.append((pid, lab))
    labs = sorted({l for _, l in ids})
    pids = sorted({p for p, _ in ids})
    return '; '.join(labs) + ' (pids ' + ', '.join(pids) + ')'


# --- A1 per-run schedule percentiles
P('**Appendix A1. Every run: schedule latency, relaxed**')
P()
P('| cell | arm | run | n | ' + ' | '.join(PCT_NAMES) + ' | errors / refusals | slow samples dropped (reader or writer clients) |')
P('|---|---|---|---|' + '---|' * 7 + '---|---|')
for cell in CELLS:
    k = 'read_from_schedule' if cell != 'c2' else 'writer_from_schedule'
    ic = ISO[cell]
    rows = [(f'{ic} (BQ-S2)', load(S2, 'relaxed', ic, 'base', 1), 'r1')]
    rows += [(f'{ic} (BQ-S1)', load(S1, 'relaxed', ic, None, r), f'r{r}') for r in (1, 2, 3)]
    for arm in ARMS:
        for r in (1, 2, 3):
            rows.append((arm, load(S2, 'relaxed', cell, arm, r), f'r{r}'))
    for lab, j, rn in rows:
        role = 'reader' if cell != 'c2' else 'writer'
        dr = sum(c['slow_dropped'] for c in j['clients'] if c['role'] == role)
        a = j[k]
        P(f'| {cell} | {lab} | {rn} | {a["n"]:,} | ' + ' | '.join(us(a[p]) for p in PCT) +
          f' | {j["errors"]} / {j["refusals"]} | {dr} |')
P()

# --- A2 host per run
P('**Appendix A2. Host state at every relaxed cell: load average and competing jobs (the driver\'s `host_before` / `host_after`), JSON written time (UTC), balancer moves**')
P()
P('The load average is `/proc/loadavg` as the driver read it before the server started and after the window (1 / 5 / 15 minute). '
  '"Competing" is the driver\'s `pgrep` list (cc1plus, cmake --build, ctest, kds_tests and the like).')
P()
P('| cell | arm | run | JSON written (UTC) | load before (1 / 5 / 15 min) | load after | competing jobs before | competing jobs after | balancer moves in window | rolls in window | bulk core(s) (cores of the window\'s rolls) | non-bulk p99 | non-bulk p99.9 | mount checkpoint of the fresh file (device thermometer) |')
P('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
NB = {}
for cell in CELLS + ['c3']:
    names = []
    if cell != 'c3':
        names.append((ISO[cell], 'base', 1, None))
    for arm in ARMS:
        for r in (1, 2, 3):
            names.append((cell, arm, r, None))
    for c, arm, r, _ in names:
        j = load(S2, 'relaxed', c, arm, r)
        w = inwin(j)
        rc = sorted({x['core'] for x in w})
        lb = ' / '.join(j['host_before']['loadavg'].split()[:3])
        la = ' / '.join(j['host_after']['loadavg'].split()[:3])
        if c != cell or cell == 'c3':
            nbs = ('n/a', 'n/a')
        else:
            nb = nonbulk(j, rc)
            NB[(cell, arm, r)] = nb
            f = lambda x: 'n/a (drops)' if x is None else ('< 1 ms' if x == 0.0 else us(x))
            nbs = (f(nb[99]), f(nb[99.9]))
        P(f'| {c} | {arm} | r{r} | {hhmm(j["_mtime"])} | {lb} | {la} | {short_comp(j["host_before"]["competing"])} | '
          f'{short_comp(j["host_after"]["competing"])} | {j.get("moves_in_window", "n/a")} | {len(w)} | {rc if rc else "-"} | {nbs[0]} | {nbs[1]} | {us(j["meta_m0"].get("recovery_checkpoint_us"))} |')
P()
print('\n'.join(OUT))

# --- non-bulk summary to stderr for the main text
import json as _json
json_out = {f'{k[0]}|{k[1]}|{k[2]}': [v[99], v[99.9], v['n'], v['cores'], v['drop']] for k, v in NB.items()}
open('/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bp-per-core-residue/93896f60-b552-41c7-9a00-cc438c9ec661/scratchpad/nb.json', 'w').write(_json.dumps(json_out))
