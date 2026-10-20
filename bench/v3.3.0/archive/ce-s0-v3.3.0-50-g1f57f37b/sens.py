import json, statistics, os
RUN = os.path.dirname(os.path.abspath(__file__))
cells = [json.loads(l) for l in open(f"{RUN}/rowcost/cells.jsonl")]
for shape, mode, rep in (("range-1000", "split", 1), ("range-1", "drain", 2)):
    same = [c for c in cells if c["shape"] == shape and c["mode"] == mode]
    full = statistics.median(c["wall_us_per_stmt"] for c in same)
    ex = [c for c in same if c["rep"] != rep]
    print(shape, mode, "reps", sorted(c["rep"] for c in same), "walls", [round(c["wall_us_per_stmt"], 1) for c in sorted(same, key=lambda c: c["rep"])],
          "median all", round(full, 1), "median without rep", rep, round(statistics.median(c["wall_us_per_stmt"] for c in ex), 1))
    for k in ("client_cpu_us_per_stmt", "server_cpu_us_per_stmt"):
        print("   ", k, [round(c[k], 1) for c in sorted(same, key=lambda c: c["rep"])])
