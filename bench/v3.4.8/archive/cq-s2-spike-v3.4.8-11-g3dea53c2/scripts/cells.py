"""cells.py <dir> - one line per cell of every driver JSON in <dir>."""
import glob
import json
import sys

for f in sorted(glob.glob(sys.argv[1] + "/*.json")):
    name = f.split("/")[-1]
    if any(s in name for s in (".server.", ".start.", ".stop.")):
        continue
    d = json.load(open(f))
    cells = d.get("cells") if isinstance(d, dict) else d
    if not cells:
        print(name, "keys:", list(d.keys())[:15])
        continue
    for c in cells:
        keep = {k: c[k] for k in c if k in ("tps", "committed", "refused", "oracle", "oracle_ok",
                                           "fatal", "server_cpu_cores", "seconds")}
        print(name, c.get("arm"), "k=%s" % c.get("k"), keep)
