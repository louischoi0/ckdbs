import sys, re
sys.path.insert(0, "/home/cdkbs/bench-run")
from edlib import *

d1 = Doc(1)
rows, order = stamp_rows(d1)
SHA = "7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a"

quiet = rows["Quiet host"][0]
quiet = quiet.replace(
    "**No cell was discarded**: the monitor saw only a `claude` process at 11 to 23 % of one CPU in 23 samples of the 1,518, all outside the 50 % line. One batch of PostgreSQL cells failed on a connection error and is listed under contamination below |",
    "The outcome is under Contamination below |")
assert quiet != rows["Quiet host"][0]

common_keys = ["Worktree / branch", "HEAD", "Tree cleanliness", "Binary provenance", "Device", "Build type", "Host", "Time budget", "Server config", "Quiet host", "PostgreSQL"]
srows = []
for k in common_keys:
    srows.append(quiet if k == "Quiet host" else rows[k][0])
stamp_md = ("## Stamp (common to the four results files)\n\n"
            "Each results file keeps its own date and time, driver, device and binary-hash line and points here for the rest.\n\n"
            "| Field | Value |\n|---|---|\n" + "\n".join(srows) +
            "\n\n")

# ---- per-file stamps
for k in (1, 2, 3, 4):
    d = Doc(k)
    rws, order = stamp_rows(d)
    keep = []
    dt = rws["Date and time"][0]
    keep.append(dt)
    dev = rws["Device"][0]
    keep.append(dev)
    keep.append(f"| Binary | `kds_server` copy in `/home/cdkbs/bench-runs/{DESC}/`, **sha256 `{SHA}`** (equal to the source binary's), source mtime 2026-10-20 03:36:57.38 UTC, built at `9682b725` as Release; provenance in the summary's Stamp |")
    keep.append("| Everything else | commit `9682b725` (`" + DESC + "`), tree clean, Release build, host, server config, quiet-host gate, PostgreSQL setup and the v3.1.0 comparator are stamped once in the summary (`summary-scenarios-" + DESC + ".md`, \"Stamp\"). **Budget lifted by the operator for this run**; nothing was cut |")
    drv = rws["Driver"][0]
    keep.append(drv)
    if k == 4:
        keep.append(rws["Server config"][-1])
    if k == 1:
        # add the re-run window to the date row
        keep[0] = keep[0].replace("join-order probe", "bar-lookup re-run 05:09 to 05:12 UTC (three cells, section 3); join-order probe", 1) if "join-order probe" in keep[0] else keep[0]
    i, j = d.section(r"^## 1\. Stamp", r"^## 2\.")
    d.s = d.s[:i] + "## 1. Stamp\n\n| Field | Value |\n|---|---|\n" + "\n".join(keep) + "\n\n" + d.s[j:]
    d.save()

s = Doc(0)
s.rep("## Headlines\n", stamp_md + "## Headlines\n")
s.save()
print("ok")
