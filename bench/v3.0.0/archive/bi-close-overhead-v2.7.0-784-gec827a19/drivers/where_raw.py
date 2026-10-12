"""Where in each arm's 12 blocks the statements over 20 ms fall. usage: where_raw.py <out-dir>/<cell> <rows> [<run> ...]"""
import gzip, json, sys

d, rows = sys.argv[1], sys.argv[2]
runs = sys.argv[3:] or ["0", "1", "2", "3"]
for r in runs:
    raw = json.load(gzip.open(f"{d}/n{rows}-r{r}.raw.json.gz", "rt"))
    summ = json.load(open(f"{d}/n{rows}-r{r}.json"))
    print(f"run {r} order_first={summ['order_first']}")
    for srv in "AB":
        for arm, lat in raw[srv].items():
            big = [(i // 250, i % 250, round(v / 1000, 1)) for i, v in enumerate(lat) if v > 20000]
            if big:
                print(f"  {srv} {arm}: (block, idx, ms) {big}")
