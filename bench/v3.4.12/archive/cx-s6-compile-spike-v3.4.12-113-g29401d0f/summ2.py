import json, statistics as st, sys
here=sys.argv[1]
for cores in (1,2):
    d=json.load(open(f"{here}/cores{cores}.json"))
    print(f"\ncores = {cores}")
    print("cell | p50 | spike us/call (med, min-max) | calls/ops excess | compile_us/call | server total us/call | spike/p50 % (med,min-max) | spike/server % | compile/p50 %")
    for c in d["cells"]:
        R=c["rounds"]; m=lambda f: st.median(f(r) for r in R)
        sp=[r["spike_ns"]/r["spike_calls"]/1000 for r in R]
        p50=m(lambda r:r["p50"]); srv=m(lambda r:r["server_total_us_per_call"]); cu=m(lambda r:r["compile_us_per_call"])
        sh=[100*x/r["p50"] for x,r in zip(sp,R)]
        ex=[r["spike_calls"]-r["ops"] for r in R]
        print(f"{c['cell']} | {p50:.1f} | {st.median(sp):.3f} ({min(sp):.3f}-{max(sp):.3f}) | {ex} | {cu:.2f} | {srv:.2f} | {st.median(sh):.2f} ({min(sh):.2f}-{max(sh):.2f}) | {100*st.median(sp)/srv:.1f} | {100*cu/p50:.1f}")
