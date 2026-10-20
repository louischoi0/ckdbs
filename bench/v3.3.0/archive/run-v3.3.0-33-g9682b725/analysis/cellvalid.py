"""Exit 0 if the cell's window holds no build/test sample and no foreign process >= 50 % CPU.
usage: cellvalid.py <tag> [<precheck-tag>]"""
import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/analysis")
from mdlib import *
import datetime

tag = sys.argv[1]
ptag = sys.argv[2] if len(sys.argv) > 2 else tag
starts, ends = timeline_windows()
tm, la, gw, pr = precheck(ptag)
t0 = ts(tm) - datetime.timedelta(seconds=1)
t1 = ts(ends[tag][0]) + datetime.timedelta(seconds=3)
n, b, h, mi, det = validity(tag, monitor_samples(), t0, t1)
print(f"{tag}: samples={n} build/test={b} heavy={h} minor={mi} who={det}")
sys.exit(1 if (b or h) else 0)
