import json, sys, os, datetime
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cellvalid
S = list(cellvalid.samples())
for l in open(cellvalid.RUN + "/rowcost/cells.jsonl"):
    c = json.loads(l)
    if c["t0"] < 1792490425:   # before the master restart at 10:00:25Z
        print(c["shape"], c["mode"], c["rep"], datetime.datetime.fromtimestamp(c["t0"], datetime.timezone.utc).strftime("%H:%M:%S"),
              round(c["t1"] - c["t0"], 1), "s")
        for t, procs in S:
            if c["t0"] - 1 <= t <= c["t1"] + 2 and procs:
                print("   ", datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%H:%M:%S"), procs)
