#!/usr/bin/env python3
"""Within-arm paired differences on one server in one run (same relation size, same
run, adjacent cells): cab-insert - plain-insert, x-insert - x-insert-plain, and
x-walk / x-select. Pools B and B2 of the x phase (n = 2 x runs) and the B arm of c1/c2."""
import json, statistics as st
R = "/home/cdkbs/bench-runs/aq-close/out/"
med = st.median


def pairs(files, arms, a_shape, b_shape, path, key="server_cpu_us"):
    out = []
    for f in files:
        for r in json.load(open(R + f + ".json"))["runs"]:
            for arm in arms:
                g = lambda sh: [x[key] for x in r["arms"] if x["shape"] == sh and x["path"] == path and x["bin"] == arm]
                if g(a_shape) and g(b_shape):
                    out.append((g(a_shape)[0], g(b_shape)[0]))
    return out


def line(label, files, arms, plain, feat, key="server_cpu_us", ratio=False):
    for path in ("literal", "prepared"):
        p = pairs(files, arms, plain, feat, path, key)
        d = [b - a for a, b in p]
        n = len(d)
        if ratio:
            print(f"| {label} | {path} | {n} | {med([a for a,_ in p]):.2f} | {med([b for _,b in p]):.2f} | {med([a/b for a,b in p]):.2f}x | {sum(1 for x in d if x > 0)} of {n} |")
        else:
            print(f"| {label} | {path} | {n} | {med([a for a,_ in p]):.2f} | {med([b for _,b in p]):.2f} | {med(d):+.2f} [{min(d):+.2f}, {max(d):+.2f}] ({100*med(d)/med([a for a,_ in p]):+.1f} %) | {sum(1 for x in d if x > 0)} of {n} |")


print("| comparison (server CPU us, same server, same run) | path | n | base | feature | feature - base, median [min, max] | feature above base |\n|---|---|---|---|---|---|---|")
line("cab-insert - plain-insert, B arm of c1+c2", ["c1", "c2"], ["B"], "plain-insert", "cab-insert")
line("cab-insert - plain-insert, A arm of c1+c2 (A has the column Cabin too)", ["c1", "c2"], ["A"], "plain-insert", "cab-insert")
line("cab-insert - plain-insert, B and B2 of x", ["x"], ["B", "B2"], "plain-insert", "cab-insert")
line("x-insert - x-insert-plain (DATE(ts) expression Cabin), B and B2 of x", ["x"], ["B", "B2"], "x-insert-plain", "x-insert")
line("x-insert - cab-insert (expression vs column Cabin), B and B2 of x", ["x"], ["B", "B2"], "cab-insert", "x-insert")
print("\n| served against walked (server CPU us, same server, same run) | path | n | walked | served | walked / served | served above walked |\n|---|---|---|---|---|---|---|")
line("DATE(ts) = d: x-walk vs x-select, B and B2 of x", ["x"], ["B", "B2"], "x-walk", "x-select", ratio=True)
line("sym = 'x': plain-select vs cab-select, B arm of c1+c2", ["c1", "c2"], ["B"], "plain-select", "cab-select", ratio=True)
