import json, sys, statistics as st
files = sys.argv[1].split(",")
shape = sys.argv[2].split(",")
runs = []
for f in files:
    runs += json.load(open(f"/home/cdkbs/bench-runs/aq-close/out/{f}.json"))["runs"]
for sh in shape:
    for path in ("literal", "prepared"):
        d = []
        for r in runs:
            v = {a["bin"]: a["server_cpu_us"] for a in r["arms"] if a["shape"] == sh and a["path"] == path}
            if v:
                d.append(v)
        ba = [x["B"] - x["A"] for x in d]
        aa = [x["A2"] - x["A"] for x in d]
        ba2 = [x["B"] - x["A2"] for x in d]
        print(f"{sh} {path} n={len(d)} B-A med {st.median(ba):+.2f} (B>A {sum(1 for x in ba if x>0)}), A'-A med {st.median(aa):+.2f} (A'>A {sum(1 for x in aa if x>0)}), "
              f"B-A' med {st.median(ba2):+.2f} (B>A' {sum(1 for x in ba2 if x>0)}); mean B-A {st.mean(ba):+.2f}, mean A'-A {st.mean(aa):+.2f}; "
              f"B-A excluding largest |A'-A| run: {st.median([b for b,a in zip(ba,aa) if abs(a)!=max(abs(x) for x in aa)]):+.2f}")
