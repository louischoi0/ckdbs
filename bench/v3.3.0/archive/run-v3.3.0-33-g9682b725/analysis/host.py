import sys, re
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/analysis")
from mdlib import timeline_windows, precheck, monitor_samples, validity, ts
import datetime

STARTS, ENDS = timeline_windows()
SAMPLES = monitor_samples()


def hostrows(tags, ptag=lambda t: t):
    print("| cell | start (UTC) | end (UTC) | load average before (1 / 5 / 15 min) | gate wait | build or test process | foreign samples >= 10 % CPU | exit |")
    print("|---|---|---|---|---|---|---|---|")
    for t in tags:
        if t not in ENDS:
            continue
        try:
            tm, la, gw, pr = precheck(ptag(t))
        except Exception:
            tm, la, gw, pr = "n/a", "n/a", 0, "n/a"
        t0 = ts(tm) - datetime.timedelta(seconds=1) if tm != "n/a" else None
        t1 = ts(ENDS[t][0]) + datetime.timedelta(seconds=3)
        n, b, h, mi, det = validity(t, SAMPLES, t0, t1) if t0 else (0, 0, 0, 0, [])
        print(f"| `{t}` | {tm[11:19]} | {ENDS[t][0][11:19]} | {la} | {gw} s | {'none' if pr == 'none' and not b else 'SEE'} | {mi + h} of {n} samples | {ENDS[t][1]} |")
    print()


if __name__ == "__main__":
    pm = {a.split("=")[0]: a.split("=")[1] for a in sys.argv[1:] if "=" in a}
    hostrows([a.split("=")[0] for a in sys.argv[1:]], lambda t: pm.get(t, t))
