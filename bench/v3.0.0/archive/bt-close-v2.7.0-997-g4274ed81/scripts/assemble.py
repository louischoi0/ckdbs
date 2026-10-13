#!/usr/bin/env python3
"""Fills {{T:name}} placeholders in a template with table output.  Tables are generated, never typed.

    assemble.py TEMPLATE.md OUT.md
"""
import os, re, subprocess, sys

R = "/home/cdkbs/bench-runs/bt-close"
S = R + "/scripts"
SCAN, BN, ID = S + "/bt_close_scan.py", S + "/bt_close_bn.py", S + "/ident.py"
O = "out/"
SC = ["S1=" + O + "p1a.json", "S1=" + O + "p1b.json", "S2=" + O + "p4.json", "S3=" + O + "p8.json"]
S1 = ["S1=" + O + "p1a.json", "S1=" + O + "p1b.json"]
UP = ["U1=" + O + "p2.json", "U2=" + O + "p3.json", "U3=" + O + "p9.json"]

T = {
    "scan_groups": (SCAN, "groups", SC, {}),
    "scan_net_s1": (SCAN, "net", S1, {}),
    "scan_net_all": (SCAN, "net", SC, {}),
    "scan_net_clean": (SCAN, "net", SC, {"CLEAN": "1"}),
    "scan_runs": (SCAN, "runs", ["S1a=" + O + "p1a.json", "S1b=" + O + "p1b.json", "S2=" + O + "p4.json", "S3=" + O + "p8.json"], {}),
    "scan_pct": (SCAN, "pct", SC, {"CLEAN": "1"}),
    "scan_waits": (SCAN, "waits", SC, {}),
    "scan_host": (SCAN, "host", SC, {}),
    "scan_sizes": (SCAN, "groups", ["Z=" + O + "p5.json"], {"NOPOOL": "1"}),
    "scan_sizes_net": (SCAN, "net", ["Z=" + O + "p5.json"], {}),
    "scan_sizes_host": (SCAN, "host", ["Z=" + O + "p5.json"], {}),
    "ident": (ID, None, ["p1a=" + O + "p1a.json", "p1b=" + O + "p1b.json", "p4=" + O + "p4.json", "p8=" + O + "p8.json", "p5=" + O + "p5.json"], {}),
    "hostsummary": (S + "/hostsummary.py", None, [], {}),
    "bn_groups_clean": (BN, "groups", UP, {"CLEAN": "1"}),
    "bn_groups": (BN, "groups", UP, {}),
    "bn_guard": (BN, "guard", UP, {}),
    "bn_runs": (BN, "runs", UP, {}),
    "bn_pct": (BN, "pct", UP, {}),
    "bn_waits": (BN, "waits", UP, {}),
    "bn_host": (BN, "host", UP, {}),
    "bn_end": (BN, "end", UP, {}),
    "bn_sizes": (BN, "groups", ["Z200=" + O + "p6.json", "Z1000=" + O + "p2.json", "Z1000=" + O + "p3.json", "Z1000=" + O + "p9.json", "Z10000=" + O + "p7.json"], {"NOPOOL": "1"}),
    "bn_sizes_host": (BN, "host", ["Z200=" + O + "p6.json", "Z10000=" + O + "p7.json"], {}),
}


def run(name):
    script, sec, files, env = T[name]
    argv = ["taskset", "-c", "0,1,6,7", "python3", "-I", script] + ([sec] if sec else []) + files
    e = dict(os.environ)
    e.update(env)
    r = subprocess.run(argv, cwd=R, env=e, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("table %s failed:\n%s" % (name, r.stderr))
    return r.stdout.rstrip("\n")


tmpl = open(sys.argv[1]).read()
out = re.sub(r"\{\{T:(\w+)\}\}", lambda m: run(m.group(1)), tmpl)
open(sys.argv[2], "w").write(out)
print("wrote", sys.argv[2], len(out.splitlines()), "lines")
