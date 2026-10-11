import glob, gzip, os, shutil, subprocess
N = "/home/cdkbs/bench-runs/nh"
S = "/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad/nh"
AR = "/home/cdkbs/ckdbs/.claude/worktrees/fix-nested-uncorrelated-subquery/bench/v3.0.0/archive/nested-hoist-overhead-v2.7.0-749-g23e53bed"
for sub in ("drivers", "main", "swap"):
    os.makedirs(f"{AR}/{sub}", exist_ok=True)
for f in ("nh_ab.py", "nh_run.py", "analyze.py", "render.py", "extra.py", "analyze_plans.py", "archive.py"):
    shutil.copy(f"{S}/{f}", f"{AR}/drivers/{f}")
for f in ("binaries.sha256", "run.log", "run-swap.log", "start.txt", "start-swap.txt"):
    shutil.copy(f"{N}/{f}", f"{AR}/{f}")
for f in glob.glob(f"{N}/fragments/*.md"):
    shutil.copy(f, AR)
for src, dst in (("res", "main"), ("res-swap", "swap")):
    for pat in ("*.driver.txt", "*.host.txt", "analysis.json", "pooled.json", "tables.md"):
        for f in glob.glob(f"{N}/{src}/{pat}"):
            shutil.copy(f, f"{AR}/{dst}/")
    for f in glob.glob(f"{N}/{src}/n*-run*.json"):
        with open(f, "rb") as i, gzip.open(f"{AR}/{dst}/{os.path.basename(f)}.gz", "wb", 9) as o:
            shutil.copyfileobj(i, o)
r = subprocess.run(["python3", f"{S}/analyze_plans.py"], capture_output=True, text=True)
open(f"{AR}/analyze-plans.txt", "w").write(r.stdout + r.stderr)
print(subprocess.run(["du", "-sh", AR], capture_output=True, text=True).stdout)
