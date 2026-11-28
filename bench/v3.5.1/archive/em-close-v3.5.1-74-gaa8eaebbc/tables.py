import json, statistics as st
D = [json.load(open(f'/home/ukjin/em-close-run/{n}.json')) for n in ('em-close', 'em-close-rep')]
names = {"base/point/text": "point `SELECT`, text", "base/point/prepared": "point `SELECT`, prepared",
         "base/update/text": "point `UPDATE`, text", "base/update/prepared": "point `UPDATE`, prepared",
         "base/insert/text": "point `INSERT`, text", "base/delete/text": "point `DELETE`, text",
         "base/join3/text": "three-relation join, text", "base/join3/prepared": "three-relation join, prepared",
         "base/qlayer/text": "query-layer `SELECT v * 2`, text", "base/nsq/text": "**`ns1.t2`, text**",
         "base/nsu/text": "`t2` under `USE ns1`, text", "view/nsq/text": "**`ns1.t2`, one view, text**",
         "plan/join3/text": "**join under a two-variant plan, text**", "plan/join3/prepared": "join under a two-variant plan, prepared"}
def f(x, a):
    return f"{x-a:+.3f} ({(x-a)/a*100:+.1f} %)"
def table(runs):
    print("| cell | A median | A range | A2 vs A (floor) | B vs A | B range |")
    print("|---|---|---|---|---|---|")
    for k, n in names.items():
        v = {a: [r['rounds'][a][i][k]['cpu_us'] for r in runs for i in range(9)] for a in ('A', 'A2', 'B')}
        m = {a: st.median(v[a]) for a in v}
        print(f"| {n} | {m['A']:.3f} | {min(v['A']):.3f} - {max(v['A']):.3f} | {f(m['A2'], m['A'])} | {f(m['B'], m['A'])} | {min(v['B']):.3f} - {max(v['B']):.3f} |")
for i in (0, 1):
    print(f"run {i+1}"); table([D[i]]); print()
print("pooled"); table(D)
