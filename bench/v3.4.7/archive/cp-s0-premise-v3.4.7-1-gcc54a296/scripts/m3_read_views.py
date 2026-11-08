#!/usr/bin/env python3
"""CP-S0 M3: read the lock family's instruments off a running kds_server and
save them as JSON plus a text summary. Preparation for the order's M3 cell
(`instructions/v3.2.0/workorder-cp-lock-model-exploration.md:594`, the
CP-S0 row): outcomes by unit and mode, `app.lock_wait`'s share of DB time,
deadlock and fault_net counts, the ASH view of the same run.

    read_views.py --port P --out PREFIX [--label L] [--text-port D]
                  [--baseline OTHER.views.json] [--no-ash]
                  [--set-ash-interval MS] [--tools DIR]

Protocol. The two scenario drivers speak **KWP/1**: both import
`ServerConnection` from `tools/ckdbs_cli.py` (`scenario2_freight.py:71`,
`:270`; `scenario0_stockmarket.py:155`, `:305`), and that class wraps
`kwp.Connection` (`ckdbs_cli.py:160-230`), rendering a result set in the
newline protocol's shape (a header line, then `\\n`-joined comma rows).
`tools/bench_common.py` has no `connect` or `Executor`; the newline debug
surface is `ckdbs_cli.TextConnection` (`ckdbs_cli.py:129-158`), reached only
on a server whose `debug_text_port` is open (`kds.conf.sample:314`,
`expeditor.cpp:671-677`). This script uses KWP by default - the drivers'
channel - and `--text-port` switches to the debug surface; the parser is the
same for both (`tools/sys_meta.py:view_rows`, `from_text`).

Aggregation is done here, in Python: aggregation over a `sys` view is
refused (AG12, `docs/spec/aggregate.md:29`; `docs/spec/sys-views.md:56-61`,
"Refused: aggregation ... A panel that needs counts reads the rows and
counts them"), so `SELECT unit, mode, outcome, COUNT(*) ... FROM
sys.lock_waits GROUP BY ...` is not a statement the engine answers.

What is read, with the columns verified at cc54a296:

(a) `sys.lock_waits` (`docs/spec/sys-views.md:183-194` §6.3;
    `src/server/sys_views.cpp:924-934`; `include/kds/stats/lock_wait_log.hpp:44-59`):
    `seq`, `core`, `started`, `waited_us`, `unit`, `mode`, `rel_oid`, `key`,
    `waiter_trx`, `holder_trx`, `outcome`. `unit` is one of `relation`,
    `range`, `slice`, `tuple`, `index key` (`src/txn/lock_table.cpp:32-46`);
    `mode` one of `IS`, `IX`, `S`, `X` (`:48-60`); `outcome` one of
    `granted`, `deadlock`, `fault_net`, `cancelled`
    (`src/stats/lock_wait_log.cpp:5-13`). One row per wait that parked,
    written when it ends; a ring of 4096 per core (`lock_wait_log.hpp:64`),
    so `lock_waits_overwritten` in `sys.meta` says whether rows were lost.
    Grouped here by (unit, mode, outcome) and by (relation, unit, mode,
    outcome): rows, sum / max / mean / p50 of `waited_us`.
(b) `sys.wait_stats` (`sys-views.md:226-235` §6.6; `sys_views.cpp:1058-1063`):
    `core`, `event`, `class`, `count`, `total_us`, `max_us`; `core` is NULL
    on the per-event total rows. DB time (I3, `sys-views.md:247-250`) is
    ON_CPU + `sched.run_queue` + every timed event outside `idle.*`, which
    is exactly the sum of `total_us` over the NULL-core rows whose event
    the registry marks `timed` and `in_db_time` (`sys.wait_event_names`,
    `sys_views.cpp:1004-1011`; `include/kds/stats/wait_event.hpp:118-162`:
    the counters `app.lock_conflict` and `app.assertion_reservation` book
    no time). `app.lock_wait` (`docs/spec/wait-events.md:208-219`, timed, p1
    = holding_trx) is reported as a share of that DB time, of `cpu.on_cpu`,
    and of the statement time in (d). `app.lock_conflict`
    (`wait-events.md:234-245`, a counter at the lock table's two refusal
    exits) is reported as a count.
(c) `sys.ash` (`sys-views.md:254-280` §6.7; `sys_views.cpp:1174-1188`):
    `sample_ts_us`, `core`, `session_id`, `session_kind`, `state`, `event`,
    `class`, `sched_group`, `late`, `arg_hash_valid`, `fingerprint`, `p1`,
    `p2`, `p3`, `wait_elapsed_us`. Counted here by event (top 10), by state,
    by core; the `app.lock_wait` samples' `p1` (the holder) distribution;
    the span and the implied sampling interval. The sampler needs
    `ash_enabled = on` (default on) and `ash_interval_ms` in [100, 10000]
    (`sys-views.md:277-278`; `include/kds/sched/ash_sampler.hpp:56-57`;
    checked at boot by `expeditor.cpp:377-385` and at runtime by
    `SET ASH INTERVAL_MS n`, `command_dispatcher.cpp:2285-2335`, admin -
    the statement-class roles are off by default). At 100 ms a 60 s run
    gives ~600 grid points per core; a sample is one row per active task
    at a grid point, so expect several times that. A core with no active
    task at a grid point emits no row, so `grid_points_per_core` and the
    implied interval describe the active span, not the sampler's clock.
    `ash_window` (below) is the same aggregate over the concurrent window.
(d) `sys.statement_stats` (`sys-views.md:153-165` §6.1; `sys_views.cpp:951-959`):
    `fetch_id`, `kind`, `calls`, `refused`, `total_us`, `max_us`,
    `first_seen`, `last_seen`, `shape_text`. Totals by kind, the grand
    total, the top shapes by `total_us`.
(e) `sys.meta` (`SHOW META`, `sys-views.md:283-292` §7): the `lock_waits`
    section (`lock_waits_recorded`, `_overwritten`, `_dropped`;
    `command_dispatcher.cpp:2214-2220`) and the `contention` section
    (`:2243-2256`): `contention_<latch>_waits` / `_wait_us` for every
    `LatchKind` - `lock_partition` and `lock_wait_for` are the family's
    (`include/kds/base/contention.hpp:46-47`, names `:63-67`) - and
    `contention_<tally>` for every `Tally` (`contention.hpp:72-139`):
    `fence_walks` (the family's only ask tally, `:109`, name `:137`),
    `inflight_asks`, `inflight_misses`, `inflight_miss_ids_read`
    (`:135-136`). The `load` section repeats per core **only** the latch
    waits, as `load_c<k>_waited_<latch>_us` and only where non-zero
    (`command_dispatcher.cpp:2116-2121`), and four WAL/window tallies
    (`wal_drain_latch_wait_us`, `wal_roll_blocked_waits`,
    `wal_roll_blocked_wait_us`, `visibility_window_lookups`, `:2126-2134`);
    `fence_walks` and the `inflight_*` tallies are instance totals in the
    `contention` section and have no per-core field. No tally counts lock
    asks (the order's §1.4, last bullet).

Separating the run from the load. `sys.wait_stats` is cumulative and not
per statement, so a post-minus-pre delta that spans a single-connection
load (scenario 0's load; scenario 2's run invocation reloads the reference
data before it books, `scenario2_freight.py:1634-1651`) dilutes
`app.lock_wait`'s share of DB time with load time in which no lock can be
contended. The ASH rows carry a timestamp, so the multi-session window is
recoverable from them: `ash_window` keeps the 1 s bins in which at least
`--window-min-sessions` (default 2) distinct client sessions were sampled
instance-wide and aggregates those rows alone. Within it the share of
samples on `app.lock_wait` is the sampling estimate of that event's share
of DB time (`sys-views.md` §6.7: a sample is one row per active task at a
grid point, CPU time reads as RUNNABLE, idle sessions are not sampled), so
the results file quotes both: the wait_stats delta (exact, diluted) and the
window share (an estimate, undiluted).

`--baseline` takes an earlier `.views.json` of the same server and adds
deltas: `wait_stats`, `statement_stats` and the numeric `sys.meta` fields
are cumulative since boot, so post minus pre is the run's own;
`lock_waits` rows are kept where `seq` is past the baseline's per-core
maximum, `ash` rows where `sample_ts_us` is past the baseline's maximum.

Every view is read in its own try/except; a refused view is recorded under
`errors` and never fails the cell. Output: `PREFIX.views.json` (everything,
raw rows included) and `PREFIX.views.txt` (the summary, also printed).
"""

import argparse
import collections
import datetime
import json
import os
import re
import statistics
import sys

DEFAULT_TOOLS = "/home/louis/ckdbs/.claude/worktrees/cp-lock-model-exploration/tools"

LOCK_WAIT = "app.lock_wait"
LOCK_CONFLICT = "app.lock_conflict"
ON_CPU = "cpu.on_cpu"
RUN_QUEUE = "sched.run_queue"

# `sys.meta` fields the summary picks out (the JSON keeps every field).
META_PICK = (
    "lock_waits_recorded", "lock_waits_overwritten", "lock_waits_dropped",
    "contention_lock_partition_waits", "contention_lock_partition_wait_us",
    "contention_lock_wait_for_waits", "contention_lock_wait_for_wait_us",
    "contention_fence_walks",
    "contention_inflight_asks", "contention_inflight_misses",
    "contention_inflight_miss_ids_read",
    "contention_structural_reruns",
    "statement_stats_dropped",
)
# The `load` section's per-core fields the family owns: the two lock latches'
# waits (`command_dispatcher.cpp:2116-2121`, printed only when non-zero). No
# per-core tally of the family exists there (see the docstring, (e)).
META_PER_CORE = re.compile(r"^load_c\d+_waited_lock_(partition|wait_for)_us$")


def _num(text):
    """A view cell as a number: None for NULL, int when it parses, else the
    text itself (names, timestamps)."""
    if text is None or text == "NULL":
        return None
    try:
        return int(text)
    except (ValueError, TypeError):
        try:
            return float(text)
        except (ValueError, TypeError):
            return text


def _int(text, default=0):
    v = _num(text)
    return v if isinstance(v, (int, float)) else default


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def read_view(conn, statement, view_rows):
    """(rows, error): the view's rows as dicts, or the refusal text."""
    try:
        reply = conn.send_command(statement)
    except Exception as e:  # noqa: BLE001 - a lost connection is a finding, not a crash
        return None, f"{type(e).__name__}: {e}"
    if reply.startswith("ERR"):
        return None, reply
    try:
        return view_rows(reply), None
    except Exception as e:  # noqa: BLE001
        return None, f"unparseable reply: {type(e).__name__}: {e}; head={reply[:120]!r}"


def p50(values):
    return statistics.median(values) if values else 0


# ---- (a) sys.lock_waits ----------------------------------------------------

def aggregate_lock_waits(rows, rel_names):
    def group(keyf, label):
        agg = collections.OrderedDict()
        for r in rows:
            k = keyf(r)
            a = agg.setdefault(k, {"rows": 0, "waits": []})
            a["rows"] += 1
            a["waits"].append(_int(r.get("waited_us")))
        out = []
        for k, a in agg.items():
            w = a["waits"]
            row = dict(zip(label, k))
            row.update({"rows": a["rows"], "waited_us_sum": sum(w), "waited_us_max": max(w),
                        "waited_us_mean": round(sum(w) / len(w), 1), "waited_us_p50": p50(w)})
            out.append(row)
        out.sort(key=lambda a: (-a["rows"], str(a)))
        return out

    def relname(r):
        oid = _num(r.get("rel_oid"))
        return rel_names.get(oid, f"oid {oid}")

    by_umo = group(lambda r: (r.get("unit", "?"), r.get("mode", "?"), r.get("outcome", "?")),
                   ("unit", "mode", "outcome"))
    by_rel = group(lambda r: (relname(r), r.get("unit", "?"), r.get("mode", "?"),
                              r.get("outcome", "?")),
                   ("relation", "unit", "mode", "outcome"))
    outcomes = collections.Counter(r.get("outcome", "?") for r in rows)
    holders = collections.Counter(r.get("holder_trx", "NULL") for r in rows)
    waits = [_int(r.get("waited_us")) for r in rows]
    return {
        "rows_total": len(rows),
        "by_outcome": dict(outcomes),
        "deadlock": outcomes.get("deadlock", 0),
        "fault_net": outcomes.get("fault_net", 0),
        "cancelled": outcomes.get("cancelled", 0),
        "granted": outcomes.get("granted", 0),
        "by_unit_mode_outcome": by_umo,
        "by_relation_unit_mode_outcome": by_rel,
        "by_core": dict(collections.Counter(r.get("core", "?") for r in rows)),
        "waited_us_sum": sum(waits),
        "waited_us_max": max(waits) if waits else 0,
        "waited_us_p50": p50(waits),
        "holder_trx_top": holders.most_common(10),
        "distinct_holders": len(holders),
        "waiter_without_trx": sum(1 for r in rows if r.get("waiter_trx") == "NULL"),
    }


def lock_waits_after(rows, baseline_rows):
    """The rows past the baseline's per-core `seq` high-water mark."""
    hw = {}
    for r in baseline_rows or []:
        c = r.get("core")
        hw[c] = max(hw.get(c, 0), _int(r.get("seq")))
    return [r for r in rows if _int(r.get("seq")) > hw.get(r.get("core"), 0)]


# ---- (b) sys.wait_stats ----------------------------------------------------

def event_registry(names_rows):
    """{event: (kind, in_db_time)} from `sys.wait_event_names`; the
    registry's defaults when the view was refused."""
    reg = {}
    for r in names_rows or []:
        reg[r.get("name")] = (r.get("kind"), _int(r.get("in_db_time")) == 1)
    if not reg:
        for ev in ("cpu.on_cpu", "commit.wal_flush", "commit.ceiling_wait", "io.page_read",
                   "io.var_heap_read", "io.scan_ring_read", "sysio.writeback",
                   "sysio.checkpoint", "sysio.wal_write", "xcore.step_response",
                   "xcore.credit", "sched.run_queue", "res.eviction_retry",
                   "net.send_backpressure", "app.lock_wait", "app.horizon_wait"):
            reg[ev] = ("timed", True)
        reg["app.lock_conflict"] = ("counter", True)
        reg["app.assertion_reservation"] = ("counter", True)
        reg["idle.client"] = ("timed", False)
        reg["idle.background"] = ("timed", False)
    return reg


def wait_totals(rows):
    """{event: {count, total_us, max_us}} from the NULL-core total rows."""
    return {r["event"]: {"count": _int(r.get("count")), "total_us": _int(r.get("total_us")),
                         "max_us": _int(r.get("max_us"))}
            for r in rows if r.get("core") == "NULL" and r.get("event")}


def db_time_of(totals, reg):
    parts = {}
    for ev, t in totals.items():
        kind, in_db = reg.get(ev, ("timed", not ev.startswith("idle.")))
        if kind != "timed" or not in_db:
            continue
        if t["total_us"]:
            parts[ev] = t["total_us"]
    return sum(parts.values()), parts


def wait_stats_summary(rows, reg, baseline_rows=None):
    totals = wait_totals(rows)
    db_us, parts = db_time_of(totals, reg)
    lw = totals.get(LOCK_WAIT, {"count": 0, "total_us": 0, "max_us": 0})
    lc = totals.get(LOCK_CONFLICT, {"count": 0, "total_us": 0, "max_us": 0})
    cpu = totals.get(ON_CPU, {"total_us": 0})["total_us"]

    def share(n, d):
        return round(n / d, 6) if d else None

    out = {
        "db_time_us": db_us,
        "db_time_parts_us": dict(sorted(parts.items(), key=lambda kv: -kv[1])),
        "cpu_on_cpu_us": cpu,
        "lock_wait": lw,
        "lock_conflict_count": lc["count"],
        "lock_wait_share_of_db_time": share(lw["total_us"], db_us),
        "lock_wait_share_of_cpu": share(lw["total_us"], cpu),
        "per_core": [],
    }
    # Per-core share, from the non-NULL rows.
    by_core = collections.defaultdict(dict)
    for r in rows:
        if r.get("core") != "NULL":
            by_core[_int(r.get("core"))][r.get("event")] = {
                "count": _int(r.get("count")), "total_us": _int(r.get("total_us")),
                "max_us": _int(r.get("max_us"))}
    for core in sorted(by_core):
        c_db, _ = db_time_of(by_core[core], reg)
        c_lw = by_core[core].get(LOCK_WAIT, {"count": 0, "total_us": 0})
        out["per_core"].append({"core": core, "db_time_us": c_db,
                                "lock_wait_count": c_lw["count"],
                                "lock_wait_us": c_lw["total_us"],
                                "lock_wait_share": share(c_lw["total_us"], c_db),
                                "lock_conflicts": by_core[core].get(LOCK_CONFLICT,
                                                                    {"count": 0})["count"]})
    if baseline_rows is not None:
        base = wait_totals(baseline_rows)
        delta = {}
        for ev, t in totals.items():
            b = base.get(ev, {"count": 0, "total_us": 0})
            d = {"count": t["count"] - b["count"], "total_us": t["total_us"] - b["total_us"]}
            if d["count"] or d["total_us"]:
                delta[ev] = d
        d_db, d_parts = db_time_of({ev: {"total_us": d["total_us"]} for ev, d in delta.items()},
                                   reg)
        d_lw = delta.get(LOCK_WAIT, {"count": 0, "total_us": 0})
        out["delta"] = {
            "events": delta,
            "db_time_us": d_db,
            "db_time_parts_us": dict(sorted(d_parts.items(), key=lambda kv: -kv[1])),
            "cpu_on_cpu_us": delta.get(ON_CPU, {"total_us": 0})["total_us"],
            "lock_wait": d_lw,
            "lock_conflict_count": delta.get(LOCK_CONFLICT, {"count": 0})["count"],
            "lock_wait_share_of_db_time": share(d_lw["total_us"], d_db),
            "lock_wait_share_of_cpu": share(d_lw["total_us"],
                                            delta.get(ON_CPU, {"total_us": 0})["total_us"]),
        }
    return out


# ---- (c) sys.ash -----------------------------------------------------------

def aggregate_ash(rows):
    events = collections.Counter(r.get("event", "?") for r in rows)
    states = collections.Counter(r.get("state", "?") for r in rows)
    kinds = collections.Counter(r.get("session_kind", "?") for r in rows)
    cores = collections.Counter(_int(r.get("core")) for r in rows)
    late = sum(1 for r in rows if _int(r.get("late")) == 1)
    ts = sorted({_int(r.get("sample_ts_us")) for r in rows})
    # Grid points per core: distinct timestamps per core; the implied interval
    # is the span over the points minus one.
    per_core_ts = collections.defaultdict(set)
    for r in rows:
        per_core_ts[_int(r.get("core"))].add(_int(r.get("sample_ts_us")))
    implied = {}
    for core, s in per_core_ts.items():
        if len(s) > 1:
            implied[core] = round((max(s) - min(s)) / (len(s) - 1) / 1000.0, 1)
    lw = [r for r in rows if r.get("event") == LOCK_WAIT]
    lw_p1 = collections.Counter(r.get("p1", "NULL") for r in lw)
    lw_fp = collections.Counter(r.get("fingerprint", "0") for r in lw)
    lw_elapsed = [_int(r.get("wait_elapsed_us")) for r in lw]
    return {
        "samples": len(rows),
        "span_us": (ts[-1] - ts[0]) if len(ts) > 1 else 0,
        "grid_points_per_core": {c: len(s) for c, s in sorted(per_core_ts.items())},
        "implied_interval_ms_per_core": implied,
        "late_samples": late,
        "by_event_top10": events.most_common(10),
        "by_state": dict(states),
        "by_session_kind": dict(kinds),
        "by_core": dict(sorted(cores.items())),
        "lock_wait": {
            "samples": len(lw),
            "share_of_samples": round(len(lw) / len(rows), 6) if rows else None,
            "p1_holder_top10": lw_p1.most_common(10),
            "distinct_holders": len(lw_p1),
            "by_fingerprint_top5": lw_fp.most_common(5),
            "wait_elapsed_us_p50": p50(lw_elapsed),
            "wait_elapsed_us_max": max(lw_elapsed) if lw_elapsed else 0,
        },
    }


def ash_after(rows, baseline_rows):
    hw = max((_int(r.get("sample_ts_us")) for r in baseline_rows or []), default=-1)
    return [r for r in rows if _int(r.get("sample_ts_us")) > hw]


def ash_window(rows, min_sessions=2, bin_us=1_000_000):
    """The concurrent window of the run: the `bin_us` bins (instance-wide, on
    the sampler's monotonic clock) in which at least `min_sessions` distinct
    client sessions (`session_kind` = `session`, keyed by (core, session_id))
    were sampled, and `aggregate_ash` over the rows of those bins alone.
    A single-connection load, the pre/post view reads and an idle server all
    fall outside it; 8 bookers or 8 traders fall inside."""
    per_bin = collections.defaultdict(set)
    for r in rows:
        if r.get("session_kind") == "session":
            per_bin[_int(r.get("sample_ts_us")) // bin_us].add((r.get("core"), r.get("session_id")))
    bins = {b for b, s in per_bin.items() if len(s) >= min_sessions}
    inside = [r for r in rows if _int(r.get("sample_ts_us")) // bin_us in bins]
    agg = aggregate_ash(inside)
    agg["window"] = {"min_sessions": min_sessions, "bin_s": bin_us / 1e6, "bins": len(bins),
                     "rows_outside": len(rows) - len(inside),
                     "max_sessions_in_a_bin": max((len(s) for s in per_bin.values()), default=0)}
    return agg


# ---- (d) sys.statement_stats -----------------------------------------------

def statement_summary(rows, baseline_rows=None):
    by_kind = collections.OrderedDict()
    for r in rows:
        k = r.get("kind", "?")
        a = by_kind.setdefault(k, {"kind": k, "shapes": 0, "calls": 0, "refused": 0, "total_us": 0,
                                   "max_us": 0})
        a["shapes"] += 1
        a["calls"] += _int(r.get("calls"))
        a["refused"] += _int(r.get("refused"))
        a["total_us"] += _int(r.get("total_us"))
        a["max_us"] = max(a["max_us"], _int(r.get("max_us")))
    top = sorted(rows, key=lambda r: -_int(r.get("total_us")))[:10]
    out = {
        "shapes": len(rows),
        "calls": sum(a["calls"] for a in by_kind.values()),
        "refused": sum(a["refused"] for a in by_kind.values()),
        "total_us": sum(a["total_us"] for a in by_kind.values()),
        "by_kind": sorted(by_kind.values(), key=lambda a: -a["total_us"]),
        "top_shapes_by_total_us": [
            {"fetch_id": r.get("fetch_id"), "kind": r.get("kind"), "calls": _int(r.get("calls")),
             "refused": _int(r.get("refused")), "total_us": _int(r.get("total_us")),
             "max_us": _int(r.get("max_us")), "shape_text": (r.get("shape_text") or "")[:120]}
            for r in top],
    }
    if baseline_rows is not None:
        base = {(r.get("fetch_id"), r.get("kind")): r for r in baseline_rows}
        d_rows = []
        for r in rows:
            b = base.get((r.get("fetch_id"), r.get("kind")), {})
            d = dict(r)
            d["calls"] = _int(r.get("calls")) - _int(b.get("calls"))
            d["refused"] = _int(r.get("refused")) - _int(b.get("refused"))
            d["total_us"] = _int(r.get("total_us")) - _int(b.get("total_us"))
            if d["calls"] or d["total_us"]:
                d_rows.append(d)
        delta = statement_summary(d_rows)
        delta.pop("shapes", None)
        out["delta"] = delta
    return out


# ---- (e) sys.meta ------------------------------------------------------------

def meta_summary(meta, baseline_meta=None):
    picked = {k: meta.get(k) for k in META_PICK if k in meta}
    per_core = {k: v for k, v in meta.items() if META_PER_CORE.match(k)}
    latch_waits = {k: v for k, v in meta.items()
                   if k.startswith("contention_") and (k.endswith("_waits") or k.endswith("_wait_us"))}
    out = {"picked": picked, "per_core": dict(sorted(per_core.items())),
           "latch_waits": dict(sorted(latch_waits.items()))}
    if baseline_meta is not None:
        delta = {}
        for k, v in meta.items():
            if not (k.startswith("contention_") or k.startswith("lock_waits") or META_PER_CORE.match(k)
                    or k.startswith("statement_stats")):
                continue
            a, b = _num(v), _num(baseline_meta.get(k))
            if isinstance(a, (int, float)) and isinstance(b, (int, float)) and a != b:
                delta[k] = a - b
        out["delta"] = dict(sorted(delta.items()))
    return out


# ---- main --------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, required=True, help="the server's KWP/1 port")
    p.add_argument("--text-port", type=int, default=None,
                   help="read over the newline debug surface (debug_text_port) instead of KWP")
    p.add_argument("--out", required=True, help="output prefix: PREFIX.views.json and .txt")
    p.add_argument("--label", default="")
    p.add_argument("--baseline", default=None,
                   help="an earlier PREFIX.views.json of this server; adds deltas")
    p.add_argument("--no-ash", action="store_true")
    p.add_argument("--set-ash-interval", type=int, default=None, metavar="MS",
                   help="send `SET ASH INTERVAL_MS MS` first (100-10000); the reply is recorded")
    p.add_argument("--tools", default=DEFAULT_TOOLS, help="the repo's tools/ directory")
    p.add_argument("--timeout", type=float, default=120.0)
    p.add_argument("--window-min-sessions", type=int, default=2, metavar="N",
                   help="ASH concurrent window: 1 s bins with at least N distinct client "
                        "sessions sampled (default 2; separates a multi-session run from a "
                        "single-connection load)")
    a = p.parse_args()

    sys.path.insert(0, a.tools)
    import ckdbs_cli  # noqa: E402
    import sys_meta  # noqa: E402

    if a.text_port:
        conn = ckdbs_cli.TextConnection(a.host, a.text_port, timeout=a.timeout)
        protocol = f"newline debug surface :{a.text_port}"
    else:
        conn = ckdbs_cli.ServerConnection(a.host, a.port, timeout=a.timeout)
        protocol = f"KWP/1 :{a.port}"

    baseline = None
    if a.baseline:
        with open(a.baseline) as f:
            baseline = json.load(f)

    result = {"label": a.label, "read_at_utc": utc_now(), "protocol": protocol,
              "port": a.port, "baseline": a.baseline, "errors": {}, "raw": {}}

    if a.set_ash_interval is not None:
        try:
            result["set_ash_interval_reply"] = conn.send_command(
                f"SET ASH INTERVAL_MS {a.set_ash_interval}")
        except Exception as e:  # noqa: BLE001
            result["errors"]["set_ash"] = str(e)

    # Relation names, for the by-relation grouping of lock waits.
    rel_names = {}
    rows, err = read_view(conn, "SELECT * FROM sys.tables", sys_meta.view_rows)
    if err:
        result["errors"]["sys.tables"] = err
    else:
        for r in rows:
            rel_names[_num(r.get("oid"))] = f"{r.get('namespace', '')}.{r.get('name', '')}"
        result["raw"]["tables"] = rows

    names_rows, err = read_view(conn, "SELECT * FROM sys.wait_event_names", sys_meta.view_rows)
    if err:
        result["errors"]["sys.wait_event_names"] = err
    reg = event_registry(names_rows)
    result["raw"]["wait_event_names"] = names_rows or []

    # (a)
    rows, err = read_view(conn, "SELECT * FROM sys.lock_waits", sys_meta.view_rows)
    if err:
        result["errors"]["sys.lock_waits"] = err
    else:
        result["raw"]["lock_waits"] = rows
        result["lock_waits"] = aggregate_lock_waits(rows, rel_names)
        if baseline is not None:
            new = lock_waits_after(rows, baseline.get("raw", {}).get("lock_waits"))
            result["lock_waits_since_baseline"] = aggregate_lock_waits(new, rel_names)

    # (b)
    rows, err = read_view(conn, "SELECT * FROM sys.wait_stats", sys_meta.view_rows)
    if err:
        result["errors"]["sys.wait_stats"] = err
    else:
        result["raw"]["wait_stats"] = rows
        result["wait_stats"] = wait_stats_summary(
            rows, reg, baseline.get("raw", {}).get("wait_stats") if baseline else None)

    # (d)
    rows, err = read_view(conn, "SELECT * FROM sys.statement_stats", sys_meta.view_rows)
    if err:
        result["errors"]["sys.statement_stats"] = err
    else:
        result["raw"]["statement_stats"] = rows
        result["statement_stats"] = statement_summary(
            rows, baseline.get("raw", {}).get("statement_stats") if baseline else None)

    # (e)
    try:
        meta = sys_meta.from_text(conn.send_command("SHOW META"))
        result["raw"]["meta"] = meta
        result["meta"] = meta_summary(meta, baseline.get("raw", {}).get("meta") if baseline else None)
    except Exception as e:  # noqa: BLE001
        result["errors"]["sys.meta"] = str(e)

    # (c)
    if not a.no_ash:
        rows, err = read_view(conn, "SELECT * FROM sys.ash", sys_meta.view_rows)
        if err:
            result["errors"]["sys.ash"] = err
        else:
            result["raw"]["ash"] = rows
            result["ash"] = aggregate_ash(rows)
            result["ash_window"] = ash_window(rows, a.window_min_sessions)
            if baseline is not None:
                new = ash_after(rows, baseline.get("raw", {}).get("ash"))
                result["ash_since_baseline"] = aggregate_ash(new)
                result["ash_window_since_baseline"] = ash_window(new, a.window_min_sessions)

    # lock_wait against statement time (the third denominator the order asks for).
    ws, ss = result.get("wait_stats"), result.get("statement_stats")
    if ws and ss:
        st = ss["total_us"]
        ws["lock_wait_share_of_statement_time"] = (
            round(ws["lock_wait"]["total_us"] / st, 6) if st else None)
        if "delta" in ws and "delta" in ss and ss["delta"]["total_us"]:
            ws["delta"]["lock_wait_share_of_statement_time"] = round(
                ws["delta"]["lock_wait"]["total_us"] / ss["delta"]["total_us"], 6)

    try:
        conn.close()
    except Exception:  # noqa: BLE001
        pass

    os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
    with open(a.out + ".views.json", "w") as f:
        json.dump(result, f, indent=1, default=str)

    text = render(result)
    with open(a.out + ".views.txt", "w") as f:
        f.write(text)
    sys.stdout.write(text)
    return 0


def render(result):
    L = [f"== views read over {result['protocol']} at {result['read_at_utc']} {result['label']}".rstrip()]
    if result.get("set_ash_interval_reply"):
        L.append(f"SET ASH reply: {result['set_ash_interval_reply']}")

    def lock_block(title, lw):
        L.append(f"-- {title}: {lw['rows_total']} rows; granted {lw['granted']} deadlock {lw['deadlock']} "
                 f"fault_net {lw['fault_net']} cancelled {lw['cancelled']}; waited_us sum {lw['waited_us_sum']} "
                 f"p50 {lw['waited_us_p50']} max {lw['waited_us_max']}; by core {lw['by_core']}")
        L.append(f"   {'unit':<11}{'mode':<5}{'outcome':<10}{'rows':>7}{'sum_us':>12}{'p50_us':>9}"
                 f"{'max_us':>10}{'mean_us':>10}")
        for r in lw["by_unit_mode_outcome"]:
            L.append(f"   {r['unit']:<11}{r['mode']:<5}{r['outcome']:<10}{r['rows']:>7}{r['waited_us_sum']:>12}"
                     f"{r['waited_us_p50']:>9}{r['waited_us_max']:>10}{r['waited_us_mean']:>10}")
        for r in lw["by_relation_unit_mode_outcome"][:12]:
            L.append(f"   {r['relation'][:40]:<41}{r['unit']:<11}{r['mode']:<5}{r['outcome']:<10}"
                     f"{r['rows']:>7}{r['waited_us_sum']:>12}{r['waited_us_max']:>10}")
        L.append(f"   holder_trx top: {lw['holder_trx_top'][:5]}; distinct holders {lw['distinct_holders']}; "
                 f"waiters with no trx {lw['waiter_without_trx']}")

    if "lock_waits" in result:
        lock_block("sys.lock_waits (whole ring)", result["lock_waits"])
    if "lock_waits_since_baseline" in result:
        lock_block("sys.lock_waits since baseline", result["lock_waits_since_baseline"])

    def wait_block(title, ws):
        L.append(f"-- {title}: DB time {ws['db_time_us']} us (cpu.on_cpu {ws['cpu_on_cpu_us']} us); "
                 f"app.lock_wait count {ws['lock_wait']['count']} total {ws['lock_wait']['total_us']} us "
                 f"max {ws['lock_wait'].get('max_us', '-')} us; share of DB time {ws['lock_wait_share_of_db_time']}, "
                 f"of CPU {ws['lock_wait_share_of_cpu']}, of statement time "
                 f"{ws.get('lock_wait_share_of_statement_time')}; app.lock_conflict {ws['lock_conflict_count']}")
        parts = list(ws["db_time_parts_us"].items())[:8]
        L.append("   DB time parts: " + ", ".join(f"{k} {v}" for k, v in parts))

    ws = result.get("wait_stats")
    if ws:
        wait_block("sys.wait_stats (since boot)", ws)
        for c in ws["per_core"]:
            L.append(f"   core {c['core']}: DB {c['db_time_us']} us, lock_wait {c['lock_wait_count']}x "
                     f"{c['lock_wait_us']} us share {c['lock_wait_share']}, conflicts {c['lock_conflicts']}")
        if "delta" in ws:
            wait_block("sys.wait_stats delta since baseline", ws["delta"])

    ss = result.get("statement_stats")
    if ss:
        L.append(f"-- sys.statement_stats (since boot): {ss['shapes']} shapes, {ss['calls']} calls, "
                 f"{ss['refused']} refused, {ss['total_us']} us")
        for k in ss["by_kind"][:8]:
            L.append(f"   {k['kind']:<10}{k['calls']:>9} calls {k['refused']:>6} refused {k['total_us']:>13} us "
                     f"max {k['max_us']}")
        if "delta" in ss:
            d = ss["delta"]
            L.append(f"   delta since baseline: {d['calls']} calls, {d['refused']} refused, {d['total_us']} us; "
                     + ", ".join(f"{k['kind']} {k['calls']}x/{k['total_us']}us" for k in d["by_kind"][:6]))

    m = result.get("meta")
    if m:
        L.append("-- sys.meta: " + ", ".join(f"{k}={v}" for k, v in m["picked"].items()))
        lp = {k: v for k, v in m["latch_waits"].items() if "lock" in k}
        L.append("   lock latches: " + ", ".join(f"{k}={v}" for k, v in lp.items()))
        if m.get("per_core"):
            L.append("   per core: " + ", ".join(f"{k}={v}" for k, v in m["per_core"].items()))
        if "delta" in m:
            L.append("   delta since baseline: " + ", ".join(f"{k}={v}" for k, v in m["delta"].items()))

    def ash_block(title, ash):
        L.append(f"-- {title}: {ash['samples']} samples over {ash['span_us'] / 1e6:.1f} s; grid points per core "
                 f"{ash['grid_points_per_core']}; implied interval ms {ash['implied_interval_ms_per_core']}; "
                 f"late {ash['late_samples']}; by state {ash['by_state']}; by kind {ash['by_session_kind']}")
        for ev, n in ash["by_event_top10"]:
            L.append(f"   {ev:<28}{n:>8}")
        lw = ash["lock_wait"]
        L.append(f"   app.lock_wait samples {lw['samples']} ({lw['share_of_samples']}); holders (p1) top "
                 f"{lw['p1_holder_top10'][:5]}; distinct {lw['distinct_holders']}; elapsed p50 "
                 f"{lw['wait_elapsed_us_p50']} max {lw['wait_elapsed_us_max']} us; fingerprints "
                 f"{lw['by_fingerprint_top5'][:3]}")

    def window_block(title, w):
        win = w["window"]
        ash_block(f"{title} [concurrent window: {win['bins']} bins of {win['bin_s']} s with >= "
                  f"{win['min_sessions']} sessions, max {win['max_sessions_in_a_bin']} in a bin, "
                  f"{win['rows_outside']} rows outside]", w)

    if "ash" in result:
        ash_block("sys.ash (whole ring)", result["ash"])
    if "ash_window" in result:
        window_block("sys.ash (whole ring)", result["ash_window"])
    if "ash_since_baseline" in result:
        ash_block("sys.ash since baseline", result["ash_since_baseline"])
    if "ash_window_since_baseline" in result:
        window_block("sys.ash since baseline", result["ash_window_since_baseline"])
    if result["errors"]:
        L.append("errors: " + json.dumps(result["errors"]))
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    sys.exit(main())
