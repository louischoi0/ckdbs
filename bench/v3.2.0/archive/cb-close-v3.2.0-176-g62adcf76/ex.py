import json, statistics as st
D = "/tmp/claude-1000/-home-cdkbs-ckdbs/00ec4c4c-8445-418e-92d3-c235d2b282e7/scratchpad/cbclose/"
for f in ('out2.json', 'out3.json'):
    o = json.load(open(D + f))
    ld = lambda x: float(x['host']['loadavg'].split()[0])
    print(f, 'max load rounds>1', max(ld(x) for x in o if x['round'] > 1), 'round1 max', max(ld(x) for x in o if x['round'] == 1))
    for c in dict.fromkeys(x['cell'] for x in o):
        for e in ('A', 'B', 'C'):
            r = [x for x in o if x['cell'] == c and x['engine'] == e]
            m = st.median(x['mean_us'] for x in r); cp = st.median(x['server_cpu_us'] for x in r)
            print(c, e, f"{m:.1f} {cp:.1f} {m-cp:.1f}")
