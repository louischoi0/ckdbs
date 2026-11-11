import json, glob, os
SHA = {'d78b1147': 'base', '049f27e6': 'close'}
def f(x): return f"{x:,.1f}"
for d in sorted(glob.glob('m5/out-*')):
    r = open(d + '/run.txt').read()
    sha = [l.split('=')[1] for l in r.splitlines() if l.startswith('binary_sha256')][0][:8]
    lab = SHA[sha]
    print(f"\n#### {os.path.basename(d)} binary {lab}")
    for c in ('kwp-w4', 'kwp-w1', 'text-w4', 'text-w1'):
        try:
            j = json.load(open(f"{d}/m5-{c}.json"))
        except Exception:
            print(c, 'missing'); continue
        ph = {p['phase']: p for p in j['phases']}
        m = j['meta']
        bt = m['block_table']
        spread = {}
        for arm in ('update-fenced', 'update-unfenced'):
            v = [b['latency']['p50_us'] for b in bt if b['arm'] == arm]
            spread[arm] = max(v) - min(v)
        for arm in ('update-fenced', 'update-unfenced'):
            p = ph[arm]
            print(f"| {c} | {arm} | {lab} | {p['ops']:,} | {f(p['p0_us'])} | {f(p['p25_us'])} | {f(p['p50_us'])} | {f(p['p95_us'])} | {f(p['p99_us'])} | {f(p['mean_us'])} | {p['qps']:,.0f} QPS | {p['errors']} |")
        dp50 = ph['update-fenced']['p50_us'] - ph['update-unfenced']['p50_us']
        dmean = ph['update-fenced']['mean_us'] - ph['update-unfenced']['mean_us']
        walks = [b['walks_per_statement'] for b in bt]
        print(f"   {c}: dp50 {dp50:+.1f} dmean {dmean:+.1f} spread f/u {spread['update-fenced']:.1f}/{spread['update-unfenced']:.1f}; walks {m['census_end']['contention_fence_walks']} part_waits {m['census_end']['contention_lock_partition_waits']}; lock_waits_rows {len(m['lock_waits_rows'])}; walkset {sorted(set(walks))}")
