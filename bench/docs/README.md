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

## The scenarios

Each scenario is a whole workload rather than a statement mix, and each has a
PostgreSQL twin beside it that drives the same work through `pg_wire.py`. The
twins import their schema and their business logic from the ckdbs driver, so
the two cannot drift into measuring different questions.

The four `scenarioN_*.py` drivers are workloads. `index_benchmark.py` at the
end of this section is the one exception — a feature matrix rather than a
workload — and it is here rather than under the statement-level tools because
it has a twin and follows the same conventions.

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

> **The index read path is built** as of `IX01`-`IX16`, so `--index-mode`
> measures an index's benefit as well as its cost: a statement with an
> equality or `BETWEEN` on an indexed column compiles to `kIndexProbe` /
> `kIndexRange` and descends. Use `index_benchmark.py` below for the
> narrow index question and this scenario for the whole-workload one.

**Two different ways to switch the index off, and the document needs both.**
`--index-mode none` declares no index at all, so comparing it against
`single` prices the index's *whole* cost — the backfill, the per-write
maintenance and the space — against its read benefit. Starting the server
with `indexes = off` (the `IX13` config key) leaves the index declared and
maintained and disables only the read path, which isolates the read benefit
with every write-side cost still being paid. Reporting only the first
credits the index for a saving whose cost sits in a phase the table does not
show. There is **no runtime `SET`** for that key — it is read at server
start — so the driver's `--server-indexes` is a *label* that records which
server a cell ran against, not a switch that changes one.

**`ANALYZE <statement>` is this engine's `EXPLAIN`.** It names each step's
access kind (`IndexProbe`, `IndexRange`, `FilterScan`, `Scan`, `Lookup`) and
an index step's `index_scanned` / `index_resolved` counts. The driver runs it
on seven shapes every run and prints the result, and `--assert-index-reads`
checks *that* rather than latency — so a plan that regressed to a scan fails
the run instead of passing quietly on a relation small enough to hide it.
The twin prints PostgreSQL's `EXPLAIN` beside it, which matters more than it
sounds: at 200 rows PostgreSQL **declines its own index** and picks a
`Seq Scan`, because its cost model says the relation is too small, while KDS
descends an index whenever one is declared. A latency table without the plan
next to it cannot tell "the index was slower" from "the index was not used".

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

### `scenario4_cabinopt_days.py` — the cabin optimizer over rotating business days

Simulated consecutive trading days for the cabin optimizer
(`docs/feat-physical-optimizer.md` Part II), built to exercise the lifecycle
past CREATE — DECAYING, DROP, re-nomination — which the single-shot PHY08
cases (`cabin_optimizer_benchmark.py`) never reach. Five BTREE relations:
two 10,000-row *boards* whose hot day rotates (day 1 `board_a`, day 2
`board_b`, day 3 `board_a` with a disjoint hot-symbol set — a whole
`(relation, column)` shape going cold overnight, the only rotation that can
trigger a DROP), and three *tapes* of 200 / 1,000 / 10,000 rows (the row-set
sweep) probed every day with hot **values** rotating disjointly — the shape
stays hot, so the per-column Cabin should persist and pay only per-value
re-observation. Each day: an `open` insert burst into the hot board, a
wall-paced `trading` session of skewed hot-symbol equality probes plus a
pk-lookup control, a `close` of COUNT/SUM/GROUP BY full scans, then an idle
overnight during which the on arm is polled every 3 s for state transitions.

Arms are one server and one fresh disk-backed data file each, identical
configs, interleaved per block inside every phase: `off` (no Cabin ever),
`on` (`SET CABIN_OPTIMIZER ON` at day 1 open), and optionally `declared`
(`CREATE CABIN` on every symbol column up front, optimizer off — the static
ceiling, and the arm that cannot retire anything). Time compression lives in
the server config, not the driver: lower `decay_half_life` (e.g. 5 s = 120×
against the 600 s default) and `cabin_optimizer_snapshot_interval_ms` (e.g.
500), and state both in the results file. Evidence captured untimed at every
phase boundary: `SHOW CABIN_OPTIMIZER` (states, action counters, budget,
per-entry B/C), `SHOW CABINS` (hits/misses), `ANALYZE` of the hot probes,
and a per-day byte-identical-reply verification across arms.

```bash
# three servers, one config (lowered half-life + snapshot interval), fresh files
for arm in off on declared; do
  ./build-release/kds_server ~/bench-cabinopt-days/$arm.db --port <port> \
      --config days.conf &
done
./tools/scenario4_cabinopt_days.py \
    --port-off 15651 --port-on 15652 --port-declared 15653 \
    --pid-off <pid> --pid-on <pid> --pid-declared <pid> --json days.json
./tools/pg_scenario4_cabinopt_days.py --port 15433 --database bench --json pg.json
```

| flag | default | what it does |
|---|---|---|
| `--days N` | 3 | simulated business days; the board rotation is a-b-a-… |
| `--session-seconds` | 45 | paced wall length of one trading session (the controller's decay clock needs wall time, so the session is slotted and the slack slept) |
| `--overnight-seconds` | 45 | the idle gap; at a 5 s half-life this is 9 half-lives of cold |
| `--blocks N` | 12 | interleave granularity: every phase runs block-by-block across all arms |
| `--board-probes` / `--tape-probes` / `--pk-ops` / `--open-inserts` / `--close-rounds` | 2400 / 396 / 240 / 240 / 3 | per arm per day; equal work across arms by construction (one drawn plan, replayed per arm) |
| `--port-*` / `--pid-*` | | one server per arm; pids enable `/proc` server-CPU sampling per phase; `--port-declared 0` skips the third arm |
| `--suffix`, `--seed`, `--json` | | as the other scenarios |

TPS is reported per arm per day over **busy time** (summed statement
latencies), because the sessions are wall-paced and wall TPS would measure
the pacing. The twin runs **one** unpaced day of the same statement shapes:
at defaults PostgreSQL builds nothing for the hot predicate and retires
nothing, so day 1 stands for every day, and `EXPLAIN (ANALYZE, BUFFERS)` of
the board, tape and zero-row probes is captured as evidence of the plan.

The **amortization window is a server key, not a driver flag** — this
scenario measures whatever `cabin_optimizer_amort_windows` the config sets,
and `bench/results-cabin-optimizer-days.md` reports the same run at both 1
and the shipped 64 (and, in its Part III, at 64 again on a later commit).
Note that at a compressed half-life the DROP cooldown can exceed the whole
run: it is `cabin_optimizer_cooldown_half_lives` (its own key since
2026-08-10, default 128 — previously fused as `2 × T_amort`, which is the
same number at the shipped window), so at `decay_half_life = 5` it is 640 s
against a 283 s three-day matrix, no DROP is reachable, and the retirement
evidence has to come from `cabinopt_cooldown_check.py` below. Work that
arithmetic out before choosing the pacing, not after reading an empty drops
counter.

### `cabinopt_cooldown_check.py` — the DROP cooldown and the DECAYING onset, per configuration and per binary

A multi-arm behavioural check for the two lifecycle instants the
business-days matrix cannot reach, because at any honest time compression
they sit outside a 283 s run. N servers identical except for their
`cabin_optimizer_*` keys, warmed on the same hot probe over the same
relation until every controller holds an ACTIVE Cabin of its own, then left
in silence and polled. It measures:

- **the DROP cooldown** — how long a DECAYING entry is given to rebound.
  The control arm must go DECAYING and DROP within a couple of half-lives;
  a test arm with a longer cooldown must outlive it. One run yields both
  facts: the key is live, and the cooldown is the number configured rather
  than some other number.
- **the DECAYING onset** — how long silence takes to push an ACTIVE Cabin
  out of ACTIVE, reported against a prediction that needs no calibration.
  `B` decays on the R1 clock and `C = P_rel / T_amort` does not, so
  widening the amortization window by a factor k moves the onset by exactly
  `log2(k)` half-lives; every workload-dependent term cancels in that
  difference. The driver takes the narrowest-window arm as the reference and
  prints observed-versus-predicted deltas for the rest.

Running the **same** config against two ports served by two *different
binaries* turns either measurement into an A/B — which is what
`bench/results-cabin-optimizer-days.md` Part III does to show that the
onset saturates at the Q24.8 underflow floor before the log-domain decay
read and tracks `log2(T_amort)` after it.

Nothing here is timed: every output is a wall-clock instant of a state
transition or a counter, so there are no latencies and no percentiles. The
relation shape and row generator are **imported from
`scenario4_cabinopt_days.py`** so the two cannot drift.

The half-life is the affordability knob and is deliberately not what is
under test — at `decay_half_life = 1` the shipped cooldown is 128 s
instead of 21 h 20 m, and the ratio between the arms is unchanged. **Every
arm must run the same half-life** or the comparison means nothing.

```bash
# cooldown: two configs differing in exactly one key, both decay_half_life = 1
./build-release/kds_server ~/bench-cabinopt-cool/a1.db  --port 15661 --config amort1.conf  &
./build-release/kds_server ~/bench-cabinopt-cool/a64.db --port 15662 --config amort64.conf &
./tools/cabinopt_cooldown_check.py \
    --arm amort1:15661:1:2 --arm amort64:15662:64:128 \
    --half-life 1 --rows 10000 --warm-seconds 25 \
    --silence-seconds 200 --poll-seconds 1 --json cool.json

# onset A/B: three windows x two binaries, same three config files on both
./tools/cabinopt_cooldown_check.py \
    --arm post-w64:15671:64:128     --arm post-w4096:15672:4096:128 \
    --arm post-w100000:15673:100000:128 \
    --arm pre-w64:15681:64:128      --arm pre-w4096:15682:4096:128 \
    --arm pre-w100000:15683:100000:128 \
    --half-life 1 --rows 10000 --warm-seconds 25 \
    --silence-seconds 40 --poll-seconds 0.5 --json wide.json
```

| flag | default | what it does |
|---|---|---|
| `--arm NAME:PORT:AMORT[:COOLDOWN]` | — | repeatable, two minimum (the control *is* the measurement). `AMORT` is the arm's `cabin_optimizer_amort_windows` — the onset prediction reads it — and the optional `COOLDOWN` is its `cabin_optimizer_cooldown_half_lives`. The driver cannot read a key back, so both are stated; omitting `COOLDOWN` falls back to the pre-decoupling `2 × AMORT`, which is what a pre-2026-08-10 build fuses |
| `--half-life S` | required | the servers' `decay_half_life`; stated, not inferred |
| `--rows N` | 10000 | rows in the probed relation; sets `P_rel` and so the cost floor `P_rel / T_amort` printed as `cost_q16` |
| `--warm-seconds` / `--warm-block` | 20 / 60 | upper bound on the warm phase and probes per arm per interleave block; the phase ends early once every arm is ACTIVE |
| `--silence-seconds` / `--poll-seconds` | 200 / 1 | the silence window and the `SHOW CABIN_OPTIMIZER` poll cadence. The poll bounds the resolution: an observed instant is accurate to ±1 poll, and is biased late by up to one poll plus one snapshot interval |
| `--suffix`, `--seed`, `--timeout`, `--json` | | as the other drivers |

The cooldown verdict is **only asked when it is reachable**: if the
shortest predicted cooldown exceeds `--silence-seconds`, the driver says so
and reports the onset alone, rather than printing a failure that is really
its own pacing. Exit status is 0 when the cooldown verdict passes, or —
in an onset-only run — when every arm reached DECAYING, so the check is
usable as a gate either way. The JSON carries every poll's full
`SHOW CABIN_OPTIMIZER` capture, which is where the per-entry B/C decay
curve and the score's Q24.8 underflow floor are readable.

### `index_benchmark.py` — secondary indexes, priced

The four questions `docs/workplan-index.md` IX14 asks, answered in one process
against one server so they are comparable: a selective non-pk equality indexed
against the walk that replaces it; the same statement with and without
`COVERING`; INSERT with 0, 1 and 2 indexes and an UPDATE that moves an indexed
key against one that does not; and a PostgreSQL twin on the same shapes.
Results at `bench/results-index.md`.

Three read relations (`ord_none`, `ord_idx`, `ord_cov`) hold **byte-identical
contents** and differ only in what is declared over them, and three write
relations (`w0`, `w1`, `w2`) carry 0, 1 and 2 indexes. Every shape is driven
with the **same argument against every relation inside one operation**, so a
table's columns are one comparison rather than three runs, and a machine that
gets busier partway through costs all of them equally.

```bash
# one size, one server, one fresh data file; sweep by repeating
for n in 200 1000 10000; do
  rm -rf ~/bench-index/idx-$n.db*
  ./build-release/kds_server ~/bench-index/idx-$n.db --port 15461 &
  ./tools/index_benchmark.py --port 15461 --suffix s$n --rows $n \
      --ops 300 --write-ops $n --update-ops 300 --verify 20 \
      --expect-indexes on --json ck-$n.json
done

# the baseline, run separately - never alongside
./tools/pg_index_benchmark.py --port 15433 --database bench \
    --rows 10000 --ops 300 --write-ops 10000 --update-ops 300 --json pg-10000.json
```

| flag | default | what it does |
|---|---|---|
| `--rows N` | 1000 | rows in each read relation, and the row-set axis. The documented sweep is **200 / 1000 / 10000** |
| `--matches N` | 6 | rows per `cust_id`; customers are scaled as `rows / matches`, which holds selectivity constant across the sweep. The range shapes span a fixed 10 customers, so their answer stays ~60-69 rows at every size |
| `--ops N` | 200 | operations per read shape **per relation** — seven shapes × three relations |
| `--write-ops N` | 400 | autocommit INSERTs per write relation. Set it to `--rows` to sweep the write test on the same axis |
| `--update-ops N` | 200 | UPDATEs per shape per write relation, capped at the rows inserted |
| `--batch N` | 200 | rows per transaction during the **load**; the measured insert phase is always autocommit |
| `--expect-indexes on\|off` | — | fail the run unless the server behaved as that setting. Read out of `ANALYZE`'s `index_scanned` counter, **not** the config: the compiled chain is identical either way by design (`feat-index.md` §12.3), so only the work done can answer it |
| `--no-writes` | off | skip the INSERT/UPDATE phases — what the `indexes = off` side of the A/B wants, since maintenance is not switchable |
| `--verify N` | 20 | argument draws for the equivalence checks: every shape's reply from each indexed relation must equal the unindexed one's **row for row and in order** (which is what IX8a's pk-order rule needs), and the two indexed write relations must answer a `region` equality identically to `w0`'s walk |
| `--suffix`, `--seed`, `--json`, `--echo` | | as the other drivers. Relation names must be valid identifiers, so a suffix cannot contain `-` |

The `indexes` key is a **startup** setting with no runtime `SET`, so the
cleanest A/B — same compiled plan, same rows, different work — is two server
processes over two freshly loaded data files, `--expect-indexes on` against
`--expect-indexes off`. Note that the driver also measures a `FilterScan` and
an index probe **inside one run**, and that in-run form is what
`bench/results-index.md` reports: the same workload drifted 6.4% between two
server processes minutes apart, which is 3.5× the in-run noise floor.

Every run carries a `PING` phase (`SELECT 1` on the PostgreSQL side): the
client and socket round trip with no engine work behind it, which is what
makes "engine time" a subtraction rather than a guess. `eq` is also executed
twice per relation, as `eq` and `eq-again`, which is the run's own noise floor.

The twin takes the same flags plus `--user`, `--database`, `--keep`,
`--synchronous-commit` and `--no-analyze`, and imports the schema, the row
generator, the shape list and the sizing arithmetic from the ckdbs driver, so
the two cannot drift. `VACUUM ANALYZE` is on by default and **is not tuning**:
without statistics PostgreSQL may not choose its index at all, and without the
vacuum the visibility map is empty so an index-only scan cannot happen even
where it is the right plan. The twin also records which scan node PostgreSQL
chose per shape — `Index Only Scan` is the plan `feat-index.md` §7 says KDS
structurally cannot produce, so seeing where PostgreSQL uses one is what
prices the missing visibility witness.

---

## The statement-level tools

| tool | what it measures |
|---|---|
| `benchmark.py` | four phases (insert / point-select / full-scan / update) against one synthetic relation, on ckdbs |
| `pg_benchmark.py` | the same four phases on PostgreSQL, same table shape, same JSON keys |
| `aggregate_benchmark.py` | the fold's cost against group count and row count (`docs/feat-aggregate.md`) |
| `assertion_benchmark.py` | a declared assertion's write-path delay, with against without, per statement shape (`docs/feat-assertion.md`) — reads `enforcing=` from the server and stamps every result with it, and `--expect-enforcing on\|off` fails a run whose server disagrees; enforcement is live (AST07), so a run today prices the admission check, the entry write and the refusal path (results: `bench/results-assertion.md`). Vary durability by server config; sweep `--rows 200/1000/10000` with `--ops ≤ rows`. No PostgreSQL twin, because PostgreSQL does not implement `CREATE ASSERTION` |
| `join_benchmark.py` | join-chain shapes |
| `latency_matrix.py` | per-statement latency across storage forms and access kinds |
| `varheap_spill_benchmark.py` | what a spilled `varchar` costs per statement, and how many WAL bytes it writes (`docs/heap-and-tuple.md` §3.4, `CommandDispatcher::LogSpills`). Results: `bench/results-wal-recovery.md` Part I. **Owns its server**, one per invocation on its own fresh data file, so a run is one (binary, rows, value length, durability) cell and two invocations are an A/B. Five phases: `ping` (`SHOW META` — the round-trip floor, which is what makes the rest decomposable), `insert-spill` / `update-spill` (a `--value-bytes` value, which spills), and `insert-inline` / `update-inline` (an `--inline-bytes` value, which does not — **the control**, since the var-heap logging path cannot reach it; a delta matched on the control is the host, not the engine). `--value-bytes` **is** the growth-rate knob: a var-heap page holds 8144 bytes and a value costs `len + 4`, so 1600 fills a page every 5 rows and 8100 fills one every row, which is the highest FPI rate the engine can be made to pay. `--rows N` **is** the row count (sweep 200 / 1000 / 10000; the update phases take their own `--update-ops` so the compared work is equal at every size). Also reports **WAL bytes written**, sampled per phase as the position of the last non-zero byte in the tail segment — the only route available, since nothing reports `append_lsn` to a client — which is how the volume cost is measured rather than inferred. `--durability strict\|group\|relaxed` is passed to the server it starts. `--verify` (on by default) reads back `COUNT(*)` on both relations and resolves an updated and an untouched spilled value to full length, and exits non-zero on a mismatch: a throughput number over a workload that lost a var-heap value is a measurement of nothing. Guards tmpfs and the 1-/5-minute load average, both overridable with `--force` and both recorded in the JSON. `--binary /path/to/other/kds_server --label base --rows 1000 --value-bytes 8100 --durability relaxed --port 15701 --json base.json`. **No PostgreSQL twin yet** — `tools/pg_varheap_spill_benchmark.py` is the task; it should import the phase shapes from here and read WAL volume from `pg_current_wal_lsn()` deltas, which are exact where this driver's byte scan is approximate |
| `mount_cost_benchmark.py` | what one mount costs and where it goes, now that recovery runs at every mount (`include/kds/server/mount_recovery.hpp`, RC09/RC11). Results: `bench/results-wal-recovery.md` Parts II-III. Mounts **one** data file `--mounts N` times, timing `exec` → the server's own "listening on" line (printed *before* `bind()`, so the number excludes bind/listen and `connect()` retries a refusal rather than trusting the line) and reading `SHOW META`'s `recovery_analysis_us` / `recovery_redo_us` / `recovery_high_water_us` / `recovery_undo_us` / `recovery_checkpoint_us` / `recovery_records` per mount. The residual — wall minus those five — is everything recovery does not own, and comparing it against a pre-recovery binary's whole mount is what closes the accounting. `--rows N` writes N rows before the sweep (`--value-bytes` over 61 spills), `--crash` SIGKILLs the loader so the first mount has winners to redo, and `--mounts` above 2 is what shows RC08's anchor paying for itself — mount 1 replays, mounts 2+ report `records=2`. Because `ScanLog` reads from the anchor to the **end of the segment**, `--rows` is also the knob that varies how much segment body is scanned: a bigger log means a *cheaper* steady-state mount. A server predating RC09 reports no counters and the columns read `-`, which is not the same as zero — that is what makes a before/after comparison possible. Registers its servers with `atexit` so an abort cannot leave one holding the port. `--binary ... --label base --rows 2000 --crash --mounts 9 --port 15806 --json out.json`. **No PostgreSQL twin yet**: the twin is `pg_ctl start` → first accepted connection, clean and after `kill -9`, over the same nine-mount shape |
| `order_by_benchmark.py` | the output sort (`docs/workplan-order-by.md` OB4/OB5, `exec::OutputSort`): what a sort costs, what the elided `ORDER BY <pk> ASC` costs, and what the feature costs a statement that does not sort. Results: `bench/results-order-by.md`. **Two passes.** Pass 2 always runs, on `--port`, and drives fifteen arms over one relation (`id, val, grp, amount, tag`, BTREE): `ping` (client+socket floor), `plain` (no `ORDER BY`), `star` (`SELECT *` — the same five columns, but rendered from the relation's schema, which needs a `TableAccess` the projection form does not), `pk-order` (`ORDER BY id ASC` — elided at compile), `nonpk` / `nonpk-desc` / `nonpk-str` (`ORDER BY val`, `val DESC`, `tag` — real sorts on an int64 and a varchar key), `nonpk-lim` (`ORDER BY val LIMIT n` — the top-N heap), `plain-lim` (`LIMIT n` with no order, whose quota *stops* the walk), `pk-point` (a control), `plain-again` (the in-run noise floor, `plain` repeated), and four **ANALYZE arms** — `an-plain`, `an-nonpk`, `an-nonpk-lim`, `an-plain-lim` — which are the same statements under `ANALYZE`. `RunAnalyze` compiles the same chain, runs the same steps through the same sink and drains the same sorter through the same quota, and then answers with one line of plan text instead of the rows, so it is the read path with the render and the reply removed: `plain` − `an-plain` prices rendering and shipping the rows, and `an-nonpk-lim` − `an-plain` prices normalizing a sort key and testing it against the heap. They are an instrument, not a workload. Pass 1 runs only when `--ab-port` names a **second server on a second binary** — typically one built from a commit predating the sort — and drives the seven arms both binaries can answer across both, interleaved block by block within each round, which is the before/after. `--rows N` **is** the row count (sweep 200 / 1000 / 10000; `--ops` defaults to 6000/2000/400 by size so each arm's server CPU clears `/proc`'s jiffy, and `--rounds` splits it into interleaved blocks). `--server-pid` / `--ab-server-pid` add server CPU per operation from `/proc/<pid>/stat` — pass the **`kds_server` pid, not a shell wrapper's**. Before timing, the driver runs `ANALYZE` on every arm and reads `sorted=` back: a binary that reports a sort for `ORDER BY <pk> ASC`, or none for `ORDER BY <non-pk>`, or a top-N holding other than `offset+limit`, **aborts the run** rather than mislabelling a row. `--verify` (on by default) checks eight properties per side including that `ORDER BY id ASC` equals the unsorted reply row for row, that `SELECT *` equals the explicit projection row for row (the star's `TableAccess` is resolved once per statement, so a hoist that resolved the wrong relation would show only here), and that `LIMIT n`'s rows are the first n of the full sorted order. `--base` marks a server that predates the sort so its unanswerable arms are skipped rather than counted as errors. `--pre-port` / `--pre-server-pid` / `--pre-label` add a **third** server whose binary *does* have the sort but predates some change to it; it answers every arm, so pass 2 runs the full arm set across it and `--port`'s interleaved. That is what keeps a "this fix is worth X" claim inside one run instead of comparing today's numbers against a results file's. **No PostgreSQL twin exists yet** — `tools/pg_order_by_benchmark.py` is the task, and it should import `COLUMNS`, `make_rows` and `arms` from here. Needs a fresh data file per server: `--port 15871 --server-pid P --pre-port 15873 --pre-server-pid R --ab-port 15872 --ab-server-pid Q --rows 10000 --ops 1200 --rounds 20 --limit 20 --seed 1 --suffix s10000 --json out.json` |
| `bulk_insert_benchmark.py` | the T1 multi-row `VALUES` statement (`docs/spec-bulkinsert.md`, BLK08): rows-per-statement 1/10/100/1000 at a fixed total row count, into scenario1's `write_probe` shape (`id` + 4 × int64, heap). Phases: `ping` (client+socket floor), `bulk-<B>` (B rows/statement, autocommitted, per-statement latencies — divide by B for per-row), `txn-1000` (`--txn-control`: the pre-T1 batching — single-row INSERTs in 1000-row transactions), and `parse-<B>` (`--parse-probe`: the B-row statement against a nonexistent table, which parses fully and dies at catalog resolution — round trip + parse, no pipeline; its ERR replies are expected). Durability is the **server's** config key, one class per run — pass `--durability` so the JSON is labeled. `--cabin` issues `CREATE CABIN ON <t>(a)` after each CREATE TABLE, which closes the T3 sorted-fill gate (`cabin_mask`) and forces the row loop — the gate-closed twin for pricing T3 (`docs/workplan-t3.md`); `--trace` keeps each bulk phase's per-statement series in `<json>.trace.json`. Verifies every reply's `rows=`, the id span, and `SELECT COUNT(*)` per phase; a mismatch aborts. Keep one server's total WAL under one 64 MiB segment (~250K rows): the stream wedges permanently when an append exactly fills a segment (found by this driver's first run). `--rows N --batches 1,10,100,1000 --suffix tag --json out.json` |
| `pg_bulk_insert_benchmark.py` | the same matrix on PostgreSQL (identity pk, explicit column list). `--synchronous-commit off` is the session-GUC twin of ckdbs `relaxed`; cluster tuning stays at defaults |
| `kwp_load_benchmark.py` | the T2 KWP binary load stream (`docs/spec-bulkinsert.md` §3, KL02–KL06): a dependency-free KWP v0 client — HELLO with the `BULK_LOAD` capability, `LOAD_BEGIN` → pre-encoded D5 chunks → windowed ACKs → `LOAD_END` — against the same int-only `write_probe` schema as `bulk_insert_benchmark.py`, so the T2-vs-T1 delta is parse+round-trip removal. Needs the server started with `kwp_port`; a text-port connection does DDL and `COUNT(*)` verification. `--chunk-rows 100,1000`, `--mode pipelined\|serial` (window 4 vs stop-and-wait — the delta prices the window), `--no-quickack` exhibits the ~40 ms Nagle stalls of the endpoint's missing `TCP_NODELAY` instead of defeating them client-side (found by this driver; see `bench/results-bulk-insert.md` Part IV). Latencies are per chunk; pipelined ones include deliberate window queueing — read the throughput. Wire layouts implemented from `include/kds/wire/kwp.hpp`, `kwp_types.hpp`, `wire/row_codec` |
| `cabin_optimizer_benchmark.py` | the cabin optimizer (`docs/feat-physical-optimizer.md` Part II, workplan PHY08), two cases on two servers. `--case null`: what `cabin_optimizer = on` costs a workload with zero eligible candidates — pk lookups + INSERTs over BTREE relations of 200/1K/10K rows, three arms (`off1`/`on`/`off2`) interleaved per round through `SET CABIN_OPTIMIZER` on one server and one data file, so the off1/off2 gap is the in-run noise floor; then the tick itself priced from `/proc/<pid>/stat` over two idle windows (`--idle-seconds` each, on then off) divided by the tick delta `SHOW CABIN_OPTIMIZER` reports. Wants a server booted with `cabin_optimizer = off` and `cabin_optimizer_snapshot_interval_ms` lowered (the results file states the value used). `--case improve`: a hot non-pk equality (`WHERE val = 7`, exactly 10 matching rows at every size — the value domain is `rows/10`) probed in three phases per size: `walk` (fixed `--probe-ops`, switch off), `transition` (SET ON; probes continue while the controller CREATEs, time-to-create and time-to-serve recorded from `SHOW CABIN_OPTIMIZER`/`SHOW CABINS` polls between blocks), `served` (same fixed ops). Captures the managed entry line, `ANALYZE` before/after (`pages=`, `cabin_hits=1 cabin_optimizer=true`), the Cabin's hits/misses, a pk-lookup control that must not move, and `--verify` compares one walked reply against one served reply row-for-row. `--server-pid` adds server-CPU-per-op to both probe phases. `--case null --rounds 60 --lookups 60 --inserts 10 --idle-seconds 60 --server-pid <pid>` / `--case improve --probe-ops 1200 --server-pid <pid>`, each against a fresh data file |
| `pg_cabin_optimizer_benchmark.py` | the improve case's twin: same relations, rows and probes (imported from the ckdbs driver), against the port-15433 cluster at defaults. Deliberately has nothing to switch on — at defaults PostgreSQL seq-scans the hot shape forever, which is the honest baseline for a structure the engine declares for itself; `EXPLAIN (ANALYZE, BUFFERS)` per size is captured as evidence of the plan |
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
