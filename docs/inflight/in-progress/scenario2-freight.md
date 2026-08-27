# KDS Scenario 2 — Freight & Cargo (Workload Specification)

**Status:** Plan confirmed 2026-08-06 (S2-1–S2-11, §0). **`S2-01` and `S2-02`
are built** — the eight relations, the loaders, `--schema-only` /
`--load-only`, the read probe that settled §6, the booking transaction in
both capacity modes, the three-way outcome accounting, and `--verify`. One
booker cannot conflict, so the conflict counters are built and unexercised
until `S2-03`, and no number here is a benchmark: `S2-06` owns the
measurements.
Deliverables: `tools/scenario2_freight.py`, `tools/pg_scenario2_freight.py`,
`tools/compare_scenario2.py`, `bench/results-scenario2-freight.md`. Markers:
`[CONFIRMED]`, `[PROPOSED]`, `[OPEN]`. Consistent with `docs/spec/txn.md`,
`docs/spec/foreign-keys.md`, `docs/spec/cabin.md`, `docs/spec/aggregate.md`,
`docs/spec/parser-v2.md`.

Sibling workloads, and what each already owns:

| Tool | Half of a financial system it measures |
|---|---|
| `tools/scenario0_stockmarket.py` | a **write** workload — trades and balance updates under a concurrent reporter, in TPS |
| `tools/scenario1_backtest.py` | a **read** workload — 30 years of bars through join chains, in QPS |
| **this one** | a **contended write** workload — bookings that can be *refused*, in TPS, with the refusals counted |

---

## 0. Decision Record `[CONFIRMED 2026-08-06]`

| # | Decision | Choice |
|---|---|---|
| S2-1 | The measured unit | **One freight booking** — a cargo placed on a voyage: two pk lookups, two filtered scans, two ledger inserts, two btree updates, under one `BEGIN`/`COMMIT` (§3) |
| S2-2 | Transactions | **Explicit `BEGIN`/`COMMIT` on by default.** This is the first scenario where autocommit is the *comparison* (`--no-txn`) rather than the default |
| S2-3 | "Recipe" | A **pricing rule set**: which additional fee applies to which `(cargo_type, route_code)` in which date window. `fees` is the rate card, `recipes` the rules over it, `charges` the ledger of what was applied |
| S2-4 | Scale and baseline | **~100k freights** by default, with a PostgreSQL twin, as both existing scenarios have |
| S2-5 | The customer | **`organizations`** — the party that places the shipping order. **A freight row *is* the order line**; there is no order-header relation |
| S2-6 | Failure taxonomy | Three outcomes, counted separately: **committed**, **rejected** (over capacity or over credit — the business said no), **conflicted** (`TXN_CONFLICT`, retried). A TPS number that folds the second into the third, or either into "errors", is a wrong number |
| S2-7 | One derived column, deliberately | `operations.booked_cbm` is the running total of its freights' CBM. scenario0 refuses derived columns; here it *is* the subject — the value two statements must agree on and a torn transaction corrupts. `freights` carries **no `org_id`**: the customer axis goes through the join, so `--verify` recomputes rather than re-reads |
| S2-8 | Encodings | Money `int64` **minor units**, volume `int32` **milli-m³**, dates `int32` **epoch days**. Forced: KDS refuses `float`/`decimal` at `CREATE TABLE` (fixed-length rule) and has no date type |
| S2-9 | Off by default | `--fk`, `--cabin`, `--isolation repeatable-read`, `--verify`. Each is a measurement of its own cost against a baseline run, which is what "off by default" is for |
| S2-10 | Contention | Bookers **share voyages and customers by default** (`--contend`), and cargos are split in every mode. **Settled at `S2-03` and no longer open**: with contention off the workload cannot exhibit the lost update that §4's invariants exist to catch, so the default that hides the defect cannot be the default |
| S2-11 | Schema construction is its own run | `--schema-only` and `--load-only` end the run before any measurement, so a data file can be prepared once and driven many times (§7.1) |

---

## 1. What this measures that the other two do not

Three things, none of which scenario0 or scenario1 can show.

**A transaction that is allowed to say no.** Every scenario0 trade succeeds —
the driver generates ids it created and balances it opened, so a rejection
means a bug. A booking here is checked against two live limits, and a
rejection is a *correct* outcome that costs a rollback. The ratio of
committed to rejected to conflicted is the workload's shape, and TPS alone
does not carry it.

**Write conflicts on two axes.** scenario0's traders partition accounts, so
`first-updater-wins` never fires and the retry path is never measured. Here
two bookers collide on `operations` when they load the same voyage and on
`organizations` when they carry the same customer's cargo — different
partitions of the same run. The cost of `ERR TXN_CONFLICT retryable=1` plus
a full re-drive of the transaction is a number this project does not have.

**A consistency invariant that a torn transaction actually breaks.**
`operations.booked_cbm` must equal `SUM(freights.cbm)` for that operation,
and `organizations.outstanding` must equal the recomputed charge total. Run
with `--no-txn` and the invariant is violable under concurrency; run with
`--txn` and it is not. §4's checker is what turns that from a claim into a
verdict.

---

## 2. Schema — eight relations `[CONFIRMED]`

Column 0 of every relation is the Keystone primary key: system-generated,
never supplied on INSERT (invariant 11), declared in `CREATE TABLE` and
omitted from every `INSERT`.

```
organizations  BTREE  id int64, org_code varchar, name varchar, country int32,
                      org_type int32, credit_limit int64, outstanding int64,
                      tier int32, contact varchar, registered_day int32, status int32

ships          BTREE  id int64, imo varchar, name varchar, ship_type int32,
                      capacity_cbm int32, dwt int64, built_year int32, flag varchar,
                      owner_id int64, home_port int32, status int32

operations     BTREE  id int64, ship_id int64, origin int32, destination int32,
                      depart_day int32, arrive_day int32, status int32,
                      booked_cbm int32, revenue int64

cargos         BTREE  id int64, org_id int64, cargo_type int32, weight_kg int64,
                      cbm int32, hazmat int32, declared_value int64, origin int32,
                      destination int32, ready_day int32

fees           BTREE  id int64, fee_name varchar, fee_code int32, basis int32,
                      amount int64, valid_from int32, valid_to int32

recipes        BTREE  id int64, cargo_type int32, route_code int32, fee_id int64,
                      priority int32, valid_from int32, valid_to int32

freights       HEAP   id int64, operation_id int64, ship_id int64, cargo_id int64,
                      cbm int32, price_per_cbm int64, booked_day int32, status int32

charges        HEAP   id int64, freight_id int64, fee_id int64, amount int64,
                      applied_day int32
```

68 columns per run against a ~7,800-column instance ceiling (catalog
relations chain — `docs/rules/keystoneid-k0-findings.md`), so ~110 runs per data
file. Nothing reclaims a catalog row: there is no `DROP TABLE`.

**Why each clustering.** BTREE wherever the transaction probes by pk or a
foreign key needs a parent to descend into — a heap parent is refused at
declaration (`docs/spec/foreign-keys.md` F1), so `--fk` requires it. HEAP for
the two append-only ledgers, which are written at the chain tail and never
probed by pk.

**Creation order** is load-bearing under `--fk` and cosmetic without it: a
parent must exist before a child references it, and there is no `ALTER TABLE`
to add a constraint later.

```
organizations → ships → operations → cargos → fees → recipes → freights → charges
```

**The foreign keys** (`--fk`): `cargos.org_id REFERENCES organizations`,
`operations.ship_id REFERENCES ships`, `freights.cargo_id REFERENCES cargos`.
All three fire on the *forward* check only — nothing in this workload deletes
a parent, which is the honest shape of an insert-dominated OLTP run.

**The Cabin** (`--cabin`): `recipes.cargo_type`. Declared as a column policy
(`cargo_type int32 CABIN`) rather than by `CREATE CABIN`, for the reason
scenario0 states: a declared Cabin observes on first selection, an
engine-created one waits for the second. It is the best-shaped Cabin
candidate in the repo — a small hot value set, probed by a non-pk equality,
once per booking, on a relation nothing writes after load. `S2-02` confirms
the shape rather than its value: a short run reports **902 hits against 8
misses** over 8 observed values, one miss per cargo type and every
subsequent probe served. What that is worth in TPS is `S2-06`'s to measure,
and the write hook — the half that costs — never fires here at all.

---

## 3. The measured transaction `[CONFIRMED]`

```
BEGIN
1  SELECT org_id, cargo_type, cbm, declared_value FROM cargos WHERE id = <cargo>
                                                        Lookup    (trail-replayable)
2  SELECT credit_limit, outstanding FROM organizations WHERE id = <org>
                                                        Lookup    (trail-replayable)
3  capacity read, per --capacity-mode:
     cached: SELECT booked_cbm FROM operations WHERE id = <op>          Lookup
     scan:   SELECT SUM(cbm) FROM freights WHERE operation_id = <op>    FilterScan + fold
4  SELECT fee_id, priority, basis, amount FROM recipes WHERE cargo_type = <t>
                                                        FilterScan  (Cabin candidate)
5  INSERT INTO freights ...
6  INSERT INTO charges ...                              × matched recipes (1–3)
7  UPDATE operations     SET booked_cbm = <lit>, revenue = <lit>
8  UPDATE organizations  SET outstanding = <lit>
COMMIT
```

A booking counts toward TPS only if every statement replied without `ERR`
**and** `COMMIT` succeeded.

**The two checks happen client-side, between statements 4 and 5**, because
the engine has no arithmetic in a select list. Over capacity or over credit →
`ROLLBACK`, counted as a *rejection*, not a failure. `TXN_CONFLICT` on
statement 7 or 8 → `ROLLBACK`, counted as a conflict, and the whole
transaction is re-driven from statement 1 (its inputs are stale by
definition).

**`--capacity-mode {cached,scan}` is the load-bearing knob.** `cached` is one
pk lookup that trusts the derived column; `scan` re-derives the truth every
booking through an aggregate over a FilterScan. The two must produce
identical outcomes, and the gap between their TPS is what the derived column
is worth.

### 3.1 Both limits are sized to the run's demand `[CONFIRMED, built]`

A ship of a fixed 20,000–250,000 CBM is unreachable at 400 cargos and
trivially full at 2,000,000, so a constant makes the capacity axis a property
of the *flags* rather than of the workload — and two of S2-6's three outcomes
would be unobservable. `S2-02` therefore derives both limits from expected
demand: a ship's capacity is `(cargos × mean CBM) / voyages` times a spread,
a customer's credit is expected spend per customer times a spread, and
`--capacity-headroom` / `--credit-headroom` scale each (1.0 = sized to
exactly this run's demand). At the bottom of each spread the limit binds; at
the top it never does.

Two floors sit under that, and they are the same rule twice: **no voyage may
be too small for the largest cargo, and no customer's credit smaller than the
most expensive booking that can be priced.** Either one creates a row every
counterparty refuses forever — drawn, rejected, returned, drawn again — which
is a rejection rate with nothing behind it. A limit must bind by
*accumulation*, never on the first attempt.

One asymmetry follows from the same reasoning, and it is not a tuning choice.
**A capacity rejection returns its cargo to the pool; a credit rejection
retires it.** `outstanding` only ever grows — nothing in this workload pays an
invoice — so a cargo its customer could not afford now can never be afforded
later. Returning it means drawing it forever: the first build of `S2-02` did,
and spent 96% of its attempts re-rejecting the same cargo. A full voyage is a
different matter, because the next draw picks a different voyage.

---

## 4. The invariants `[CONFIRMED]`

`--verify N` samples `N` operations and `N` organizations after the run and
checks four things. It is **off by default** (`--verify 0`) — it is a
measurement of correctness, not of speed, and it re-reads a large part of the
relation.

| # | Invariant | How it is checked |
|---|---|---|
| I1 | `operations.booked_cbm == SUM(freights.cbm)` for that operation | `SELECT SUM(cbm) FROM freights WHERE operation_id = <n>` against the stored column |
| I2 | No operation exceeds its ship's `capacity_cbm` | join `operations`→`ships`, compare |
| I3 | `organizations.outstanding` equals the recomputed charge total | `FROM freights JOIN cargos ON freights.cargo_id = cargos.id WHERE cargos.org_id = <n>`, charges summed client-side |
| I4 | Every freight's `charges` rows equal the recipe set recomputed for its cargo type and day | client-side replay of the rule set |

I1 and I3 are what `--no-txn` is expected to **break** under concurrency and
`--txn` is expected to hold. That contrast is the point of the flag, and it
belongs in the results file as a table, not as a sentence.

**`S2-02` cannot show that contrast and does not claim to.** One booker in
one process has no concurrency, so `--no-txn` passes all four invariants
exactly as `--txn` does — correctly, since a lost update needs a second
writer. What `S2-02` establishes is that the checker is sound on a run known
to be consistent; `S2-03` is what makes it capable of failing.

---

## 5. Contention `[CONFIRMED at S2-03]`

`--bookers N` client processes, each driving the §3 transaction in a loop.

- `--contend` (default `[OPEN]`, see below): bookers draw voyages and cargos
  from the whole space, so two may load the same voyage or carry the same
  customer.
- `--no-contend`: the space is partitioned by booker index — no conflict is
  possible, and the number is directly comparable to scenario0's.

Counters, per axis, because a single "conflicts" number cannot be acted on:
conflicts on `operations`, conflicts on `organizations`, retries per commit
(mean and max), and retry time as a fraction of the run.

**Cargos are partitioned in both modes.** A cargo ships once; two bookers
holding the same cargo would book it onto two voyages, which is a driver that
lost track of its pool rather than contention. What `--contend` shares is the
two rows a booking *updates*, because those are the axes.

**Settled: `--contend` defaults on**, and the reason is stronger than the
"more honest number" this section originally gave. With it off, four bookers
produce zero conflicts and pass every invariant at every scale measured. With
it on, at READ COMMITTED, the run **loses updates** — `booked_cbm` drifts
below `SUM(freights.cbm)` and `outstanding` below its recomputed total,
because two bookers read the same value and both commit. The engine's
first-updater-wins rejects only a writer whose target is held by a
*concurrent uncommitted* transaction; two read-modify-write transactions that
merely overlap in time, and commit in sequence, are not that case.

`--isolation repeatable-read` converts those silent losses into visible
retryable conflicts and the invariants hold. Measured cost: −1.7% at four
bookers. See `bench/results-scenario2-freight.md`.

The consequence for this scenario is that the mode which cannot fail cannot
be the default. The consequence for the engine is an open question about what
a read-modify-write is supposed to look like here, recorded in §10.

---

## 6. The reporter

One separate process (`--manifest`, on by default), scenario0's
`profit_process` shape — an analytic reader contending with the writes on one
thread's dispatcher:

```
SELECT * FROM freights WHERE operation_id = <n>                       manifest
SELECT status, COUNT(*), SUM(cbm) FROM freights WHERE operation_id = <n>
    GROUP BY status                                                   voyage rollup
SELECT c.org_id, SUM(f.cbm) FROM freights f JOIN cargos c
    ON f.cargo_id = c.id GROUP BY c.org_id                            customer statement
```

The third had to be confirmed before the rest was written, because **nothing
else in this repo aggregates over a joined chain**. `docs/spec/aggregate.md`
AG1 puts the fold over the statement's `RowSink` and leaves the compiled
chain byte-identical, so a group key resolving to a *second* step's column
should work — and `S2-01`'s probe measured that it does, on real rows and not
only at compile:

```
SELECT c.org_id, SUM(f.cbm) FROM freights AS f
    JOIN cargos AS c ON f.cargo_id = c.id
    WHERE c.org_id = 29 GROUP BY c.org_id      ->  29  30000   (three freights)
```

The per-organization filtered-aggregate fallback is therefore not needed.
All six reads §3 and §6 depend on are accepted by the engine as of
2026-08-06.

---

## 7. Flags

| Flag | Default | What |
|---|---|---|
| `--host`, `--port`, `--timeout` | as scenario0 | connection |
| `--suffix` | timestamp | relation-name suffix, so runs share a data file |
| **`--schema-only`** | off | **create the eight relations and exit.** No load, no workload, no measurement |
| **`--load-only`** | off | create and load the reference data, then exit |
| `--organizations`, `--ships`, `--operations`, `--cargos` | 2000 / 200 / 2000 / 200000 | load sizes |
| `--capacity-headroom`, `--credit-headroom` | 1.0 | scale each limit against expected demand (§3.1) |
| `--bookers` | 4 | client processes (`S2-03`) |
| `--seconds` | 60 | run length |
| `--bookings` | 0 | stop after N commits; `--seconds` then a ceiling |
| `--capacity-mode` | `cached` | `cached` \| `scan` (§3) |
| `--max-retries` | 5 | attempts after a `TXN_CONFLICT` |
| `--max-fees` | 0 | cap fees per booking by priority; 0 is uncapped (§10) |
| `--txn` / `--no-txn` | **on** | explicit `BEGIN`/`COMMIT` (S2-2) |
| `--contend` / `--no-contend` | `[OPEN]` | §5 |
| `--manifest` / `--no-manifest` | on | the reporter process |
| `--fk` | off | declare the three foreign keys |
| `--cabin` | off | declare the Cabin on `recipes.cargo_type` |
| `--isolation` | server default | `read-committed` \| `repeatable-read` |
| `--verify` | 0 (off) | sample size for §4 |
| `--seed`, `--json`, `--echo`, `--sync`, `--server-log` | as scenario0 | |

### 7.1 Why schema construction is its own run (S2-11)

Three reasons, all of them things the other two scenarios do awkwardly.

1. **DDL is not transactional and is unlogged.** A `CREATE TABLE` inside a
   failed run leaves the relation behind, and there is no `DROP TABLE` to
   undo it. Separating construction from measurement makes "which relations
   exist" a decision rather than a side effect.
2. **A prepared data file is reusable.** `--schema-only` once, then many
   measured runs against the same `--suffix`, is how a sweep over
   `--capacity-mode` or `--bookers` should be driven — the load is the
   expensive part and it does not change between them.
3. **It is the smallest thing that can fail.** Every schema-shaped refusal
   this engine has — a heap FK parent, a `float` column, an unrecognized
   `CABIN` policy, an exhausted catalog range — surfaces in a run that takes
   a second, with an error naming the flag that caused it, instead of eight
   relations into a load.

---

## 8. Engine constraints this workload is written around

Each is a real limit as of 2026-08-06, not a preference:

- **No arithmetic in a select list.** New `booked_cbm`, `revenue` and
  `outstanding` values are computed by the driver and sent as literals. Every
  total in §4 is recomputed client-side.
- **No `ORDER BY`, no `HAVING`, no `AVG`.** Recipe `priority` is ordered
  client-side; there is no server-side top-N anywhere.
- **`SUM` over `uint64` is `Unsupported`** — all money and volume columns are
  signed.
- **`float`/`decimal` are refused at `CREATE TABLE`** — S2-8's encodings are
  forced, not chosen.
- **A heap FK parent is refused** — which is why `cargos`, `ships` and
  `organizations` are BTREE.
- **Single core.** The cross-core pipeline is not built; a chain spanning
  cores is refused. Run `cores = 1`.
- **Nothing purges undo, and there is no recovery.** A long run grows the
  data file monotonically, and durability holds only as far as `SYNC` or a
  clean shutdown. A `--no-txn` run is not a crash-safety comparison, only a
  concurrency one.
- **DDL is not transactional** — `CREATE TABLE` inside a transaction is not
  rolled back.

---

## 9. Workplan

| Task | Delivers | Done when |
|---|---|---|
| `S2-01` **done** | schema, loaders, `--schema-only` / `--load-only`, the §6 join-aggregate probe | eight relations create on a fresh file; `--schema-only` exits without loading; the probe reports all six reads accepted. `--verify` moves to `S2-02`, which is where there is state worth verifying |
| `S2-02` **done** | the §3 transaction, one booker, both `--capacity-mode` values, §3.1's demand sizing, `--verify` | a single run commits, rejects for capacity **and** rejects for credit, reports the three outcomes separately, and passes all four invariants in every flag combination |
| `S2-03` **done** | `--bookers`, `--contend`, the retry loop, per-axis conflict counters | conflicts are observed and retried rather than counted as errors, split by the row they hit; §5's `[OPEN]` default is settled with both numbers measured; **and the invariant checker fails a run for the first time** |
| `S2-04` **done** | the `--manifest` reporter process | reporter latency reported beside TPS, as scenario0 does. The three reads are the three shapes the engine treats differently: a FilterScan over a growing ledger, the same walk with a fold, and the join-aggregate `S2-01` proved |
| `S2-05` **done** | `pg_scenario2_freight.py`, `compare_scenario2.py` | same schema, same transaction, same phase names; the two tools' JSON is diffable. The twin's reporter is the same three statements *interleaved between bookings* — one connection cannot contend with itself — and the compare tool labels that placement difference rather than hiding it. Identity is checked before any table: same seed, sizes, headrooms, modes, booker count, and the loaded reference counts, which the shared generator makes exactly reproducible |
| `S2-06` | `bench/results-scenario2-freight.md` | `--txn` vs `--no-txn` invariant table, capacity-mode gap **at two ledger sizes**, conflict cost, `--fk` and `--cabin` deltas, PostgreSQL side by side |

`bench/results-scenario2-freight.md` exists already, written from `S2-02`'s
six-configuration matrix. It prices the transaction, not the workload, and
`S2-06` supersedes it once there is contention to measure. Three results from
it change what later tasks should expect:

- **A booking's commit is half its cost** (1,811 µs of 3,635 µs), so
  `--no-txn` is 3.5× *slower*, not faster — explicit transactions are the
  fast path as well as the correct one.
- **The noise floor is ±2%** on this driver, established by an accidental
  control: `--isolation repeatable-read`, which cannot change a
  single-connection run's behaviour, measured +2.1%. Any `S2-03`+ claim
  smaller than that needs more than one run.
- **Waystone state was 71% of the data file** in the default configuration
  (562 trail pages against 230 data pages), and switching `--capacity-mode`
  from `cached` to `scan` cut it to 204 — because the derived column makes
  the capacity check lookup-class and therefore recorded. A schema decision
  moved trail volume 2.75×.

---

## 10. Open items — do not assume

- ~~**`--contend` default**~~ — **settled at `S2-03`: on.** With it off, four
  bookers produce zero conflicts and zero invariant failures at every scale
  measured; with it on, they produce lost updates (§5). A default that cannot
  fail is not a baseline, it is a blindfold.
- **Whether `--isolation repeatable-read` should become this scenario's
  default, or whether the engine should offer something narrower.** `S2-03`
  shows READ COMMITTED silently loses updates on this workload and REPEATABLE
  READ does not, for ~1.7%. But RR is a heavier promise than the workload
  needs: what a booking actually wants is an atomic read-modify-write of one
  column, which this engine has no way to express — there is no
  `SET c = c + n` (the grammar takes a literal) and no `SELECT ... FOR
  UPDATE`. Naming that gap is `S2-03`'s output; choosing between "make RR the
  default", "add an atomic increment" and "leave it to the client" is a
  design decision for `docs/spec/txn.md`, not for a benchmark driver.
- ~~**Recipe match cap**~~ — **settled at `S2-02`: uncapped.** The generated
  rule set bounds itself at 8 matches per booking (four route-agnostic rules,
  at most three per cargo type, at most one route-specific), so a cap adds no
  guarantee the rule set does not already give. `--max-fees N` exists to
  *lower* the fan-out for a variance experiment and defaults to 0. A cap was
  never needed to make the workload work — only to make it narrower.
- ~~**Whether `GROUP BY` resolves a key on a joined chain**~~ — **answered
  2026-08-06 by `S2-01`'s probe: it does**, with correct values over real
  rows (§6). It was a capability question about the engine, not a choice.
  `docs/spec/aggregate.md` documents no join case either way; this is the
  first workload to exercise one.
- **Whether the credit check needs its own status code.** Today an over-credit
  booking is a driver-side rollback, indistinguishable at the wire from a
  voluntary one. Making it an engine-side constraint would need `CHECK`, which
  does not exist and is not proposed here.
