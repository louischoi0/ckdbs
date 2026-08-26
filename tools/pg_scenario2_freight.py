#!/usr/bin/env python3
"""The PostgreSQL twin of tools/scenario2_freight.py — the same booking.

One workload, two engines, one report shape: this drives the identical
eight-statement freight booking against PostgreSQL so the ckdbs numbers have
a baseline. Everything that decides *what work happens* — the schema, the
rate card, the pricing rules, the demand sizing, the two limits and their
floors, the client-side pricing of a booking — is **imported from
scenario2_freight.py**, not restated. Only the dialect and the connection
differ, which is what keeps the two drivers from drifting into measuring
different questions.

    ./tools/pg_setup.sh init                       once
    ./tools/pg_scenario2_freight.py --bookings 1500

What is deliberately *not* mirrored, and why the comparison is still fair:

  **ids** ckdbs issues the Keystone pk itself (invariant 11) and refuses a
  caller-supplied one; PostgreSQL gets `bigserial`, which is the same
  contract - a system-generated ascending identity the client never chooses.

  **clustering** ckdbs picks heap or clustered-btree per relation at CREATE
  TABLE. PostgreSQL has one table shape plus a pk index, so `freights` and
  `charges` - HEAP on ckdbs, deliberately unindexed there - get a pk index
  here whether they want one or not. The booking never probes them by pk, so
  this costs PostgreSQL a little on insert and gives it nothing back.

  **the non-pk lookups** get the index PostgreSQL's planner would expect:
  `freights(operation_id)` and `recipes(cargo_type)`. On ckdbs those two
  reads are a FilterScan and a Cabin candidate respectively. Indexing them
  here rather than leaving PostgreSQL to sequential-scan is the honest
  comparison: it is what a DBA would do, and it is the baseline the ckdbs
  structures are trying to reach.

  **durability** ckdbs `group` against PostgreSQL's default
  `synchronous_commit = on`. Both fsync per commit; `--synchronous-commit`
  changes PostgreSQL's if a run wants the other point.

Everything else - the work target, the seed, the outcome accounting, the
percentile phases and `--verify` - is the same code path or the same shape,
so the two `--json` files are directly diffable.
"""

import argparse
import random
import sys
import time

from bench_common import Phase, report, write_json
from pg_wire import DEFAULT_HOST, PgConnection, PgError
from scenario2_freight import (
    BASE_RATE_PER_CBM, CARGO_MAX_CBM, CARGO_MIN_CBM, CARGO_TYPES, COMMITTED,
    CONFLICTED, COUNTRIES, CREATE_ORDER, CREDIT_FLOOR, CREDIT_SPREAD,
    CAPACITY_FLOOR_CBM, CAPACITY_SPREAD, DAY0, DECLARED_MAX, DECLARED_MIN,
    FAILED, FEES, HORIZON_DAYS, MANIFEST_PHASES, MILLI, ORG_TYPES, PORTS,
    RATE_JITTER, REJECTED_CAPACITY, REJECTED_CREDIT, RULE_WINDOW_DAYS,
    SHIP_TYPES, BookingState, charge_lines, demand_of, fees_by_id,
    print_bookings, route_code)

# The same eight relations, in PostgreSQL's dialect. `bigserial` stands in
# for the Keystone column and `integer`/`bigint` for the fixed-width types;
# `text` for varchar, since ckdbs pins one inline cell width for every string
# and PostgreSQL has no equivalent knob to mirror.
SCHEMA = {
    "organizations": (
        "id bigserial primary key, org_code text, name text, country integer, "
        "org_type integer, credit_limit bigint, outstanding bigint, tier integer, "
        "contact text, registered_day integer, status integer"),
    "ships": (
        "id bigserial primary key, imo text, name text, ship_type integer, "
        "capacity_cbm integer, dwt bigint, built_year integer, flag text, "
        "owner_id bigint, home_port integer, status integer"),
    "operations": (
        "id bigserial primary key, ship_id bigint, origin integer, "
        "destination integer, depart_day integer, arrive_day integer, "
        "status integer, booked_cbm integer, revenue bigint"),
    "cargos": (
        "id bigserial primary key, org_id bigint, cargo_type integer, "
        "weight_kg bigint, cbm integer, hazmat integer, declared_value bigint, "
        "origin integer, destination integer, ready_day integer"),
    "fees": (
        "id bigserial primary key, fee_name text, fee_code integer, basis integer, "
        "amount bigint, valid_from integer, valid_to integer"),
    "recipes": (
        "id bigserial primary key, cargo_type integer, route_code integer, "
        "fee_id bigint, priority integer, valid_from integer, valid_to integer"),
    "freights": (
        "id bigserial primary key, operation_id bigint, ship_id bigint, "
        "cargo_id bigint, cbm integer, price_per_cbm bigint, booked_day integer, "
        "status integer"),
    "charges": (
        "id bigserial primary key, freight_id bigint, fee_id bigint, "
        "amount bigint, applied_day integer"),
}

# The two non-pk equalities the booking issues. See the module docstring for
# why they are indexed here and not on ckdbs.
INDEXES = (
    ("freights", "operation_id"),
    ("recipes", "cargo_type"),
)


def abort(message, reply=None):
    print(f"pg scenario2 aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


class Client:
    """The same one-command-one-reply surface `scenario2_freight.Client`
    presents, so the booking body is the same shape on both engines."""

    def __init__(self, args):
        try:
            self._conn = PgConnection(args.host, args.port, args.user,
                                      args.database, args.password,
                                      timeout=args.timeout,
                                      application_name="pg_scenario2_freight.py")
        except (OSError, PgError) as e:
            abort(f"could not connect to {args.host}:{args.port}/{args.database}: {e}\n"
                  f"  start one with: ./tools/pg_setup.sh init")
        self.errors = 0
        self.first_error = None
        if args.synchronous_commit:
            reply = self(f"SET synchronous_commit = {args.synchronous_commit}")
            if reply.startswith("ERR"):
                abort("SET synchronous_commit failed", reply)

    def __call__(self, command):
        tag, rows, _bytes, error = self._conn.query(command)
        if error is not None:
            self._note(command, f"ERR {error}")
            return f"ERR {error}"
        return tag or "OK"

    def rows(self, command):
        result, error = self._conn.fetch(command)
        if error is not None:
            self._note(command, f"ERR {error}")
            return None
        return [[None if v is None else v.decode() for v in row] for row in result]

    def _note(self, command, reply):
        self.errors += 1
        if self.first_error is None:
            self.first_error = f"{command}  ->  {reply}"

    def close(self):
        self._conn.close()


def send(client, phase, command):
    t0 = time.perf_counter()
    reply = client(command)
    phase.record(time.perf_counter() - t0, reply)
    return reply


def rows_timed(client, phase, command):
    t0 = time.perf_counter()
    result = client.rows(command)
    phase.record(time.perf_counter() - t0, "ERR" if result is None else "OK")
    return result


def sum_value(rows):
    """`SUM` over no rows is NULL on both engines; the wire spells it
    differently and this is the PostgreSQL side of the same coercion."""
    if not rows or rows[0][0] is None or rows[0][0].strip() == "":
        return 0
    return int(rows[0][0])


# ---- DDL and load --------------------------------------------------------

def create_tables(client, suffix, phase):
    created = []
    for base in CREATE_ORDER:
        name = f"{base}_{suffix}"
        reply = send(client, phase, f"CREATE TABLE {name} ({SCHEMA[base]})")
        if reply.startswith("ERR"):
            abort(f"could not create {name}", reply)
        created.append(name)
    for base, column in INDEXES:
        reply = send(client, phase,
                     f"CREATE INDEX ON {base}_{suffix} ({column})")
        if reply.startswith("ERR"):
            abort(f"could not index {base}_{suffix}.{column}", reply)
    return created


def insert_returning(client, phase, statement):
    """PostgreSQL's `RETURNING id` is the twin of the ckdbs reply's `id=<n>`:
    both hand back the system-generated identity the client never chose."""
    result = rows_timed(client, phase, statement + " RETURNING id")
    return int(result[0][0]) if result else None


def load_organizations(client, table, count, demand, rng, phase):
    orgs = []
    for i in range(count):
        limit = max(CREDIT_FLOOR,
                    int(demand["spend_per_customer"] * rng.uniform(*CREDIT_SPREAD)))
        got = insert_returning(client, phase,
                               f"INSERT INTO {table} (org_code, name, country, org_type, "
                               f"credit_limit, outstanding, tier, contact, "
                               f"registered_day, status) VALUES "
                               f"('ORG{i:06d}', 'org{i:06d}', "
                               f"{rng.randrange(len(COUNTRIES))}, "
                               f"{rng.randrange(ORG_TYPES)}, {limit}, 0, "
                               f"{rng.randint(0, 3)}, 'ops{i:06d}@example.test', "
                               f"{DAY0 - rng.randint(0, 3650)}, 0)")
        if got is not None:
            orgs.append((got, limit))
    return orgs


def load_ships(client, table, count, demand, rng, phase):
    ships = []
    for i in range(count):
        capacity = max(CAPACITY_FLOOR_CBM,
                       int(demand["cbm_per_voyage"] * rng.uniform(*CAPACITY_SPREAD)))
        got = insert_returning(client, phase,
                               f"INSERT INTO {table} (imo, name, ship_type, capacity_cbm, "
                               f"dwt, built_year, flag, owner_id, home_port, status) VALUES "
                               f"('IMO{9000000 + i}', 'vessel{i:05d}', "
                               f"{rng.randrange(SHIP_TYPES)}, {capacity}, "
                               f"{rng.randint(10_000, 200_000)}, "
                               f"{rng.randint(1995, 2025)}, "
                               f"'{rng.choice(COUNTRIES)}', 0, "
                               f"{rng.randrange(PORTS)}, 0)")
        if got is not None:
            ships.append((got, capacity))
    return ships


def load_operations(client, table, count, ships, rng, phase):
    operations = []
    for _ in range(count):
        ship_id, capacity = rng.choice(ships)
        origin = rng.randrange(PORTS)
        destination = (origin + rng.randint(1, PORTS - 1)) % PORTS
        depart = DAY0 + rng.randint(0, 180)
        got = insert_returning(client, phase,
                               f"INSERT INTO {table} (ship_id, origin, destination, "
                               f"depart_day, arrive_day, status, booked_cbm, revenue) "
                               f"VALUES ({ship_id}, {origin}, {destination}, {depart}, "
                               f"{depart + rng.randint(3, 40)}, 0, 0, 0)")
        if got is not None:
            operations.append((got, ship_id, capacity, origin, destination))
    return operations


def load_cargos(client, table, count, orgs, rng, phase):
    cargos = []
    for _ in range(count):
        org_id, _limit = rng.choice(orgs)
        cargo_type = rng.randrange(len(CARGO_TYPES))
        cbm = rng.randint(CARGO_MIN_CBM, CARGO_MAX_CBM) * MILLI
        value = rng.randint(DECLARED_MIN, DECLARED_MAX)
        origin = rng.randrange(PORTS)
        destination = (origin + rng.randint(1, PORTS - 1)) % PORTS
        got = insert_returning(client, phase,
                               f"INSERT INTO {table} (org_id, cargo_type, weight_kg, cbm, "
                               f"hazmat, declared_value, origin, destination, ready_day) "
                               f"VALUES ({org_id}, {cargo_type}, "
                               f"{rng.randint(500, 40_000)}, {cbm}, "
                               f"{1 if CARGO_TYPES[cargo_type] == 'hazmat' else 0}, "
                               f"{value}, {origin}, {destination}, "
                               f"{DAY0 + rng.randint(0, 180)})")
        if got is not None:
            cargos.append((got, org_id, cargo_type, cbm, value))
    return cargos


def load_fees(client, table, phase):
    fees = {}
    for name, code, basis, amount in FEES:
        got = insert_returning(client, phase,
                               f"INSERT INTO {table} (fee_name, fee_code, basis, amount, "
                               f"valid_from, valid_to) VALUES "
                               f"('{name}', {code}, {basis}, {amount}, "
                               f"{DAY0 - RULE_WINDOW_DAYS}, {DAY0 + RULE_WINDOW_DAYS})")
        if got is not None:
            fees[code] = (got, basis, amount)
    return fees


def load_recipes(client, table, fees, hot_routes, rng, phase):
    """The same rule set scenario2_freight.load_recipes builds; the shape is
    restated here only because the INSERT dialect differs."""
    always = (101, 102, 104, 110)
    per_type = {
        "hazmat": (106, 107, 111), "reefer": (105, 112), "liquid": (108, 112),
        "vehicle": (108, 105), "project": (108, 109), "bulk": (109,),
        "dry": (), "container": (105,),
    }
    rows = []
    for cargo_type, type_name in enumerate(CARGO_TYPES):
        codes = [(code, -1) for code in always]
        codes += [(code, -1) for code in per_type[type_name]]
        codes += [(103, route) for route in hot_routes]
        for priority, (code, route) in enumerate(codes):
            if code not in fees:
                continue
            fee_id, _basis, _amount = fees[code]
            got = insert_returning(client, phase,
                                   f"INSERT INTO {table} (cargo_type, route_code, fee_id, "
                                   f"priority, valid_from, valid_to) VALUES "
                                   f"({cargo_type}, {route}, {fee_id}, {priority}, "
                                   f"{DAY0 - RULE_WINDOW_DAYS}, "
                                   f"{DAY0 + RULE_WINDOW_DAYS})")
            if got is not None:
                rows.append((got, cargo_type, route, code, priority))
    return rows


# ---- the booking ---------------------------------------------------------

def book_once(client, tables, state, args, phases, rng, day):
    """The same eight statements, in the same order, with the same two
    client-side checks between the fourth and the fifth."""
    index = state.take_cargo()
    if index is None:
        return None
    cargo_id, _org, cargo_type, cbm, declared_value = state.cargos[index]
    op_id = rng.choice(state.operations)[0]
    ship_id, capacity, route = state.voyage[op_id]

    def finish(outcome, opened):
        if opened:
            client("ROLLBACK")
        if outcome not in (COMMITTED, REJECTED_CREDIT):
            state.give_back(index)
        return outcome

    opened = False
    if args.txn:
        if client("BEGIN").startswith("ERR"):
            state.give_back(index)
            return FAILED
        opened = True

    rows = rows_timed(client, phases["cargo-lookup"],
                      f"SELECT org_id, cargo_type, cbm, declared_value "
                      f"FROM {tables['cargos']} WHERE id = {cargo_id}")
    if not rows:
        return finish(FAILED, opened)
    org_id = int(rows[0][0])

    rows = rows_timed(client, phases["credit-lookup"],
                      f"SELECT credit_limit, outstanding FROM "
                      f"{tables['organizations']} WHERE id = {org_id}")
    if not rows:
        return finish(FAILED, opened)
    credit_limit, outstanding = int(rows[0][0]), int(rows[0][1])

    if args.capacity_mode == "cached":
        rows = rows_timed(client, phases["capacity-read"],
                          f"SELECT booked_cbm FROM {tables['operations']} "
                          f"WHERE id = {op_id}")
        if rows is None:
            return finish(FAILED, opened)
        booked = int(rows[0][0]) if rows else 0
    else:
        rows = rows_timed(client, phases["capacity-read"],
                          f"SELECT SUM(cbm) FROM {tables['freights']} "
                          f"WHERE operation_id = {op_id}")
        if rows is None:
            return finish(FAILED, opened)
        booked = sum_value(rows)

    rows = rows_timed(client, phases["recipe-read"],
                      f"SELECT fee_id, route_code, priority, valid_from, valid_to "
                      f"FROM {tables['recipes']} WHERE cargo_type = {cargo_type}")
    if rows is None:
        return finish(FAILED, opened)
    recipe_rows = [(int(r[0]), int(r[1]), int(r[2]), int(r[3]), int(r[4]))
                   for r in rows]

    if booked + cbm > capacity:
        return finish(REJECTED_CAPACITY, opened)

    rate = BASE_RATE_PER_CBM + rng.randint(-RATE_JITTER, RATE_JITTER)
    freight_amount = (cbm // MILLI) * rate
    lines = charge_lines(recipe_rows, state.fee_book, route, day, cbm,
                         declared_value, args.max_fees)
    total = freight_amount + sum(charged for _fee, charged in lines)

    if outstanding + total > credit_limit:
        return finish(REJECTED_CREDIT, opened)

    freight_id = insert_returning(
        client, phases["freight-insert"],
        f"INSERT INTO {tables['freights']} (operation_id, ship_id, cargo_id, cbm, "
        f"price_per_cbm, booked_day, status) VALUES "
        f"({op_id}, {ship_id}, {cargo_id}, {cbm}, {rate}, {day}, 0)")
    if freight_id is None:
        return finish(FAILED, opened)

    for fee_id, charged in lines:
        reply = send(client, phases["charge-insert"],
                     f"INSERT INTO {tables['charges']} (freight_id, fee_id, amount, "
                     f"applied_day) VALUES ({freight_id}, {fee_id}, {charged}, {day})")
        if reply.startswith("ERR"):
            return finish(FAILED, opened)

    reply = send(client, phases["operation-update"],
                 f"UPDATE {tables['operations']} SET booked_cbm = {booked + cbm}, "
                 f"revenue = {state.revenue[op_id] + total} WHERE id = {op_id}")
    if reply.startswith("ERR"):
        return finish(FAILED, opened)

    reply = send(client, phases["org-update"],
                 f"UPDATE {tables['organizations']} SET "
                 f"outstanding = {outstanding + total} WHERE id = {org_id}")
    if reply.startswith("ERR"):
        return finish(FAILED, opened)

    if opened:
        reply = send(client, phases["commit"], "COMMIT")
        if reply.startswith("ERR"):
            client("ROLLBACK")
            state.give_back(index)
            return FAILED

    state.booked_cbm[op_id] = booked + cbm
    state.revenue[op_id] = state.revenue[op_id] + total
    state.outstanding[org_id] = outstanding + total
    state.freight_total[freight_id] = total
    state.freight_charges[freight_id] = len(lines)
    return COMMITTED


# ---- the reporter (docs/inflight/in-progress/scenario2-freight.md §6) -------------------------

def make_manifest(args, operations, orgs):
    """The reporter's state. The ckdbs driver runs its reporter in a second
    process on its own connection; PostgreSQL's twin is one process on one
    connection, so the same three reads interleave *between* bookings on the
    same clock interval instead. Both read a ledger growing under them; what
    differs — and what the comparison must label — is that here the reporter
    displaces bookings rather than contending with them."""
    return {
        "phases": {name: Phase(name) for name in MANIFEST_PHASES},
        "rng": random.Random(args.seed + 9000),
        "op_ids": [op for op, _s, _c, _o, _d in operations],
        "org_ids": [org for org, _limit in orgs],
        "passes": 0, "rows_read": 0,
    }


def manifest_pass(client, tables, manifest, args):
    """One reporter pass: the three §6 reads, verbatim from the ckdbs
    driver's manifest_process, over the same sample sizes."""
    rng, phases = manifest["rng"], manifest["phases"]
    for op_id in rng.sample(manifest["op_ids"],
                            min(args.manifest_voyages, len(manifest["op_ids"]))):
        rows = rows_timed(client, phases["manifest-scan"],
                          f"SELECT * FROM {tables['freights']} "
                          f"WHERE operation_id = {op_id}")
        manifest["rows_read"] += len(rows) if rows else 0
        rows_timed(client, phases["voyage-rollup"],
                   f"SELECT status, COUNT(*), SUM(cbm) FROM {tables['freights']} "
                   f"WHERE operation_id = {op_id} GROUP BY status")
    for org_id in rng.sample(manifest["org_ids"],
                             min(args.manifest_customers,
                                 len(manifest["org_ids"]))):
        rows_timed(client, phases["customer-statement"],
                   f"SELECT c.org_id, SUM(f.cbm) FROM {tables['freights']} AS f "
                   f"JOIN {tables['cargos']} AS c ON f.cargo_id = c.id "
                   f"WHERE c.org_id = {org_id} GROUP BY c.org_id")
    manifest["passes"] += 1


def run_bookings(client, tables, state, args, phases, rng, manifest=None):
    counts = {COMMITTED: 0, REJECTED_CAPACITY: 0, REJECTED_CREDIT: 0,
              CONFLICTED: 0, FAILED: 0}
    booking = phases["booking"]
    started = time.perf_counter()
    deadline = started + args.seconds
    last_pass = started
    exhausted = False
    while time.perf_counter() < deadline:
        if args.bookings and counts[COMMITTED] >= args.bookings:
            break
        if manifest is not None and \
                time.perf_counter() - last_pass >= args.manifest_interval:
            manifest_pass(client, tables, manifest, args)
            last_pass = time.perf_counter()
        elapsed = time.perf_counter() - started
        day = DAY0 + min(HORIZON_DAYS - 1,
                         int(elapsed / max(args.seconds, 1e-9) * HORIZON_DAYS))
        t0 = time.perf_counter()
        outcome = book_once(client, tables, state, args, phases, rng, day)
        if outcome is None:
            exhausted = True
            break
        counts[outcome] += 1
        booking.record(time.perf_counter() - t0, "" if outcome != FAILED else "ERR")
    return {"counts": counts, "retries": 0,
            "elapsed": time.perf_counter() - started,
            "cargo_pool_exhausted": exhausted,
            "cargo_pool_left": len(state.pool)}


def verify(client, tables, state, sample, rng):
    """§4's invariants, PostgreSQL side. Same four questions, same
    recomputation from returned rows."""
    checks = failures = 0
    first = None

    def fail(message):
        nonlocal failures, first
        failures += 1
        if first is None:
            first = message

    booked = [op for op, total in state.booked_cbm.items() if total > 0]
    for op_id in rng.sample(booked, min(sample, len(booked))):
        ledger_rows = client.rows(f"SELECT SUM(cbm) FROM {tables['freights']} "
                                  f"WHERE operation_id = {op_id}")
        stored = client.rows(f"SELECT booked_cbm FROM {tables['operations']} "
                             f"WHERE id = {op_id}")
        if ledger_rows is None or not stored:
            continue
        ledger, column = sum_value(ledger_rows), int(stored[0][0])
        checks += 1
        if ledger != column:
            fail(f"I1 operation {op_id}: booked_cbm={column}, ledger={ledger}")
        elif column != state.booked_cbm[op_id]:
            fail(f"I1 operation {op_id}: stored {column}, driver wrote "
                 f"{state.booked_cbm[op_id]}")
        _ship, capacity, _route = state.voyage[op_id]
        checks += 1
        if column > capacity:
            fail(f"I2 operation {op_id}: {column} over capacity {capacity}")

    owing = [org for org, total in state.outstanding.items() if total > 0]
    for org_id in rng.sample(owing, min(sample, len(owing))):
        rows = client.rows(
            f"SELECT f.id, f.cbm, f.price_per_cbm FROM {tables['freights']} AS f "
            f"JOIN {tables['cargos']} AS c ON f.cargo_id = c.id "
            f"WHERE c.org_id = {org_id}")
        stored = client.rows(f"SELECT outstanding FROM {tables['organizations']} "
                             f"WHERE id = {org_id}")
        if rows is None or not stored:
            continue
        recomputed = 0
        for freight in rows:
            freight_id, cbm, rate = int(freight[0]), int(freight[1]), int(freight[2])
            charged = client.rows(f"SELECT SUM(amount) FROM {tables['charges']} "
                                  f"WHERE freight_id = {freight_id}")
            recomputed += (cbm // MILLI) * rate + sum_value(charged)
        checks += 1
        if recomputed != int(stored[0][0]):
            fail(f"I3 organization {org_id}: outstanding={stored[0][0]}, "
                 f"recomputed={recomputed}")

    for freight_id in rng.sample(list(state.freight_charges),
                                 min(sample, len(state.freight_charges))):
        rows = client.rows(f"SELECT id FROM {tables['charges']} "
                           f"WHERE freight_id = {freight_id}")
        if rows is None:
            continue
        checks += 1
        if len(rows) != state.freight_charges[freight_id]:
            fail(f"I4 freight {freight_id}: {len(rows)} rows stored, "
                 f"{state.freight_charges[freight_id]} written")
    return checks, failures, first


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=15433,
                        help="the scratch cluster's port (default: 15433)")
    parser.add_argument("--user", default=None)
    parser.add_argument("--database", default="bench")
    parser.add_argument("--password", default=None)
    parser.add_argument("--suffix", default=None)
    parser.add_argument("--organizations", type=int, default=2000)
    parser.add_argument("--ships", type=int, default=200)
    parser.add_argument("--operations", type=int, default=2000)
    parser.add_argument("--cargos", type=int, default=200000)
    parser.add_argument("--capacity-headroom", type=float, default=1.0)
    parser.add_argument("--credit-headroom", type=float, default=1.0)
    parser.add_argument("--hot-routes", type=int, default=6)
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--bookings", type=int, default=0)
    parser.add_argument("--capacity-mode", choices=("cached", "scan"), default="cached")
    parser.add_argument("--manifest", dest="manifest", action="store_true",
                        default=True,
                        help="interleave the §6 reporter reads between bookings "
                             "every --manifest-interval seconds (default). The "
                             "ckdbs driver runs them in a second process; one "
                             "connection cannot, so here they displace bookings "
                             "instead of contending with them")
    parser.add_argument("--no-manifest", dest="manifest", action="store_false")
    parser.add_argument("--manifest-interval", type=float, default=1.0)
    parser.add_argument("--manifest-voyages", type=int, default=20)
    parser.add_argument("--manifest-customers", type=int, default=10)
    parser.add_argument("--txn", dest="txn", action="store_true", default=True)
    parser.add_argument("--no-txn", dest="txn", action="store_false")
    parser.add_argument("--max-fees", type=int, default=0)
    parser.add_argument("--max-retries", type=int, default=5)
    parser.add_argument("--verify", type=int, default=0)
    parser.add_argument("--synchronous-commit", default=None,
                        help="SET synchronous_commit (default: PostgreSQL's own, "
                             "which is `on` - both engines then fsync per commit)")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--json", metavar="PATH")
    # The shared print_bookings reports these; the twin has exactly one
    # booker with the whole space to itself, which is what a partitioned
    # single slice is.
    parser.set_defaults(bookers=1, contend=False)
    args = parser.parse_args()

    suffix = args.suffix or time.strftime("%H%M%S")
    rng = random.Random(args.seed)
    client = Client(args)
    tables = {base: f"{base}_{suffix}" for base in CREATE_ORDER}

    phases = []

    def phase(name, detail=""):
        p = Phase(name, detail)
        phases.append(p)
        return p

    ddl = phase("ddl", "8 relations + 2 indexes")
    created = create_tables(client, suffix, ddl)
    print(f"created {len(created)} relations with suffix _{suffix}")

    demand = demand_of(args)
    orgs = load_organizations(client, tables["organizations"], args.organizations,
                              demand, rng, phase("load-organizations"))
    ships = load_ships(client, tables["ships"], args.ships, demand, rng,
                       phase("load-ships"))
    operations = load_operations(client, tables["operations"], args.operations,
                                 ships, rng, phase("load-operations"))
    fees = load_fees(client, tables["fees"], phase("load-fees"))
    hot_routes = [route_code(rng.randrange(PORTS), rng.randrange(PORTS))
                  for _ in range(args.hot_routes)]
    recipes = load_recipes(client, tables["recipes"], fees, hot_routes, rng,
                           phase("load-recipes"))
    cargos = load_cargos(client, tables["cargos"], args.cargos, orgs, rng,
                         phase("load-cargos"))

    booking_phases = {name: phase(name) for name in (
        "booking", "cargo-lookup", "credit-lookup", "capacity-read", "recipe-read",
        "freight-insert", "charge-insert", "operation-update", "org-update",
        "commit")}
    booking_phases["booking"].detail = (
        f"{'BEGIN/COMMIT' if args.txn else 'autocommit'}, "
        f"capacity={args.capacity_mode}")
    state = BookingState(operations, orgs, cargos, fees_by_id(fees), rng)
    manifest = make_manifest(args, operations, orgs) if args.manifest else None
    result = run_bookings(client, tables, state, args, booking_phases, rng,
                          manifest)
    if manifest is not None:
        for name in MANIFEST_PHASES:
            p = manifest["phases"][name]
            p.elapsed = result["elapsed"]
            phases.append(p)
        # The tuple shape print_bookings expects, shared with the ckdbs
        # driver's manifest_process result.
        result["manifest"] = {
            "passes": manifest["passes"], "rows_read": manifest["rows_read"],
            "phases": {name: (p.latencies, p.errors, p.first_error)
                       for name, p in manifest["phases"].items()},
        }
    if args.verify:
        result["verify"] = verify(client, tables, state, args.verify, rng)

    loaded = {"organizations": len(orgs), "ships": len(ships),
              "operations": len(operations), "fees": len(fees),
              "recipes": len(recipes), "cargos": len(cargos),
              "freights": result["counts"][COMMITTED],
              "charges": sum(state.freight_charges.values())}
    meta = {
        "engine": "postgresql", "scenario": "freight",
        "columns": sum(len(SCHEMA[b].split(",")) for b in CREATE_ORDER),
        "rows": sum(loaded.values()),
        "host": args.host, "port": args.port,
        "table": f"scenario2_{suffix}", "loaded": loaded,
        "txn": args.txn, "capacity_mode": args.capacity_mode, "seed": args.seed,
        # The workload identity, so compare_scenario2.py can refuse to diff
        # two runs that did different work. One key per flag that changes
        # what is loaded or what a booking does.
        "organizations": args.organizations, "ships": args.ships,
        "operations": args.operations, "cargos": args.cargos,
        "capacity_headroom": args.capacity_headroom,
        "credit_headroom": args.credit_headroom,
        "hot_routes": args.hot_routes, "max_fees": args.max_fees,
        "bookings": args.bookings, "bookers": 1,
        "manifest": args.manifest,
        "synchronous_commit": args.synchronous_commit or "on (default)",
        "outcomes": result["counts"], "retries": 0,
        "tps": (result["counts"][COMMITTED] / result["elapsed"]
                if result["elapsed"] > 0 else 0.0),
    }
    if "verify" in result:
        checks, failures, first = result["verify"]
        meta["verify"] = {"checks": checks, "failures": failures, "first": first}
    if result.get("manifest"):
        meta["manifest_passes"] = result["manifest"]["passes"]
        meta["manifest_rows_read"] = result["manifest"]["rows_read"]
    report(phases, meta, footer=(
        "PostgreSQL twin of tools/scenario2_freight.py: same booking, same",
        "work target, same seed. freights and charges carry a pk index they",
        "do not want and the two non-pk reads carry one they do.",
    ))
    print_bookings(result, args)
    if args.json:
        write_json(args.json, meta, phases)
    if client.errors:
        print(f"\n{client.errors} statement(s) failed; first: {client.first_error}",
              file=sys.stderr)
    client.close()
    return 1 if client.errors else 0


if __name__ == "__main__":
    sys.exit(main())
