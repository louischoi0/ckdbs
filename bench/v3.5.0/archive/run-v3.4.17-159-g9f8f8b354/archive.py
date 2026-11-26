"""Copy the run's raw driver output (JSON summaries, logs, configs - never a data file) into
bench/v3.5.0/archive/ of the given repository root, split by scenario, plus the run scripts.
Usage: python3 -I archive.py <repo-root>"""
import os, re, shutil, sys, glob

R = "/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354"
D = "v3.4.17-159-g9f8f8b354"
A = sys.argv[1] + "/bench/v3.5.0/archive"


def scen(name):
    if re.search(r"(^|-)s4($|[-.])", name):
        return 4
    if re.search(r"(^|-)s3-", name):
        return 3
    if re.search(r"(^|-)s2-", name):
        return 2
    if re.search(r"(^|-)(s1|s1s)-", name):
        return 1
    return None


n = {1: 0, 2: 0, 3: 0, 4: 0}
for sub in ("conf", "json", "logs"):
    for f in sorted(glob.glob(f"{R}/out/{sub}/*")):
        b = os.path.basename(f)
        s = scen(b)
        if s is None:
            print("unclassified:", b)
            continue
        d = f"{A}/scenario{s}-{D}/{sub}"
        os.makedirs(d, exist_ok=True)
        shutil.copy2(f, d + "/" + b)
        n[s] += 1
print("files per scenario:", n)

rd = f"{A}/run-{D}"
os.makedirs(rd + "/tables", exist_ok=True)
os.makedirs(rd + "/logs", exist_ok=True)
for f in ("run.sh", "run_pin.sh", "run_s4r.sh", "watchdog.sh", "watchdog2.sh", "watchdog3.sh", "monitor.sh", "build.sh",
          "gen.py", "sumgen.py", "fill.py", "facts.py", "ranges.py", "contam.py", "archive.py",
          "bin/pin_new.sh", "bin/pin_old.sh", "build.status", "out/binaries.sha256", "out/df.txt"):
    if os.path.exists(f"{R}/{f}"):
        shutil.copy2(f"{R}/{f}", rd + "/" + os.path.basename(f))
for f in ("status", "hold", "monitor.log", "watchdog.log", "watchdog2.log", "watchdog3.log", "runner.log", "runner2.log",
          "runner3.log", "measure-start.txt"):
    if os.path.exists(f"{R}/run/{f}"):
        shutil.copy2(f"{R}/run/{f}", rd + "/logs/" + f)
for f in glob.glob(f"{R}/out/tables/*.md"):
    shutil.copy2(f, rd + "/tables/" + os.path.basename(f))
for f in glob.glob(f"{R}/docs-src/*.md"):
    os.makedirs(rd + "/templates", exist_ok=True)
    shutil.copy2(f, rd + "/templates/" + os.path.basename(f))
# the discarded first attempt's status only (three cells, see the summary's Watchdog section)
if os.path.exists(f"{R}/attempt1/run/status"):
    shutil.copy2(f"{R}/attempt1/run/status", rd + "/logs/attempt1-status")
print("run dir:", rd, len(os.listdir(rd)), "entries")
