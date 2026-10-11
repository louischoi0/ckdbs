#!/usr/bin/env python3
import glob, gzip, os, shutil, subprocess
W = "/home/cdkbs/ckdbs/.claude/worktrees/fix-nested-uncorrelated-subquery"
D = f"{W}/bench/v3.0.0/archive/nested-hoist-overhead-followup-v2.7.0-750-g9004a0cb"
S = "/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad/nhf"
R = "/home/cdkbs/bench-runs"
for sub in ("drivers", "main", "swap", "ext", "control"):
    os.makedirs(f"{D}/{sub}", exist_ok=True)
for f in ("nh_ab.py", "nh_run.py", "analyze.py", "report.py", "pool.py", "perrun.py", "runall.sh", "runext.sh", "buildbp.sh", "archive_followup.py"):
    shutil.copy(f"{S}/{f}", f"{D}/drivers/{f}")
for src, dst in (("nhf/res", "main"), ("nhf/res-swap", "swap"), ("nhf/res-ext", "ext"), ("nhf-ctl/res", "control")):
    for pat in ("*.host.txt", "*.driver.txt"):
        for f in glob.glob(f"{R}/{src}/{pat}"):
            shutil.copy(f, f"{D}/{dst}/")
    shutil.copy(f"{R}/{src}/analysis.json", f"{D}/{dst}/analysis.json")
    for f in glob.glob(f"{R}/{src}/n*-run*.json"):
        with open(f, "rb") as i, gzip.open(f"{D}/{dst}/{os.path.basename(f)}.gz", "wb") as o:
            shutil.copyfileobj(i, o)
for a, b in (("nhf/run.log", "run-main.log"), ("nhf/run-swap.log", "run-swap.log"), ("nhf/run-ext.log", "run-ext.log"),
             ("nhf-ctl/run.log", "run-control.log"), ("nhf/binaries.sha256", "binaries.sha256"),
             ("nhf-ctl/binaries.sha256", "binaries-control.sha256")):
    shutil.copy(f"{R}/{a}", f"{D}/{b}")
shutil.copy(f"{S}/build-Bp.log", f"{D}/build-Bp.log")
dirs = [f"{R}/nhf/res", f"{R}/nhf/res-swap", f"{R}/nhf/res-ext", f"{R}/nhf-ctl/res"]
for script, out, args in (("report.py", "report.txt", dirs), ("pool.py", "pooled-16-runs.txt", dirs[0:1] + dirs[2:3]),
                          ("perrun.py", "per-run-deltas.txt", dirs)):
    with open(f"{D}/{out}", "w") as o:
        subprocess.run(["python3", f"{S}/{script}"] + args, stdout=o, check=True)
print(subprocess.run(["du", "-sh", D], capture_output=True, text=True).stdout)
