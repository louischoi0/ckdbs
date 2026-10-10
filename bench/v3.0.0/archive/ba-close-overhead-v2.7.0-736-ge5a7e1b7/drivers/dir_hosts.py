"""Per-directory host record of an overhead run. usage: dir_hosts.py <dir> [<dir> ...]
For each directory: run files, 1-min loadavg before and after each run (min-max), the number of runs whose `competing`
list holds a process that is itself a compiler, build driver or test binary (cc1plus, cmake, ctest, kds_tests as the
program, not as a word inside a shell wait loop), and the control arm's worst p50 gap between A and B."""
import glob, json, os, re, sys

REAL = re.compile(r"^\d+ (\S*/)?(cc1plus|cmake|ctest|kds_tests|c\+\+|g\+\+|ld|cc1)\b")
for d in sys.argv[1:]:
    fs = [f for f in glob.glob(d + "/n*-r*.json") if not f.endswith(".raw.json")]
    if not fs:
        print(f"{os.path.basename(d)}: no run files")
        continue
    lb, la, real, anyc, mx, errs = [], [], 0, 0, 0.0, 0
    ts = []
    for f in fs:
        s = json.load(open(f))
        lb.append(float(s["host_before"]["loadavg"].split()[0]))
        la.append(float(s["host_after"]["loadavg"].split()[0]))
        comp = s["host_before"].get("competing", []) + s["host_after"].get("competing", [])
        anyc += bool(comp)
        real += any(REAL.match(x) for x in comp)
        for a in ("iso", "ping2"):
            if a in s["arms"]["A"]:
                mx = max(mx, abs(s["arms"]["B"][a]["p50_us"] - s["arms"]["A"][a]["p50_us"]))
        for srv in ("A", "B"):
            errs += sum(x.get("errors", 0) for x in s["arms"][srv].values())
        ts.append(os.path.getmtime(f))
    import time
    t0, t1 = (time.strftime("%H:%M", time.gmtime(t)) for t in (min(ts), max(ts)))
    print(f"{os.path.basename(d)}: {len(fs)} run files, written {t0}-{t1} UTC; load before {min(lb):.2f}-{max(lb):.2f}, "
          f"after {min(la):.2f}-{max(la):.2f}; runs with a non-empty competing list {anyc}, with a live compiler/build/test program {real}; "
          f"errors {errs}; control p50 gap max {mx:.2f} us")
