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

**Re-measured 2026-08-06 after two engine changes** - lazy decode (`a1559e3`)
and the clustered range seek (`43d05aa`). The ckdbs columns are the new
figures; the PostgreSQL columns are unchanged, and the two pk shapes are the
**controls**: neither change touches their path and neither moved.

| shape | ckdbs was | ckdbs now | pg warm | was vs pg | now vs pg |
|---|---:|---:|---:|---:|---:|
| bar-lookup (pk =) *control* | 7,714 | 8,076 | 4,636 | 1.66× | **1.74×** |
| point-join (3-rel, pk) *control* | 6,897 | 6,793 | 2,735 | 2.52× | **2.48×** |
| model-join (FilterScan+Probe) | 688 | 886 | 311 | 2.21× | **2.85×** |
| symbol-history (FilterScan) | 24 | 34 | 8 | 2.88× | **4.25×** |
| **bar-range (pk BETWEEN)** | 83 | **2,154** | 405 | 0.20× | **5.32×** |
| **day-slice (FilterScan)** | 35 | **76** | 131 | 0.27× | **0.58×** |
| **cross-join (FilterScan+2 Probe)** | 34 | **78** | 130 | 0.26× | **0.60×** |

Three things this says.

**The pk path is ckdbs's.** A clustered-btree descent plus a tagged-cell
decode beats PostgreSQL's index scan plus heap fetch by 1.74×, and the
advantage grows to 2.48× on the three-relation point join where two more
probes compound it. These two are also what makes the rest of the table
readable: they are the shapes neither change touches, and they moved by 1-4%
across builds and across a machine whose load varied.

**The full-relation scan is still PostgreSQL's, now by 1.7× rather than
3.8×.** The investigation below found the reason - ckdbs built a full row
object for every one of the 60,472 rows it rejected - and lazy decode closed
about half the gap: `day-slice` 35 -> 76 and `cross-join` 34 -> 78. What is
left is no longer decode; it has not been attributed.

**`bar-range` was the one result that named a known gap, and the gap is
closed.** A pk `BETWEEN` compiled to a `Range` step that pruned the *tail* -
the first page whose `min_key` passes the high bound ends the walk - and
never the head, so a range 200 wide cost everything before it. It now
descends to its low bound (`btree::BtreeSeekLeaf`) and walks siblings from
there: **83 -> 2,154 QPS, from 5× behind PostgreSQL to 5.3× ahead.** A heap
relation still starts at the head, because it has no index to descend and
finding the low bound *is* the walk.

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
| day-slice | 76 | **6,703** | **87.82×** | 131 | 2,937 | 22.37× |
| cross-join | 78 | **5,275** | **67.70×** | 130 | 1,844 | 14.24× |
| model-join | 886 | 1,304 | 1.47× | 311 | 287 | 0.92× |
| symbol-history | 34 | 21 | **0.61×** | 8 | 9 | 1.07× |

**The gains fell and the Cabin did not get worse** - 184× became 88×
because the *baseline* nearly doubled, while the served number barely moved
(6,407 -> 6,703). A ratio against a moving denominator is the thing to read
carefully here: what a Cabin is worth in absolute QPS is unchanged, and what
it is worth *relative to walking* halves the moment walking gets cheaper.
Both are still far above PostgreSQL's indexed answer on the same shape.

**The Cabin turns ckdbs's worst two shapes into its best.** `day-slice` goes
from 3.8× behind PostgreSQL to 2.2× ahead of it; `cross-join` from 3.8×
behind to 3.05× ahead. That is the feature working exactly as
`docs/feat-cabin.md` §1 specifies — an observed value's entry set is served
without opening the relation — on a workload whose filter column repeats.

**Two rows where nothing helps, and one of them now actively hurts.**
`symbol-history` has 8 distinct values each matching 7,560 rows: the entry
set is as large as the scan it replaces. It used to cost what walking cost
(0.97×); now that walking is cheaper it costs **more** - 34 QPS walking
against 21 served, **0.61×**. Resolving 7,560 entries one pk at a time was
always the more expensive way to read them, and lazy decode made the
comparison honest by removing the per-row penalty the walk was paying.
That is an argument for the `CABIN AUTO` threshold (`feat-cabin.md` §8.1)
having to consider selectivity, not just repetition - a Cabin on a value
matching an eighth of the relation is a pessimization. `model-join` is
where PostgreSQL's planner *declines the index it was just given* — 8 values
over 2,872 rows, where a seqscan is genuinely cheaper — which is the planner
being right, not the measurement being wrong.

**Drop check: every cell returned to its warm baseline** (ckdbs 0.95-1.04×,
PostgreSQL 0.97-1.02×). The third column measured the accelerator and not a
cache that happened to warm while it existed. On the ckdbs side this is also
`feat-cabin.md` §1's corollary demonstrated: un-observing is always legal,
and dropping a Cabin is a performance event and never a correctness one.

## Write QPS, by transaction batch size

> **These numbers are not comparable to the read tables above, and were not
> re-measured with them.** The reproduce line at the foot of this document
> puts the data file at `/tmp/cmp.db`, and `/tmp` on this host is **tmpfs**,
> where `fsync` costs ~0.3 us. That is why autocommit reads 3,324 rows/s -
> 300 us a row, faster than one `fsync` on the real volume, which
> `bench/results-latency-matrix.md` measures at ~1 ms. Re-run on xfs the same
> sweep gives **679** rows/s at batch 1 and 8,785 at batch 1,000; the batched
> figures barely move because they amortise the sync, and the unbatched one
> falls 4.9x because it *is* the sync.
>
> So the table below is a tmpfs measurement of ckdbs against a PostgreSQL
> cluster whose data directory was not on tmpfs. **It overstates ckdbs's
> write advantage and should be re-run like for like before it is quoted.**
> The read tables are unaffected: both engines serve those from cache, and
> the two pk controls landed within 1-4% of their previous values.


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

| connections | ckdbs was | ckdbs now | pg | ckdbs scale | pg scale |
|---|---:|---:|---:|---:|---:|
| 1 | 35 | 72 | 130 | 1.00× | 1.00× |
| 2 | 35 | 74 | 188 | 1.03× | 1.44× |
| 4 | 35 | 77 | 198 | 1.07× | 1.52× |
| 8 | 35 | 76 | 194 | 1.06× | 1.49× |

The absolute figures doubled with lazy decode - this sweep runs `day-slice`
- and the shape of the line did not change at all, which is the point of
the paragraph below.

ckdbs is flat because it dispatches one core's statements on one thread, so
more connections buy round-trip overlap and this workload has none to
recover. PostgreSQL runs a backend process per connection and scales 1.5×
before saturating. This is an architectural difference reported correctly,
not a measurement error — and it is the one line of the report where the two
numbers mean structurally different things. The cross-core pipeline
(`docs/crosscore.md` P4) is what would move the ckdbs column.

## Load

> Same caveat as the write sweep: taken on tmpfs. The re-run on xfs gives
> load-bars 8,867/s, load-stats 7,140/s and load-sessions 9,337/s - within a
> few percent of the figures below, because these phases batch 200 rows per
> transaction and so pay one sync per 200 rows either way. It is the
> unbatched path that the medium decides.


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
# NOT /tmp - that is tmpfs on this host, where fsync is free and every write
# number becomes a measurement of the page cache (see the write sweep's note).
./build-release/kds_server ~/scratch/cmp.db --port 15734 --log-dir ~/scratch --log-file cmp.log
./tools/pg_setup.sh init          # scratch cluster on 15433, untuned

# sequentially — running both at once makes each measure the other:
python3 tools/scenario1_backtest.py    --port 15734 --years 30 --symbols 8 --seed 1 --json kds.json
python3 tools/pg_scenario1_backtest.py --port 15433 --years 30 --symbols 8 --seed 1 --json pg.json
python3 tools/compare_scenario1.py kds.json pg.json
```

The comparison refuses to print if the two runs did not run the same
workload, and says which parameter or which model's P&L disagreed.
