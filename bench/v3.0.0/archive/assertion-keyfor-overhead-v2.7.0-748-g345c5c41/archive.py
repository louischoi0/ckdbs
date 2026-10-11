import glob, gzip, os, shutil
SRC = "/home/cdkbs/bench-runs/keyfor"
DST = "/home/cdkbs/ckdbs/.claude/worktrees/fix-assertion-literal-group-key/bench/v3.0.0/archive/assertion-keyfor-overhead-v2.7.0-748-g345c5c41"
S = "/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad"
os.makedirs(DST, exist_ok=True)
for d in sorted(glob.glob(SRC + "/int-*") + glob.glob(SRC + "/date-*") + glob.glob(SRC + "/failed-*")):
    t = os.path.join(DST, os.path.basename(d))
    os.makedirs(t, exist_ok=True)
    for f in ("driver.txt", "host.txt", "A.conf", "B.conf"):
        if os.path.exists(os.path.join(d, f)):
            shutil.copy(os.path.join(d, f), t)
    rj = os.path.join(d, "result.json")
    if os.path.exists(rj):
        with open(rj, "rb") as i, gzip.open(os.path.join(t, "result.json.gz"), "wb") as o:
            shutil.copyfileobj(i, o)
for f in ("binaries.sha256", "analysis.txt", "analysis.json", "pct.txt", "clean.txt", "tables.md",
          "tables-compact.md", "runall.log"):
    shutil.copy(os.path.join(SRC, f), DST)
for f in glob.glob(S + "/drv/*.py"):
    shutil.copy(f, DST)
shutil.copy(S + "/bs/build-release.sh", DST + "/build-release-with-root-arg.sh")
