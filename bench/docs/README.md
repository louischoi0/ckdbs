# The benchmark drivers — what each one measures, and how to run it

Every measurement in `bench/` comes from a driver in `tools/`. This directory
documents those drivers; the files in `bench/` above it document *results*. A
results file states findings and links here — it does not re-explain how to
run a tool.

The rules those results files follow are in `.claude/agents/ck-tester.md`, the
agent that owns this directory. Two of them decide whether a run is worth
recording at all, so they are repeated here:

- **Release build.** `CMakeLists.txt` defaults `CMAKE_BUILD_TYPE` to
  **Debug** — roughly 14× slower on a scan, with assertions live. Measure
  with `build-release/kds_server`, and rebuild it before measuring, because a
  stale binary silently measures an older engine than the one at `HEAD`.
- **A block device, never tmpfs.** `/tmp` on this machine is tmpfs. A data
  file there makes fsync free, which turns every write number into fiction
  and inflates every read-side structure. Put data files under `$HOME`.

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release -j
./build-release/kds_server ~/bench.db --port 15432          # the engine
./tools/pg_setup.sh init                                    # the baseline, port 15433
```

---

## The four scenarios

Each scenario is a whole workload rather than a statement mix, and each has a
PostgreSQL twin beside it that drives the same work through `pg_wire.py`. The
twins import their schema and their business logic from the ckdbs driver, so
the two cannot drift into measuring different questions.

### `scenario0_stockmarket.py` — a write workload, in TPS

A brokerage book: five relations, and one measured transaction of four
statements (two `trades` inserts, two `accounts` updates) driven by
`--traders` processes while a separate reporter process runs a periodic
non-pk scan against them. The contention is the point — an analytic walk
against a point-lookup write workload on a server that dispatches every
client on one thread.

```bash
./tools/scenario0_stockmarket.py --seconds 60 --users 10000 --traders 4 --txn
./tools/pg_scenario0_stockmarket.py --port 15433 --database bench --seconds 60
```

Key flags: `--traders N`, `--seconds`, `--txn` / `--no-txn`,
`--cabin` (a Cabin on `accounts.user_id`), `--fk`
(`trades.account_id REFERENCES accounts`), `--profit` / `--no-profit` and
`--profit-interval` (the reporter), `--verify N`, `--suffix`, `--json`.

### `scenario1_backtest.py` — a read workload, in QPS

Daily bars across seven relations, walked forward by eight strategies. Produces
a QPS matrix over every read shape the workload issues, priced cold, warm, with
a Cabin and after dropping it, plus sweeps against transaction batch size and
connection count. Results at `bench/results-scenario1-vs-pg.md`.

**The row count is `--years × 252 × --symbols`** (252 trading days a year), and
it sets the length of the load as well as the size of every relation the read
shapes walk: `daily_stats` gets one feature row per bar, and `sessions` gets
`--years × 252` rows. That arithmetic is the knob to reach for when sweeping
size — e.g. `--symbols 1` with `--years 1 / 4 / 40` gives 252 / 1,008 / 10,080
bars while holding every result set the same size.

```bash
# one size, both engines, sequentially — never at the same time
./tools/scenario1_backtest.py --port 15432 --symbols 1 --years 40 \
    --seed 1 --verify --json ck.json
./tools/pg_scenario1_backtest.py --port 15433 --database bench \
    --symbols 1 --years 40 --seed 1 --verify --json pg.json
./tools/compare_scenario1.py ck.json pg.json      # side by side
```

**Pass the same `--years`, `--symbols`, `--rebalance`, `--top-k` and `--seed` to
both sides.** `compare_scenario1.py` refuses two files whose parameters or model
P&L disagree, which is what stops a comparison of two different workloads.

| flag | default | what it does |
|---|---|---|
| `--years`, `--symbols` | 30 / 8 | the size ladder: bars = `years × 252 × symbols` |
| `--exchanges`, `--start-year` | 2 / 1995 | the lookup relations |
| `--rebalance`, `--top-k` | 21 / 3 | sessions between rebalances (each is one 3-relation join + 8 result inserts), and positions per model |
| `--bars-clustered` | `btree` | storage for `daily_bars`. `heap` is what a missing pk index costs: the join's `Probe` has nothing to descend and walks the chain |
| `--batch` / `--no-load-txn` | 200 / — | rows per `BEGIN`/`COMMIT` during the load; 0 is one durability point per row |
| `--ops` | 200 | operations per `read-*` phase; the whole-relation ones run a twentieth of it |
| `--replay`, `--compare-rounds` | 1 / 4 | extra passes of the cross-section join and of the per-model read. **Both matter under `--cabin`**: a value is observed on its first read and can only be served on the second |
| `--sweep` / `--no-sweep` | **on** | the QPS matrix — 7 shapes × {cold, warm, cabin, dropped}. **Mutually exclusive with `--cabin`**: the sweep creates and drops its own Cabins |
| `--qps-ops`, `--warm-keys` | 100 / 8 | statements per matrix cell, and distinct arguments cycled in the warm/cabin/dropped cells. A shape with fewer distinct arguments than `--qps-ops` reports a shorter `cold` run rather than repeating one |
| `--aggregates` / `--no-aggregates` | **on** | the `agg-*` phases. Off is needed against a server predating `GROUP BY` |
| `--write-sweep`, `--write-batches`, `--write-ops` | on / `1,10,100,1000` / 2000 | INSERT rows·s⁻¹ against transaction batch size, on a relation of the sweep's own |
| `--connections`, `--conn-ops` | `1,2,4,8` / 200 | aggregate QPS of the join by connection count; empty `--connections` skips it |
| `--cabin` | off | declare Cabins on `daily_stats.session_no` and `model_results.model_id` up front (needs `--no-sweep`) |
| `--fk` | off | declare the four foreign keys. Requires `--bars-clustered btree` |
| `--analyze` | off | print each read shape's step chain and examined-row count — how a `Probe` that became a chain scan shows up |
| `--verify` / `--no-verify` | **on** | every model's P&L, read back through the comparison join, against the driver's running total |
| `--suffix` | `<epoch>_<rand>` | relation-name suffix. There is no `DROP TABLE`, and a run spends 49 columns, so prefer a scratch data file per run over sharing one |
| `--seed`, `--json`, `--echo`, `--sync`, `--timeout`, `--show-models`, `--server-log` | | as the other scenarios; `--server-log` needs the server at `--log-level debug` and adds per-statement server-side microseconds |

The twin takes the same flags except `--cabin`, `--fk`, `--bars-clustered`,
`--analyze`, `--echo`, `--sync` and `--server-log`, which have no PostgreSQL
meaning; it adds `--user`, `--database`, `--explain` and `--keep`, and its
sweep's third column is a btree `index` where ckdbs's is a `cabin`.

### `scenario2_freight.py` — a contended write workload, in TPS

A freight and cargo book: eight relations, and one measured transaction of
eight statements that can be **refused** two ways (over a voyage's capacity,
over a customer's credit) and can **conflict** on either of the two rows it
updates. Documented in full at `docs/scenario2-freight.md`; results at
`bench/results-scenario2-freight.md`.

```bash
# prepare a data file once, then drive it many times
./tools/scenario2_freight.py --schema-only --suffix run1
./tools/scenario2_freight.py --suffix run1 --bookings 1500 --verify 25

# a full measured run, and its PostgreSQL twin
./tools/scenario2_freight.py --organizations 200 --ships 40 --operations 400 \
    --cargos 5000 --bookings 1500 --seed 1 --verify 25 --json ck.json
./tools/pg_scenario2_freight.py --port 15433 --database bench \
    --organizations 200 --ships 40 --operations 400 \
    --cargos 5000 --bookings 1500 --seed 1 --verify 25 --json pg.json
```

| flag | default | what it does |
|---|---|---|
| `--schema-only` | off | create the eight relations and exit — no load, no measurement |
| `--load-only` | off | create and load the reference data, then exit |
| `--suffix` | timestamp | relation-name suffix, so runs share a data file |
| `--organizations`, `--ships`, `--operations`, `--cargos` | 2000 / 200 / 2000 / 200000 | load sizes |
| `--capacity-headroom`, `--credit-headroom` | 1.0 | scale each limit against the run's expected demand; 1.0 means the smaller ships and customers start refusing |
| `--bookings N` | 0 | stop after N commits (`--seconds` becomes a ceiling). **Use this, not `--seconds`, when comparing configurations** — equal work, not equal time |
| `--seconds` | 60 | run length |
| `--capacity-mode` | `cached` | `cached` reads `operations.booked_cbm`; `scan` re-derives it with `SUM` over the ledger |
| `--txn` / `--no-txn` | **on** | one `BEGIN`/`COMMIT` per booking, or eight autocommitted statements |
| `--max-retries` | 5 | attempts after an `ERR TXN_CONFLICT` |
| `--max-fees N` | 0 | cap the fees applied per booking; 0 is uncapped |
| `--fk` | off | declare the three foreign keys |
| `--cabin` | off | declare a Cabin on `recipes.cargo_type` |
| `--isolation` | server default | `read-committed` or `repeatable-read` |
| `--verify N` | 0 | check the four invariants over a sample of N |
| `--seed`, `--json`, `--echo`, `--sync`, `--server-log` | | as the other scenarios |

The twin takes the same flags plus `--synchronous-commit`, and connects with
`--port 15433 --database bench`.

### `scenario3_library.py` — a read workload against a secondary index

A library circulation system: four relations (`users`, `books`,
`reservations`, `loans`) and ten read shapes, built to ask one narrow
question — **what does a non-primary-key equality cost, and what can be done
about it?** KDS answers it three ways, in three different trust classes: a
`FilterScan` walks the chain, a **Cabin** is authoritative only for values
queries have observed, and a **secondary index** is authoritative for every
value. PostgreSQL answers it one way, with a btree index, which is what makes
it a clean baseline here rather than a second pile of numbers.

> **The index read path is not built.** `docs/workplan-index.md` has
> `IX01`-`IX07` and `IX09` — key encoding, page format, catalog row, grammar,
> write hook, backfill — and stops at `IX10`/`IX11`, which are what would make
> the compiler emit `kIndexProbe`/`kIndexRange` and the step VM run them. So
> today `CREATE INDEX` succeeds and backfills, `SHOW INDEXES` reports a real
> tree, every INSERT and UPDATE pays the index's maintenance, and **no SELECT
> can use it**. Running `--index-mode single` against `--index-mode none` is
> therefore a measurement of what an index *costs* with its benefit still
> switched off. That is a real number and the driver reports it as such; when
> `IX10`/`IX11` land, the identical invocation measures the benefit.
> `--assert-index-reads` fails the run if the read shapes did not improve, so
> a later run cannot quietly report "no change" as a result.

```bash
# the row-set sweep the documentation rules require
for n in 200 1000 10000; do
  ./tools/scenario3_library.py --loans $n --index-mode none   --json ck-none-$n.json
  ./tools/scenario3_library.py --loans $n --index-mode single --json ck-idx-$n.json
  ./tools/scenario3_library.py --loans $n --index-mode none --cabin --json ck-cab-$n.json
done

# the baseline, run separately — never alongside, each would measure the other
for n in 200 1000 10000; do
  ./tools/pg_scenario3_library.py --port 15433 --database bench \
      --loans $n --index-mode none   --json pg-none-$n.json
  ./tools/pg_scenario3_library.py --port 15433 --database bench \
      --loans $n --index-mode single --json pg-idx-$n.json
done
```

| flag | default | what it does |
|---|---|---|
| `--loans N` | 1000 | the bulk relation and the row-set axis; the documented sweep is **200 / 1000 / 10000** |
| `--matches N` | 5 | rows per key for the equality shapes. `users` and `books` are scaled as `loans / matches`, which holds selectivity constant across the sweep — see below |
| `--index-mode` | `none` | `none`, `single` (one per hot equality column), `composite` (multi-column keys), `covering` (`COVERING (...)`), `all` |
| `--index-when` | `after` | `after` declares the indexes on loaded relations, exercising and timing the `IX09` backfill; `before` declares them empty so the `IX06` write hook maintains them, moving the cost into the load phase |
| `--cabin` | off | declare a Cabin on `loans.user_id` — the other accelerator for a non-pk equality, and the one with no PostgreSQL twin |
| `--ops N` | 200 | operations per read shape |
| `--verify N` | 25 | three invariants, including that a `WHERE user_id = ?` answer equals a client-side-filtered full scan — the check that would catch an index serving an incomplete set |
| `--assert-index-reads` | off | fail if `--index-mode` did not improve the equality shapes |
| `--suffix`, `--seed`, `--json`, `--echo` | | as the other scenarios |

**Why `users` and `books` scale with `loans`.** If they did not, a bigger
relation would also mean a *less selective* predicate, and the three sizes
would move two variables at once. Holding matches-per-key at 5 while the
relation grows 200 → 10,000 is exactly the axis on which a scan (O(rows)) and
an index probe (O(log rows + matches)) diverge. `books-by-genre` is the
deliberate counter-case: genre cardinality is fixed at 16, so its match count
*does* grow with the relation, and it is where an index should pay least.

The twin adds `--synchronous-commit` and `--no-analyze`. **`ANALYZE` is on by
default and is not tuning**: without statistics PostgreSQL may not choose its
index at all, which would make the baseline a coin toss rather than a
baseline. The twin also runs `EXPLAIN` on three shapes and prints the plans,
because a declared index is not necessarily a used one — KDS has no `EXPLAIN`,
which is itself worth recording. There is **no Cabin equivalent** on the
PostgreSQL side and the twin does not invent one; a `--cabin` run simply has
no twin column in the comparison.

---

## The statement-level tools

| tool | what it measures |
|---|---|
| `benchmark.py` | four phases (insert / point-select / full-scan / update) against one synthetic relation, on ckdbs |
| `pg_benchmark.py` | the same four phases on PostgreSQL, same table shape, same JSON keys |
| `aggregate_benchmark.py` | the fold's cost against group count and row count (`docs/feat-aggregate.md`) |
| `join_benchmark.py` | join-chain shapes |
| `latency_matrix.py` | per-statement latency across storage forms and access kinds |
| `bench/keystone_alloc_bench.cpp` | the id allocator, in-process — no client, no socket |
| `bench/txn_layers_bench.cpp` | the transaction layers' cost, in-process |

## The shared harness

`bench_common.py` is the timing and reporting harness both engines' drivers
use. `Phase.record()` takes one latency and one reply — a reply beginning
`ERR` counts as an error — and `Phase.summary()` emits **p0, p25, p50, p95,
p99, max and mean**, which is what the documentation rules require of every
latency table. `report()` prints them; `write_json()` writes them.

Latencies measured through these drivers include the Python client's socket
cost, which on a release build is most of a small statement — a pk lookup is
~130 µs end to end on ckdbs and ~220 µs on PostgreSQL, of which the engine is
a fraction. They are what a client pays, not what an engine costs. Treat
sub-20 µs differences on those rows as noise, and establish the throughput
noise floor per run rather than assuming one.

## Cluster lifecycle for the baseline

```bash
./tools/pg_setup.sh init | start | stop | status | destroy --yes
./tools/pg_setup.sh psql "SELECT 1"
./tools/pg_setup.sh timing on        # log_min_duration_statement 0
```

It lives at `$HOME/pg-bench`, listens on **15433** (ckdbs uses 15432), and is
left at PostgreSQL's default tuning on purpose — a baseline tuned by hand is
not a baseline.

## Before you trust a number

1. Is the machine quiet? `uptime` and `pgrep cc1plus`. A concurrent build
   cut this project's booking throughput by **3×** in one measured instance,
   and the run looked perfectly normal from inside.
2. Is the binary current? Compare `stat -c %y build-release/kds_server`
   against `git log -1 --format=%ci`.
3. Is the data file on a real device, and does each configuration get a fresh
   one? Catalog rows are never reclaimed and undo never purges, so a second
   run on one file is not a repeat of the first.
4. Did `--verify` pass? A throughput number over a workload that lost writes
   is a measurement of nothing.
5. Was it measured at more than one row-set size? A single cardinality cannot
   separate a per-statement fixed cost from a per-row one, and in this engine
   they point in opposite directions — see the fit table in
   `bench/results-scenario1-vs-pg.md`, where the same shape reads 1.43× faster
   than PostgreSQL at 252 rows and 1.16× slower at 10,080.
