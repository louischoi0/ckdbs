#!/usr/bin/env python3
"""Freight and cargo: the schema and reference data for scenario 2.

The plan this implements is `docs/scenario2-freight.md`. **This file is task
`S2-01` of its §9 and nothing beyond it**: it builds the eight relations,
loads the reference data, and probes the three reads the reporter will need.
The measured booking transaction (`S2-02`), the contention model (`S2-03`)
and the reporter process (`S2-04`) are not here yet, and this tool prints no
TPS number because it drives no business transaction.

Where the other two scenarios sit:

    tools/scenario0_stockmarket.py   a write workload    - trades, in TPS
    tools/scenario1_backtest.py      a read workload     - joins, in QPS
    this one                         a *contended* write workload, in TPS,
                                     with the refusals counted separately

The eight relations, and what each is for:

    organizations   the customer that places the shipping order
    ships           the fleet
    operations      one voyage of one ship        - the capacity axis
    cargos          the goods, owned by an organization
    fees            the rate card
    recipes         which fee applies to which cargo type / route / window
    freights        one cargo booked onto one voyage = the order line
    charges         the fees actually applied to a freight

A freight row **is** the shipping order (decision S2-5): there is no order
header relation, because one cargo placed on one voyage is the unit a
customer buys and the unit this scenario will measure.

Three encodings are forced rather than chosen (S2-8). KDS refuses `float`
and `decimal` at CREATE TABLE - a fixed row size must reserve a width for
every column, and reserving one for an undecided encoding would be half of
settling it - and it has no date type. So:

    money       int64, in minor units      (cents; 1_000_00 is a thousand)
    volume      int32, in milli-m^3        (10_000 is 10 CBM)
    dates       int32, in epoch days

Nothing here rounds, and every total this workload verifies is an exact
integer sum. That is the point: a scenario about consistency cannot have a
float in it.

Two flags end the run before any measurement, which is what makes a data
file preparable once and drivable many times (S2-11):

    --schema-only   create the eight relations and exit
    --load-only     create, load the reference data, and exit

Usage:

    ./scenario2_freight.py --schema-only --suffix run1
    ./scenario2_freight.py --load-only --suffix run1 --cargos 20000
    ./scenario2_freight.py --suffix run1 --fk --cabin
"""

import argparse
import datetime
import random
import re
import sys
import time

from bench_common import Phase, report, write_json
from benchmark import read_durability
from ckdbs_cli import DEFAULT_HOST, DEFAULT_PORT, ServerConnection, format_reply

# ---- schema (docs/scenario2-freight.md §2) -------------------------------
#
# Column 0 of every relation is the Keystone primary key: system-generated,
# never supplied on INSERT (invariant 11). It is written out in each column
# list because CREATE TABLE declares it; only INSERT omits it.
#
# BTREE wherever the booking transaction probes by pk, or where a foreign
# key needs a parent to descend into - a heap parent is refused at
# declaration, so `--fk` requires it. HEAP for the two append-only ledgers,
# which are written at the chain tail and never probed by pk.
#
# 68 columns per run against a ~7,800-column instance ceiling, so ~110 runs
# per data file. Nothing reclaims a catalog row: there is no DROP TABLE.

SCHEMA = {
    "organizations": (
        "id int64, org_code varchar, name varchar, country int32, org_type int32, "
        "credit_limit int64, outstanding int64, tier int32, contact varchar, "
        "registered_day int32, status int32", "BTREE"),
    "ships": (
        "id int64, imo varchar, name varchar, ship_type int32, capacity_cbm int32, "
        "dwt int64, built_year int32, flag varchar, owner_id int64, "
        "home_port int32, status int32", "BTREE"),
    "operations": (
        "id int64, ship_id int64, origin int32, destination int32, depart_day int32, "
        "arrive_day int32, status int32, booked_cbm int32, revenue int64", "BTREE"),
    "cargos": (
        "id int64, org_id int64, cargo_type int32, weight_kg int64, cbm int32, "
        "hazmat int32, declared_value int64, origin int32, destination int32, "
        "ready_day int32", "BTREE"),
    "fees": (
        "id int64, fee_name varchar, fee_code int32, basis int32, amount int64, "
        "valid_from int32, valid_to int32", "BTREE"),
    "recipes": (
        "id int64, cargo_type int32, route_code int32, fee_id int64, priority int32, "
        "valid_from int32, valid_to int32", "BTREE"),
    "freights": (
        "id int64, operation_id int64, ship_id int64, cargo_id int64, cbm int32, "
        "price_per_cbm int64, booked_day int32, status int32", "HEAP"),
    "charges": (
        "id int64, freight_id int64, fee_id int64, amount int64, "
        "applied_day int32", "HEAP"),
}

# Creation order is load-bearing under `--fk` and cosmetic without it: a
# parent must exist before a child references it, and there is no
# ALTER TABLE to add the constraint afterwards.
CREATE_ORDER = ("organizations", "ships", "operations", "cargos", "fees",
                "recipes", "freights", "charges")

# ---- the foreign keys (docs/impl-foreign-keys.md) ------------------------
#
# `--fk` declares these three, as (child, column, parent):
FOREIGN_KEYS = (
    ("cargos", "org_id", "organizations"),
    ("operations", "ship_id", "ships"),
    ("freights", "cargo_id", "cargos"),
)

# Each is a relationship the data has always had and nothing enforced: the
# driver generates every id it writes, so referential integrity is a
# property of *this file* until the flag moves it into the database. All
# three fire on the forward check only - nothing in this workload deletes a
# parent, which is the honest shape of an insert-dominated OLTP run.

# ---- the Cabin (docs/feat-cabin.md) --------------------------------------
#
# `--cabin` declares one, on exactly this column:
CABIN_RELATION, CABIN_COLUMN, CABIN_TYPE = "recipes", "cargo_type", "int32"

# The booking transaction reads `WHERE cargo_type = <t>` once per booking -
# a non-pk equality, so a FilterScan - over a small relation nothing writes
# after load. That is the best-shaped Cabin candidate this repo has: a hot
# value set, drawn from a handful of values, on a read-only relation, so the
# write hook that pays for the authority never fires at all.
#
# Declared as a column policy rather than by CREATE CABIN, for the reason
# scenario0 states: a declared Cabin observes a value on its *first*
# selection, where an engine-created one waits for the second.

# ---- the reference data --------------------------------------------------

# Ports are small integer codes; a route is one packed pair. ROUTE_ANY is
# what a recipe rule carries when it applies regardless of route, and it is
# -1 rather than 0 because 0 is the legitimate route (port 0 -> port 0).
PORTS = 24
ROUTE_ANY = -1

# Cargo types. The booking's recipe read is keyed on this column, so its
# cardinality is the Cabin's value-set size and wants to stay small.
CARGO_TYPES = ("dry", "reefer", "hazmat", "bulk", "liquid", "vehicle",
               "project", "container")

# Fee bases. A charge is computed client-side from one of these, because the
# engine has no arithmetic in a select list.
BASIS_FLAT, BASIS_PER_CBM, BASIS_PER_MILLE = 0, 1, 2

# The rate card. Twelve fees, priced in minor units: a flat fee is the whole
# amount, a per-CBM fee is per cubic metre, a per-mille fee is tenths of a
# percent of the cargo's declared value.
FEES = (
    ("terminal-handling", 101, BASIS_PER_CBM, 1_450),
    ("bunker-adjustment", 102, BASIS_PER_CBM, 900),
    ("currency-adjustment", 103, BASIS_PER_MILLE, 3),
    ("documentation", 104, BASIS_FLAT, 4_500),
    ("seal", 105, BASIS_FLAT, 800),
    ("hazmat-surcharge", 106, BASIS_PER_CBM, 6_200),
    ("customs-clearance", 107, BASIS_FLAT, 12_000),
    ("lashing", 108, BASIS_PER_CBM, 700),
    ("demurrage-deposit", 109, BASIS_FLAT, 30_000),
    ("wharfage", 110, BASIS_PER_CBM, 350),
    ("security", 111, BASIS_FLAT, 2_200),
    ("cargo-insurance", 112, BASIS_PER_MILLE, 8),
)

COUNTRIES = ("KR", "US", "JP", "GB", "DE", "SG", "HK", "AU", "NL", "CN")
SHIP_TYPES = 6
ORG_TYPES = 4

# Volume is milli-m^3 throughout (S2-8). A ship carries 20,000 to 250,000
# CBM; a cargo is 5 to 400 CBM, so a voyage fills after a few hundred
# bookings - which is what makes the capacity limit reachable inside a
# 60-second run rather than a limit that exists only in the schema.
SHIP_CAPACITY_MIN_CBM, SHIP_CAPACITY_MAX_CBM = 20_000, 250_000
CARGO_MIN_CBM, CARGO_MAX_CBM = 5, 400
MILLI = 1_000

# Credit is what the customer axis is bounded by, and it is deliberately
# reachable for the same reason: a limit nothing ever hits is not an
# invariant, it is a column.
CREDIT_LIMIT_MIN, CREDIT_LIMIT_MAX = 5_000_000_00, 400_000_000_00

# Epoch day of 2026-01-01, which the simulated business clock counts from.
DAY0 = (datetime.date(2026, 1, 1) - datetime.date(1970, 1, 1)).days
RULE_WINDOW_DAYS = 3650

INSERTED_ID = re.compile(r"\bid=(\d+)")


def abort(message, reply=None):
    print(f"scenario2 aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


def connect(host, port, timeout):
    try:
        return ServerConnection(host, port, timeout=timeout)
    except OSError as e:
        abort(f"could not connect to {host}:{port}: {e}\n"
              f"  start one with: ./build-release/kds_server /tmp/freight.db "
              f"--port {port}")


ECHO = False
ECHO_REPLY_MAX = 96


def set_echo(enabled):
    global ECHO
    ECHO = bool(enabled)


class Client:
    """One connection plus the one-command-one-reply callable everything
    below is written against. Counts errors so a caller that does not
    inspect every reply still cannot report a clean run over a failing
    one."""

    def __init__(self, host, port, timeout):
        self._conn = connect(host, port, timeout)
        self.errors = 0
        self.first_error = None

    def __call__(self, command):
        reply = format_reply(self._conn.send_command(command))
        if ECHO:
            shown = (reply if len(reply) <= ECHO_REPLY_MAX
                     else reply[:ECHO_REPLY_MAX] + "...")
            print(f"[main] {command}  ->  {shown}", file=sys.stderr, flush=True)
        if reply.startswith("ERR"):
            self.errors += 1
            if self.first_error is None:
                self.first_error = f"{command}  ->  {reply}"
        return reply

    def close(self):
        self._conn.close()


def send(exec_, phase, command):
    t0 = time.perf_counter()
    reply = exec_(command)
    phase.record(time.perf_counter() - t0, reply)
    return reply


def inserted_id(reply):
    """The Keystone id the server issued, or None if the insert failed.

    Read back rather than assumed: ids are ascending but **not gapless** -
    a failed insert burns one (invariant 11 promises unique and monotonic,
    never dense) - so a run that hits an error must not go on addressing
    rows by ordinal."""
    got = INSERTED_ID.search(reply)
    return int(got.group(1)) if got else None


# ---- DDL -----------------------------------------------------------------

def schema_for(base, cabin, fk, suffix):
    """The column list for `base`, with the cabin policy and any foreign key
    applied.

    Textual substitution rather than a second SCHEMA table: the point is
    that the runs are the *same* schema apart from one clause each, and two
    tables would let them drift.

    `suffix` is needed for a foreign key and not for the cabin, because
    REFERENCES names a relation and every relation in a run carries the
    suffix - `REFERENCES cargos` would point at another run's table, or at
    nothing."""
    columns, clustered = SCHEMA[base]
    if cabin and base == CABIN_RELATION:
        columns = columns.replace(f"{CABIN_COLUMN} {CABIN_TYPE}",
                                  f"{CABIN_COLUMN} {CABIN_TYPE} CABIN", 1)
    if fk:
        for child, column, parent in FOREIGN_KEYS:
            if child != base:
                continue
            columns = columns.replace(
                f"{column} int64",
                f"{column} int64 REFERENCES {parent}_{suffix}", 1)
    return columns, clustered


def create_tables(exec_, suffix, phase, cabin=False, fk=False):
    """The eight relations, in CREATE_ORDER. Returns their names."""
    created = []
    for base in CREATE_ORDER:
        columns, clustered = schema_for(base, cabin, fk, suffix)
        name = f"{base}_{suffix}"
        reply = send(exec_, phase, f"CREATE TABLE {name} ({columns}) {clustered}")
        if reply.startswith("ERR"):
            explain_ddl_failure(base, suffix, reply, cabin, fk)
            abort(f"could not create {name}", reply)
        created.append(name)
    return created


def explain_ddl_failure(base, suffix, reply, cabin, fk):
    """Turns the four refusals this schema can actually provoke into an
    error that names the flag responsible, instead of a syntax error
    pointing into the middle of a column definition."""
    upper = reply.upper()
    if fk and any(child == base for child, _, _ in FOREIGN_KEYS) and \
            "REFERENCES" in upper:
        cols = ", ".join(f"{c}.{col} -> {p}"
                         for c, col, p in FOREIGN_KEYS if c == base)
        abort(f"--fk: this server does not understand REFERENCES.\n"
              f"  {cols} needs a build with docs/impl-foreign-keys.md in it "
              f"(FK-M1); re-run without --fk, or rebuild the server.", reply)
    if fk and "heap relation" in reply:
        abort(f"--fk: the parent of a foreign key on {base} is a heap relation, "
              f"and a foreign key references the parent's primary key.\n"
              f"  A heap relation has no pk index, so every check would scan it; "
              f"the declaration is refused rather than made slow.\n"
              f"  This scenario declares every fk parent BTREE, so seeing this "
              f"means SCHEMA was edited.", reply)
    if cabin and base == CABIN_RELATION and "CABIN" in upper:
        abort(f"--cabin: this server does not understand the column cabin "
              f"policy.\n  `{CABIN_RELATION}.{CABIN_COLUMN} {CABIN_TYPE} CABIN` "
              f"needs a build with docs/feat-cabin.md in it; re-run without "
              f"--cabin, or rebuild the server.", reply)
    if "no room" in reply or "reserved catalog page range" in reply:
        abort(f"could not create {base}_{suffix}: the catalog is out of column "
              f"space.\n  Catalog relations chain into a reserved range "
              f"(~7,800 columns for the whole instance); this scenario needs 68 "
              f"per run and nothing reclaims them, because there is no DROP "
              f"TABLE.\n  Restart the server on a fresh data file.", reply)


# ---- load ----------------------------------------------------------------

def load_organizations(exec_, table, count, rng, phase):
    """Returns [(org_id, credit_limit)] in creation order.

    `outstanding` opens at 0 and is the column the booking transaction
    moves; `credit_limit` is carried back because the booker checks against
    it client-side (the engine has no arithmetic in a select list, so there
    is no server-side CHECK to lean on)."""
    orgs = []
    for i in range(count):
        limit = rng.randint(CREDIT_LIMIT_MIN, CREDIT_LIMIT_MAX)
        reply = send(exec_, phase,
                     f"INSERT INTO {table} VALUES "
                     f"('ORG{i:06d}', 'org{i:06d}', "
                     f"{rng.randrange(len(COUNTRIES))}, "
                     f"{rng.randrange(ORG_TYPES)}, {limit}, 0, "
                     f"{rng.randint(0, 3)}, 'ops{i:06d}@example.test', "
                     f"{DAY0 - rng.randint(0, 3650)}, 0)")
        got = inserted_id(reply)
        if got is not None:
            orgs.append((got, limit))
    return orgs


def load_ships(exec_, table, count, rng, phase):
    """Returns [(ship_id, capacity_cbm)] in creation order."""
    ships = []
    for i in range(count):
        capacity = rng.randint(SHIP_CAPACITY_MIN_CBM,
                               SHIP_CAPACITY_MAX_CBM) * MILLI
        reply = send(exec_, phase,
                     f"INSERT INTO {table} VALUES "
                     f"('IMO{9000000 + i}', 'vessel{i:05d}', "
                     f"{rng.randrange(SHIP_TYPES)}, {capacity}, "
                     f"{rng.randint(10_000, 200_000)}, "
                     f"{rng.randint(1995, 2025)}, "
                     f"'{rng.choice(COUNTRIES)}', 0, "
                     f"{rng.randrange(PORTS)}, 0)")
        got = inserted_id(reply)
        if got is not None:
            ships.append((got, capacity))
    return ships


def load_operations(exec_, table, count, ships, rng, phase):
    """Returns [(operation_id, ship_id, capacity_cbm, origin, destination)].

    The ship's capacity travels with the voyage because the booker needs it
    per booking and re-reading `ships` would put a third pk lookup in the
    measured transaction for a value that cannot change during the run."""
    operations = []
    for _ in range(count):
        ship_id, capacity = rng.choice(ships)
        origin = rng.randrange(PORTS)
        destination = (origin + rng.randint(1, PORTS - 1)) % PORTS
        depart = DAY0 + rng.randint(0, 180)
        reply = send(exec_, phase,
                     f"INSERT INTO {table} VALUES "
                     f"({ship_id}, {origin}, {destination}, {depart}, "
                     f"{depart + rng.randint(3, 40)}, 0, 0, 0)")
        got = inserted_id(reply)
        if got is not None:
            operations.append((got, ship_id, capacity, origin, destination))
    return operations


def load_cargos(exec_, table, count, orgs, rng, phase):
    """Returns [(cargo_id, org_id, cargo_type, cbm, declared_value)].

    The bulk relation: at the default 200,000 rows this is most of the load
    phase's wall clock, and it is the one loader worth scaling down while
    developing (`--cargos 20000`)."""
    cargos = []
    for _ in range(count):
        org_id, _limit = rng.choice(orgs)
        cargo_type = rng.randrange(len(CARGO_TYPES))
        cbm = rng.randint(CARGO_MIN_CBM, CARGO_MAX_CBM) * MILLI
        value = rng.randint(1_000_00, 5_000_000_00)
        origin = rng.randrange(PORTS)
        destination = (origin + rng.randint(1, PORTS - 1)) % PORTS
        reply = send(exec_, phase,
                     f"INSERT INTO {table} VALUES "
                     f"({org_id}, {cargo_type}, {rng.randint(500, 40_000)}, "
                     f"{cbm}, {1 if CARGO_TYPES[cargo_type] == 'hazmat' else 0}, "
                     f"{value}, {origin}, {destination}, "
                     f"{DAY0 + rng.randint(0, 180)})")
        got = inserted_id(reply)
        if got is not None:
            cargos.append((got, org_id, cargo_type, cbm, value))
    return cargos


def load_fees(exec_, table, phase):
    """The rate card: FEES, verbatim. Returns {fee_code: (fee_id, basis,
    amount)}."""
    fees = {}
    for name, code, basis, amount in FEES:
        reply = send(exec_, phase,
                     f"INSERT INTO {table} VALUES "
                     f"('{name}', {code}, {basis}, {amount}, "
                     f"{DAY0 - RULE_WINDOW_DAYS}, {DAY0 + RULE_WINDOW_DAYS})")
        got = inserted_id(reply)
        if got is not None:
            fees[code] = (got, basis, amount)
    return fees


def load_recipes(exec_, table, fees, hot_routes, rng, phase):
    """The pricing rule set (S2-3): which fee applies to which cargo type on
    which route, in which date window.

    Two shapes per cargo type, and the split is deliberate. **Route-agnostic
    rules** (`route_code = ROUTE_ANY`) are what every booking matches, so
    they set the floor on how many `charges` rows a transaction writes.
    **Route-specific rules** are what make the match *variable* - a booking
    on a hot route pays more fees than one on a quiet route, so the
    transaction's statement count is not a constant. A workload whose unit
    of work is fixed-size hides exactly the tail this scenario is for.

    Returns [(recipe_id, cargo_type, route_code, fee_code, priority)]."""
    always = (101, 102, 104, 110)          # THC, BAF, documentation, wharfage
    per_type = {
        "hazmat": (106, 107, 111),
        "reefer": (105, 112),
        "liquid": (108, 112),
        "vehicle": (108, 105),
        "project": (108, 109),
        "bulk": (109,),
        "dry": (),
        "container": (105,),
    }
    rows = []
    for cargo_type, type_name in enumerate(CARGO_TYPES):
        codes = [(code, ROUTE_ANY) for code in always]
        codes += [(code, ROUTE_ANY) for code in per_type[type_name]]
        codes += [(103, route) for route in hot_routes]
        for priority, (code, route) in enumerate(codes):
            if code not in fees:
                continue
            fee_id, _basis, _amount = fees[code]
            reply = send(exec_, phase,
                         f"INSERT INTO {table} VALUES "
                         f"({cargo_type}, {route}, {fee_id}, {priority}, "
                         f"{DAY0 - RULE_WINDOW_DAYS}, "
                         f"{DAY0 + RULE_WINDOW_DAYS})")
            got = inserted_id(reply)
            if got is not None:
                rows.append((got, cargo_type, route, code, priority))
    return rows


def route_code(origin, destination):
    """One packed route. PORTS is small and fixed, so this is a stable
    integer key rather than a hash - two runs of this tool address the same
    route with the same number."""
    return origin * PORTS + destination


# ---- the capability probe (docs/scenario2-freight.md §6) -----------------

def probe_reads(exec_, tables, sample_op, sample_org):
    """Runs each read the later tasks depend on, once, and reports which the
    server accepts.

    The third one is why this function exists. **Nothing in this repo
    aggregates over a joined chain today**: `docs/feat-aggregate.md` AG1
    puts the fold over the statement's RowSink and leaves the compiled chain
    byte-identical, so a group key resolving to a *second* step's column
    should work - but 'should' is not a measurement, and the reporter of
    S2-04 is written differently depending on the answer.

    Every probe runs against relations that may be empty, which is
    deliberate: what is being asked is whether the statement *compiles*, and
    an empty relation answers that as well as a full one and much faster."""
    freights, cargos, operations = (
        tables["freights"], tables["cargos"], tables["operations"])
    probes = (
        ("pk lookup",
         f"SELECT booked_cbm FROM {operations} WHERE id = {sample_op}"),
        ("capacity aggregate",
         f"SELECT SUM(cbm) FROM {freights} WHERE operation_id = {sample_op}"),
        ("recipe filterscan",
         f"SELECT fee_id, priority FROM {tables['recipes']} WHERE cargo_type = 0"),
        ("voyage manifest",
         f"SELECT * FROM {freights} WHERE operation_id = {sample_op}"),
        ("voyage rollup",
         f"SELECT status, COUNT(*), SUM(cbm) FROM {freights} "
         f"WHERE operation_id = {sample_op} GROUP BY status"),
        ("customer statement",
         f"SELECT c.org_id, SUM(f.cbm) FROM {freights} AS f "
         f"JOIN {cargos} AS c ON f.cargo_id = c.id "
         f"WHERE c.org_id = {sample_org} GROUP BY c.org_id"),
    )
    results = []
    for name, statement in probes:
        reply = exec_(statement)
        results.append((name, not reply.startswith("ERR"), statement, reply))
    return results


def print_probe(results):
    print()
    print("read probe - which of the reads S2-02..S2-04 need does this server take?")
    print()
    width = max(len(name) for name, _, _, _ in results)
    for name, ok, _statement, reply in results:
        verdict = "ok" if ok else "REFUSED"
        print(f"  {name:<{width}}  {verdict}")
        if not ok:
            print(f"  {'':<{width}}  {reply}")
    refused = [name for name, ok, _, _ in results if not ok]
    if refused:
        print()
        print(f"  {len(refused)} refused. The fallback for 'customer statement' is a")
        print( "  per-organization filtered aggregate; a refusal anywhere else is a")
        print( "  blocker for S2-02, not a fallback (docs/scenario2-freight.md §10).")
    print()


# ---- main ----------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"default: {DEFAULT_HOST}")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"default: {DEFAULT_PORT}")
    parser.add_argument("--suffix", default=None,
                        help="relation-name suffix, so several runs can share one "
                             "data file (default: a timestamp)")
    parser.add_argument("--schema-only", action="store_true",
                        help="create the eight relations and exit: no load, no "
                             "probe, no measurement. Prepares a data file that "
                             "later runs drive with the same --suffix")
    parser.add_argument("--load-only", action="store_true",
                        help="create and load the reference data, then exit "
                             "(skips the read probe)")
    parser.add_argument("--organizations", type=int, default=2000,
                        help="customers (default: 2000)")
    parser.add_argument("--ships", type=int, default=200,
                        help="vessels (default: 200)")
    parser.add_argument("--operations", type=int, default=2000,
                        help="voyages (default: 2000)")
    parser.add_argument("--cargos", type=int, default=200000,
                        help="the bulk relation, and most of the load's wall "
                             "clock (default: 200000)")
    parser.add_argument("--hot-routes", type=int, default=6,
                        help="routes carrying a route-specific pricing rule, "
                             "which is what makes a booking's fee count vary "
                             "(default: 6)")
    parser.add_argument("--fk", action="store_true",
                        help="declare the three foreign keys (docs/impl-foreign-keys.md)")
    parser.add_argument("--cabin", action="store_true",
                        help=f"declare a Cabin on {CABIN_RELATION}.{CABIN_COLUMN} "
                             f"(docs/feat-cabin.md)")
    parser.add_argument("--echo", action="store_true",
                        help="print every statement and its reply to stderr. Not "
                             "free: a write per statement")
    parser.add_argument("--seed", type=int, default=1, help="RNG seed (default: 1)")
    parser.add_argument("--timeout", type=float, default=120.0,
                        help="socket timeout in seconds (default: 120)")
    parser.add_argument("--sync", action="store_true",
                        help="SYNC before exiting, so the load survives a restart "
                             "(there is no recovery: durability holds only as far "
                             "as SYNC or a clean shutdown)")
    parser.add_argument("--json", metavar="PATH", help="also write results as JSON")
    parser.add_argument("--server-log", metavar="PATH",
                        help="the server's log, read for its durability class")
    args = parser.parse_args()

    if args.schema_only and args.load_only:
        abort("--schema-only and --load-only are exclusive: the first ends the "
              "run before the load the second asks for")

    set_echo(args.echo)
    suffix = args.suffix or time.strftime("%H%M%S")
    rng = random.Random(args.seed)
    client = Client(args.host, args.port, args.timeout)
    tables = {base: f"{base}_{suffix}" for base in CREATE_ORDER}

    ddl = Phase("ddl", "8 relations")
    created = create_tables(client, suffix, ddl, cabin=args.cabin, fk=args.fk)
    print(f"created {len(created)} relations with suffix _{suffix}:")
    for name in created:
        print(f"  {name}")

    if args.schema_only:
        print()
        print("--schema-only: stopping before the load. Drive this data file with")
        print(f"  {sys.argv[0]} --suffix {suffix} [--load-only]")
        if args.sync:
            client("SYNC")
        client.close()
        return 0

    phases = [ddl]

    def phase(name, detail=""):
        p = Phase(name, detail)
        phases.append(p)
        return p

    orgs = load_organizations(client, tables["organizations"],
                              args.organizations, rng, phase("load-organizations"))
    ships = load_ships(client, tables["ships"], args.ships, rng, phase("load-ships"))
    if not orgs or not ships:
        abort("the load produced no organizations or no ships; every later phase "
              "addresses rows by the ids it returns",
              client.first_error)
    operations = load_operations(client, tables["operations"], args.operations,
                                 ships, rng, phase("load-operations"))
    fees = load_fees(client, tables["fees"], phase("load-fees"))
    hot_routes = [route_code(rng.randrange(PORTS), rng.randrange(PORTS))
                  for _ in range(args.hot_routes)]
    recipes = load_recipes(client, tables["recipes"], fees, hot_routes, rng,
                           phase("load-recipes"))
    cargos = load_cargos(client, tables["cargos"], args.cargos, orgs, rng,
                         phase("load-cargos"))

    loaded = {
        "organizations": len(orgs), "ships": len(ships),
        "operations": len(operations), "fees": len(fees),
        "recipes": len(recipes), "cargos": len(cargos),
        "freights": 0, "charges": 0,
    }

    if not args.load_only and operations and orgs:
        print_probe(probe_reads(client, tables, operations[0][0], orgs[0][0]))

    meta = {
        "engine": "ckdbs",
        "scenario": "freight",
        "columns": sum(len(SCHEMA[b][0].split(",")) for b in CREATE_ORDER),
        "rows": sum(loaded.values()),
        "host": args.host, "port": args.port,
        "table": f"scenario2_{suffix}",
        "loaded": loaded,
        "fk": args.fk, "cabin": args.cabin,
        "seed": args.seed,
    }
    if args.server_log:
        durability = read_durability(args.server_log)
        if durability:
            meta["durability"] = durability

    report(phases, meta, footer=(
        "S2-01 only: this tool builds and loads. It drives no booking",
        "transaction, so there is no TPS number here by construction -",
        "see docs/scenario2-freight.md §9 for what S2-02..S2-06 add.",
    ))
    if args.json:
        write_json(args.json, meta, phases)
    if args.sync:
        client("SYNC")
    if client.errors:
        print(f"\n{client.errors} statement(s) failed; first: {client.first_error}",
              file=sys.stderr)
    client.close()
    return 1 if client.errors else 0


if __name__ == "__main__":
    sys.exit(main())
