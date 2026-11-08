#!/usr/bin/env python3
"""CP-S0: read the lock family's instruments off a running kds_server after a
cell and save them beside the driver's JSON.

    read_views.py --port P --out PREFIX [--text-port D] [--no-ash]
                  [--tools /path/to/tools]

Speaks **KWP/1** through `ckdbs_cli.ServerConnection` by default - the same
class `tools/lock_contention_benchmark.py:82,119` uses, which renders a
result set in the newline protocol's shape (`ckdbs_cli.py:160-230`), so
`sys_meta.view_rows` (`tools/sys_meta.py`) parses it. With `--text-port`
it uses `ckdbs_cli.TextConnection` on the server's `debug_text_port`
(`ckdbs_cli.py:129-158`) instead; the replies have the same shape.

What it reads (columns verified at cc54a296):

* `sys.lock_waits` - `docs/spec/sys-views.md:183-194` (section 6.3),
  `src/server/sys_views.cpp:924-934`: `seq`, `core`, `started`,
  `waited_us`, `unit`, `mode`, `rel_oid`, `key`, `waiter_trx`,
  `holder_trx`, `outcome` (`granted`, `deadlock`, `fault_net`,
  `cancelled`). One row per lock wait that parked, written when it ends;
  a ring of 4096 per core. Aggregated here by `(unit, mode, outcome)`:
  rows, `waited_us` sum, max, mean. The raw rows are kept too.
* `sys.wait_stats` - `docs/spec/sys-views.md:226-233` (section 6.6),
  `src/server/sys_views.cpp:1058-1063`: `core`, `event`, `class`, `count`,
  `total_us`, `max_us`; `core` is NULL on the per-event total rows. The
  rows for `app.lock_wait` (timed, `docs/spec/wait-events.md:208-219`) and
  `app.lock_conflict` (a counter, `wait-events.md:234-245`) are picked out,
  and `app.lock_wait`'s share of DB time is computed from the NULL-core
  total rows: DB time = `cpu.on_cpu` + `sched.run_queue` + every timed
  event outside `idle.*` (`sys-views.md:247-250`).
* `sys.meta` - the `lock_waits_recorded` / `_overwritten` / `_dropped`
  counters (`sys-views.md:194`) and the `core` field, by prefix.
* `sys.ash` (unless `--no-ash`) - `sys-views.md:254-280` (section 6.7), sampled
  once per `ash_interval_ms`; aggregated by `(event, state)`.

Every view is read in its own try/except: a refused view is recorded as
an error string and never fails the cell. Output: `PREFIX.views.json`
(everything, raw rows included) and `PREFIX.views.txt` (the aggregates).
"""

import argparse
import collections
import json
import os
import sys

DEFAULT_TOOLS = "/home/louis/ckdbs/.claude/worktrees/cp-lock-model-exploration/tools"


def _num(text):
    if text is None or text == "NULL":
        return None
    try:
        return int(text)
    except ValueError:
        try:
            return float(text)
        except ValueError:
            return text


def read_view(conn, statement, view_rows):
    reply = conn.send_command(statement)
    if reply.startswith("ERR"):
        return None, reply
    return view_rows(reply), None


def aggregate_lock_waits(rows):
    agg = collections.OrderedDict()
    for r in rows:
        k = (r.get("unit", "?"), r.get("mode", "?"), r.get("outcome", "?"))
        w = _num(r.get("waited_us")) or 0
        a = agg.setdefault(k, {"unit": k[0], "mode": k[1], "outcome": k[2],
                               "rows": 0, "waited_us_sum": 0, "waited_us_max": 0})
        a["rows"] += 1
        a["waited_us_sum"] += w
        a["waited_us_max"] = max(a["waited_us_max"], w)
    out = []
    for a in agg.values():
        a["waited_us_mean"] = round(a["waited_us_sum"] / a["rows"], 1) if a["rows"] else 0.0
        out.append(a)
    out.sort(key=lambda a: (-a["rows"], a["unit"], a["mode"], a["outcome"]))
    by_outcome = collections.Counter(r.get("outcome", "?") for r in rows)
    by_core = collections.Counter(r.get("core", "?") for r in rows)
    return {"rows_total": len(rows), "by_unit_mode_outcome": out,
            "by_outcome": dict(by_outcome), "by_core": dict(by_core)}


def lock_wait_share(wait_rows):
    """`app.lock_wait` and `app.lock_conflict` rows, and the share of DB time
    from the NULL-core totals."""
    picked = [r for r in wait_rows if r.get("event") in ("app.lock_wait", "app.lock_conflict")]
    totals = {r["event"]: r for r in wait_rows if r.get("core") == "NULL"}
    db_time_us = 0
    parts = {}
    for ev, r in totals.items():
        if ev.startswith("idle."):
            continue
        t = _num(r.get("total_us")) or 0
        if t:
            parts[ev] = t
        db_time_us += t
    lw = _num(totals.get("app.lock_wait", {}).get("total_us")) or 0
    share = (lw / db_time_us) if db_time_us else None
    return {
        "app_rows": picked,
        "totals_non_idle_us": parts,
        "db_time_us": db_time_us,
        "app_lock_wait_total_us": lw,
        "app_lock_wait_count": _num(totals.get("app.lock_wait", {}).get("count")),
        "app_lock_wait_max_us": _num(totals.get("app.lock_wait", {}).get("max_us")),
        "app_lock_conflict_count": _num(totals.get("app.lock_conflict", {}).get("count")),
        "app_lock_wait_share_of_db_time": round(share, 6) if share is not None else None,
    }


def aggregate_ash(rows):
    c = collections.Counter((r.get("event", "?"), r.get("state", "?")) for r in rows)
    return {"samples": len(rows),
            "by_event_state": [{"event": e, "state": s, "samples": n}
                               for (e, s), n in c.most_common()]}


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, required=True, help="the server's KWP port")
    p.add_argument("--text-port", type=int, default=None,
                   help="read over the newline debug surface at this port instead of KWP")
    p.add_argument("--out", required=True, help="output prefix: PREFIX.views.json/.txt")
    p.add_argument("--no-ash", action="store_true")
    p.add_argument("--tools", default=DEFAULT_TOOLS, help="the repo's tools/ directory")
    p.add_argument("--label", default="")
    a = p.parse_args()

    sys.path.insert(0, a.tools)
    import ckdbs_cli  # noqa: E402
    import sys_meta  # noqa: E402

    if a.text_port:
        conn = ckdbs_cli.TextConnection(a.host, a.text_port, timeout=60.0)
        protocol = f"newline debug surface :{a.text_port}"
    else:
        conn = ckdbs_cli.ServerConnection(a.host, a.port, timeout=60.0)
        protocol = f"KWP/1 :{a.port}"

    result = {"label": a.label, "protocol": protocol, "port": a.port, "errors": {}}

    rows, err = read_view(conn, "SELECT * FROM sys.lock_waits", sys_meta.view_rows)
    if err:
        result["errors"]["sys.lock_waits"] = err
    else:
        result["lock_waits"] = aggregate_lock_waits(rows)
        result["lock_waits_rows"] = rows

    rows, err = read_view(conn, "SELECT * FROM sys.wait_stats", sys_meta.view_rows)
    if err:
        result["errors"]["sys.wait_stats"] = err
    else:
        result["wait_stats"] = lock_wait_share(rows)
        result["wait_stats_rows"] = rows

    try:
        meta = sys_meta.from_text(conn.send_command("SHOW META"))
        result["meta"] = {k: v for k, v in meta.items()
                          if k.startswith("lock_waits") or k in ("core", "cores", "durability",
                                                                   "wal_topology", "version")}
    except Exception as e:  # a refused SHOW META is a finding, not a crash
        result["errors"]["sys.meta"] = str(e)

    if not a.no_ash:
        rows, err = read_view(conn, "SELECT * FROM sys.ash", sys_meta.view_rows)
        if err:
            result["errors"]["sys.ash"] = err
        else:
            result["ash"] = aggregate_ash(rows)
            result["ash_rows"] = rows

    try:
        conn.close()
    except Exception:
        pass

    os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
    with open(a.out + ".views.json", "w") as f:
        json.dump(result, f, indent=2)

    lines = [f"views read over {protocol} {a.label}".rstrip()]
    lw = result.get("lock_waits")
    if lw:
        lines.append(f"sys.lock_waits: {lw['rows_total']} rows; by outcome {lw['by_outcome']}; "
                     f"by core {lw['by_core']}")
        lines.append(f"  {'unit':<14}{'mode':<6}{'outcome':<10}{'rows':>8}{'sum_us':>12}"
                     f"{'max_us':>10}{'mean_us':>10}")
        for r in lw["by_unit_mode_outcome"]:
            lines.append(f"  {r['unit']:<14}{r['mode']:<6}{r['outcome']:<10}{r['rows']:>8}"
                         f"{r['waited_us_sum']:>12}{r['waited_us_max']:>10}"
                         f"{r['waited_us_mean']:>10}")
    ws = result.get("wait_stats")
    if ws:
        lines.append(f"sys.wait_stats: app.lock_wait count={ws['app_lock_wait_count']} "
                     f"total_us={ws['app_lock_wait_total_us']} max_us={ws['app_lock_wait_max_us']}; "
                     f"app.lock_conflict count={ws['app_lock_conflict_count']}; "
                     f"DB time {ws['db_time_us']} us; "
                     f"lock_wait share {ws['app_lock_wait_share_of_db_time']}")
        for r in ws["app_rows"]:
            lines.append("  " + ",".join(f"{k}={v}" for k, v in r.items()))
    if "meta" in result:
        lines.append("sys.meta: " + ", ".join(f"{k}={v}" for k, v in sorted(result["meta"].items())))
    ash = result.get("ash")
    if ash:
        lines.append(f"sys.ash: {ash['samples']} samples")
        for r in ash["by_event_state"][:12]:
            lines.append(f"  {r['event']:<28}{r['state']:<10}{r['samples']:>8}")
    if result["errors"]:
        lines.append("errors: " + json.dumps(result["errors"]))
    text = "\n".join(lines) + "\n"
    with open(a.out + ".views.txt", "w") as f:
        f.write(text)
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
