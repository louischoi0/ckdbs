"""AZ-S7 cell 3: each transaction split into its inserts and the rest (BEGIN,
COMMIT, the driver loop). stmt_us_by_pos sums every insert latency per octile,
so rest = sum(txn_us) - sum(inserts), per run; see the results file, cell 3."""
import json
import statistics as st
R = "/home/cdkbs/bench-runs/az-s7b"
runs = [json.load(open(f"{R}/c3-run{r}/result.json")) for r in (1, 2, 3, 4, 5)]
for k in (1024, 4096, 16384):
    rest = {}
    for kind in ("fk", "plain"):
        for l in "BA":
            per_run = []
            for d in runs:
                c = d["cells"][l][f"{kind}-K{k}"]
                stmt = sum(s for s, _ in c["stmt_us_by_pos"])
                per_run.append((sum(c["txn_us"]) - stmt) / len(c["txn_us"]))
            rest[(kind, l)] = per_run
            print(f"K={k} {kind} {l} rest ms/txn per run:", [round(v / 1000, 2) for v in per_run])
    ins = {}
    for l in "BA":
        ins[l] = [(t - rest[("fk", l)][i]) / k for i, d in enumerate(runs) for t in d["cells"][l][f"fk-K{k}"]["txn_us"]]
    dr = st.mean(rest[("fk", "B")]) - st.mean(rest[("fk", "A")])
    di = st.median(ins["B"]) - st.median(ins["A"])
    print(f"K={k} fk B-A rest {dr / 1000:+.2f} ms/txn = {dr / k:+.2f} us/row; inserts B-A (median) {di:+.2f} us/row")
