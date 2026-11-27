#!/usr/bin/env python3
"""Markdown tables for `tools/dw_close.py`'s run directory.

    tools/dw_close_report.py <run-dir> part1|part2
"""
import glob
import json
import os
import sys
from statistics import median

SIZES = (200, 1000, 10000)
SHAPES = ("sel_pk", "sel_ix", "ins", "upd_nk", "upd_ix", "del")


def part1(rd):
    res = json.load(open(os.path.join(rd, "part1.json")))
    rounds = sorted({r["round"] for r in res})
    by = {(r["round"], r["arm"]): r for r in res}
    print(f"rounds: {len(rounds)}; arms per round: {sorted({r['arm'] for r in res})}\n")
    print("| relation | rows | shape | A stmts/s (client) | B-off stmts/s | A cpu us/stmt | B-off cpu us/stmt | "
          "B-off / A cpu, median [min-max over rounds] | control A2 / A cpu, median [min-max] | B-off beyond control |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    allB, allC, beyond = [], [], 0
    rows_out = 0
    for kind in ("bt", "hp"):
        for size in SIZES:
            for shape in SHAPES + ("tail",):
                def val(arm, r, key="cpu_us_med"):
                    cell = by[(r, arm)]["cells"][f"{kind}{size}"]
                    if shape == "tail":
                        return cell["tail_cpu_us_per_deleted_row"]
                    return cell[shape][key]
                if shape == "tail" and kind == "bt":
                    continue
                ra = [val("B-off", r) / val("A", r) for r in rounds]
                rc = [val("A2", r) / val("A", r) for r in rounds]
                mA = median([val("A", r) for r in rounds])
                mB = median([val("B-off", r) for r in rounds])
                if shape == "tail":
                    qa = qb = "-"
                else:
                    qa = round(median([val("A", r, "qps_client") for r in rounds]))
                    qb = round(median([val("B-off", r, "qps_client") for r in rounds]))
                med = median(ra)
                lo, hi = min(rc), max(rc)
                out = not (lo <= med <= hi)
                beyond += out
                rows_out += 1
                allB.append(med)
                allC.append(median(rc))
                name = "BTREE" if kind == "bt" else "HEAP"
                sh = "heap release tail (cpu us per deleted row, idle 1.5 s)" if shape == "tail" else shape
                print(f"| {name} | {size} | {sh} | {qa} | {qb} | {mA:.2f} | {mB:.2f} | {med:.3f} [{min(ra):.3f}-{max(ra):.3f}] "
                      f"| {median(rc):.3f} [{lo:.3f}-{hi:.3f}] | {'YES' if out else 'no'} |")
    print(f"\ncells: {rows_out}; B-off/A median over cells: {median(allB):.4f}; A2/A median over cells: {median(allC):.4f}; "
          f"cells whose median ratio lies outside the control's range: {beyond}")


def lat_row(w):
    L = w["latency_us"]
    return f"{L['ops']} | {L['p0']} | {L['p25']} | {L['p50']} | {L['p95']} | {L['p99']} | {L['p99.9']} | {L['max']}"


def part2(rd):
    cells = [json.load(open(f)) for f in sorted(glob.glob(os.path.join(rd, "p2", "*.json")))]
    mixes = []
    for c in cells:
        if c["mix"] not in mixes:
            mixes.append(c["mix"])
    print("### summary\n")
    print("| mix | arm | order | stmts/s | on / off | server cpu us/stmt | entries at end | entries after final PURGE | "
          "probe cpu us at end (3x200 median) | probe cpu us clean | multiple of clean | passes | entries read | entries removed | rows skipped |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for m in mixes:
        pair = {c["arm"]: c for c in cells if c["mix"] == m}
        off = pair["B-off"]
        for arm in ("B-off", "B-on"):
            c = pair[arm]
            end = [s for s in c["series"] if s["label"] == "end"][0]
            clean = [s for s in c["series"] if s["label"] == "after-purge"][0]
            ctr = c["counters_end"]
            print(f"| {m} | {arm} | {c['cell'].split('-')[0]} | {c['stmts_per_s']:.0f} | "
                  f"{c['stmts_per_s'] / off['stmts_per_s']:.3f} | {c['server_cpu_us_per_stmt']:.3f} | {end['entries']} | "
                  f"{clean['entries']} | {end['probe']['cpu_us_med']:.2f} | {clean['probe']['cpu_us_med']:.2f} | "
                  f"{end['probe']['cpu_us_med'] / clean['probe']['cpu_us_med']:.2f} | "
                  f"{ctr.get('index_purge_passes', '-')} | {ctr.get('index_purge_entries_read', '-')} | "
                  f"{ctr.get('index_purge_entries_removed', '-')} | {ctr.get('index_purge_rows_skipped', '-')} |")
    print("\n### latency of the mix statements, client, us\n")
    print("| mix | arm | writer (core) | statements | p0 | p25 | p50 | p95 | p99 | p99.9 | max | over 1 ms | over 5 ms | over 50 ms |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for m in mixes:
        for arm in ("B-off", "B-on"):
            c = [x for x in cells if x["mix"] == m and x["arm"] == arm][0]
            for w in c["writers"]:
                print(f"| {m} | {arm} | {w['wid']} ({w['core_start']}->{w['core_end']}) | {lat_row(w)} | "
                      f"{w['over_ms']['1']} | {w['over_ms']['5']} | {w['over_ms']['50']} |")
    print("\n### per-writer throughput in 10 s buckets (statements per bucket)\n")
    for m in mixes:
        for arm in ("B-off", "B-on"):
            c = [x for x in cells if x["mix"] == m and x["arm"] == arm][0]
            for w in c["writers"]:
                print(f"- {m} {arm} w{w['wid']}: {w['buckets_10s']}; refused={w['refused']}; wrong_affected={w['wrong_affected']}")
    print("\n### series\n")
    for m in mixes:
        print(f"\n#### {m}\n")
        print("| t (s) | off entries | on entries | off probe cpu us | on probe cpu us | off probe client p50 us | on probe client p50 us | "
              "on passes | on entries read | on entries removed |")
        print("|---|---|---|---|---|---|---|---|---|---|")
        off = [x for x in cells if x["mix"] == m and x["arm"] == "B-off"][0]
        on = [x for x in cells if x["mix"] == m and x["arm"] == "B-on"][0]
        for so, sn in zip(off["series"], on["series"]):
            ip = sn["index_purge"]
            print(f"| {so['t_s']} ({so['label']}) | {so['entries']} | {sn['entries']} | {so['probe']['cpu_us_med']:.2f} | "
                  f"{sn['probe']['cpu_us_med']:.2f} | {so['probe']['lat']['p50']} | {sn['probe']['lat']['p50']} | "
                  f"{ip.get('index_purge_passes')} | {ip.get('index_purge_entries_read')} | {ip.get('index_purge_entries_removed')} |")
    print("\n### final PURGE INDEX and top latencies\n")
    for c in cells:
        print(f"- {c['cell']}: {c['purge_at_end']}; top-20 latencies per writer (us): "
              + "; ".join(str(w["top20_us"][-5:]) for w in c["writers"]))


if __name__ == "__main__":
    {"part1": part1, "part2": part2}[sys.argv[2]](sys.argv[1])
