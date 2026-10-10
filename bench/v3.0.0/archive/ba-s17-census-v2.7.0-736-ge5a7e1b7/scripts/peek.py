import json, os, sys
S = [json.loads(l) for l in open('/home/cdkbs/bench-runs/ba-close/host-samples.jsonl')]
for fn in sys.argv[1:]:
    m = os.path.getmtime('/home/cdkbs/bench-runs/ba-s17/kds/' + fn)
    w = [s for s in S if m - 30 <= s['t'] <= m]
    print(fn, len(w))
    for s in w[::5]:
        print("  ", s['load'], [f[:90] for f in s['foreign']][:2], {c: s['busy'][c] for c in ('1', '3')})
