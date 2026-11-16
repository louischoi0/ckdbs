import json,sys,statistics as st
r=json.load(open(sys.argv[1]))
from collections import defaultdict
d=defaultdict(lambda: defaultdict(list))
for x in r: d[(x['shape'],x['rows'])][x['bin']].append(x)
print("| shape | rows | arm | ops/run | runs | server CPU us/unit (mean of run means) | min..max | client mean us | p0 | p25 | p50 | p75 | p99 | bad |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
summ={}
for (sh,n),arms in sorted(d.items(), key=lambda kv:(["select-pk","update-pk","txn4-rw"].index(kv[0][0]),kv[0][1])):
    for lab in ("A","B","A2"):
        xs=arms[lab]
        cpu=[x['server_cpu_us'] for x in xs]; mean=[x['mean_us'] for x in xs]
        f=lambda k: st.mean(x[k] for x in xs)
        print(f"| {sh} | {n} | {lab} | {xs[0]['ops']} | {len(xs)} | {st.mean(cpu):.2f} | {min(cpu):.2f}..{max(cpu):.2f} | {st.mean(mean):.1f} | {f('p0_us'):.1f} | {f('p25_us'):.1f} | {f('p50_us'):.1f} | {f('p75_us'):.1f} | {f('p99_us'):.1f} | {sum(x['bad'] for x in xs)} |")
        summ[(sh,n,lab)]=(st.mean(cpu),st.mean(mean),cpu,mean)
print()
print("| shape | rows | B-A cpu us | A2-A cpu us | B-A client mean us | A2-A client mean us | paired B-A cpu (per-run) mean +- sd | paired A2-A cpu mean +- sd |")
print("|---|---|---|---|---|---|---|---|")
for (sh,n),arms in sorted(d.items(), key=lambda kv:(["select-pk","update-pk","txn4-rw"].index(kv[0][0]),kv[0][1])):
    g=lambda l,i: summ[(sh,n,l)][i]
    ba=[b-a for a,b in zip(summ[(sh,n,'A')][2],summ[(sh,n,'B')][2])]
    a2=[b-a for a,b in zip(summ[(sh,n,'A')][2],summ[(sh,n,'A2')][2])]
    print(f"| {sh} | {n} | {g('B',0)-g('A',0):+.2f} | {g('A2',0)-g('A',0):+.2f} | {g('B',1)-g('A',1):+.1f} | {g('A2',1)-g('A',1):+.1f} | {st.mean(ba):+.2f} +- {st.stdev(ba):.2f} | {st.mean(a2):+.2f} +- {st.stdev(a2):.2f} |")
