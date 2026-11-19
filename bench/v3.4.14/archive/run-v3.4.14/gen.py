import json, statistics, os, sys, math
RUN="/home/louis/bench-runs/scenarios-v3.4.14"
WT="/home/louis/ckdbs/.claude/worktrees/scenarios-v3-4-14"
PA=WT+"/bench/v3.3.0/archive"
PD="v3.3.0-38-ga40882af"; PG="v3.3.0-33-g9682b725"
FR=RUN+"/frag"
os.makedirs(FR,exist_ok=True)
PFX=["","r2-","r3-"]
def jl(p):
    try: return json.load(open(p))
    except Exception: return None
def N(tag): return [d for d in (jl(f"{RUN}/json/{p}{tag}.json") for p in PFX) if d]
def P(n,tag): return [d for d in (jl(f"{PA}/scenario{n}-{PD}/json/{p}{tag}.json") for p in PFX) if d]
def G(n,tag): return [d for d in (jl(f"{PA}/scenario{n}-{PG}/json/{p}{tag}.json") for p in PFX) if d]
def med(x):
    x=[v for v in x if v is not None]; return statistics.median(x) if x else None
def spread(x):
    x=[v for v in x if v is not None]
    if len(x)<2: return None
    m=statistics.median(x); return 100*(max(x)-min(x))/m if m else None
def f1(x,u=""): return "n/a" if x is None else f"{x:,.1f}{u}"
def f0(x,u=""): return "n/a" if x is None else f"{x:,.0f}{u}"
def sp(x): return "n/a" if x is None else f"{x:.1f} %"
def ratio(a,b): return "n/a" if not a or not b else f"{a/b:.2f}x"
def delta(a,b): return "n/a" if not a or not b else f"{100*(a-b)/b:+.1f} %"
def read(a,b,sa,sb):
    if not a or not b: return "n/a"
    d=100*(a-b)/b; lim=max(sa or 0,sb or 0)
    if sa is None and sb is None: return "single run, no claim"
    if lim>20: return "spread > 20 %, no claim"
    return "**finding**" if abs(d)>lim else "inside the spread"
def ph(d,n):
    for p in d["phases"]:
        if p["phase"]==n: return p
def pv(rr,n,k="qps"): return [ph(d,n)[k] for d in rr if ph(d,n)]
def sweep(d): return {s["shape"]:s for s in d["meta"]["qps_sweep"]}
def sv(rr,sh,key):
    o=[]
    for d in rr:
        s=sweep(d).get(sh)
        if s and s.get(key): o.append(s[key]["qps"])
    return o
def pct_table(d):
    s="| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |\n|---|---|---|---|---|---|---|---|---|\n"
    for p in d["phases"]:
        s+=f"| {p['phase']} | {p['ops']:,} ops | {p['p0_us']:,.1f} µs | {p['p25_us']:,.1f} µs | {p['p50_us']:,.1f} µs | {p['p95_us']:,.1f} µs | {p['p99_us']:,.1f} µs | {p['max_us']:,.1f} µs | {p['errors']} errors |\n"
    return s
def put(n,t): open(f"{FR}/{n}.md","w").write(t)

S1=["bar-lookup","bar-range","day-slice","symbol-history","cross-join","point-join","model-join"]
def s1():
    for cell in ("c1-g","c8-g","c1-s"):
        R,O=N("s1-"+cell),P(1,"s1-"+cell)
        s="| shape | cold now | warm now | spread | warm v3.3.0-38 | spread | warm delta | read | cabin now | cabin v3.3.0-38 | cabin delta |\n|---|---|---|---|---|---|---|---|---|---|---|\n"
        for sh in S1:
            w,ow,c,oc=sv(R,sh,"warm"),sv(O,sh,"warm"),sv(R,sh,"cabin"),sv(O,sh,"cabin")
            s+=f"| {sh} | {f1(med(sv(R,sh,'cold')),' qps')} | {f1(med(w),' qps')} | {sp(spread(w))} | {f1(med(ow),' qps')} | {sp(spread(ow))} | {delta(med(w),med(ow))} | {read(med(w),med(ow),spread(w),spread(ow))} | {f1(med(c),' qps') if c else '-'} | {f1(med(oc),' qps') if oc else '-'} | {delta(med(c),med(oc)) if c and oc else '-'} |\n"
        put("s1-mat-"+cell,s)
    R,O,Pg=N("s1-c1-g"),P(1,"s1-c1-g"),G(1,"pg-s1")
    names=[p["phase"] for p in R[0]["phases"]]
    s="| phase | ops | now | spread | v3.3.0-38 | spread | delta | read | PG 18.6 (v3.3.0-33 run) | KDS/PG now | KDS/PG v3.3.0-38 |\n|---|---|---|---|---|---|---|---|---|---|---|\n"
    for n in names:
        a,b,c=pv(R,n),pv(O,n),pv(Pg,n); ops=ph(R[0],n)["ops"]
        s+=f"| {n} | {ops:,} ops | {f1(med(a),' qps')} | {sp(spread(a))} | {f1(med(b),' qps')} | {sp(spread(b))} | {delta(med(a),med(b))} | {read(med(a),med(b),spread(a),spread(b))} | {f1(med(c),' qps')} | {ratio(med(a),med(c))} | {ratio(med(b),med(c))} |\n"
    put("s1-phases",s)
    for cell in ("c1-g","c8-g","c1-s"):
        put("s1-pct-"+cell,pct_table(N("s1-"+cell)[0]))
    for key,col,lab in (("write_sweep","batch","rows per BEGIN/COMMIT"),("concurrency_sweep","connections","connections")):
        s=f"| {lab} | c1-g now | c8-g now | c1-s now | c1-g v3.3.0-38 | c8-g v3.3.0-38 | c1-s v3.3.0-38 | PG |\n|---|---|---|---|---|---|---|---|\n"
        u=" rows/s" if key=="write_sweep" else " qps"
        for i,k in enumerate([e[col] for e in N("s1-c1-g")[0]["meta"][key]]):
            def cellv(rr):
                v=[d["meta"][key][i]["qps"] for d in rr]; return f"{f0(med(v),u)} ({sp(spread(v))})"
            s+=f"| {k} | "+" | ".join(cellv(N("s1-"+c)) for c in ("c1-g","c8-g","c1-s"))+" | "+" | ".join(cellv(P(1,"s1-"+c)) for c in ("c1-g","c8-g","c1-s"))+f" | {f0(med([d['meta'][key][i]['qps'] for d in G(1,'pg-s1')]),u)} |\n"
        put("s1-"+key,s)
    s="| bars | shape | warm now | spread | warm v3.3.0-38 | delta | read | PG warm | KDS/PG now | KDS/PG v3.3.0-38 |\n|---|---|---|---|---|---|---|---|---|---|\n"
    for lab,tn,pn in (("252","s1s-n252-c1-g","pg-s1s-n252"),("1,008","s1s-n1008-c1-g","pg-s1s-n1008"),("10,080","s1s-n10080-c1-g","pg-s1s-n10080"),("60,480","s1-c1-g","pg-s1")):
        R,O,Pg=N(tn),P(1,tn),G(1,pn)
        for sh in S1:
            w,ow,pw=sv(R,sh,"warm"),sv(O,sh,"warm"),sv(Pg,sh,"warm")
            s+=f"| {lab} | {sh} | {f1(med(w),' qps')} | {sp(spread(w))} | {f1(med(ow),' qps')} | {delta(med(w),med(ow))} | {read(med(w),med(ow),spread(w),spread(ow))} | {f1(med(pw),' qps')} | {ratio(med(w),med(pw))} | {ratio(med(ow),med(pw))} |\n"
    put("s1-size",s)
    R,O,Pg=N("s1-c1-g")[0],P(1,"s1-c1-g")[0],G(1,"pg-s1")[0]
    def comp(d):
        bl=ph(d,"read-bar-lookup")["mean_us"]; br=ph(d,"read-bar-range")["mean_us"]; sh=ph(d,"read-symbol-history")["mean_us"]; ds=ph(d,"read-day-slice")["mean_us"]
        return bl,ph(d,"read-bar-lookup")["p0_us"],(br-bl)/200,(sh-bl)/7560,1000*(ds-bl)/60480
    s="| Component | now | v3.3.0-38 | PG 18.6 (v3.3.0-33 run) | derived from |\n|---|---|---|---|---|\n"
    a,b,c=comp(R),comp(O),comp(Pg)
    s+=f"| Fixed round trip | {a[0]:.1f} µs mean, p0 {a[1]:.1f} µs | {b[0]:.1f} µs, p0 {b[1]:.1f} µs | {c[0]:.1f} µs, p0 {c[1]:.1f} µs | `read-bar-lookup`, run 1 |\n"
    s+=f"| Per returned row (200-row range) | {a[2]:.2f} µs | {b[2]:.2f} µs | {c[2]:.2f} µs | (`read-bar-range` mean - fixed)/200 |\n"
    s+=f"| Per returned row (symbol history, 7,560 rows) | {a[3]:.2f} µs | {b[3]:.2f} µs | {c[3]:.2f} µs | (`read-symbol-history` mean - fixed)/7,560 |\n"
    s+=f"| Per examined, unreturned row | {a[4]:.1f} ns | {b[4]:.1f} ns | {c[4]:.1f} ns | (`read-day-slice` mean - fixed)/60,480 |\n"
    put("s1-comp",s)
    ws=N("s1-c1-g")[0]["meta"]["write_sweep"]
    put("s1-commit",f"autocommit insert {1e6/ws[0]['qps']:.1f} µs per row, 1,000-row batch {1e6/ws[-1]['qps']:.1f} µs per row (run 1); one durable commit is about {1e6/ws[0]['qps']-1e6/ws[-1]['qps']:.0f} µs")
    ok=[d["meta"]["verify_problems"]==[] for t in ("s1-c1-g","s1-c8-g","s1-c1-s","s1s-n252-c1-g","s1s-n1008-c1-g","s1s-n10080-c1-g") for d in N(t)]
    put("s1-verify",f"{sum(ok)} of {len(ok)}")

CELLS2=[("s2-c1-g","1 core","group"),("s2-c8-g","8 cores","group"),("s2-c1-s","1 core","strict"),("s2-c8-s","8 cores","strict"),("b1-s2-c1-g","1 core","group"),("b1-s2-c1-s","1 core","strict")]
def s2():
    s="| cell | cores | durability | run 1 | run 2 | run 3 | median | spread | v3.3.0-38 median | spread | delta | read |\n|---|---|---|---|---|---|---|---|---|---|---|---|\n"
    for t,c,du in CELLS2:
        a=[d["meta"]["tps"] for d in N(t)]; b=[d["meta"]["tps"] for d in P(2,t)]
        r=[f1(x,' tps') for x in a]+["n/a"]*(3-len(a))
        s+=f"| `{t}` | {c} | {du} | {r[0]} | {r[1]} | {r[2]} | **{f1(med(a),' tps')}** | {sp(spread(a))} | {f1(med(b),' tps')} | {sp(spread(b))} | {delta(med(a),med(b))} | {read(med(a),med(b),spread(a),spread(b))} |\n"
    put("s2-tp",s)
    for t in ("s2-c1-g","s2-c8-g","s2-c1-s","s2-c8-s","b1-s2-c1-g","b1-s2-c1-s"):
        put("s2-pct-"+t,pct_table(N(t)[0]))
    names=["cargo-lookup","credit-lookup","capacity-read","recipe-read","freight-insert","charge-insert","operation-update","org-update","commit"]
    s="| cell | "+" | ".join(names)+" | unattributed | booking mean |\n|"+"---|"*(len(names)+3)+"\n"
    def dec(d):
        bk=ph(d,"booking"); tot=bk["mean_us"]*bk["ops"]; sh=[]
        for n in names:
            p=ph(d,n); sh.append(p["mean_us"]*p["ops"]/tot*100 if p else 0)
        return sh,100-sum(sh),bk["mean_us"]
    for t,_,_ in CELLS2:
        for lab,d in (("now",N(t)[0]),("v3.3.0-38",(P(2,t) or [None])[0])):
            if not d: continue
            sh,un,m=dec(d)
            s+=f"| `{t}` ({lab}) | "+" | ".join(f"{x:.1f} %" for x in sh)+f" | {un:.1f} % | {m:,.0f} µs |\n"
    put("s2-wait",s)
    s="| cell | run | committed | rejected-capacity | rejected-credit | conflicted | abandoned | axes (op / org / read / commit) | verify failures / checks |\n|---|---|---|---|---|---|---|---|---|\n"
    for t,_,_ in CELLS2:
        for i,d in enumerate(N(t)):
            m=d["meta"];o=m["outcomes"];a=m["axes"]
            s+=f"| `{PFX[i]}{t}` | run {i+1} | {o['committed']:,} | {o['rejected-capacity']} | {o['rejected-credit']} | {o['conflicted']} | {m['abandoned']} | {a['operations']} / {a['organizations']} / {a['read']} / {a['commit']} | {m['verify']['failures']} / {m['verify']['checks']} |\n"
    put("s2-outcomes",s)
    s="| bookers | durability | median | runs | spread | v3.3.0-38 median | delta | conflicted (run 1) | verify failures (run 1) |\n|---|---|---|---|---|---|---|---|---|\n"
    for B in (1,2,4,8,16,32):
        for du,c in (("group","g"),("strict","s")):
            if B==8: t=f"s2-c8-{c}"; a=N(t); o=P(2,t)
            elif B==32: t=f"sat-b32-s2-c8-{c}"; a=N(t)[:1]; o=P(2,t)[:1]
            else: t=f"x-b{B}-s2-c8-{c}"; a=N(t)[:1]; o=P(2,t)[:1]
            av=[d["meta"]["tps"] for d in a]; ov=[d["meta"]["tps"] for d in o]
            if not a: continue
            d0=a[0]["meta"]
            s+=f"| {B} | {du} | **{f1(med(av),' tps')}** | {len(av)} | {sp(spread(av))} | {f1(med(ov),' tps')} | {delta(med(av),med(ov))} | {d0['outcomes']['conflicted']} | {d0['verify']['failures']} / {d0['verify']['checks']} |\n"
    put("s2-sweep",s)
    pg=G(2,"pg-s2-b1")
    if pg:
        put("s2-pg",", ".join(f"{d['meta'].get('tps',0):,.1f} tps" for d in pg))
        put("s2-pgpct",pct_table(pg[0]))

S3=["pk-user","loans-by-user","loans-by-book","resv-by-user","books-by-author","books-by-genre","loans-by-daterange","overdue","join-loan-user","join-no-literal","exists-correlated","count-by-user"]
def q(RR,sh): return [1e6/ph(d,sh)["mean_us"] for d in RR if ph(d,sh)]
def s3():
    for C in (1,8):
        s="| loans | index | shape | now | spread | runs | v3.3.0-38 | spread | delta | read | PG 18.6 | KDS/PG now |\n|---|---|---|---|---|---|---|---|---|---|---|---|\n"
        for n in (200,1000,10000):
            for M in ("none","all"):
                t=f"s3-c{C}-n{n}-{M}"; R=N(t); O=P(3,t); Pg=G(3,f"pg-s3-n{n}-{M}")
                for sh in S3:
                    a,b,c=q(R,sh),q(O,sh),q(Pg,sh)
                    s+=f"| {n:,} | {M} | {sh} | {f0(med(a),' qps')} | {sp(spread(a))} | {len(a)} | {f0(med(b),' qps')} | {sp(spread(b))} | {delta(med(a),med(b))} | {read(med(a),med(b),spread(a),spread(b))} | {f0(med(c),' qps')} | {ratio(med(a),med(c))} |\n"
        put(f"s3-tp-c{C}",s)
    s="| loans | index | geometric mean KDS/PG over 12 shapes, now | v3.3.0-38 |\n|---|---|---|---|\n"
    for n in (200,1000,10000):
        for M in ("none","all"):
            t=f"s3-c1-n{n}-{M}"; R=N(t); O=P(3,t); Pg=G(3,f"pg-s3-n{n}-{M}")
            def gm(RR):
                rs=[]
                for sh in S3:
                    a=med(q(RR,sh)); c=med(q(Pg,sh))
                    if a and c: rs.append(a/c)
                return f"{math.exp(sum(map(math.log,rs))/len(rs)):.2f}x" if rs else "n/a"
            s+=f"| {n:,} | {M} | {gm(R)} | {gm(O)} |\n"
    put("s3-gm",s)
    for n in (200,1000,10000):
        for M in ("none","all"):
            put(f"s3-pct-{n}-{M}",pct_table(N(f"s3-c1-n{n}-{M}")[0]))
    ok=[]
    for f in os.listdir(RUN+"/json"):
        if "s3-" in f:
            d=jl(RUN+"/json/"+f); ok.append(d["meta"]["verify_problems"]==[])
    put("s3-verify",f"{sum(ok)} of {len(ok)}")
if __name__=="__main__":
    for f in sys.argv[1:]: globals()[f]()

def abpairs(fmt):
    o=[jl(f"{RUN}/json/ab-old{i}-{fmt}.json") for i in (1,2,3)]
    n=[jl(f"{RUN}/json/ab-new{i}-{fmt}.json") for i in (1,2,3)]
    return [d for d in o if d],[d for d in n if d]
def abrow(lab,o,n,u):
    return f"| {lab} | {f1(med(o),u)} | {sp(spread(o))} | {f1(med(n),u)} | {sp(spread(n))} | {ratio(med(n),med(o))} | {read(med(n),med(o),spread(n),spread(o))} |\n"
def ab():
    H="| measure | v3.3.0-38 engine | spread | v3.4.14 engine | spread | new over old | read |\n|---|---|---|---|---|---|---|\n"
    o,n=abpairs("s1-c1-g"); s=H
    for sh in S1: s+=abrow(f"s1 {sh} warm",sv(o,sh,"warm"),sv(n,sh,"warm"),' qps')
    for ph_ in ("backtest-read","result-insert","compare-one","read-day-slice","load-bars","agg-by-session"):
        s+=abrow(f"s1 phase {ph_}",pv(o,ph_),pv(n,ph_),' qps')
    put("ab-s1",s)
    o,n=abpairs("s2-c1-g"); s=H+abrow("s2 8 bookers, cores 1, group",[d["meta"]["tps"] for d in o],[d["meta"]["tps"] for d in n],' tps')
    o2,n2=abpairs("b1-s2-c1-s"); s+=abrow("s2 1 booker, cores 1, strict",[d["meta"]["tps"] for d in o2],[d["meta"]["tps"] for d in n2],' tps')
    for nm in ("commit","recipe-read","charge-insert"):
        s+=abrow(f"s2 1 booker strict, {nm} mean (derived qps)",[1e6/ph(d,nm)["mean_us"] for d in o2],[1e6/ph(d,nm)["mean_us"] for d in n2],' qps')
    put("ab-s2",s)
    o,n=abpairs("s3-c1-n10000-none"); s=H
    for sh in S3: s+=abrow(f"s3 {sh} (10,000 loans, unindexed)",q(o,sh),q(n,sh),' qps')
    put("ab-s3",s)

def extra2():
    names=["booking","cargo-lookup","credit-lookup","capacity-read","recipe-read","freight-insert","charge-insert","operation-update","org-update","commit"]
    for t,lab in (("b1-s2-c1-g","group"),("b1-s2-c1-s","strict")):
        s=f"| statement | KDS now p50 ({lab}) | v3.3.0-38 p50 | PG 18.6 p50 (other host) | KDS now mean | v3.3.0-38 mean | PG mean |\n|---|---|---|---|---|---|---|\n"
        a,b,c=N(t)[0],P(2,t)[0],G(2,"pg-s2-b1")[0]
        for n in names:
            x,y,z=ph(a,n),ph(b,n),ph(c,n)
            s+=f"| {n} | {x['p50_us']:,.1f} µs | {y['p50_us']:,.1f} µs | {z['p50_us']:,.1f} µs | {x['mean_us']:,.1f} µs | {y['mean_us']:,.1f} µs | {z['mean_us']:,.1f} µs |\n"
        put("s2-stmt-"+t,s)
    s="| cell | booking mean | commit mean | everything but the commit |\n|---|---|---|---|\n"
    for t in ("b1-s2-c1-g","b1-s2-c1-s","s2-c1-g","s2-c8-g"):
        for lab,d in (("now",N(t)[0]),("v3.3.0-38",P(2,t)[0])):
            b=ph(d,"booking"); c=ph(d,"commit")
            cm=c["mean_us"]*c["ops"]/b["ops"]
            s+=f"| `{t}` ({lab}) | {b['mean_us']:,.0f} µs | {cm:,.0f} µs per booking | {b['mean_us']-cm:,.0f} µs |\n"
    put("s2-commit",s)
