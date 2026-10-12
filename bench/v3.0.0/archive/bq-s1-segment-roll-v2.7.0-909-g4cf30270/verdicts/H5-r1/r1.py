import json, glob, os, re, math, sys
from fractions import Fraction
BASE = '/home/cdkbs/bench-runs/bq/out-4cf30270/bq'
ROLL = re.compile(r'\[wal\] roll seg=(\d+) core=(\d+) start_ns=(\d+) end_ns=(\d+) hold_us=(\d+)')

def nr(xs, p):
    if not xs: return 0
    r = max(1, math.ceil(Fraction(str(p)) * len(xs) / 100)) - 1
    return xs[min(r, len(xs)-1)]

def delta(m0, m1, k):
    a = m0.get(k) or 0.0; b = m1.get(k) or 0.0
    return b - a

def load(run):
    js = glob.glob(run + '/bl-mixed-*.json'); lg = glob.glob(run + '/*.s.log')
    d = json.load(open(js[0]))
    rolls = []
    for line in open(lg[0], errors='replace'):
        m = ROLL.search(line)
        if m:
            rolls.append(dict(seg=int(m[1]), core=int(m[2]), s=int(m[3]), e=int(m[4]), hold=int(m[5])))
    return d, rolls

def pctile_from_slow(n, slow_lats, p):
    # fast samples (<1ms) unknown; treat as 0 placeholder
    nfast = n - len(slow_lats)
    xs = [0]*nfast + sorted(slow_lats)
    return nr(xs, p)

def analyze(cellpath, role):
    out = []
    for run in sorted(glob.glob(cellpath + '/r*')):
        d, rolls = load(run)
        m0, m1 = d['meta_m0'], d['meta_m1']
        w0, w1 = d['m0_mono_ns'], d['m1_mono_ns']
        win = [r for r in rolls if r['e'] > w0 and r['s'] < w1]
        holdwin = sum(min(r['e'], w1) - max(r['s'], w0) for r in win) / 1e6
        rollers = {}
        for r in win: rollers[r['core']] = rollers.get(r['core'], 0) + 1
        percore = {}
        for k in range(4):
            percore[k] = dict(
                stream=delta(m0, m1, f'load_c{k}_waited_wal_stream_us')/1e3,
                drain=delta(m0, m1, f'load_c{k}_wal_drain_latch_wait_us')/1e3,
                rollcov=delta(m0, m1, f'load_c{k}_wal_roll_blocked_wait_us')/1e3,
                gate=delta(m0, m1, f'load_c{k}_waited_wal_sync_gate_us')/1e3)
        tot_drain = delta(m0, m1, 'contention_wal_drain_latch_wait_us')/1e3
        # stalls
        cl = [c for c in d['clients'] if c['role'] == role]
        n = sum(c['ok'] + c['errors'] + c['refusals'] for c in cl)
        slow = [s for c in cl for s in c['slow']]
        dropped = sum(c['slow_dropped'] for c in cl)
        def aligned(s):
            a, b = s[0], s[0] + s[1]
            for r in win:
                if a < r['e'] and b > r['s'] and r['e'] - 5e6 <= b <= r['e'] + 30e6:
                    return True
            return False
        def overlaps(s):
            a, b = s[0], s[0] + s[1]
            return any(a < r['e'] and b > r['s'] for r in win)
        big = [s for s in slow if s[1] >= 10e6]
        al = [s for s in big if aligned(s)]
        ov = [s for s in big if overlaps(s)]
        share = sum(s[1] for s in al) / max(1, sum(s[1] for s in big))
        lats = [s[1] for s in slow]
        p99 = pctile_from_slow(n, lats, 99)/1e6; p999 = pctile_from_slow(n, lats, 99.9)/1e6
        alset = set(id(s) for s in slow if aligned(s))
        lats2 = [s[1] for s in slow if id(s) not in alset]
        p99b = pctile_from_slow(n, lats2, 99)/1e6; p999b = pctile_from_slow(n, lats2, 99.9)/1e6
        key = 'read_from_schedule' if role == 'reader' else 'writer_from_schedule'
        jp99 = d[key].get('p99_us', 0)/1e3; jp999 = d[key].get('p99.9_us', 0)/1e3
        rollwall = holdwin / ((w1 - w0)/1e6)
        out.append(dict(run=os.path.basename(run), nrolls=len(win), holdwin=holdwin, rollers=rollers, percore=percore,
                        tot_drain=tot_drain, n=n, nslow=len(slow), dropped=dropped, nbig=len(big), nal=len(al), nov=len(ov),
                        share=share, p99=p99, jp99=jp99, p999=p999, jp999=jp999, p99b=p99b, p999b=p999b,
                        rollwall=rollwall, ctl=d.get('meta_ctl_moved'), cores=(d.get('meta_core_m0'), d.get('meta_core_m1')),
                        holds=sorted(r['hold'] for r in win)))
    return out

for dur in ('relaxed', 'group'):
    for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2', 'c3', 'c1-iso-segregated-no0', 'c1-iso-batch0', 'c1-iso-spread', 'c2-iso'):
        role = 'writer' if cell.startswith('c2') else 'reader'
        res = analyze(f'{BASE}/{dur}/{cell}', role)
        for r in res:
            pc = r['percore']
            pcs = ' '.join(f"c{k}:{pc[k]['drain']:.1f}/{pc[k]['stream']:.1f}/{pc[k]['rollcov']:.1f}/g{pc[k]['gate']:.0f}" for k in range(4))
            h = r['holds']
            hs = f"holdp50={h[len(h)//2]/1e3:.1f} max={h[-1]/1e3:.1f}" if h else ''
            print(f"{dur} {cell} {r['run']} rolls={r['nrolls']} holdwin={r['holdwin']:.0f}ms rollers={r['rollers']} rollwall={r['rollwall']*100:.0f}% {hs} ctl={r['ctl']} cores={r['cores']}")
            print(f"    drain/stream/rollcov ms: {pcs} totdrain={r['tot_drain']:.1f}")
            print(f"    n={r['n']} slow={r['nslow']} drop={r['dropped']} big={r['nbig']} ov={r['nov']} aligned={r['nal']} share={r['share']*100:.0f}% p99={r['p99']:.3f}(json {r['jp99']:.3f}) p999={r['p999']:.3f}(json {r['jp999']:.3f}) -> noaligned p99={r['p99b']:.3f} p999={r['p999b']:.3f}")
