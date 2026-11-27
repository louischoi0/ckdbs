#!/usr/bin/env python3
"""Tables for DW-S0 from a `tools/dw_s0_premise.py` run directory.

    tools/dw_s0_report.py ~/bench-runs/dw-s0/run

Prints, per cell, the dead-entry series (entries less live rows), its linear
fit, the probe-cost series on x_c, and the read phase's churned / reference
ratios against the control's spread. Everything is derived from the run's
JSON; nothing is measured here.
"""

import json
import sys

CELLS = ("D-rec", "D-fresh", "M-rec", "M-fresh")
RELS = "abc"


def fit(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return slope, my - slope * mx


def phase(res, ref, others):
    """ref: the reference relation; others: the two churned ones. Per-rep CPU."""
    s = res["summary"]
    reps = len(s["a"]["per_rep"])
    cpu = lambda r, i: s[r]["per_rep"][i]["cpu_us_per_stmt"]
    mean = lambda r: sum(cpu(r, i) for i in range(reps)) / reps
    ref_cpu = mean(ref)
    ch = sum(mean(o) for o in others) / len(others)
    # the control is the two churned twins against each other; with one churned twin
    # (an M cell after the purge: a is rebuilt) there is none, and the phase-1 one stands
    ctl = [cpu(others[0], i) - cpu(others[-1], i) for i in range(reps)] if len(others) == 2 else [0.0]
    return {"ref": ref_cpu, "churned": ch, "ratio": ch / ref_cpu, "delta": ch - ref_cpu,
            "ctl_lo": min(ctl), "ctl_hi": max(ctl), "ctl_pct": (max(ctl) - min(ctl)) / ref_cpu * 100,
            "means": {r: mean(r) for r in RELS}}


def med(xs):
    xs = sorted(xs)
    return (xs[len(xs) // 2] + xs[(len(xs) - 1) // 2]) / 2


def main(d):
    for c in CELLS:
        ser = [json.loads(l) for l in open(f"{d}/series-{c}.jsonl")]
        mix = json.load(open(f"{d}/mix-{c}.json"))
        rd = json.load(open(f"{d}/read-{c}.json"))
        live = mix["live_rows"]
        n_ops = mix["ops"]["deletes"] + mix["ops"]["moves"]
        print(f"\n## {c}  (n={mix['n']}, domain={mix['domain']}, mix {mix['mix_wall_s']} s, "
              f"{n_ops} {'deletes' if mix['kind'] == 'D' else 'moves'}, refused {mix['refused']})")
        print("t_s | entries a | dead a | dead/s since 0 | stmts/s | probe CPU us | probe wall us | probe rows | pages alloc")
        for s in ser:
            e = s["entries"]["a"]
            t = s["t_s"]
            print(f"{t:6.1f} | {e} | {e - live} | {(e - live) / t if t else 0:8.0f} | {s['stmt_rate_per_s']} | "
                  f"{s['probe_x_c']['cpu_us']} | {s['probe_x_c']['wall_us']} | {s['probe_x_c']['rows']} | {s['pages_allocated']}")
        ts = [s["t_s"] for s in ser]
        dead = [s["entries"]["a"] - live for s in ser]
        slope, icpt = fit(ts, dead)
        half = len(ser) // 2
        s1, _ = fit(ts[:half + 1], dead[:half + 1])
        s2, _ = fit(ts[half:], dead[half:])
        print(f"dead entries: linear fit {slope:.1f}/s (intercept {icpt:.0f}); first half {s1:.1f}/s, second half {s2:.1f}/s; "
              f"per {'delete' if mix['kind'] == 'D' else 'move'}: {dead[-1] / n_ops:.4f}; "
              f"twins equal at end: {len({ser[-1]['entries'][r] for r in RELS}) == 1}")
        if not mix["fresh"]:
            print(f"stale entries per key value at end: {dead[-1] / mix['domain']:.1f} "
                  f"(live rows per value {live / mix['domain']:.2f})")
        base = ser[0]["probe_x_c"]["cpu_us"]
        cost = [s["probe_x_c"]["cpu_us"] / base for s in ser]
        cs, ci = fit(ts, [s["probe_x_c"]["cpu_us"] for s in ser])
        cross = next((s["t_s"] for s, r in zip(ser, cost) if r > 1.05), None)
        print(f"probe CPU on x_c: t=0 {base} us; fit {cs * 1000:.3f} ns/s; first sample > 1.05x t=0: {cross} s; "
              f"fit crosses 1.05x at {(0.05 * base - (ci - base)) / cs if cs > 0 else float('nan'):.1f} s")
        print(f"entries before/after REINDEX x_a: {rd['entries_before']} -> {rd['entries_after_reindex']}; "
              f"height {rd['height_before']} -> {rd['height_after_reindex']}; pages_allocated delta {rd['pages_allocated_delta_reindex']}; "
              f"{rd['reindex']}")
        print(f"purge: {rd.get('purge')}; entries after {rd.get('entries_after_purge')}; height {rd.get('height_after_purge')}; "
              f"pages_allocated delta {rd.get('pages_allocated_delta_purge')}")
        print(f"live distinct keys {rd['live_distinct_keys']}, live rows/key {rd['live_rows_per_key_mean']}, "
              f"rows/probe in the pool {rd['rows_per_probe_mean']}, refused in read {rd['refused_during_read']}")
        print("analyze:", [l for l in rd["analyze"]["a"] if "step" in l or "analyze" in l][:2])
        p1 = phase(rd["after_reindex"], "a", ("b", "c"))
        print(f"after REINDEX x_a (wrong-row counts {rd['after_reindex']['wrong_row_counts']}): CPU us/stmt "
              f"a(rebuilt) {p1['means']['a']:.2f}, b {p1['means']['b']:.2f}, c {p1['means']['c']:.2f}; churned/rebuilt {p1['ratio']:.3f}; "
              f"delta {p1['delta']:.2f} us; control b-c range {p1['ctl_lo']:.2f}..{p1['ctl_hi']:.2f} us ({p1['ctl_pct']:.1f} % of ref)")
        if "after_purge" in rd:
            p2 = phase(rd["after_purge"], "b", ("a", "c") if mix["kind"] == "D" else ("c",))
            print(f"after PURGE x_b (wrong {rd['after_purge']['wrong_row_counts']}): CPU us/stmt "
                  f"a {p2['means']['a']:.2f}, b(purged) {p2['means']['b']:.2f}, c {p2['means']['c']:.2f}; churned/purged {p2['ratio']:.3f}; "
                  f"delta {p2['delta']:.2f} us; control a-c range {p2['ctl_lo']:.2f}..{p2['ctl_hi']:.2f} us"
                  f" ({p2['ctl_pct']:.1f} % of ref; a D cell's churned a and c, an M cell has none)")
        for ph in ("after_reindex", "after_purge"):
            if ph in rd:
                print(f"{ph} median of the 12 blocks' CPU us/stmt:",
                      {r: round(med(rd[ph]["summary"][r]["block_cpu"]), 2) for r in RELS})
        print("latency us (p0 p25 p50 p95 p99, ops):")
        for ph in ("after_reindex", "after_purge"):
            if ph in rd:
                for r in RELS:
                    L = rd[ph]["summary"][r]["latency_us"]
                    print(f"  {ph} x_{r}: {L['p0']} {L['p25']} {L['p50']} {L['p95']} {L['p99']} ops={L['ops']} mean={L['mean']}")
                print(f"  {ph} per-rep CPU:", {r: [x['cpu_us_per_stmt'] for x in rd[ph]['summary'][r]['per_rep']] for r in RELS},
                      "wait us:", {r: round(sum(x['wait_us_per_stmt'] for x in rd[ph]['summary'][r]['per_rep']) / 3, 1) for r in RELS},
                      "wall us:", {r: round(sum(x['wall_us_per_stmt'] for x in rd[ph]['summary'][r]['per_rep']) / 3, 1) for r in RELS})
        if not mix["fresh"]:
            ref = p2["ref"] if mix["kind"] == "D" and "after_purge" in rd else p1["ref"]
            ch = p2["churned"] if mix["kind"] == "D" and "after_purge" in rd else p1["churned"]
            stale = dead[-1] / mix["domain"]
            per = (ch - ref) / stale
            print(f"per stale entry in the key run: {per * 1000:.1f} ns CPU (delta {ch - ref:.2f} us / {stale:.1f} stale per run)")
            print(f"stale per run for +5 %: {0.05 * ref / per:.1f}; at the fit growth "
                  f"{(0.05 * ref / per) / (slope / mix['domain']):.1f} s; statements: "
                  f"{(0.05 * ref / per) * mix['domain'] / (dead[-1] / n_ops):.0f} {'deletes' if mix['kind'] == 'D' else 'moves'}")


if __name__ == "__main__":
    main(sys.argv[1])
