#!/usr/bin/env python3
"""Markdown tables for the results file. usage: make_m0_tables.py <out/m0> -> prints every table (needs analysis.json)."""
import json, sys

OUT = sys.argv[1]
r = json.load(open(f"{OUT}/analysis.json"))
DESC = {"iso": "SET ISOLATION LEVEL (control)", "insert": "INSERT, pk omitted", "insert-again": "INSERT again (replicate)",
        "select-pk": "SELECT by pk", "update-pk": "UPDATE by pk"}


def ci(t, u, d=2):
    return f"{t[0]:+.{d}f} {u} [{t[1][0]:+.{d}f}, {t[1][1]:+.{d}f}]"


def table(cell, aa, arms, title):
    print(f"\n#### {title}\n")
    hdr = "| shape | rows | runs | A QPS | B QPS | B - A QPS (95 % CI) | A p50 | B p50 | B - A p50 (95 % CI) |"
    sep = "|---|---|---|---|---|---|---|---|---|"
    if aa:
        hdr += " A/A p50 delta (95 % CI) |"
        sep += "---|"
    hdr += " A p99 | B p99 | B - A p99 (95 % CI) | B - A server CPU per statement (95 % CI) |"
    sep += "---|---|---|---|"
    print(hdr)
    print(sep)
    for arm in arms:
        for rows in (200, 1000, 10000):
            x = r.get(f"{cell}|{arm}|{rows}")
            if not x:
                continue
            line = (f"| {DESC[arm]} | {rows:,} | {x['n']} runs | {x['Aq']:,.0f} /s | {x['Bq']:,.0f} /s | {ci(x['qps'], '/s', 0)} | "
                    f"{x['A50']:.1f} µs | {x['B50']:.1f} µs | {ci(x['p50'], 'µs')} |")
            if aa:
                a = r.get(f"{aa}|{arm}|{rows}")
                line += f" {ci(a['p50'], 'µs')} |" if a else " - |"
            line += f" {x['A99']:.1f} µs | {x['B99']:.1f} µs | {ci(x['p99'], 'µs')} | {ci(x['cpu'], 'µs')} |"
            print(line)


ARMS = ["iso", "insert", "insert-again", "select-pk", "update-pk"]
table("relaxed-c1", "aa-c1", ARMS, "relaxed, cores = 1")
table("relaxed-c4", "aa-c4", ARMS, "relaxed, cores = 4")
table("group-c1", None, ["iso", "insert", "insert-again"], "group, cores = 1 (1,500 statements per arm)")
table("group-c4", None, ["iso", "insert", "insert-again"], "group, cores = 4 (1,500 statements per arm)")
