import json, sys, statistics as st
from collections import defaultdict
s0 = json.load(open(sys.argv[1]))['cells']
q2 = json.load(open(sys.argv[2]))['cells']
def agg(cells):
    d = defaultdict(list)
    for x in cells:
        d[(x['cores'], x['dur'], x['conns'], x['k'], 'A' if x['arm'] in ('A', 'A2') else x['arm'])].append(x['gps'])
    return {k: st.mean(v) for k, v in d.items()}
a0, a1 = agg(s0), agg(q2)
print("| cores | dur | conns | k | S0 W | now W (B) | S0 A | now A (B) | S0 (W-A)/A | now (W-A)/A |")
print("|---|---|---|---|---|---|---|---|---|---|")
for k in sorted({k[:4] for k in a1}, key=lambda k: (k[0], ["strict", "relaxed"].index(k[1]), k[2], k[3])):
    w0, aa0, w1, aa1 = a0[k + ('W',)], a0[k + ('A',)], a1[k + ('W',)], a1[k + ('A',)]
    print(f"| {k[0]} | {k[1]} | {k[2]} | {k[3]} | {w0:.0f} | {w1:.0f} | {aa0:.0f} | {aa1:.0f} | {(w0-aa0)/aa0*100:+.0f}% | {(w1-aa1)/aa1*100:+.0f}% |")
