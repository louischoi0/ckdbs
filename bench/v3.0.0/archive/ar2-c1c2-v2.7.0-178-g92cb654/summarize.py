#!/usr/bin/env python3
"""Summarize the AR2 C1/C2 cells from the drivers' own stdout tables.

Reads every <cell>.run.stdout.txt in RUN (scenario 0 writes the run table to
its only stdout file, scenario 2 to the run phase's), pulls the headline
TPS and the percentile row of the phase that carries the business
transaction (scenario 2: `booking` and `commit`; scenario 0: `txn`), plus
the precheck loadavg from <cell>.cell.json, and prints one markdown table.
The driver's own numbers are the source; nothing is recomputed here.
"""
import glob
import json
import os
import re

RUN = "/home/cdkbs/bench-runs/ar2-c1c2-92cb654"

ROW = re.compile(r"^(\S+)\s+([\d,]+)\s+([\d,]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\d+)\s*$")


def rows(path):
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = ROW.match(line.rstrip("\n"))
        if m:
            name = m.group(1)
            out[name] = {"ops": m.group(2), "rate": m.group(3), "mean": m.group(4),
                         "p0": m.group(5), "p25": m.group(6), "p50": m.group(7),
                         "p95": m.group(8), "p99": m.group(9), "max": m.group(10),
                         "err": m.group(11)}
    return out


def tps(path, scenario):
    text = open(path, encoding="utf-8", errors="replace").read()
    if scenario == "s2":
        m = re.search(r"committed\s+(\d+)\s+([\d.]+) TPS", text)
        return (m.group(2), m.group(1)) if m else ("?", "?")
    m = re.search(r"TPS\s+([\d.]+)\s+committed transactions/sec", text)
    c = re.search(r"committed\s+([\d,]+)", text)
    return (m.group(1) if m else "?", c.group(1) if c else "?")


def torn(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"torn\s+(\d+)", text)
    return m.group(1) if m else "-"


def main():
    print("| cell | cores | peer_listeners | range_size_ids | TPS | committed | torn | phase | p50 µs | p95 µs | p99 µs | max µs | precheck loadavg | driver exit |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for cell_json in sorted(glob.glob(os.path.join(RUN, "*.cell.json"))):
        rec = json.load(open(cell_json))
        cell = rec["cell"]
        scenario = rec["scenario"]
        run_out = os.path.join(RUN, f"{cell}.run.stdout.txt")
        if not os.path.exists(run_out):
            print(f"| {cell} | {rec['cores']} | {rec['peer_listeners']} | {rec['range_size_ids']} | no run output | | | | | | | | {rec['precheck']['loadavg']} | {[p['exit_code'] for p in rec.get('phases', [])]} |")
            continue
        t, committed = tps(run_out, scenario)
        r = rows(run_out)
        phase = "booking" if scenario == "s2" else "txn"
        p = r.get(phase, {})
        exits = [ph["exit_code"] for ph in rec.get("phases", [])]
        print(f"| {cell} | {rec['cores']} | {rec['peer_listeners']} | {rec['range_size_ids'] or 'off'} | {t} | {committed} | {torn(run_out)} | {phase} | {p.get('p50','?')} | {p.get('p95','?')} | {p.get('p99','?')} | {p.get('max','?')} | {rec['precheck']['loadavg'].split()[0]} | {exits} |")
        if scenario == "s2" and "commit" in r:
            c = r["commit"]
            print(f"| {cell} | | | | | | | commit | {c['p50']} | {c['p95']} | {c['p99']} | {c['max']} | | |")


if __name__ == "__main__":
    main()
