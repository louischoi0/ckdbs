#!/usr/bin/env python3
"""CP-S0 cell M2: the slice `IS`'s price per leaf page, read off
tools/bm_scan.py's census JSON (two arms: the baseline and the build whose
`ReadBorrow::Position` declines slices).

    m2_summary.py census.json[.gz] --base base --noslice noslice
                  [--cells rows-100k,wide-a] [--out summary.md]

Per cell and driver path, per run:

  scan CPU         the arm's server on-CPU time per scan statement (us), the
                   census's `server_cpu_us` (sum of /proc/<pid>/task/*/schedstat
                   over the block, divided by the block's statements)
  delta / stmt     base - noslice (us); positive means the slice cost that much
  delta / page     delta / `pages=` of the cell's ANALYZE at run 0 (ns per leaf
                   page): the number C1 is priced by
  delta / row      delta / rows scanned (ns)
  point control    base - noslice on the relation's point statement (us), which
                   both builds answer identically: the run's noise floor
  block spread     per arm, max - min of the per-block server us/stmt inside the
                   run (`block_server_us`): the within-arm spread the delta is
                   read against (there is no A' arm in this cell)

Each quantity is reported as the median over runs with [min, max]; the wall
percentiles are pooled over runs per arm. Units sit in every cell
(bench/README.md). The digest check says whether both arms answered the same
rows at run 0; `bad` counts wrong replies.
"""
import argparse, gzip, json, re, statistics, sys


def pct(xs, p):
    xs = sorted(xs)
    return xs[max(0, min(len(xs) - 1, -(-p * len(xs) // 100) - 1))] if xs else 0.0


def load(path):
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt") as f:
        return json.load(f)


def pages_of(run0, cell, arm):
    txt = run0.get("analyze", {}).get(cell, {}).get(arm, "")
    m = re.search(r"pages=(\d+)", txt)
    return int(m.group(1)) if m else None


def arm_rec(run, arm, cell, path):
    for r in run["arms"]:
        if r["arm"] == arm and r["cell"] == cell and r["path"] == path:
            return r
    return None


def med(xs, nd=2):
    xs = [x for x in xs if x is not None]
    if not xs:
        return "n/a"
    return "%.*f [%.*f, %.*f]" % (nd, statistics.median(xs), nd, min(xs), nd, max(xs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("census")
    ap.add_argument("--base", default="base")
    ap.add_argument("--noslice", default="noslice")
    ap.add_argument("--replicate", default=None,
                    help="label of a second arm running the BASE binary (M2_REPLICATE=1's `base2`): "
                    "its base - replicate delta per run is the A-A' floor, S = max |delta|, and the "
                    "base - noslice delta is read against it (BM-Q12's bar)")
    ap.add_argument("--cells", default="rows-100k,wide-a")
    ap.add_argument("--out")
    a = ap.parse_args()
    d = load(a.census)
    runs = d.get("runs", [])
    if not runs:
        sys.exit("no runs in %s" % a.census)
    A, B, R = a.base, a.noslice, a.replicate
    lines = []
    w = lines.append
    w("# CP-S0 M2: the slice `IS` per leaf page (`%s` - `%s`)" % (A, B))
    w("")
    w("Source `%s`: %d run(s), blocks %s, scan_ops %s, point_ops %s, cells %s, arms %s." % (
        a.census, len(runs), d["args"].get("blocks"), d["args"].get("scan_ops"),
        d["args"].get("point_ops"), d["args"].get("cells"), runs[0].get("arm_order")))
    w("Load per run: %s s; idle wait (s, us/s): %s; quiet gate: %s." % (
        [r.get("load_s") for r in runs], runs[0].get("idle_wait"),
        [r.get("quiet_gate", {}).get("load") for r in runs]))
    w("")
    for cell in a.cells.split(","):
        rec0 = arm_rec(runs[0], A, cell, "literal")
        if rec0 is None:
            w("## %s: not in the census" % cell)
            w("")
            continue
        rel, rows = rec0["rel"], rec0["rows"]
        pages = {arm: pages_of(runs[0], cell, arm) for arm in (A, B)}
        dig = runs[0].get("digest", {}).get(cell, {})
        w("## %s (`%s`, %s rows, %s leaf pages; %s rows per page)" % (
            cell, rel, "{:,}".format(rows), pages[A],
            "%.1f" % (rows / pages[A]) if pages[A] else "n/a"))
        w("")
        w("ANALYZE pages: %s = %s, %s = %s %s; rows digest at run 0: %s" % (
            A, pages[A], B, pages[B], "(same)" if pages[A] == pages[B] else "(DIFFER)",
            "same" if dig.get(A) == dig.get(B) and dig else "DIFFER or missing (%s)" % dig))
        w("")
        w("| path | %s scan CPU | %s scan CPU | delta / stmt | delta / page | delta / row | share of %s | point control (delta / stmt) | block spread %s | block spread %s | %s wall p50 | %s wall p50 | bad %s/%s |" % (
            A, B, A, A, B, A, B, A, B))
        w("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for path in ("literal", "prepared"):
            S = {A: [], B: []}
            P = {A: [], B: []}
            spread = {A: [], B: []}
            lat = {A: [], B: []}
            bad = {A: 0, B: 0}
            d_stmt, d_page, d_row, d_point, share = [], [], [], [], []
            for run in runs:
                ra, rb = arm_rec(run, A, cell, path), arm_rec(run, B, cell, path)
                pa, pb = arm_rec(run, A, "point:" + rel, path), arm_rec(run, B, "point:" + rel, path)
                if None in (ra, rb, pa, pb):
                    continue
                for arm, r, p in ((A, ra, pa), (B, rb, pb)):
                    S[arm].append(r["server_cpu_us"])
                    P[arm].append(p["server_cpu_us"])
                    bs = r.get("block_server_us") or []
                    spread[arm].append(max(bs) - min(bs) if bs else None)
                    lat[arm].extend(r.get("lat_us") or [])
                    bad[arm] += r.get("bad", 0)
                ds = ra["server_cpu_us"] - rb["server_cpu_us"]
                d_stmt.append(ds)
                d_page.append(ds * 1e3 / pages[A] if pages[A] else None)
                d_row.append(ds * 1e3 / rows)
                d_point.append(pa["server_cpu_us"] - pb["server_cpu_us"])
                share.append(100.0 * ds / ra["server_cpu_us"])
            if not d_stmt:
                w("| %s | no complete run | | | | | | | | | | | |" % path)
                continue
            w("| %s | %s us | %s us | %s us | %s ns | %s ns | %s %% | %s us | %s us | %s us | %s us | %s us | %d/%d |" % (
                path, med(S[A], 1), med(S[B], 1), med(d_stmt, 1), med(d_page, 0), med(d_row, 2),
                med(share, 2), med(d_point, 2), med(spread[A], 1), med(spread[B], 1),
                "%.1f" % pct(lat[A], 50), "%.1f" % pct(lat[B], 50), bad[A], bad[B]))
            if R:
                # The A-A' floor: the same binary under two labels, per run.
                d_rep = []
                for run in runs:
                    ra, rr = arm_rec(run, A, cell, path), arm_rec(run, R, cell, path)
                    if ra is not None and rr is not None:
                        d_rep.append(ra["server_cpu_us"] - rr["server_cpu_us"])
                if d_rep:
                    s_floor = max(abs(x) for x in d_rep)
                    m_ns = statistics.median(d_stmt)
                    verdict = ("outside the floor (|%.1f| > S = %.1f us)" if abs(m_ns) > s_floor
                               else "inside the floor (|%.1f| <= S = %.1f us)") % (m_ns, s_floor)
                    w("| %s A-A' (`%s` - `%s`) | | | %s us | %s ns | | | | | | | | %s |" % (
                        path, A, R, med(d_rep, 1),
                        med([x * 1e3 / pages[A] for x in d_rep] if pages[A] else [], 0), verdict))
        w("")
        w("Wall per statement, pooled over runs (us): " + "; ".join(
            "%s %s p50 %.1f / p90 %.1f / p99 %.1f" % (
                arm, path, pct(L, 50), pct(L, 90), pct(L, 99))
            for path in ("literal", "prepared")
            for arm, L in ((A, [x for run in runs for x in ((arm_rec(run, A, cell, path) or {}).get("lat_us") or [])]),
                           (B, [x for run in runs for x in ((arm_rec(run, B, cell, path) or {}).get("lat_us") or [])]))
            if L))
        w("")
    w("Reading: `delta / page` is the slice `IS`'s on-CPU price per leaf page at `cores = 1` "
      "(no partition latch exists at one core, `lock_table.hpp:511-512`); a `point control` "
      "of the same order as `delta / stmt` means the delta is inside the run's noise.")
    text = "\n".join(lines) + "\n"
    if a.out:
        with open(a.out, "w") as f:
            f.write(text)
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
