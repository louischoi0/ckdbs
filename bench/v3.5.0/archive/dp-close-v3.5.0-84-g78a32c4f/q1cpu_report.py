import json, statistics as st
d = json.load(open('/home/louis/bench-runs/dp-close/archive/q1cpu/dk_close_m1.json'))
print(d['sha256'], d['started'], d['ended'])
for n, shapes in d['m1'].items():
    for s, v in shapes.items():
        c = v['cpu_us']
        A, A2, B = c['A'], c['A2'], c['B']
        m = lambda x: st.median(x)
        ba = [b - a for a, b in zip(A, B)]
        a2a = [b - a for a, b in zip(A, A2)]
        print(f"n={n} {s:9s} A {m(A):.2f} A2 {m(A2):.2f} B {m(B):.2f} us | A2-A {m(a2a):+.2f} [{min(a2a):+.1f},{max(a2a):+.1f}] "
              f"B-A {m(ba):+.2f} [{min(ba):+.1f},{max(ba):+.1f}] ({100*m(ba)/m(A):+.1f}%) B-A2 {m(B)-m(A2):+.2f} host {v['host_before']['loadavg'].split()[0]}->{v['host_after']['loadavg'].split()[0]} cc1={v['host_after']['cc1plus']}")
