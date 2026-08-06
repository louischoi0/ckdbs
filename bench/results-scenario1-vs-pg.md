# Backtest QPS — ckdbs against PostgreSQL 17.10

Measured with `tools/scenario1_backtest.py` and
`tools/pg_scenario1_backtest.py`, compared by `tools/compare_scenario1.py`.
30 years of daily bars: 7,560 sessions × 8 symbols = 60,480 rows in
`daily_bars` and 60,480 derived feature rows in `daily_stats`, plus 2,872
`model_results` rows written by eight strategy models walking forward.

**Every number in this document is from one re-measurement on 2026-08-06,
after `2251623`**, with the earlier partial figures kept only where they are
labelled "was". The conditions, which the previous version of this document
did not hold constant and had to caveat table by table:

- **ckdbs built at HEAD.** The binary in the tree was five minutes older
  than `43d05aa` and did not contain the range seek; it was rebuilt first.
- **Both data directories on the same xfs volume.** The earlier ckdbs runs
  put the data file in `/tmp`, which is **tmpfs on this host**, against a
  PostgreSQL cluster whose data directory was not. That inflated every
  unbatched ckdbs write by ~4.9× and is the correction below.
- **Sequential**, one engine at a time, 1-minute load average 0.29 at the
  start.

**Independently reproduced 2026-08-06 at `983d72d`**, when the `agg-*`
phases were added. Every read shape landed within 1-3% of the table below —
`bar-lookup` 7,656 against 7,842, `bar-range` 2,199 against 2,183,
`day-slice` 82 against 78, `cross-join` 82 against 79 — across a different
build directory, a different data file and a separate run of both engines.
That reproduction is what licenses reading the ratios here as properties of
the engines rather than of one afternoon.

**The reproduction also found the trap that produces a fake regression.**
`CMakeLists.txt` defaults `CMAKE_BUILD_TYPE` to **Debug**, so a run against
`./build` measures an unoptimized binary with debug assertions live against
a production PostgreSQL. Done by accident once here, it reported `bar-range`
at 302 q/s instead of 2,199, `day-slice` at 5 instead of 82, and `bar-lookup`
below PostgreSQL's — a uniform 3-25× loss that looks exactly like a real
regression and is not one. **Check `CMAKE_BUILD_TYPE` in the cache before
believing any number in this file**, and see the Reproducing section.

**Headline: the two engines lose and win in opposite places, and the Cabin
is the largest single effect in the table.** ckdbs is 1.7-2.6× faster on the
primary-key shapes, 5.7× on a pk range, and 3-4× on the two FilterScans with
low match counts; PostgreSQL is ~1.7× faster on the two full-relation scans
that dominate this workload. Given each engine's own accelerator on those
scans, ckdbs's Cabin is worth **86×** where PostgreSQL's btree index is
worth **20×**, and lands 2.4× higher in absolute QPS.

**Correction to the previous headline: "4-8× faster on writes" was a tmpfs
artifact and is withdrawn.** On the same volume the two engines are within
5% of each other at autocommit (684 against 717 rows/s), and ckdbs's
advantage appears only with batching, reaching 1.75× at 1,000 rows per
transaction.

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

Both engines re-measured together, after lazy decode (`a1559e3`) and the
clustered range seek (`43d05aa`). "was" is the pre-change ckdbs figure. The
two pk shapes are the **controls**: neither change touches their path, and
neither moved.

| shape | ckdbs was | **ckdbs** | **pg** | was vs pg | **now vs pg** |
|---|---:|---:|---:|---:|---:|
| bar-lookup (pk =) *control* | 7,714 | 7,842 | 4,538 | 1.66× | **1.73×** |
| point-join (3-rel, pk) *control* | 6,897 | 6,910 | 2,688 | 2.52× | **2.57×** |
| **bar-range (pk BETWEEN)** | 83 | **2,183** | 381 | 0.20× | **5.73×** |
| symbol-history (FilterScan) | 24 | 34 | 9 | 2.88× | **3.94×** |
| model-join (FilterScan+Probe) | 688 | 820 | 278 | 2.21× | **2.95×** |
| **day-slice (FilterScan)** | 35 | **78** | 140 | 0.27× | **0.56×** |
| **cross-join (FilterScan+2 Probe)** | 34 | **79** | 130 | 0.26× | **0.61×** |

The controls landed within 1-2% of their pre-change values across a rebuild,
a different filesystem and a separate day, which is the reproducibility this
table rests on.

Three things this says.

**The pk path is ckdbs's.** A clustered-btree descent plus a tagged-cell
decode beats PostgreSQL's index scan plus heap fetch by 1.73×, and the
advantage grows to 2.57× on the three-relation point join where two more
probes compound it.

**The full-relation scan is still PostgreSQL's, now by 1.7× rather than
3.8×.** The investigation below found the reason - ckdbs built a full row
object for every one of the 60,472 rows it rejected - and lazy decode closed
about half the gap: `day-slice` 35 -> 78 and `cross-join` 34 -> 79. What is
left is no longer decode; it has not been attributed. Note this is the one
place where the two full-relation shapes and the two *selective* FilterScans
part company: `symbol-history` and `model-join` are also unindexed walks and
ckdbs wins both by 3-4×, so "PostgreSQL scans faster" is too coarse a
reading. What PostgreSQL is faster at here is specifically the 60,480-row
walk that discards 60,472 rows.

**`bar-range` was the one result that named a known gap, and the gap is
closed.** A pk `BETWEEN` compiled to a `Range` step that pruned the *tail* -
the first page whose `min_key` passes the high bound ends the walk - and
never the head, so a range 200 wide cost everything before it. It now
descends to its low bound (`btree::BtreeSeekLeaf`) and walks siblings from
there: **83 -> 2,183 QPS, from 5× behind PostgreSQL to 5.7× ahead** - the
largest single movement in this document. A heap relation still starts at
the head, because it has no index to descend and finding the low bound *is*
the walk.

### The second run, 2026-08-06 at `983d72d`

A full independent re-run on a Release build, which is where the `agg-*`
section below comes from. Kept beside the table above rather than replacing
it, because those "was" columns are tied to the run that produced them and a
substitution would strand them.

| shape | run A ckdbs | run B ckdbs | run A pg | run B pg | run B vs pg |
|---|---:|---:|---:|---:|---:|
| bar-lookup | 7,842 | 7,656 | 4,538 | 4,447 | 1.72× |
| point-join | 6,910 | 6,711 | 2,688 | 2,046 | 3.28× |
| bar-range | 2,183 | 2,199 | 381 | 391 | 5.63× |
| symbol-history | 34 | 36 | 9 | 9 | 4.04× |
| model-join | 820 | 890 | 278 | 220 | 4.04× |
| day-slice | 78 | 82 | 140 | 138 | 0.59× |
| cross-join | 79 | 82 | 130 | 127 | 0.65× |

**Every ckdbs figure reproduced within 3%.** The PostgreSQL side is the
noisier of the two — `point-join` moved 24% and `model-join` 21% between
runs, both downward — so a ckdbs/pg ratio quoted to three significant
figures is over-reading this data. The direction and the order of magnitude
are what reproduce.

The Cabin gains reproduce too: day-slice **83.22×** against PostgreSQL's
index at 20.94×, cross-join **64.83×** against 13.04×, and `symbol-history`
still **0.59×** — a Cabin on a column with 8 values over 60,480 rows costs
more than it saves, which is the row this table exists to keep honest.

The write sweep reproduced almost exactly: 683 against 707 rows/s at
autocommit (0.97×), reaching **1.75×** at 1,000 rows per transaction, with
each engine's gain against its own autocommit rate at 13.60× and 7.51×.

## Why ckdbs lost those three — the investigation that produced the fixes

Kept because it is the reasoning behind `a1559e3` and `43d05aa`, and because
the measurements in it are what the tables above should be read against.
**The numbers in this section are pre-fix**: `bar-range` is now 5.7× ahead
rather than 5× behind, and `day-slice`/`cross-join` have roughly halved
their gap. The three losing shapes had **two** root causes, and neither was
"PostgreSQL scans faster".

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

### What each took — both are now done

- **Lazy decode** (`a1559e3`): decode the columns the residual references,
  evaluate, and materialise the rest only on a match. `Step::residual`
  already recorded which columns those are. Predicted roughly a 12× cut in
  decode work at 8/60,480 selectivity; **delivered 2.2× end to end**
  (`day-slice` 35 → 78), which says decode was 75% of the scan and the
  remaining 25% is now the ceiling.
- **A range seek** (`43d05aa`): descend to the low bound, then walk siblings
  until the high bound. The primitive existed and `kRange` never called it;
  it now calls `btree::BtreeSeekLeaf`. **26× end to end** (83 → 2,183),
  which is what removing "walk half the relation first" is worth.

Neither was a throughput ceiling. Both were design choices, and the
measurements above were their price.

## The accelerator each engine offers for a non-pk equality

This is the comparison the tools were built for. Both accelerators are
created at runtime on an already-loaded relation, measured, dropped, and
measured again — `CREATE CABIN`/`DROP CABIN` against `CREATE INDEX`/`DROP
INDEX`. Each gain is measured entirely within one engine, so it carries
nothing about the client, the machine, or the wire.

| shape | ckdbs warm | ckdbs **cabin** | gain | pg warm | pg **index** | gain |
|---|---:|---:|---:|---:|---:|---:|
| day-slice | 78 | **6,668** | **85.60×** | 140 | 2,805 | 20.08× |
| cross-join | 79 | **5,497** | **69.58×** | 130 | 1,316 | 10.16× |
| model-join | 820 | 1,125 | 1.37× | 278 | 268 | 0.97× |
| symbol-history | 34 | 21 | **0.62×** | 9 | 9 | 1.03× |

**The gains fell and the Cabin did not get worse** - 184× became 86×
because the *baseline* more than doubled, while the served number barely
moved (6,407 -> 6,668). A ratio against a moving denominator is the thing
to read carefully here: what a Cabin is worth in absolute QPS is unchanged,
and what it is worth *relative to walking* halves the moment walking gets
cheaper.
Both are still far above PostgreSQL's indexed answer on the same shape.

**The Cabin turns ckdbs's worst two shapes into its best.** `day-slice` goes
from 1.8× behind PostgreSQL to 2.4× ahead of it; `cross-join` from 1.6×
behind to 4.2× ahead. That is the feature working exactly as
`docs/feat-cabin.md` §1 specifies — an observed value's entry set is served
without opening the relation — on a workload whose filter column repeats.

**Two rows where nothing helps, and one of them now actively hurts.**
`symbol-history` has 8 distinct values each matching 7,560 rows: the entry
set is as large as the scan it replaces. It used to cost what walking cost
(0.97×); now that walking is cheaper it costs **more** - 34 QPS walking
against 21 served, **0.62×**. Resolving 7,560 entries one pk at a time was
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

Both data directories on the same xfs volume. The previous version of this
table had the ckdbs file on tmpfs and is withdrawn; its autocommit figure of
3,324 rows/s was measuring a page cache, not a durable write.

| rows/txn | ckdbs | pg | ckdbs/pg | ckdbs gain | pg gain |
|---|---:|---:|---:|---:|---:|
| 1 (autocommit) | 684 | 717 | **0.95×** | 1.00× | 1.00× |
| 10 | 3,621 | 3,144 | 1.15× | 5.29× | 4.38× |
| 100 | 7,998 | 5,012 | 1.60× | 11.69× | 6.99× |
| 1,000 | 9,186 | 5,259 | **1.75×** | **13.42×** | 7.33× |

**On equal media the two engines are the same speed at autocommit** - 684
against 717 rows/s, a 5% difference that is inside this harness's noise.
Both are paying one `fsync` per row on the same device, and that `fsync`
dominates everything either engine does around it.

ckdbs's advantage is entirely in how well it amortises: **13.42× from
batching against PostgreSQL's 7.33×**, which is what a WAL with a
group-commit durability class buys over one that fully honours
`synchronous_commit = on` per commit. That reaches 1.75× in absolute rows
per second at 1,000 rows a transaction. Neither cluster was tuned, on the
principle that a baseline tuned by hand is not a baseline - so this is a
comparison of two defaults, and PostgreSQL's default is the more
conservative one.

## Concurrency — the weakest row, and not a defect

| connections | ckdbs was | ckdbs now | pg | ckdbs scale | pg scale |
|---|---:|---:|---:|---:|---:|
| 1 | 35 | 80 | 130 | 1.00× | 1.00× |
| 2 | 35 | 80 | 191 | 0.99× | 1.48× |
| 4 | 35 | 80 | 197 | 1.00× | 1.52× |
| 8 | 35 | 79 | 190 | 0.99× | 1.46× |

The absolute figures more than doubled with lazy decode - this sweep runs
the `cross-join` shape - and the shape of the line did not change at all:
ckdbs is flat to within 1% across an 8× change in connection count. That
invariance is the point of the paragraph below.

ckdbs is flat because it dispatches one core's statements on one thread, so
more connections buy round-trip overlap and this workload has none to
recover. PostgreSQL runs a backend process per connection and scales 1.5×
before saturating. This is an architectural difference reported correctly,
not a measurement error — and it is the one line of the report where the two
numbers mean structurally different things. The cross-core pipeline
(`docs/crosscore.md` P4) is what would move the ckdbs column.

## Load

Batched 200 rows per transaction on both sides, both on xfs.

| phase | ckdbs | pg | ckdbs/pg |
|---|---:|---:|---:|
| load-bars (60,480 rows) | 8,818/s | 4,376/s | 2.01× |
| load-stats (60,480 rows) | 7,207/s | 4,407/s | 1.64× |
| load-sessions (7,560 rows) | 9,197/s | 4,919/s | 1.87× |

**The unbatched phases correct a claim the previous version made.** On tmpfs
`load-exchanges`, `load-symbols`, `load-models` and `result-insert` showed
ckdbs 6-9× ahead, and that document said to read the write sweep instead of
them. It was right to, and the size of the error is now visible: on xfs the
same four phases read **0.39×, 1.08×, 0.94× and 0.85×** - ckdbs at parity or
slightly behind. They are unbatched autocommit statements, so they measure
one `fsync` per row, which is the write sweep's first row at a smaller
sample. The batched phases above barely moved, because one sync per 200 rows
is not what the medium decides.

## Aggregation — the `agg-*` phases

Added 2026-08-06, when `docs/feat-aggregate.md` resolved `docs/parser-v2.md`
I14 and `GROUP BY` became expressible. Both tools run the identical
statements; `bench/results-aggregate.md` measures the fold on its own.

This is the **least even table in the report, and the most interesting**.
PostgreSQL plans these: it picks a HashAggregate or a GroupAggregate, may
read the grouping column from an index, and may parallelise the scan beneath
it. ckdbs makes no plan choice at all — it walks the relation and folds
outside the executor, which is what keeps the compiled chain identical to
the same statement without a `GROUP BY` (AG1).

| phase | groups | ckdbs p50 | pg p50 | ckdbs/pg |
|---|---:|---:|---:|---:|
| `agg-global` (`COUNT/MIN/MAX/SUM`) | 1 | 31.6 ms | 11.5 ms | 0.36× |
| `agg-distinct` (`COUNT(DISTINCT)`) | 1 | 32.6 ms | 11.9 ms | 0.37× |
| `agg-by-symbol` | 8 | 34.7 ms | 18.7 ms | 0.54× |
| **`agg-by-session`** | **7,560** | **46.2 ms** | **63.7 ms** | **1.37×** |
| `agg-day-slice` (FilterScan + fold) | 8 | 12.2 ms | 7.5 ms | 0.62× |

**The result the tool's own comments predicted backwards.** Those comments
said "nothing here is expected to be close on the high-cardinality shape",
reasoning that a planned HashAggregate must beat an unplanned walk by more
as the work grows. The opposite happened, and the two engines' own curves say
why:

| groups | ckdbs p50 | pg p50 |
|---:|---:|---:|
| 1 | 31.6 ms | 11.5 ms |
| 8 | 34.7 ms (+9.8%) | 18.7 ms (+63%) |
| 7,560 | 46.2 ms (+46%) | 63.7 ms (**+454%**) |

Over the same 60,480 rows, **PostgreSQL's aggregate degrades ~10× harder
with group count than this fold does.** ckdbs starts 2.7× behind on the
global form — that gap is the scan, not the fold, and the rest of this
document is about the scan — and closes it entirely by 7,560 groups.

Two things worth stating about what this is and is not. It is **not** a
claim that the fold is better engineered than a HashAggregate; it is a
first-seen-ordered vector plus a heterogeneous map probe, and it does less.
And it is **not** a plan-quality result: ckdbs has no plan to get wrong here,
which is the only reason its curve is flat.

What it does license is narrower and still useful: the AG1 placement — a fold
outside the executor, no operator in the chain, no step-kind table to teach —
does not cost anything that shows up as group count rises. `agg-day-slice`
against `read-day-slice` (12.23 ms against 12.12 ms, +0.9%) says the same
from the other side: where the scan dominates, the fold is free on both
engines and the ratio is just the scan's.

## What this does not measure

- **Recovery.** ckdbs has none (`docs/wal.md`); PostgreSQL's is being paid
  for in every write number above. This is the largest unpriced difference
  in the report.
- **Aggregates inside the model comparison.** That phase is still a plain
  join reduced client-side on both sides, and deliberately: its job is to
  price the join, and rewriting one engine's statement would make the two
  tools time different questions. Written idiomatically, PostgreSQL would
  return 8 rows instead of 2,872 and the `compare-all` row would invert.
  Aggregates are now measured **as their own phases** — see the section
  below, added 2026-08-06 when `docs/feat-aggregate.md` resolved I14.
- **Concurrent readers and writers.** Every phase here is single-connection
  apart from the concurrency sweep. `tools/scenario0_stockmarket.py` and its
  PostgreSQL twin are where contention is measured.
- **Anything past 8 symbols or one core.** `cores = 1` throughout.

## Reproducing

```
# Build first, and build **Release**: a stale binary is the easiest way to
# measure the wrong engine, and CMakeLists defaults CMAKE_BUILD_TYPE to Debug,
# which is the second easiest. Check it rather than assume it:
grep CMAKE_BUILD_TYPE build-release/CMakeCache.txt   # must say Release
cmake --build build-release -j

# NOT /tmp - that is tmpfs on this host, where fsync is free and every write
# number becomes a measurement of the page cache. Put the data file on the
# same volume as the PostgreSQL cluster's data directory, or the write sweep
# compares two media rather than two engines.
./build-release/kds_server ~/scn1.db --port 15740 --log-dir ~ --log-file scn1.log
./tools/pg_setup.sh init          # scratch cluster on 15433, untuned

# sequentially — running both at once makes each measure the other:
python3 tools/scenario1_backtest.py    --port 15740 --years 30 --symbols 8 --seed 1 --json kds.json
python3 tools/pg_scenario1_backtest.py --port 15433 --years 30 --symbols 8 --seed 1 --json pg.json
python3 tools/compare_scenario1.py kds.json pg.json
```

The comparison refuses to print if the two runs did not run the same
workload, and says which parameter or which model's P&L disagreed.
