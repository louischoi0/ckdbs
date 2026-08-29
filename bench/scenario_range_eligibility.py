#!/usr/bin/env python3
"""R4-M / IM1 (CK4) — would a scenario relation spread, and if it did,
could anything still read it?

The order gates the aggregate cell on this rather than on its results,
because the failure mode is a **refusal** and not a slow number.

## The instrument, after the obvious one turned out not to work

The obvious instrument is `SHOW META`'s `range_split_decline_detail`,
which prints `oid:gate=count`. It answers nothing here, and why is a
finding rather than a nuisance: **the pump is heap-only**
(`command_dispatcher.cpp`'s `heap_omitting_pk`, which requires
`clustered_type == kHeap`), so a btree relation never records row-id
demand, never causes a refill, and so never reaches `RangeEligible` at
all. Measured: 300 peer inserts into a btree relation leave
`rowid_refill_requests=0` and the decline detail absent, where the same
300 into a heap twin leave `requests=5 grants=5`. So D1 - the gate that
declines eighteen of the twenty-four relations below - is **structurally
invisible** to the counter that exists to say which gate to lift first.
Correct behaviour, and an under-reporting instrument.

So this probe asks the question CK4 actually needs, behaviourally: run a
peer workload against the relation, then **try to read it**. A relation
that did not spread reads normally; one that did is refused, and the
refusal is what the aggregate cell would meet. Each relation gets the
read shapes its scenario uses.

The schemas are copied from the drivers rather than imported, because the
drivers build them through connections to a running server with load
phases attached and this probe wants the DDL alone.

Usage:
    bench/scenario_range_eligibility.py --server build-release/kds_server \\
        --workdir ~/eligibility
"""

import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from multicore_benchmark import (  # noqa: E402
    Conn, check_host, collect_connections, field, stop_server, wait_for_port,
)

# (scenario, relation, columns, clustered) - lifted verbatim from each
# driver's own SCHEMA dict, at the commit this file lands with.
SCHEMAS = [
    ("s0-stockmarket", "accounts",
     "id int64, user_id int64, balance int64, asset_qty int64, "
     "trade_count int64, opened_day int32", "BTREE"),
    ("s0-stockmarket", "users",
     "id int64, name varchar, country varchar, tier int32, created_day int32", "BTREE"),
    ("s0-stockmarket", "assets",
     "id int64, symbol varchar, asset_class int32, last_price int64", "BTREE"),
    ("s0-stockmarket", "trades",
     "id int64, account_id int64, asset_id int64, side int32, qty int64, "
     "price int64, trade_day int32", "HEAP"),
    ("s0-stockmarket", "user_periodic_profit",
     "id int64, user_id int64, period_day int32, realized int64, "
     "trade_count int64", "HEAP"),

    ("s1-backtest", "exchanges",
     "id int64, code varchar, country varchar, tz_min int32", "BTREE"),
    ("s1-backtest", "symbols",
     "id int64, ticker varchar, exchange_id int64, sector int32, "
     "listed_session int32", "BTREE"),
    ("s1-backtest", "sessions",
     "id int64, session_no int32, year int32, month int32, dow int32", "BTREE"),
    ("s1-backtest", "daily_bars",
     "id int64, symbol_id int64, session_id int64, session_no int32, "
     "open int64, high int64, low int64, close int64, volume int64", "BTREE"),
    ("s1-backtest", "daily_stats",
     "id int64, bar_id int64, symbol_id int64, session_no int32, "
     "ret_bp int64, mom5_bp int64, mom20_bp int64, mom60_bp int64, "
     "mom120_bp int64, vol10_bp int64, vol20_bp int64, vol60_bp int64", "HEAP"),
    ("s1-backtest", "models",
     "id int64, name varchar, family int32, lookback int32, top_k int32, "
     "param_bp int64", "BTREE"),
    ("s1-backtest", "model_results",
     "id int64, model_id int64, period_no int32, session_no int32, "
     "pnl_bp int64, equity_bp int64, positions int32, trades int32", "HEAP"),

    ("s2-freight", "organizations",
     "id int64, org_code varchar, name varchar, country int32, org_type int32, "
     "credit_limit int64, outstanding int64, tier int32, contact varchar, "
     "registered_day int32, status int32", "BTREE"),
    ("s2-freight", "ships",
     "id int64, imo varchar, name varchar, ship_type int32, capacity_cbm int32, "
     "dwt int64, built_year int32, flag varchar, owner_id int64, "
     "home_port int32, status int32", "BTREE"),
    ("s2-freight", "operations",
     "id int64, ship_id int64, origin int32, destination int32, depart_day int32, "
     "arrive_day int32, status int32, booked_cbm int32, revenue int64", "BTREE"),
    ("s2-freight", "cargos",
     "id int64, org_id int64, cargo_type int32, weight_kg int64, cbm int32, "
     "hazmat int32, declared_value int64, origin int32, destination int32, "
     "ready_day int32", "BTREE"),
    ("s2-freight", "fees",
     "id int64, fee_name varchar, fee_code int32, basis int32, amount int64, "
     "valid_from int32, valid_to int32", "BTREE"),
    ("s2-freight", "recipes",
     "id int64, cargo_type int32, route_code int32, fee_id int64, priority int32, "
     "valid_from int32, valid_to int32", "BTREE"),
    ("s2-freight", "freights",
     "id int64, operation_id int64, ship_id int64, cargo_id int64, cbm int32, "
     "price_per_cbm int64, booked_day int32, status int32", "HEAP"),
    ("s2-freight", "charges",
     "id int64, freight_id int64, fee_id int64, amount int64, "
     "applied_day int32", "HEAP"),

    ("s3-library", "users",
     "id int64, member_code varchar, name varchar, email varchar, "
     "joined_day int32, member_type int32, status int32, branch_id int32, "
     "fines_owed int64", "BTREE"),
    ("s3-library", "books",
     "id int64, isbn varchar, title varchar, author_id int64, "
     "publisher_id int32, published_year int32, genre int32, "
     "branch_id int32, copies int32, status int32", "BTREE"),
    ("s3-library", "reservations",
     "id int64, user_id int64, book_id int64, placed_day int32, "
     "expires_day int32, status int32, queue_pos int32", "BTREE"),
    ("s3-library", "loans",
     "id int64, user_id int64, book_id int64, loaned_day int32, "
     "due_day int32, returned_day int32, status int32, renewals int32", "BTREE"),
]

# The reads each scenario actually issues against its heap ledgers, in the
# shapes the drivers use. Asked of a spread relation to see which survive.
READ_SHAPES = (
    "SELECT * FROM {t}",
    "SELECT * FROM {t} WHERE id = 1",
    "SELECT COUNT(*) FROM {t}",
    "SELECT id FROM {t}",
    "SELECT * FROM {t} LIMIT 10",
)


def peer_lease_grants(conns, cores):
    """Row-id lease grants across every peer, summed. Core 0 is excluded
    because it has no lease table at all (M5's asymmetry): it bumps the mark
    directly and never opens a range of its own."""
    total = 0
    for c in range(1, cores):
        meta = conns[c][0].cmd("SHOW META")
        if "rowid_refill_grants=" in meta:
            total += field(meta, "rowid_refill_grants")
    return total


def values_for(columns):
    """One INSERT's worth of values for `columns`, id omitted so the engine
    issues it - which is the route that records lease demand (R4/IS1) and so
    the only route that makes the allocator decide about this relation."""
    out = []
    for column in columns.split(","):
        name, _, kind = column.strip().partition(" ")
        if name == "id":
            continue
        out.append("'x'" if kind.startswith("varchar") or kind.startswith("char")
                   else "0")
    return "(" + ", ".join(out) + ")"


def start(binary, workdir, tag, cores, port, range_size_ids, placement):
    conf = os.path.join(workdir, f"{tag}.conf")
    stderr_path = os.path.join(workdir, f"{tag}.stderr")
    with open(conf, "w") as f:
        f.write(f"data_file = {workdir}/{tag}.db\nport = {port}\ncores = {cores}\n"
                f"placement = {placement}\npeer_listeners = on\n"
                f"range_size_ids = {range_size_ids}\ndurability = relaxed\n"
                f"log_dir = {workdir}\nlog_file = {tag}.log\nlog_level = warn\n")
    subprocess.run(["rm", "-rf", f"{workdir}/{tag}.db", f"{workdir}/{tag}.db.wal"],
                   check=False)
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([binary, "--config", conf], stdout=err,
                                stderr=subprocess.STDOUT)
    wait_for_port(port, stderr_path)
    return proc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--range-size-ids", type=int, default=64)
    ap.add_argument("--rows", type=int, default=120,
                    help="peer inserts per relation before its gate is called "
                         "unasked; one drain tick serves one relation")
    ap.add_argument("--port", type=int, default=15850)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    # Refused here rather than three ways further down: `peer_listeners = on`
    # is refused by the engine at `cores = 1` (so `wait_for_port` would time
    # out), the peer round-robin below divides by `cores - 1`, and with no
    # peer there is no foreign writer and so nothing this probe asks about.
    if args.cores < 2:
        sys.exit("--cores must be at least 2: this probe measures what a *peer* "
                 "writer does to a relation, and a one-core instance has none")
    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)

    # ---- part 1: which relations are eligible ---------------------------
    #
    # `range_split_declines_detail` prints `oid:gate=count`, so creating
    # every relation and then making each one's *first* peer INSERT is what
    # makes the allocator answer for it. A small `range_size_ids` keeps the
    # blocks cheap; the gate does not depend on the size.
    # **One server per scenario**, not one for all four: the decline
    # counter's detail list is capped at `kMaxDetailKeys = 16`
    # (`refusal_counters.hpp`) and truncates with `+Nmore`, and twenty-four
    # relations at once would drop a third of the answer.
    rows = []
    port = args.port
    scenarios = []
    for scenario, rel, columns, clustered in SCHEMAS:
        if not scenarios or scenarios[-1][0] != scenario:
            scenarios.append((scenario, []))
        scenarios[-1][1].append((rel, columns, clustered))

    for scenario, relations in scenarios:
        tag = f"eligibility-{scenario}"
        proc = start(args.server, args.workdir, tag, args.cores, port,
                     args.range_size_ids, "creating")
        try:
            conns, _ = collect_connections(port, {c: 1 for c in range(args.cores)},
                                           max_attempts=200 * args.cores)
            c0, peer = conns[0][0], conns[1][0]
            # **One relation at a time, and enough inserts to spread.**
            # `MaybeRefillRowIds` answers `NeediestRelation()` - one
            # relation per drain tick - so creating twenty-four and giving
            # each a single INSERT leaves twenty-three demands unserved.
            for rel, columns, clustered in relations:
                reply = c0.cmd(f"CREATE TABLE {rel} ({columns}) {clustered}")
                if reply.startswith("ERR"):
                    rows.append({"scenario": scenario, "relation": rel,
                                 "clustered": clustered, "create": reply[:120]})
                    continue
                oid = field(c0.cmd(f"DESCRIBE {rel}"), "oid")
                stmt = f"INSERT INTO {rel} VALUES {values_for(columns)}"
                # **The delta, not the reading.** `rowid_refill_grants` is a
                # per-core lifetime counter, so the value after this
                # relation's workload includes every earlier relation's
                # grants on the same server - which read a btree relation as
                # having leased purely because a heap one ran before it.
                before = peer_lease_grants(conns, args.cores)
                placed = refused = 0
                # Round-robin the peers, which is what keeps the top range's
                # owner changing and so defeats IS5's suppression - the
                # shape a real multi-session workload has.
                for i in range(args.rows):
                    out = conns[1 + (i % (args.cores - 1))][0].cmd(stmt)
                    if out.startswith("ERR"):
                        refused += 1
                    else:
                        placed += 1
                grants = peer_lease_grants(conns, args.cores) - before
                reads = {}
                for shape in READ_SHAPES:
                    sql = shape.format(t=rel)
                    out = c0.cmd(sql)
                    reads[shape.format(t="<rel>")] = (
                        "REFUSED: " + out[4:80] if out.startswith("ERR") else "ok")
                # **A lease grant is not a range, and reading it as one
                # over-reported this whole table.** `row_id_lease_service.cpp`
                # fills `grant.count` and *then* calls
                # `OpenRangeOnSystemCore`, which returns `kInvalidPageId` -
                # block already handed over - for a catalog namespace, for
                # any of `RangeEligible`'s five gates, for a durable
                # `sys.assertions` row, for `first_id == 0`, and for IS5's
                # top-owner suppression. So `grants > 0` says "a peer took a
                # block of ids", which is true of *every* heap relation here
                # and says nothing about whether a boundary opened.
                #
                # The behavioural witness is the read, and it is exact.
                # Under `placement = creating` the relation is core 0's, so
                # the fan-in route is skipped (it wants `owner_core !=
                # core_id_`) and `CheckReadAffinity` runs
                # `WhollyOwnedBy(0)` - which fails if and only if some range
                # landed on another core, and every range this path opens is
                # owned by the requesting *peer*. A relation that never
                # split answers all five shapes here.
                whole_scan = reads["SELECT * FROM <rel>"]
                rows.append({"scenario": scenario, "relation": rel,
                             "clustered": clustered, "oid": oid,
                             "rows_placed": placed, "write_refusals": refused,
                             "peer_lease_grants": grants,
                             "leased": grants > 0,
                             "spread": whole_scan != "ok",
                             "reads": reads,
                             "readable": all(v == "ok" for v in reads.values()),
                             "whole_scan": whole_scan})
        finally:
            stop_server(port)
            proc.wait(timeout=30)
        port += 1

    print(f"\n{'scenario':<16} {'relation':<22} {'clus':<6} {'rows':>6} {'leased':>7} "
          f"{'spread':>7} {'whole scan after the workload'}")
    print("-" * 108)
    for r in rows:
        if "create" in r:
            print(f"{r['scenario']:<16} {r['relation']:<22} {r['clustered']:<6} "
                  f"CREATE refused: {r['create']}")
            continue
        print(f"{r['scenario']:<16} {r['relation']:<22} {r['clustered']:<6} "
              f"{r['rows_placed']:>6} {r['peer_lease_grants']:>7} "
              f"{'yes' if r['spread'] else 'no':>7} {r['whole_scan'][:48]}")

    # Two counts, because they answer two questions and the first used to
    # be printed as the second: `leased` is how far the heap-only pump
    # reaches, `spread` is how many relations a boundary actually opened in.
    leased = [r for r in rows if r.get("leased")]
    spread = [r for r in rows if r.get("spread")]
    print(f"\n{len(leased)} of {len(rows)} scenario relations took a peer lease block "
          f"(the rest never asked for one, which is the heap-only pump).")
    print(f"{len(spread)} of those {len(leased)} actually split - a grant opens no "
          f"boundary when a gate declines, when it is the first block, or when the "
          f"asking core already owns the top range.")
    for r in spread:
        lost = [s for s, v in r["reads"].items() if v != "ok"]
        print(f"  {r['scenario']}.{r['relation']}: {len(lost)} of "
              f"{len(r['reads'])} read shapes refused after the workload")
    print()
    print(json.dumps(rows, indent=2))


if __name__ == "__main__":
    main()
