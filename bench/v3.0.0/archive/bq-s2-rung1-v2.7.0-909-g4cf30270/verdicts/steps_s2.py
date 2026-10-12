"""Per arm: step shares of summed hold, all rolls and rolls >= hold p99, relaxed C1+C2+C3; per-run held seconds."""
import glob, json, math, re, sys
from fractions import Fraction
OUT = sys.argv[1]
ROLL = re.compile(r'\[wal\] roll (.*)$')
def kv(t):
    o = {}
    for tok in t.split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            try: o[k] = int(v)
            except ValueError: o[k] = v
    return o
ST = ['seal_us','flush_us','sync_gate_us','sync_us','body_wait_us','body_us','header_us','rename_us','dirsync_us']
for arm in ('base','opt3','s16','s256','s1024'):
    pool = []
    for js in glob.glob(f'{OUT}/relaxed/c*/{arm}/r*/*.json'):
        if '-iso' in js: continue
        j = json.load(open(js)); m0, m1 = j['m0_mono_ns'], j['m1_mono_ns']
        pool += [r for r in (kv(m.group(1)) for m in map(ROLL.search, open(js[:-5] + '.s.log')) if m) if m0 <= r['start_ns'] <= m1]
    H = sorted(r['hold_us'] for r in pool)
    p99 = H[max(1, math.ceil(Fraction(99) * len(H) / 100)) - 1]
    top = [r for r in pool if r['hold_us'] >= p99]
    def sh(rs):
        t = sum(r['hold_us'] for r in rs)
        return ' '.join(f"{s[:-3]}={100*sum(r[s] for r in rs)/t:.1f}" for s in ST)
    bodies = {}
    for r in pool: bodies[r['body']] = bodies.get(r['body'], 0) + 1
    print(f'{arm:6} rolls={len(pool)} p99={p99/1e3:.0f}ms bodies={bodies}\n   all: {sh(pool)}\n   >=p99 ({len(top)}): {sh(top)}')
