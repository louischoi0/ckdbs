#!/usr/bin/env python3
"""Per-phase summary of hostwatch.log (1-min load every 10 s, competing-build count) against progress."""
import re, statistics, sys

R = "/home/cdkbs/bench-runs/bt-close"


def secs(t):
    h, m, s = map(int, t.split(":"))
    return h * 3600 + m * 60 + s


samples = []
for line in open(R + "/out/hostwatch.log"):
    m = re.match(r"(\d\d:\d\d:\d\d) (\S+) (\S+) (\S+) .* competing=(\d+)", line)
    if m:
        samples.append((secs(m.group(1)), float(m.group(2)), int(m.group(5))))

phases = []
starts = {}
for line in open(R + "/progress"):
    m = re.match(r"start (\w+) (\d\d:\d\d:\d\d)", line)
    if m:
        starts[m.group(1)] = secs(m.group(2))
    m = re.match(r"end (\w+) rc=\d+ (\d\d:\d\d:\d\d)", line)
    if m and m.group(1) in starts:
        phases.append((m.group(1), starts[m.group(1)], secs(m.group(2))))
# p1 (aborted at 08:55:29)
phases.insert(0, ("p1", starts["p1"], secs("08:55:29")))

print("| phase | window (UTC) | samples | 1-min load min / median / max | samples with a competing build or test (pgrep count > 0) |")
print("|---|---|---|---|---|")
hms = lambda s: "%02d:%02d:%02d" % (s // 3600, s % 3600 // 60, s % 60)
for name, a, b in phases:
    xs = [s for s in samples if a <= s[0] <= b]
    if not xs:
        continue
    L = [x[1] for x in xs]
    print("| %s | %s-%s | %d | %.2f / %.2f / %.2f | %d of %d |" % (
        name, hms(a), hms(b), len(xs), min(L), statistics.median(L), max(L), sum(1 for x in xs if x[2] > 0), len(xs)))
L = [s[1] for s in samples]
print("\nwhole watch: %d samples, %s-%s, load min %.2f median %.2f max %.2f" % (len(L), hms(samples[0][0]), hms(samples[-1][0]), min(L), statistics.median(L), max(L)))
