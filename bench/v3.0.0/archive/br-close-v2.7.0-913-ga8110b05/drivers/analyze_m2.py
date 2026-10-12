#!/usr/bin/env python3
"""M2 tables. usage: analyze_m2.py <m2.json>"""
import json, statistics as st, sys

d = json.load(open(sys.argv[1]))
S, series = d["summary"], d["series"]
seg = 64 << 20
mk = S["marks"]
t_fail, t_rec, t_end = mk["fail_start"], mk["recover_start"], mk["load_end"]
print("marks:", mk)
print("writers:", S["writers"]["n"], "stmts", S["writers"]["stmts"], "errors", S["writers"]["errors"], S["writers"]["latency"])


def at(t):
    # last sample at or before t
    best = series[0]
    for s in series:
        if s["t"] <= t:
            best = s
    return best


def window(a, b, label):
    sa, sb = at(a), at(b)
    dt = sb["t"] - sa["t"]
    gen = (sb["wal_durable_lsn"] - sa["wal_durable_lsn"]) / (1 << 20) / dt
    arch = (sb["wal_archived"] - sa["wal_archived"]) * seg / (1 << 20) / dt
    lags = [s["wal_archive_lag_segments"] for s in series if a <= s["t"] <= b]
    print(f"| {label} | {a:.0f}-{b:.0f} s | {dt:.1f} s | {gen:.1f} MiB/s | {arch:.1f} MiB/s | {max(lags):.0f} segments | {sb['wal_archive_failures'] - sa['wal_archive_failures']:.0f} |")
    return gen, arch, max(lags)


print("\n| phase | span | duration | WAL generation (durable LSN) | archive rate (segments archived x 64 MiB) | max wal_archive_lag_segments | archive failures in the phase |")
print("|---|---|---|---|---|---|---|")
window(0, t_fail, "steady (archive writable)")
window(t_fail, t_rec, "archive directory unwritable")
window(t_rec, t_end, "archive writable again, load running")
window(t_end, series[-1]["t"], "load stopped (tail)")
window(0, t_end, "whole load")

# 5-second sliding windows: peak WAL generation
best = (0, 0)
rates = []
for i, s in enumerate(series):
    t = s["t"]
    if t + 5 > t_end:
        break
    e = at(t + 5)
    g = (e["wal_durable_lsn"] - s["wal_durable_lsn"]) / (1 << 20) / (e["t"] - t)
    rates.append((t, g))
    if g > best[0]:
        best = (g, t)
print(f"\npeak WAL generation over any 5 s window: {best[0]:.1f} MiB/s (starting at {best[1]:.1f} s); median of 5 s windows {st.median([g for _, g in rates]):.1f} MiB/s; "
      f"min {min(g for _, g in rates):.1f}")
# peak over 1 s windows
r1 = []
for s in series:
    if s["t"] + 1 > t_end:
        break
    e = at(s["t"] + 1)
    if e["t"] > s["t"]:
        r1.append((e["wal_durable_lsn"] - s["wal_durable_lsn"]) / (1 << 20) / (e["t"] - s["t"]))
print(f"peak over any 1 s window {max(r1):.1f} MiB/s")

# archive progress events: times at which wal_archived increased
ev = []
prev = series[0]["wal_archived"]
for s in series:
    if s["wal_archived"] > prev:
        ev.append((s["t"], s["wal_archived"] - prev, s["wal_archive_lag_segments"]))
        prev = s["wal_archived"]
print("\narchive progress events (t, segments added, lag after):")
print(ev[:12], "...", ev[-12:])
# the failure and drain
fail_samples = [s for s in series if t_fail <= s["t"] <= t_rec + 40]
print("\nlag trace through the failure (t: lag, failures):")
print([(round(s["t"]), s["wal_archive_lag_segments"], s["wal_archive_failures"]) for s in series if t_fail - 2 <= s["t"] <= t_rec + 45][::8])
# when did the drain start after the directory was made writable?
first_after = next((s for s in series if s["t"] >= t_rec and s["wal_archived"] > at(t_rec)["wal_archived"]), None)
print("first archived segment after the directory was writable again at t =", None if not first_after else round(first_after["t"] - t_rec, 2), "s after")
peak_lag = max(series, key=lambda s: s["wal_archive_lag_segments"])
print("max lag", peak_lag["wal_archive_lag_segments"], "at t =", peak_lag["t"])
# drain rate: from the peak lag until lag <= 1
after_peak = [s for s in series if s["t"] >= peak_lag["t"]]
low = next((s for s in after_peak if s["wal_archive_lag_segments"] <= 1), None)
if low:
    dt = low["t"] - peak_lag["t"]
    arch_seg = low["wal_archived"] - peak_lag["wal_archived"]
    gen_mib = (low["wal_durable_lsn"] - peak_lag["wal_durable_lsn"]) / (1 << 20)
    print(f"drain from lag {peak_lag['wal_archive_lag_segments']:.0f} to <= 1: {dt:.1f} s, {arch_seg:.0f} segments archived = {arch_seg * 64 / dt:.1f} MiB/s, "
          f"while the log grew {gen_mib / dt:.1f} MiB/s (net drain {(arch_seg * 64 - gen_mib) / dt:.1f} MiB/s)")
