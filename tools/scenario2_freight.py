#!/usr/bin/env python3
"""Freight and cargo: the schema and reference data for scenario 2.

The plan this implements is `docs/inflight/in-progress/scenario2-freight.md`, tasks `S2-01`
through `S2-03` of its §9: it builds the eight relations, loads the reference
data, drives the eight-statement booking transaction from `--bookers`
concurrent processes, accounts for every outcome separately - committed,
refused by the business, or lost to a write conflict and retried - and runs
the analytic reporter (`--manifest`) beside them. The PostgreSQL twin
(`S2-05`) is `tools/pg_scenario2_freight.py`.

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
import multiprocessing
import random
import re
import sys
import time

from bench_common import Phase, report, write_json
from benchmark import read_durability
from ckdbs_cli import DEFAULT_HOST, DEFAULT_PORT, ServerConnection, format_reply

# ---- schema (docs/inflight/in-progress/scenario2-freight.md §2) -------------------------------
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

# ---- the foreign keys (docs/spec/foreign-keys.md) ------------------------
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

# ---- the Cabin (docs/spec/cabin.md) --------------------------------------
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

# The freight's own price, in minor units per m^3, before any fee. Jittered
# per booking so `revenue` is not a multiple of one constant, which would
# make a lost update arithmetically invisible in a verify pass.
BASE_RATE_PER_CBM = 2_500
RATE_JITTER = 400

COUNTRIES = ("KR", "US", "JP", "GB", "DE", "SG", "HK", "AU", "NL", "CN")
SHIP_TYPES = 6
ORG_TYPES = 4

# Volume is milli-m^3 throughout (S2-8). A cargo is 5 to 400 CBM.
CARGO_MIN_CBM, CARGO_MAX_CBM = 5, 400
MILLI = 1_000

# **Both limits are sized to the run's own demand, not written as
# constants**, and that is the difference between a scenario that measures
# rejection and one that only claims to. A fixed 20,000-250,000 CBM ship is
# unreachable at 400 cargos and trivially full at 2,000,000, so a fixed
# number makes the capacity axis a property of the flags rather than of the
# workload - and the two rejection classes are half of what S2-6 counts.
#
# So: expected demand per voyage is (cargos x mean cargo CBM) / voyages, and
# a ship's capacity is that times a spread. A ship at the bottom of the
# spread fills and starts refusing; one at the top never does. Same
# construction for credit, over expected spend per customer. The headroom
# flags scale both, and 1.0 means "sized to exactly this run's demand".
CAPACITY_SPREAD = (0.5, 2.5)
CREDIT_SPREAD = (0.4, 2.0)

# A cargo's declared value, in minor units: 10,000.00 to 2,000,000.00. The
# per-mille fees price off it, so its spread is most of the spread in what a
# booking costs.
DECLARED_MIN, DECLARED_MAX = 10_000_00, 2_000_000_00

# A booking's total is its freight amount plus fees. The three route-agnostic
# per-CBM fees come to 2,700 per m^3 against a 2,500 base rate, so a booking
# costs a little over twice its freight - which is what sizes credit.
FEE_MULTIPLE = 2.2

# **Two floors, and they are the same rule twice.** No voyage may be too
# small for the largest cargo that exists, and no customer's credit may be
# smaller than the most expensive booking that can be priced. Either one
# creates a row that is refused by every counterparty forever: it returns to
# the pool on each rejection, is drawn again, and drives a rejection rate
# with nothing behind it. A limit is meant to bind by *accumulation* - after
# a voyage has filled, after a customer has spent - and never on the first
# attempt.
CAPACITY_FLOOR_CBM = CARGO_MAX_CBM * 2 * MILLI

# The worst case a booking can price: the largest cargo, every per-CBM fee
# this rule set can match, and every per-mille fee against the highest
# declared value. Times three, so a customer at the floor can accumulate
# rather than being stopped at one.
MAX_PER_CBM_FEES = 1_450 + 900 + 350 + 6_200 + 700
MAX_PER_MILLE = 3 + 8
CREDIT_FLOOR = 3 * (
    CARGO_MAX_CBM * (BASE_RATE_PER_CBM + RATE_JITTER + MAX_PER_CBM_FEES)
    + DECLARED_MAX * MAX_PER_MILLE // 1000
    + 50_000)

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
              f"  {cols} needs a build with docs/spec/foreign-keys.md in it "
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
              f"needs a build with docs/spec/cabin.md in it; re-run without "
              f"--cabin, or rebuild the server.", reply)
    if "no room" in reply or "reserved catalog page range" in reply:
        abort(f"could not create {base}_{suffix}: the catalog is out of column "
              f"space.\n  Catalog relations chain into a reserved range "
              f"(~7,800 columns for the whole instance); this scenario needs 68 "
              f"per run and nothing reclaims them, because there is no DROP "
              f"TABLE.\n  Restart the server on a fresh data file.", reply)


# ---- load ----------------------------------------------------------------

def demand_of(args):
    """What this run is expected to ask of the fleet and of the customers.

    Computed once, from the flags, before anything is loaded - both limits
    are derived from it, which is what keeps the two rejection axes
    reachable at any scale (see CAPACITY_SPREAD)."""
    mean_cbm = (CARGO_MIN_CBM + CARGO_MAX_CBM) / 2 * MILLI
    per_voyage = mean_cbm * args.cargos / max(args.operations, 1)
    mean_total = mean_cbm / MILLI * BASE_RATE_PER_CBM * FEE_MULTIPLE
    per_customer = mean_total * args.cargos / max(args.organizations, 1)
    return {
        "cbm_per_voyage": per_voyage * args.capacity_headroom,
        "spend_per_customer": per_customer * args.credit_headroom,
    }


def load_organizations(exec_, table, count, demand, rng, phase):
    """Returns [(org_id, credit_limit)] in creation order.

    `outstanding` opens at 0 and is the column the booking transaction
    moves; `credit_limit` is carried back because the booker checks against
    it client-side (the engine has no arithmetic in a select list, so there
    is no server-side CHECK to lean on)."""
    orgs = []
    for i in range(count):
        limit = max(CREDIT_FLOOR,
                    int(demand["spend_per_customer"] * rng.uniform(*CREDIT_SPREAD)))
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


def load_ships(exec_, table, count, demand, rng, phase):
    """Returns [(ship_id, capacity_cbm)] in creation order."""
    ships = []
    for i in range(count):
        capacity = max(CAPACITY_FLOOR_CBM,
                       int(demand["cbm_per_voyage"] * rng.uniform(*CAPACITY_SPREAD)))
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
        value = rng.randint(DECLARED_MIN, DECLARED_MAX)
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


def fees_by_id(fees):
    """The rate card re-keyed by the Keystone id the server issued, which is
    what a recipe row names. `load_fees` keys by `fee_code` because that is
    the stable business number this file writes; the booking joins on
    neither, it looks up by id."""
    return {fee_id: (basis, amount) for fee_id, basis, amount in fees.values()}


def route_code(origin, destination):
    """One packed route. PORTS is small and fixed, so this is a stable
    integer key rather than a hash - two runs of this tool address the same
    route with the same number."""
    return origin * PORTS + destination


# ---- the booking transaction (docs/inflight/in-progress/scenario2-freight.md §3) --------------
#
# The measured unit (S2-1): one cargo placed on one voyage. Eight statements
# under one BEGIN/COMMIT - two pk lookups, two filtered reads, two ledger
# inserts, two btree updates - and three possible outcomes, counted
# separately because a TPS number that folds them together is a wrong number
# (S2-6):
#
#   committed   the business said yes and the engine agreed
#   rejected    the business said no  - over the voyage's capacity, or over
#               the customer's credit limit. A *correct* outcome, not a
#               failure, and the driver rolls back and moves on
#   conflicted  ERR TXN_CONFLICT - another booker wrote one of the two rows
#               this one updates. Retried from the top, because every input
#               it read is stale by definition
#
# **The two checks are client-side**, between statements 4 and 5, and they
# have to be: the engine has no arithmetic in a select list and no CHECK
# constraint, so there is no server-side expression that could refuse a
# booking. What the engine provides is the part that matters - that the
# read the check was made against and the write the check authorized are one
# atomic unit - and `--no-txn` is what measures the difference.

# How far the simulated business clock runs. Only the fee validity windows
# read it, and they are wider than this on purpose: a booking refused
# because every rule expired would be a rejection this scenario does not
# mean to measure.
HORIZON_DAYS = 180

# The phases every booker records, in report order.
BOOKING_PHASES = ("booking", "cargo-lookup", "credit-lookup", "capacity-read",
                  "recipe-read", "freight-insert", "charge-insert",
                  "operation-update", "org-update", "commit")

COMMITTED, REJECTED_CAPACITY, REJECTED_CREDIT, CONFLICTED, FAILED = (
    "committed", "rejected-capacity", "rejected-credit", "conflicted", "failed")

# **Which row two bookers collided on.** A single "conflicts" count cannot be
# acted on: the two updates are on different relations, contended by
# different things - the voyage by bookers loading the same ship, the
# customer by bookers carrying the same shipper's cargo - and a workload can
# be heavy on one and free of the other. `read` covers a conflict raised
# before either update, which today means only a foreign-key check meeting an
# in-flight writer (docs/spec/foreign-keys.md reuses kTxnConflict for it);
# `commit` covers one raised by COMMIT itself.
AXIS_OPERATIONS, AXIS_ORGANIZATIONS = "operations", "organizations"
AXIS_READ, AXIS_COMMIT = "read", "commit"
CONFLICT_AXES = (AXIS_OPERATIONS, AXIS_ORGANIZATIONS, AXIS_READ, AXIS_COMMIT)


def select_rows(reply):
    """The data rows of a SELECT reply, each split into its fields.

    None when the statement failed, which is not the same as an empty list -
    the caller has to be able to tell "no such row" from "no answer"."""
    if reply.startswith("ERR"):
        return None
    lines = [line for line in reply.split("\n") if line]
    return [line.split(",") for line in lines[1:]]


def sum_value(rows):
    """A `SELECT SUM(...)` answer as an integer.

    SUM over no rows is NULL by AG4's standard semantics, and the text
    protocol spells that `NULL`. Reading it as zero is right *here* -
    "nothing booked on this voyage yet" - and would not be right in general,
    which is why the coercion lives at the two call sites' shared helper and
    not in `select_rows`."""
    if not rows or not rows[0] or rows[0][0].strip() in ("", "NULL"):
        return 0
    return int(rows[0][0])


def is_conflict(reply):
    """`ERR TXN_CONFLICT retryable=1 ...` - the one error worth
    special-casing (docs/spec/client-manual.md). Every other ERR is a defect in
    this driver or in the engine, and is counted as one."""
    return reply.startswith("ERR") and "TXN_CONFLICT" in reply


def charge_lines(recipe_rows, fee_book, route, day, cbm, declared_value, max_fees):
    """The fees that apply to one booking, priced.

    Computed here rather than read per fee, because the rate card is
    immutable for the length of a run: `fees` is loaded once and `recipes`
    is read per booking, which is the split the workload actually has - the
    rules are what a query filters on, the amounts are reference data.

    `max_fees` caps the list at N by priority. The generated rule set bounds
    itself at 8 matches (§10), so the cap is inert at its default of 0 and
    exists to *lower* the fan-out for a variance experiment, never to make
    the workload work."""
    lines = []
    for fee_id, rc, priority, valid_from, valid_to in recipe_rows:
        if rc != ROUTE_ANY and rc != route:
            continue
        if not (valid_from <= day <= valid_to):
            continue
        priced = fee_book.get(fee_id)
        if priced is None:
            continue
        basis, amount = priced
        if basis == BASIS_FLAT:
            charged = amount
        elif basis == BASIS_PER_CBM:
            charged = amount * (cbm // MILLI)
        else:
            charged = declared_value * amount // 1000
        lines.append((priority, fee_id, charged))
    lines.sort()
    if max_fees > 0:
        lines = lines[:max_fees]
    return [(fee_id, charged) for _priority, fee_id, charged in lines]


class BookingState:
    """Everything one booker draws from, and everything it believes it
    wrote.

    The belief half is what `--verify` checks the engine against. It is kept
    per relation rather than as one running total, because the two questions
    a torn transaction raises are different: "is this voyage's stored
    `booked_cbm` the sum of its freights" is answerable from the engine
    alone, and "did a write this driver counted get lost" is only answerable
    against what the driver thinks it sent."""

    def __init__(self, operations, orgs, cargos, fee_book, rng):
        self.operations = operations
        self.voyage = {op: (ship, capacity, route_code(origin, destination))
                       for op, ship, capacity, origin, destination in operations}
        self.credit_limit = dict(orgs)
        self.cargos = cargos
        # A cargo ships once. Drawn without replacement so a booking never
        # re-books one, which would make `booked_cbm` grow past what the
        # cargo relation can account for and turn I1 into a false alarm.
        self.pool = list(range(len(cargos)))
        rng.shuffle(self.pool)
        self.fee_book = fee_book
        # Driver-side belief, keyed by row id.
        self.booked_cbm = {op: 0 for op, _s, _c, _o, _d in operations}
        self.revenue = {op: 0 for op, _s, _c, _o, _d in operations}
        self.outstanding = {org: 0 for org, _limit in orgs}
        self.freight_total = {}      # freight_id -> what the customer owes
        self.freight_charges = {}    # freight_id -> number of charge rows

    def take_cargo(self):
        """The pool holds *indices*, so returning a cargo is O(1) - a
        rejection rate in the tens of percent must not cost a linear scan of
        200,000 rows each time."""
        return self.pool.pop() if self.pool else None

    def give_back(self, index):
        """A rejected or conflicted booking returns its cargo to the pool:
        it was never shipped, and a run that consumed cargos on rejection
        would exhaust the pool at a rate set by the rejection rate."""
        self.pool.append(index)


def book_once(client, tables, state, args, phases, rng, day, axes):
    """One booking attempt. Returns one of the five outcome constants.

    A `conflicted` return means the caller should retry: nothing was
    written, the transaction is rolled back, and every value this attempt
    read is by definition stale. `axes` is the per-axis conflict tally the
    attempt adds to on its way out."""
    index = state.take_cargo()
    if index is None:
        return None
    cargo_id, _cargo_org, cargo_type, cbm, declared_value = state.cargos[index]
    op_id = rng.choice(state.operations)[0]
    ship_id, capacity, route = state.voyage[op_id]

    def finish(outcome, opened):
        if opened:
            client("ROLLBACK")
        # **A capacity rejection returns the cargo; a credit rejection
        # retires it.** The asymmetry is not a tuning choice: `outstanding`
        # only ever grows - nothing in this workload pays an invoice - so a
        # cargo its customer could not afford now can never be afforded
        # later, and putting it back means drawing it again forever. A
        # voyage that was full is a different matter: the next draw picks a
        # different voyage, and this cargo may well fit it.
        if outcome not in (COMMITTED, REJECTED_CREDIT):
            state.give_back(index)
        return outcome

    def conflict(axis):
        axes[axis] = axes.get(axis, 0) + 1
        return CONFLICTED

    opened = False
    if args.txn:
        if client("BEGIN").startswith("ERR"):
            state.give_back(cargo)
            return FAILED
        opened = True

    # 1. the cargo - a pk lookup, and the only statement that tells the
    #    booking who the customer is.
    reply = send(client, phases["cargo-lookup"],
                 f"SELECT org_id, cargo_type, cbm, declared_value "
                 f"FROM {tables['cargos']} WHERE id = {cargo_id}")
    rows = select_rows(reply)
    if rows is None:
        return finish(conflict(AXIS_READ) if is_conflict(reply) else FAILED, opened)
    if not rows:
        return finish(FAILED, opened)
    org_id = int(rows[0][0])

    # 2. the customer's credit - the second lookup, and the second row this
    #    transaction will update.
    reply = send(client, phases["credit-lookup"],
                 f"SELECT credit_limit, outstanding FROM "
                 f"{tables['organizations']} WHERE id = {org_id}")
    rows = select_rows(reply)
    if rows is None:
        return finish(conflict(AXIS_READ) if is_conflict(reply) else FAILED, opened)
    if not rows:
        return finish(FAILED, opened)
    credit_limit, outstanding = int(rows[0][0]), int(rows[0][1])

    # 3. the capacity, one of two ways. `cached` trusts the derived column;
    #    `scan` re-derives it from the ledger every booking. Identical
    #    outcomes, very different cost - the gap is what the derived column
    #    is worth (S2-7).
    if args.capacity_mode == "cached":
        # Two columns, one statement. `revenue` is read rather than
        # remembered because a second booker may have moved it since this
        # one last looked - the engine has no `SET revenue = revenue + n`,
        # so a running total can only be maintained by reading it in the
        # transaction that writes it.
        reply = send(client, phases["capacity-read"],
                     f"SELECT booked_cbm, revenue FROM {tables['operations']} "
                     f"WHERE id = {op_id}")
        rows = select_rows(reply)
        if rows is None:
            return finish(conflict(AXIS_READ) if is_conflict(reply) else FAILED, opened)
        booked = int(rows[0][0]) if rows else 0
        revenue = int(rows[0][1]) if rows else 0
    else:
        reply = send(client, phases["capacity-read"],
                     f"SELECT SUM(cbm) FROM {tables['freights']} "
                     f"WHERE operation_id = {op_id}")
        rows = select_rows(reply)
        if rows is None:
            return finish(conflict(AXIS_READ) if is_conflict(reply) else FAILED, opened)
        booked = sum_value(rows)
        # `scan` mode never reads the operations row, so it has no value to
        # add to and **does not maintain `revenue` at all**. That is the
        # honest shape of the mode rather than a gap in it: maintaining a
        # money total requires reading it, and reading it is precisely the
        # pk lookup this mode exists to avoid. The derived column's real
        # cost is that it forces the read; its real benefit is that having
        # read it, a second derived value is free.
        revenue = None

    # 4. the pricing rules for this cargo type - a non-pk equality, so a
    #    FilterScan, and the statement `--cabin` serves.
    reply = send(client, phases["recipe-read"],
                 f"SELECT fee_id, route_code, priority, valid_from, valid_to "
                 f"FROM {tables['recipes']} WHERE cargo_type = {cargo_type}")
    rows = select_rows(reply)
    if rows is None:
        return finish(conflict(AXIS_READ) if is_conflict(reply) else FAILED, opened)
    recipe_rows = [(int(r[0]), int(r[1]), int(r[2]), int(r[3]), int(r[4]))
                   for r in rows]

    # The two checks. Client-side, because there is no server-side
    # expression that could make them - and this is the seam `--no-txn`
    # measures: the read above and the write below are one unit or they are
    # not.
    if booked + cbm > capacity:
        return finish(REJECTED_CAPACITY, opened)

    rate = BASE_RATE_PER_CBM + rng.randint(-RATE_JITTER, RATE_JITTER)
    freight_amount = (cbm // MILLI) * rate
    lines = charge_lines(recipe_rows, state.fee_book, route, day, cbm,
                         declared_value, args.max_fees)
    total = freight_amount + sum(charged for _fee, charged in lines)

    if outstanding + total > credit_limit:
        return finish(REJECTED_CREDIT, opened)

    # 5. the order line.
    reply = send(client, phases["freight-insert"],
                 f"INSERT INTO {tables['freights']} VALUES "
                 f"({op_id}, {ship_id}, {cargo_id}, {cbm}, {rate}, {day}, 0)")
    freight_id = inserted_id(reply)
    if freight_id is None:
        return finish(conflict(AXIS_READ) if is_conflict(reply) else FAILED, opened)

    # 6. what it was charged.
    for fee_id, charged in lines:
        reply = send(client, phases["charge-insert"],
                     f"INSERT INTO {tables['charges']} VALUES "
                     f"({freight_id}, {fee_id}, {charged}, {day})")
        if reply.startswith("ERR"):
            return finish(conflict(AXIS_READ) if is_conflict(reply) else FAILED, opened)

    # 7. the voyage. The value written depends on the value read at step 3,
    #    which is exactly the lost-update shape - and is the first of the two
    #    rows two concurrent bookers collide on.
    sets = f"booked_cbm = {booked + cbm}"
    if revenue is not None:
        sets += f", revenue = {revenue + total}"
    reply = send(client, phases["operation-update"],
                 f"UPDATE {tables['operations']} SET {sets} WHERE id = {op_id}")
    if reply.startswith("ERR"):
        if is_conflict(reply):
            return finish(conflict(AXIS_OPERATIONS), opened)
        return finish(FAILED, opened)

    # 8. the customer. Same shape, different axis - two bookers collide here
    #    when they carry one customer's cargo, and at step 7 when they load
    #    one voyage.
    reply = send(client, phases["org-update"],
                 f"UPDATE {tables['organizations']} SET "
                 f"outstanding = {outstanding + total} WHERE id = {org_id}")
    if reply.startswith("ERR"):
        if is_conflict(reply):
            return finish(conflict(AXIS_ORGANIZATIONS), opened)
        return finish(FAILED, opened)

    if opened:
        reply = send(client, phases["commit"], "COMMIT")
        if reply.startswith("ERR"):
            client("ROLLBACK")
            state.give_back(index)
            if is_conflict(reply):
                axes[AXIS_COMMIT] = axes.get(AXIS_COMMIT, 0) + 1
                return CONFLICTED
            return FAILED

    state.booked_cbm[op_id] = booked + cbm
    if revenue is not None:
        state.revenue[op_id] = revenue + total
    state.outstanding[org_id] = outstanding + total
    state.freight_total[freight_id] = total
    state.freight_charges[freight_id] = len(lines)
    return COMMITTED


def run_bookings(client, tables, state, args, phases, rng, target, deadline_wall):
    """Drives bookings until `target` commits or `deadline_wall` passes.

    `target` is this booker's own share of `--bookings`, not the run's total,
    and `deadline_wall` is a `time.time()` value shared by every booker so
    they stop together rather than each measuring from its own start.

    **A retry is not an error and a conflicted attempt is not a booking.**
    Each `TXN_CONFLICT` is counted, on the axis it happened on, and the whole
    transaction is re-driven from its first read - every value the failed
    attempt held is stale by definition. Only a terminal outcome is charged
    to the `booking` phase, so the latency of a booking that took three
    attempts is the latency of all three, which is what a client actually
    waits."""
    counts = {COMMITTED: 0, REJECTED_CAPACITY: 0, REJECTED_CREDIT: 0,
              CONFLICTED: 0, FAILED: 0}
    axes = {axis: 0 for axis in CONFLICT_AXES}
    retries = 0
    abandoned = 0
    exhausted = False
    booking = phases["booking"]
    started = time.perf_counter()

    while time.time() < deadline_wall:
        if target and counts[COMMITTED] >= target:
            break
        elapsed = time.perf_counter() - started
        day = DAY0 + min(HORIZON_DAYS - 1,
                         int(elapsed / max(args.seconds, 1e-9) * HORIZON_DAYS))
        t0 = time.perf_counter()
        outcome = None
        for attempt in range(args.max_retries + 1):
            if attempt:
                retries += 1
            outcome = book_once(client, tables, state, args, phases, rng, day, axes)
            if outcome != CONFLICTED:
                break
            counts[CONFLICTED] += 1
        if outcome is None:
            exhausted = True
            break
        if outcome == CONFLICTED:
            # Out of retries. The booking never happened, and saying so is
            # the difference between a retry budget that is generous and one
            # that is silently too small.
            abandoned += 1
            continue
        counts[outcome] += 1
        booking.record(time.perf_counter() - t0, "" if outcome != FAILED else "ERR")

    return {
        "counts": counts,
        "axes": axes,
        "retries": retries,
        "abandoned": abandoned,
        "elapsed": time.perf_counter() - started,
        "cargo_pool_exhausted": exhausted,
        "cargo_pool_left": len(state.pool),
    }


# ---- several bookers (docs/inflight/in-progress/scenario2-freight.md §5) ----------------------

def partition(operations, orgs, cargos, bookers, contend):
    """One (operations, orgs, cargos) slice per booker.

    **Cargos are split in both modes**, and that is not what `--contend`
    means. A cargo is shipped once; two bookers holding the same cargo would
    book it onto two voyages, which is not contention but a driver that lost
    track of its own pool. What `--contend` shares is the two rows a booking
    *updates* - the voyage and the customer - because those are the conflict
    axes.

    Under `--contend` every booker draws from every voyage and every
    customer, so two may load the same ship or carry the same shipper.

    Under `--no-contend` the slices are **disjoint on both axes at once**:
    voyages round-robin, customers round-robin, and a booker takes only the
    cargos its own customers own. Partitioning voyages alone would not do it,
    because a cargo's customer is a property of the cargo, so two bookers on
    different ships would still collide on `organizations`. With both split,
    no conflict is possible - which is what makes it the baseline the
    contended run is measured against."""
    if contend:
        return [(operations, orgs, cargos[i::bookers]) for i in range(bookers)]
    slices = []
    for i in range(bookers):
        orgs_i = orgs[i::bookers]
        owned = {org for org, _limit in orgs_i}
        slices.append((operations[i::bookers], orgs_i,
                       [c for c in cargos if c[1] in owned]))
    return slices


def booker_process(index, args, suffix, slice_, fee_book, deadline_wall,
                   target, result_q):
    """One booker: its own connection, its own RNG, its own slice.

    Everything it needs is passed in rather than shared, because the point of
    several bookers is that they contend **in the database** and nowhere
    else. What comes back is the outcome tally, the per-axis conflict tally,
    the latencies for every phase, and the belief state `--verify` needs."""
    try:
        set_echo(getattr(args, "echo", False))
        rng = random.Random(args.seed + 1000 + index)
        operations, orgs, cargos = slice_
        client = Client(args.host, args.port, args.timeout)
        if args.isolation:
            client(f"SET ISOLATION LEVEL {args.isolation.replace('-', ' ').upper()}")
        tables = {base: f"{base}_{suffix}" for base in CREATE_ORDER}
        phases = {name: Phase(name) for name in BOOKING_PHASES}
        state = BookingState(operations, orgs, cargos, fee_book, rng)
        result = run_bookings(client, tables, state, args, phases, rng,
                              target, deadline_wall)
        client.close()
    except Exception as e:                       # noqa: BLE001 - reported, not raised
        result_q.put({"index": index, "fatal": f"{type(e).__name__}: {e}"})
        return
    result_q.put({
        "index": index,
        "counts": result["counts"], "axes": result["axes"],
        "retries": result["retries"], "abandoned": result["abandoned"],
        "elapsed": result["elapsed"],
        "cargo_pool_exhausted": result["cargo_pool_exhausted"],
        "cargo_pool_left": result["cargo_pool_left"],
        "phases": {name: (p.latencies, p.errors, p.first_error)
                   for name, p in phases.items()},
        # The belief half of --verify. booked_cbm and outstanding are only
        # authoritative when the slices are disjoint; freight_charges always
        # is, because one freight is written by exactly one booker.
        "booked_cbm": result_state_booked(state),
        "outstanding": {k: v for k, v in state.outstanding.items() if v},
        "freight_charges": state.freight_charges,
        "voyage": state.voyage,
        "errors": client.errors if hasattr(client, "errors") else 0,
    })


def result_state_booked(state):
    return {k: v for k, v in state.booked_cbm.items() if v}


# ---- the reporter (docs/inflight/in-progress/scenario2-freight.md §6) -------------------------

MANIFEST_PHASES = ("manifest-scan", "voyage-rollup", "customer-statement")


def manifest_process(args, suffix, operations, orgs, stop_event, result_q):
    """The analytic reader, in its own process, contending with the bookers.

    This is the half of the workload that does not commit anything: every
    `--manifest-interval` seconds it wakes, reads a sample of voyages and
    customers, and goes back to sleep. Its three reads are deliberately the
    three shapes the engine treats differently:

      manifest-scan       SELECT * ... WHERE operation_id = n   FilterScan over
                          a relation that is *growing under it* - the only
                          read here whose cost rises as the run proceeds
      voyage-rollup       the same walk with a GROUP BY folded over it, so the
                          pair prices the fold against the walk it rides on
      customer-statement  a two-step join chain with the fold on the *second*
                          step's column - the shape S2-01 had to prove the
                          engine would even compile

    It counts rows as well as latency, because a FilterScan's cost is a
    function of how much relation there is and a latency alone cannot say
    whether a slow pass read more or read slower.

    It never writes, so it can never conflict, and its latency is reported
    beside TPS rather than inside it: a reporter that got slower while the
    bookers got faster is the contention this scenario exists to create."""
    try:
        set_echo(getattr(args, "echo", False))
        rng = random.Random(args.seed + 9000)
        client = Client(args.host, args.port, args.timeout)
        tables = {base: f"{base}_{suffix}" for base in CREATE_ORDER}
        phases = {name: Phase(name) for name in MANIFEST_PHASES}
        op_ids = [op for op, _s, _c, _o, _d in operations]
        org_ids = [org for org, _limit in orgs]
        passes = rows_read = 0

        while not stop_event.is_set():
            started = time.perf_counter()
            for op_id in rng.sample(op_ids, min(args.manifest_voyages, len(op_ids))):
                if stop_event.is_set():
                    break
                reply = send(client, phases["manifest-scan"],
                             f"SELECT * FROM {tables['freights']} "
                             f"WHERE operation_id = {op_id}")
                rows = select_rows(reply)
                rows_read += len(rows) if rows else 0
                send(client, phases["voyage-rollup"],
                     f"SELECT status, COUNT(*), SUM(cbm) FROM {tables['freights']} "
                     f"WHERE operation_id = {op_id} GROUP BY status")
            for org_id in rng.sample(org_ids, min(args.manifest_customers,
                                                  len(org_ids))):
                if stop_event.is_set():
                    break
                send(client, phases["customer-statement"],
                     f"SELECT c.org_id, SUM(f.cbm) FROM {tables['freights']} AS f "
                     f"JOIN {tables['cargos']} AS c ON f.cargo_id = c.id "
                     f"WHERE c.org_id = {org_id} GROUP BY c.org_id")
            passes += 1
            # Sleep what is left of the interval, but wake at once when the
            # bookers finish: a reporter still running after the measured
            # work has stopped is reading an idle server and would flatter
            # its own percentiles.
            rest = args.manifest_interval - (time.perf_counter() - started)
            if rest > 0:
                stop_event.wait(rest)
        client.close()
    except Exception as e:                       # noqa: BLE001 - reported, not raised
        result_q.put({"fatal": f"{type(e).__name__}: {e}"})
        return
    result_q.put({
        "passes": passes, "rows_read": rows_read,
        "phases": {name: (p.latencies, p.errors, p.first_error)
                   for name, p in phases.items()},
    })


def merge_bookers(results, elapsed):
    """One run's worth of numbers out of N bookers' worth.

    `elapsed` is the run's wall clock, not the sum of the contributors': the
    throughput being reported is aggregate, so dividing by any one booker's
    elapsed time would multiply it by N."""
    counts = {COMMITTED: 0, REJECTED_CAPACITY: 0, REJECTED_CREDIT: 0,
              CONFLICTED: 0, FAILED: 0}
    axes = {axis: 0 for axis in CONFLICT_AXES}
    merged = {"retries": 0, "abandoned": 0, "cargo_pool_left": 0,
              "cargo_pool_exhausted": False}
    phases = {name: Phase(name) for name in BOOKING_PHASES}
    state = {"booked_cbm": {}, "outstanding": {}, "freight_charges": {},
             "voyage": {}}
    for r in results:
        for k, v in r["counts"].items():
            counts[k] += v
        for k, v in r["axes"].items():
            axes[k] += v
        merged["retries"] += r["retries"]
        merged["abandoned"] += r["abandoned"]
        merged["cargo_pool_left"] += r["cargo_pool_left"]
        merged["cargo_pool_exhausted"] |= r["cargo_pool_exhausted"]
        for name, (latencies, errors, first_error) in r["phases"].items():
            p = phases[name]
            p.latencies.extend(latencies)
            p.errors += errors
            if p.first_error is None:
                p.first_error = first_error
        state["booked_cbm"].update(r["booked_cbm"])
        state["outstanding"].update(r["outstanding"])
        state["freight_charges"].update(r["freight_charges"])
        state["voyage"].update(r["voyage"])
    for p in phases.values():
        p.elapsed = elapsed
    merged.update({"counts": counts, "axes": axes, "elapsed": elapsed,
                   "phases": phases, "state": state,
                   "per_booker": [r["counts"][COMMITTED] for r in results]})
    return merged


# ---- verification (docs/inflight/in-progress/scenario2-freight.md §4) -------------------------

def verify(client, tables, state, sample, rng, trust_belief=True):
    """I1-I4, on a sample. Returns (checks, failures, first).

    What is being asked is not "is the arithmetic right" - the driver did
    the arithmetic - but **whether a write this driver counted as applied
    was lost**, and whether the engine's own two accounts of the same
    quantity agree with each other. Under `--txn` both must hold; under
    `--no-txn` and concurrency they need not, and that contrast is the
    reason the flag exists."""
    checks = failures = 0
    first = None

    def fail(message):
        nonlocal failures, first
        failures += 1
        if first is None:
            first = message

    booked = [op for op, total in state["booked_cbm"].items() if total > 0]
    for op_id in rng.sample(booked, min(sample, len(booked))):
        rows = select_rows(client(f"SELECT SUM(cbm) FROM {tables['freights']} "
                                  f"WHERE operation_id = {op_id}"))
        stored = select_rows(client(f"SELECT booked_cbm FROM {tables['operations']} "
                                    f"WHERE id = {op_id}"))
        if rows is None or stored is None or not stored:
            continue
        ledger = sum_value(rows)
        column = int(stored[0][0])
        checks += 1
        # I1: the derived column against the ledger it derives from.
        if ledger != column:
            fail(f"I1 operation {op_id}: booked_cbm={column}, "
                 f"SUM(freights.cbm)={ledger}")
        elif trust_belief and column != state["booked_cbm"].get(op_id):
            # Only asked when the bookers' slices are disjoint. Under
            # --contend a voyage is written by several bookers and no single
            # one of them knows what the total should be, so the driver-side
            # arm of I1 is not a question that has an answer.
            fail(f"I1 operation {op_id}: stored {column}, driver wrote "
                 f"{state['booked_cbm'].get(op_id)} - a write was lost")
        # I2: and neither of them past what the ship can carry.
        _ship, capacity, _route = state["voyage"][op_id]
        checks += 1
        if column > capacity:
            fail(f"I2 operation {op_id}: booked_cbm={column} over capacity {capacity}")

    owing = [org for org, total in state["outstanding"].items() if total > 0]
    for org_id in rng.sample(owing, min(sample, len(owing))):
        rows = select_rows(
            client(f"SELECT f.id, f.cbm, f.price_per_cbm FROM {tables['freights']} AS f "
                   f"JOIN {tables['cargos']} AS c ON f.cargo_id = c.id "
                   f"WHERE c.org_id = {org_id}"))
        stored = select_rows(client(f"SELECT outstanding FROM "
                                    f"{tables['organizations']} WHERE id = {org_id}"))
        if rows is None or stored is None or not stored:
            continue
        # I3: recomputed from the rows the engine returns, never re-read
        # from a column that would only be agreeing with itself.
        recomputed = 0
        for freight in rows:
            freight_id, cbm, rate = int(freight[0]), int(freight[1]), int(freight[2])
            charged = select_rows(client(f"SELECT SUM(amount) FROM {tables['charges']} "
                                         f"WHERE freight_id = {freight_id}"))
            recomputed += (cbm // MILLI) * rate + sum_value(charged)
        checks += 1
        if recomputed != int(stored[0][0]):
            fail(f"I3 organization {org_id}: outstanding={stored[0][0]}, "
                 f"recomputed from its freights and charges={recomputed}")

    # I4: a freight's charge rows against the rule set replayed for it.
    written = list(state["freight_charges"])
    for freight_id in rng.sample(written, min(sample, len(written))):
        rows = select_rows(client(f"SELECT id FROM {tables['charges']} "
                                  f"WHERE freight_id = {freight_id}"))
        if rows is None:
            continue
        checks += 1
        if len(rows) != state["freight_charges"][freight_id]:
            fail(f"I4 freight {freight_id}: {len(rows)} charge rows stored, "
                 f"{state['freight_charges'][freight_id]} written")

    return checks, failures, first


# ---- the capability probe (docs/inflight/in-progress/scenario2-freight.md §6) -----------------

def probe_reads(exec_, tables, sample_op, sample_org):
    """Runs each read the later tasks depend on, once, and reports which the
    server accepts.

    The third one is why this function exists. **Nothing in this repo
    aggregates over a joined chain today**: `docs/spec/aggregate.md` AG1
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
        print( "  blocker for S2-02, not a fallback (docs/inflight/in-progress/scenario2-freight.md §10).")
    print()


def print_bookings(result, args):
    """The three outcomes, separately (S2-6), and the TPS that only the
    first of them earns."""
    counts = result["counts"]
    committed = counts[COMMITTED]
    attempted = sum(counts.values()) - counts[CONFLICTED]
    elapsed = result["elapsed"]
    print("bookings")
    print(f"  {'committed':<20}{committed:>10}"
          f"{committed / elapsed if elapsed else 0:>12.1f} TPS")
    for name in (REJECTED_CAPACITY, REJECTED_CREDIT):
        share = 100.0 * counts[name] / attempted if attempted else 0.0
        print(f"  {name:<20}{counts[name]:>10}{share:>11.1f}% of attempts")
    print(f"  {'conflicted':<20}{counts[CONFLICTED]:>10}"
          f"{result['retries']:>11} retries")
    axes = result.get("axes") or {}
    if counts[CONFLICTED]:
        # Which row they collided on. A single conflict count cannot be
        # acted on; these can - one says the fleet is the contended
        # resource, the other says the customer book is.
        for axis in CONFLICT_AXES:
            if axes.get(axis):
                share = 100.0 * axes[axis] / counts[CONFLICTED]
                print(f"    on {axis:<16}{axes[axis]:>10}{share:>11.1f}% of conflicts")
        per_commit = result["retries"] / committed if committed else 0.0
        print(f"    {'retries/booking':<18}{per_commit:>10.2f}")
    if result.get("abandoned"):
        print(f"  {'abandoned':<20}{result['abandoned']:>10}"
              f"  <- out of retries after --max-retries {args.max_retries}")
    if counts[FAILED]:
        print(f"  {'failed':<20}{counts[FAILED]:>10}"
              f"  <- not a rejection: an ERR this driver did not expect")
    print()
    print(f"  mode                 {'BEGIN/COMMIT' if args.txn else 'autocommit'}, "
          f"capacity={args.capacity_mode}, max_fees="
          f"{args.max_fees or 'uncapped'}")
    print(f"  bookers              {args.bookers}, "
          f"{'contended (shared voyages and customers)' if args.contend else 'partitioned (disjoint slices, no conflict possible)'}")
    if result.get("per_booker") and len(result["per_booker"]) > 1:
        spread = "  ".join(f"#{i}:{n}" for i, n in enumerate(result["per_booker"]))
        print(f"  per booker           {spread}")
    if result["cargo_pool_exhausted"]:
        print(f"  the cargo pool ran out before --seconds did: every cargo was "
              f"booked.\n  Raise --cargos, or read the TPS as an average over a "
              f"run that ended early.")
    else:
        print(f"  cargo pool left      {result['cargo_pool_left']}")

    if result.get("manifest"):
        m = result["manifest"]
        reads = sum(len(m["phases"][name][0]) for name in MANIFEST_PHASES)
        print()
        print(f"  manifest reporter    {m['passes']} passes, {reads} reads, "
              f"{m['rows_read']:,} freight rows scanned")
        print(f"  {'':<20} latency is in the table above, beside the bookings "
              f"it contended with")

    if "verify" in result:
        checks, failures, first = result["verify"]
        print()
        print(f"verify (§4)          {checks} checks, {failures} failure(s)")
        if first:
            print(f"  first: {first}")
        elif checks:
            print("  I1 booked_cbm == SUM(freights.cbm), I2 within ship capacity,")
            print("  I3 outstanding == recomputed charges, I4 charge rows == rules")
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
    parser.add_argument("--bookers", type=int, default=1, metavar="N",
                        help="booker processes, each with its own connection "
                             "(default: 1)")
    parser.add_argument("--contend", dest="contend", action="store_true",
                        default=True,
                        help="every booker draws from every voyage and every "
                             "customer, so two can collide on either of the "
                             "rows a booking updates (default)")
    parser.add_argument("--no-contend", dest="contend", action="store_false",
                        help="give each booker a disjoint slice of voyages "
                             "*and* customers, so no conflict is possible - "
                             "the baseline the contended run is measured "
                             "against")
    parser.add_argument("--manifest", dest="manifest", action="store_true",
                        default=True,
                        help="run the analytic reporter process beside the "
                             "bookers (default). It never writes, so it never "
                             "conflicts; its latency is reported beside TPS, "
                             "not inside it")
    parser.add_argument("--no-manifest", dest="manifest", action="store_false",
                        help="book with no concurrent reader")
    parser.add_argument("--manifest-interval", type=float, default=1.0,
                        help="seconds between reporter passes (default: 1.0)")
    parser.add_argument("--manifest-voyages", type=int, default=20,
                        help="voyages read per pass (default: 20)")
    parser.add_argument("--manifest-customers", type=int, default=10,
                        help="customer statements per pass (default: 10)")
    parser.add_argument("--seconds", type=float, default=60.0,
                        help="how long to book for (default: 60)")
    parser.add_argument("--bookings", type=int, default=0, metavar="N",
                        help="stop after N committed bookings; --seconds is "
                             "then a ceiling (default: 0, time-based)")
    parser.add_argument("--capacity-mode", choices=("cached", "scan"),
                        default="cached",
                        help="how the voyage's used capacity is read: `cached` "
                             "trusts operations.booked_cbm (one pk lookup), "
                             "`scan` re-derives it with SUM over the ledger "
                             "every booking (default: cached)")
    parser.add_argument("--txn", dest="txn", action="store_true", default=True,
                        help="run each booking inside BEGIN/COMMIT (default)")
    parser.add_argument("--no-txn", dest="txn", action="store_false",
                        help="send the eight statements as eight transactions. "
                             "The checks then read state the writes cannot be "
                             "assumed to still match - which is what §4's "
                             "invariants are for")
    parser.add_argument("--max-retries", type=int, default=5,
                        help="attempts after a TXN_CONFLICT before the booking "
                             "is abandoned (default: 5)")
    parser.add_argument("--max-fees", type=int, default=0, metavar="N",
                        help="cap the fees applied to one booking at N by "
                             "priority. The rule set bounds itself at 8, so 0 "
                             "(the default) is uncapped and this only lowers it")
    parser.add_argument("--verify", type=int, default=0, metavar="N",
                        help="after the run, check §4's invariants over a "
                             "sample of N operations, organizations and "
                             "freights (default: 0, off)")
    parser.add_argument("--isolation", choices=("read-committed", "repeatable-read"),
                        default=None,
                        help="SET ISOLATION LEVEL for the booking connection "
                             "(default: the server's)")
    parser.add_argument("--capacity-headroom", type=float, default=1.0,
                        help="scales every ship's capacity against this run's "
                             "expected demand per voyage. 1.0 sizes the fleet "
                             "to exactly the cargo it is offered, so the "
                             "smaller ships fill and start refusing; raise it "
                             "to make the capacity axis quiet (default: 1.0)")
    parser.add_argument("--credit-headroom", type=float, default=1.0,
                        help="the same, for customer credit against expected "
                             "spend (default: 1.0)")
    parser.add_argument("--hot-routes", type=int, default=6,
                        help="routes carrying a route-specific pricing rule, "
                             "which is what makes a booking's fee count vary "
                             "(default: 6)")
    parser.add_argument("--fk", action="store_true",
                        help="declare the three foreign keys (docs/spec/foreign-keys.md)")
    parser.add_argument("--cabin", action="store_true",
                        help=f"declare a Cabin on {CABIN_RELATION}.{CABIN_COLUMN} "
                             f"(docs/spec/cabin.md)")
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

    demand = demand_of(args)
    orgs = load_organizations(client, tables["organizations"], args.organizations,
                              demand, rng, phase("load-organizations"))
    ships = load_ships(client, tables["ships"], args.ships, demand, rng,
                       phase("load-ships"))
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

    if args.load_only:
        print()
        print("--load-only: stopping before the bookings. Drive this data file with")
        print(f"  {sys.argv[0]} --suffix {suffix}")
        result = None
    else:
        print_probe(probe_reads(client, tables, operations[0][0], orgs[0][0]))
        if args.isolation:
            level = args.isolation.replace("-", " ").upper()
            reply = client(f"SET ISOLATION LEVEL {level}")
            if reply.startswith("ERR"):
                abort(f"--isolation {args.isolation}", reply)

        slices = partition(operations, orgs, cargos, args.bookers, args.contend)
        empty = [i for i, (ops, orgs_i, cg) in enumerate(slices)
                 if not ops or not orgs_i or not cg]
        if empty:
            abort(f"--bookers {args.bookers} --no-contend leaves booker(s) "
                  f"{empty} with no voyages, no customers or no cargo.\n"
                  f"  Disjoint slices need at least one of each per booker: "
                  f"raise --operations/--organizations/--cargos, or lower "
                  f"--bookers.")

        # One shared wall-clock deadline and one share of the target each, so
        # the bookers stop together and their sum is the run's target.
        target = -(-args.bookings // args.bookers) if args.bookings else 0
        deadline_wall = time.time() + args.seconds
        fee_book = fees_by_id(fees)
        result_q = multiprocessing.Queue()
        workers = [multiprocessing.Process(
            target=booker_process,
            args=(i, args, suffix, slices[i], fee_book, deadline_wall, target,
                  result_q))
            for i in range(args.bookers)]

        stop_event = multiprocessing.Event()
        manifest_q = multiprocessing.Queue()
        reporter = None
        if args.manifest:
            reporter = multiprocessing.Process(
                target=manifest_process,
                args=(args, suffix, operations, orgs, stop_event, manifest_q))

        run_started = time.perf_counter()
        if reporter is not None:
            reporter.start()
        for w in workers:
            w.start()
        results = [result_q.get() for _ in workers]
        for w in workers:
            w.join()
        run_elapsed = time.perf_counter() - run_started
        # The reporter stops when the measured work does, so its percentiles
        # describe a contended server and not an idle one.
        stop_event.set()
        manifest = None
        if reporter is not None:
            manifest = manifest_q.get()
            reporter.join(timeout=30)
            if "fatal" in manifest:
                abort(f"the manifest reporter died: {manifest['fatal']}")

        fatal = [r for r in results if "fatal" in r]
        if fatal:
            abort(f"booker {fatal[0]['index']} died: {fatal[0]['fatal']}")

        result = merge_bookers(results, run_elapsed)
        for name in BOOKING_PHASES:
            p = result["phases"][name]
            phases.append(p)
        result["phases"]["booking"].detail = (
            f"{'BEGIN/COMMIT' if args.txn else 'autocommit'}, "
            f"capacity={args.capacity_mode}, {args.bookers} booker"
            f"{'' if args.bookers == 1 else 's'}, "
            f"{'contended' if args.contend else 'partitioned'}")
        if manifest is not None:
            for name in MANIFEST_PHASES:
                latencies, errors, first_error = manifest["phases"][name]
                p = Phase(name)
                p.latencies, p.errors, p.first_error = latencies, errors, first_error
                p.elapsed = run_elapsed
                phases.append(p)
            result["manifest"] = manifest
        loaded["freights"] = result["counts"][COMMITTED]
        loaded["charges"] = sum(result["state"]["freight_charges"].values())
        if args.verify:
            result["verify"] = verify(client, tables, result["state"],
                                      args.verify, rng,
                                      trust_belief=not args.contend)

    meta = {
        "engine": "ckdbs",
        "scenario": "freight",
        "columns": sum(len(SCHEMA[b][0].split(",")) for b in CREATE_ORDER),
        "rows": sum(loaded.values()),
        "host": args.host, "port": args.port,
        "table": f"scenario2_{suffix}",
        "loaded": loaded,
        "fk": args.fk, "cabin": args.cabin,
        "txn": args.txn, "capacity_mode": args.capacity_mode,
        "seed": args.seed,
        # The workload identity, so compare_scenario2.py can refuse to diff
        # two runs that did different work. One key per flag that changes
        # what is loaded or what a booking does.
        "organizations": args.organizations, "ships": args.ships,
        "operations": args.operations, "cargos": args.cargos,
        "capacity_headroom": args.capacity_headroom,
        "credit_headroom": args.credit_headroom,
        "hot_routes": args.hot_routes, "max_fees": args.max_fees,
        "bookings": args.bookings, "bookers": args.bookers,
        "contend": args.contend, "manifest": args.manifest,
        "isolation": args.isolation or "server default",
    }
    if args.server_log:
        durability = read_durability(args.server_log)
        if durability:
            meta["durability"] = durability
    if result is not None:
        meta["outcomes"] = result["counts"]
        meta["retries"] = result["retries"]
        meta["axes"] = result["axes"]
        meta["abandoned"] = result["abandoned"]
        meta["tps"] = (result["counts"][COMMITTED] / result["elapsed"]
                       if result["elapsed"] > 0 else 0.0)
        if "verify" in result:
            checks, failures, first = result["verify"]
            meta["verify"] = {"checks": checks, "failures": failures,
                              "first": first}
        if result.get("manifest"):
            meta["manifest_passes"] = result["manifest"]["passes"]
            meta["manifest_rows_read"] = result["manifest"]["rows_read"]

    report(phases, meta, footer=(
        "S2-01..S2-05: builds, loads, books from --bookers processes, and",
        "runs the analytic reporter beside them. The PostgreSQL twin is",
        "tools/pg_scenario2_freight.py (single booker, reporter interleaved);",
        "diff the two --json files with tools/compare_scenario2.py.",
    ))
    if result is not None:
        print_bookings(result, args)
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
