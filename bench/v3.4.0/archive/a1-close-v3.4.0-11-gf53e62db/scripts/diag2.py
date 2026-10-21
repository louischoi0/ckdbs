import json
O = "/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-fix-ddl-build-fault-net/2fe39e3f-40fc-42ed-910b-fde09fa4cb64/scratchpad/out/"
d = json.load(open(O + "diag2.json"))
first = {}; last_mtime = {}
t = None
for line in open(O + "diag2-poll.log"):
    line = line.strip()
    if line.startswith("T0 "): continue
    if line.startswith("T "):
        t = float(line[2:]); continue
    name = line.split()[0].split("/")[-1]
    first.setdefault(name, t)
    last_mtime[name] = line
base = min(first.values())
for k in ("CREATE INDEX", "CREATE ASSERTION"):
    v = d[k]; w = v["wal"]
    s0 = w["epoch_start"]; s1 = w["epoch_end"]
    print(k, "wall", v["build_wall_s"], "epoch_start", s0)
    for f in sorted(set(w["wal_before"]) | set(w["wal_after"])):
        print("  ", f, w["wal_before"].get(f), "->", w["wal_after"].get(f))
    print("  segments first seen (offset ms from build start):")
    for n, ft in sorted(first.items(), key=lambda x: x[1]):
        off = (ft - s0) * 1e3
        if -2000 < off < v["build_wall_s"] * 1e3 + 2000:
            print("    ", n, round(off, 1))
    print("  slow probes (start ms, latency ms, end ms):", [(a, b, round(a + b, 1)) for a, b in v["slow_probes"]])
