"""Which row-cost cells overlap a monitor sample with a foreign process at >= 50 % CPU?  Cell windows are
reconstructed from the jsonl (order and (n+1) x wall per statement) anchored at the file's last line = progress mtime."""
import json, os, re, datetime
RC = "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/rowcost"
heavy = []
for l in open(f"{RC}/monitor.log"):
    m = re.match(r"(\S+) load=\S+ foreign=(.*)", l.rstrip())
    if not m or m.group(2).strip() == "none":
        continue
    for e in m.group(2).split(" | "):
        parts = e.strip().split(":", 2)
        if len(parts) == 3:
            try:
                cpu = float(parts[1])
            except ValueError:
                continue
            if cpu >= 50:
                heavy.append((datetime.datetime.strptime(m.group(1), "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc), cpu, parts[2][:40]))
print("heavy samples:", len(heavy))
for h in heavy:
    print("  ", h[0].strftime("%H:%M:%S"), h[1], h[2])
for f in ("cells-new", "cells-old", "cells-send-new", "cells-send-old"):
    cells = [json.loads(l) for l in open(f"{RC}/{f}.jsonl")]
    end = None
    # the last cell ends at the file's mtime
    end = datetime.datetime.fromtimestamp(os.path.getmtime(f"{RC}/{f}.jsonl"), datetime.timezone.utc)
    t = end
    wins = []
    for c in reversed(cells):
        dur = datetime.timedelta(microseconds=c["wall_us_per_stmt"] * (c["n"] + 1))
        wins.append((t - dur, t, c))
        t = t - dur
    wins.reverse()
    hit = []
    for (a, b, c) in wins:
        for h in heavy:
            if a - datetime.timedelta(seconds=2) <= h[0] <= b + datetime.timedelta(seconds=2):
                hit.append((c["rep"], c["mode"], c["size"], a.strftime("%H:%M:%S"), b.strftime("%H:%M:%S"), h[2], h[1]))
    print(f, "cells", len(cells), "window", wins[0][0].strftime("%H:%M:%S"), "to", wins[-1][1].strftime("%H:%M:%S"), "cells overlapping a heavy sample:", hit)
