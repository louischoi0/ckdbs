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

## The three scenarios

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

Thirty years of daily bars across seven relations, walked forward by eight
strategies. Produces a QPS matrix over every read shape the workload issues,
priced cold, warm, with a Cabin and after dropping it, plus write sweeps
against transaction batch size and connection count.

```bash
./tools/scenario1_backtest.py --years 30 --symbols 200 --sweep
./tools/pg_scenario1_backtest.py --port 15433 --database bench --years 30
./tools/compare_scenario1.py ck.json pg.json      # side by side
```

Key flags: `--years`, `--symbols`, `--rebalance`, `--top-k`,
`--bars-clustered`, `--sweep` / `--no-sweep`, `--write-sweep`,
`--connections`, `--aggregates` (the `agg-*` phases), `--cabin`, `--fk`,
`--analyze`, `--verify`, `--json`.

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
