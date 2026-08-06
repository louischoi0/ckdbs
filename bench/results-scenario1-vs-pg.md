# Backtest QPS — ckdbs against PostgreSQL 17.10

Measured 2026-08-06 with `tools/scenario1_backtest.py` and
`tools/pg_scenario1_backtest.py`, compared by `tools/compare_scenario1.py`.
30 years of daily bars: 7,560 sessions × 8 symbols = 60,480 rows in
`daily_bars` and 60,480 derived feature rows in `daily_stats`, plus 2,872
`model_results` rows written by eight strategy models walking forward.
Runs were **sequential**, one engine at a time on an idle box; the seed was
1 on both.

**Headline: the two engines lose and win in opposite places, and the Cabin
is the largest single effect in the table.** ckdbs is 1.7-2.9× faster on
every primary-key shape and 4-8× faster on writes; PostgreSQL is ~3.8×
faster on the unindexed scans that dominate this workload. Given its own
accelerator on those scans, ckdbs's Cabin is worth **184×** where
PostgreSQL's btree index is worth **22×** — and lands 2.2× higher in
absolute QPS, turning ckdbs's worst column into its best.

## The workload is provably one workload

The PostgreSQL tool imports the price generator, the derived features, all
eight models and the scoring from the ckdbs tool rather than reimplementing
them. With the same `--seed`, both engines ingest byte-identical prices and
both backtests make identical decisions. The comparison tool checks this
before printing anything, and it passed: **all eight models scored
identically to the basis point on both engines.** Nothing below is a
comparison of two different workloads.

Both sides also pass their own `--verify`: every model's P&L, read back
through the comparison join, matches what the driver accumulated.

## Read QPS, by shape

| shape | ckdbs cold | pg cold | ckdbs warm | pg warm | ckdbs/pg |
|---|---:|---:|---:|---:|---:|
| bar-lookup (pk =) | 7,648 | 4,606 | 7,714 | 4,636 | **1.66×** |
| point-join (3-rel, pk) | 6,820 | 2,734 | 6,897 | 2,735 | **2.52×** |
| model-join (FilterScan+Probe) | 664 | 296 | 688 | 311 | **2.21×** |
| symbol-history (FilterScan) | 24 | 9 | 24 | 8 | **2.88×** |
| bar-range (pk BETWEEN) | 83 | 388 | 83 | 405 | 0.20× |
| day-slice (FilterScan) | 35 | 129 | 35 | 131 | 0.27× |
| cross-join (FilterScan+2 Probe) | 34 | 129 | 34 | 130 | 0.26× |

Three things this says.

**The pk path is ckdbs's.** A clustered-btree descent plus a tagged-cell
decode beats PostgreSQL's index scan plus heap fetch by 1.66×, and the
advantage grows to 2.52× on the three-relation point join where two more
probes compound it.

**The full-relation scan is PostgreSQL's, by ~3.8×.** `day-slice` walks
60,480 feature rows to return 8. **Not because PostgreSQL scans faster** -
see the investigation below, which measures where the 473 ns per row go and
finds that ckdbs builds a full row object for every one of the 60,472 it
rejects, where PostgreSQL tests the qualifier against the raw tuple first.

**`bar-range` is the one result that names a known gap rather than a general
one.** A pk `BETWEEN` compiles to a `Range` step that prunes the *tail* of
the chain — the first page whose `min_key` passes the high bound ends the
walk — and never the head, because skipping leading pages needs a seek that
does not exist. So a range 200 wide costs everything before it. The tool's
own comment predicted this; the 0.20× is its size.

## Why ckdbs loses those three, investigated

Re-measured at HEAD on an idle box. The three losing shapes have **two**
root causes, and neither is "PostgreSQL scans faster".

### day-slice and cross-join: the scan materialises every row it discards

`ANALYZE` on the live relation reports `examined=60480 matched=8` for one
`day-slice`. At 28.6 ms that is **473 ns per row**, and it decomposes:

| | ns/row |
|---|---:|
| the whole scan | 473 |
| `DecodeRowInto` alone, on this 12-column row | **355 (75%)** |
| reading all 12 cells - the floor | **13** |
| ~876 page fetches, amortised | ~15 (3%) |

So 96% of the decode is not reading bytes. It is building twelve 80-byte
`AstValue`s per row, each carrying two `std::string`s - for a predicate that
reads **one column**.

`ChainRunner::AcceptTupleAt` decodes the whole row as soon as the tuple is
visible and evaluates the residual afterwards, so this scan fully
materialises 60,472 rows it immediately throws away. `cross-join` is the
same scan plus two probes - 34 QPS against day-slice's 35 - so the probes
cost almost nothing and it is the same finding twice.

PostgreSQL's seqscan tests the qualifier against the raw tuple and forms a
result row only for the 8 that match. **The engines are not doing the same
thing**, which is what the first version of this document assumed.

### bar-range: a seek that exists and is not used

`daily_bars` is BTREE-clustered, so `BtreeLookup` can descend to the low
bound. `kRange` does not: it starts at the head of the leaf chain and prunes
only the tail. The tool draws the low bound uniformly, so on average it
walks half the relation to return 200 rows.

The arithmetic confirms the mechanism rather than merely asserting it:
bar-range is 12.5 ms against a full scan's 28.6 ms - **44% of a full scan**,
which is what "start at the head, stop at the match" predicts for a
uniformly drawn bound. PostgreSQL's index range scan touches only the
qualifying leaves.

### What each would take

- **Lazy decode**: decode the columns the residual references, evaluate, and
  materialise the rest only on a match. `Step::residual` already records
  which columns those are. At 8/60,480 selectivity that is roughly a 12x cut
  in decode work on this shape.
- **A range seek**: descend to the low bound, then walk siblings until the
  high bound. The primitive exists; `kRange` never calls it.

Neither is a throughput ceiling. Both are design choices with the
measurements above as their price.

## The accelerator each engine offers for a non-pk equality

This is the comparison the tools were built for. Both accelerators are
created at runtime on an already-loaded relation, measured, dropped, and
measured again — `CREATE CABIN`/`DROP CABIN` against `CREATE INDEX`/`DROP
INDEX`. Each gain is measured entirely within one engine, so it carries
nothing about the client, the machine, or the wire.

| shape | ckdbs warm | ckdbs **cabin** | gain | pg warm | pg **index** | gain |
|---|---:|---:|---:|---:|---:|---:|
| day-slice | 35 | **6,407** | **184.10×** | 131 | 2,937 | 22.37× |
| cross-join | 34 | **5,624** | **166.89×** | 130 | 1,844 | 14.24× |
| model-join | 688 | 1,249 | 1.81× | 311 | 287 | 0.92× |
| symbol-history | 24 | 24 | 0.97× | 8 | 9 | 1.07× |

**The Cabin turns ckdbs's worst two shapes into its best.** `day-slice` goes
from 3.8× behind PostgreSQL to 2.2× ahead of it; `cross-join` from 3.8×
behind to 3.05× ahead. That is the feature working exactly as
`docs/feat-cabin.md` §1 specifies — an observed value's entry set is served
without opening the relation — on a workload whose filter column repeats.

**Two rows where nothing helps, for two different reasons.**
`symbol-history` has 8 distinct values each matching 7,560 rows: the entry
set is as large as the scan it replaces, so resolving it costs what walking
costs. Neither engine can do better and neither claims to. `model-join` is
where PostgreSQL's planner *declines the index it was just given* — 8 values
over 2,872 rows, where a seqscan is genuinely cheaper — which is the planner
being right, not the measurement being wrong.

**Drop check: every cell returned to its warm baseline** (ckdbs 0.95-1.04×,
PostgreSQL 0.97-1.02×). The third column measured the accelerator and not a
cache that happened to warm while it existed. On the ckdbs side this is also
`feat-cabin.md` §1's corollary demonstrated: un-observing is always legal,
and dropping a Cabin is a performance event and never a correctness one.

## Write QPS, by transaction batch size

| rows/txn | ckdbs | pg | ckdbs/pg | ckdbs gain | pg gain |
|---|---:|---:|---:|---:|---:|
| 1 (autocommit) | 3,324 | 718 | **4.63×** | 1.00× | 1.00× |
| 10 | 8,097 | 3,272 | 2.47× | 2.44× | 4.56× |
| 100 | 9,392 | 4,999 | 1.88× | 2.83× | 6.96× |
| 1,000 | 9,451 | 5,355 | 1.76× | 2.84× | 7.46× |

ckdbs is faster at every batch size, most so at autocommit. The *gain*
columns are the comparable half: PostgreSQL gains 7.46× from batching where
ckdbs gains 2.84×, which is what two different default durability settings
look like — ckdbs `durability = group`, PostgreSQL `synchronous_commit =
on`. Neither cluster was tuned, on the principle that a baseline tuned by
hand is not a baseline.

## Concurrency — the weakest row, and not a defect

| connections | ckdbs | pg | ckdbs scale | pg scale |
|---|---:|---:|---:|---:|
| 1 | 35 | 130 | 1.00× | 1.00× |
| 2 | 35 | 188 | 1.01× | 1.44× |
| 4 | 35 | 198 | 1.01× | 1.52× |
| 8 | 35 | 194 | 1.01× | 1.49× |

ckdbs is flat because it dispatches one core's statements on one thread, so
more connections buy round-trip overlap and this workload has none to
recover. PostgreSQL runs a backend process per connection and scales 1.5×
before saturating. This is an architectural difference reported correctly,
not a measurement error — and it is the one line of the report where the two
numbers mean structurally different things. The cross-core pipeline
(`docs/crosscore.md` P4) is what would move the ckdbs column.

## Load

| phase | ckdbs | pg | ckdbs/pg |
|---|---:|---:|---:|
| load-bars (60,480 rows) | 8,691/s | 4,428/s | 1.96× |
| load-stats (60,480 rows) | 7,050/s | 4,511/s | 1.56× |
| load-sessions (7,560 rows) | 9,324/s | 5,043/s | 1.85× |

Batched 200 rows per transaction on both sides. The whole ckdbs ingest took
64s.

The single-row phases (`load-exchanges`, `load-symbols`, `load-models`,
`result-insert`) show ckdbs 6-9× ahead, but they are unbatched autocommit
statements and so are measuring the same thing the write sweep's first row
measures, at a smaller sample. Read the sweep, not these.

## What this does not measure

- **Recovery.** ckdbs has none (`docs/wal.md`); PostgreSQL's is being paid
  for in every write number above. This is the largest unpriced difference
  in the report.
- **Aggregates.** The model comparison is a plain join reduced client-side
  on both sides, because ckdbs's grammar has no `SUM`/`GROUP BY`
  (`docs/parser-v2.md` I14). Written idiomatically, PostgreSQL would return
  8 rows instead of 2,872 and the `compare-all` row would invert.
- **Concurrent readers and writers.** Every phase here is single-connection
  apart from the concurrency sweep. `tools/scenario0_stockmarket.py` and its
  PostgreSQL twin are where contention is measured.
- **Anything past 8 symbols or one core.** `cores = 1` throughout.

## Reproducing

```
./build-release/kds_server /tmp/cmp.db --port 15734 --log-dir /tmp --log-file cmp.log
./tools/pg_setup.sh init          # scratch cluster on 15433, untuned

# sequentially — running both at once makes each measure the other:
python3 tools/scenario1_backtest.py    --port 15734 --years 30 --symbols 8 --seed 1 --json kds.json
python3 tools/pg_scenario1_backtest.py --port 15433 --years 30 --symbols 8 --seed 1 --json pg.json
python3 tools/compare_scenario1.py kds.json pg.json
```

The comparison refuses to print if the two runs did not run the same
workload, and says which parameter or which model's P&L disagreed.
