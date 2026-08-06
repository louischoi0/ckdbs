# What a fold costs

Measurements for `docs/feat-aggregate.md` (workplan AG10). Driver:
`tools/aggregate_benchmark.py`, plus the `agg-*` phases of
`tools/scenario1_backtest.py`.

## Device, build and configuration

**Two things this file gets wrong if you skip them.**

**A block device, not tmpfs.** `bench/results-cabin.md` records what happens
otherwise: with fsync free the scan becomes the bottleneck and anything
touching a scan measures as a much larger win than it is.

**A Release build, not the CMake default.** `CMakeLists.txt` defaults
`CMAKE_BUILD_TYPE` to **Debug** — unoptimized, with debug assertions live —
so `cmake -S . -B build` produces a binary that is roughly 14× slower on a
scan than `-O3 -DNDEBUG`. The first version of this document was measured
that way, and it was wrong in **both directions**: it reported +7.9% where
the release build reports −3.7%, and +0.4% where the release build reports
+30.1%. A debug build does not merely scale everything down; it hides fixed
costs inside inflated variable ones, which is exactly the distinction a fold
is about. Every number below is `build-release`.

| | |
|---|---|
| device | `/dev/nvme0n1p1` — Amazon EBS gp3, non-rotational, 8 GB |
| build | **`-DCMAKE_BUILD_TYPE=Release`** (`-O3 -DNDEBUG`), gcc 11.5 |
| kernel | 6.18.38-73.137.amzn2023.x86_64 |
| cores | 2 (server `cores = 1`) |
| durability | `group` (default) |
| client | one connection, 30 reps/statement |

Latencies include the Python client's own socket cost, which on a release
build is now a **large** share of the small statements — a pk lookup is
~100 µs end to end. Treat differences under ~20 µs on those rows as noise.

## What is being measured

A *difference*, not a throughput. AG1 places the fold outside the executor
and compiles the same chain either way, so the only question the numbers can
answer is what folding a row costs on top of producing it. Every statement
runs against its **unaggregated twin** — same chain, same rows, same access
kind.

## Grouped scan — 20,000 rows, heap relation

| statement | mean | p50 | rows back | vs plain |
|---|---:|---:|---:|---:|
| `SELECT bucket, qty FROM aggscan2` | 11,108 µs | 10,748 µs | 20,000 | — |
| … `COUNT(*), SUM(qty) … GROUP BY bucket` (2 groups) | 10,695 µs | 9,226 µs | 2 | **−3.7%** |
| `SELECT bucket, qty FROM aggscan64` | 10,983 µs | 10,602 µs | 20,000 | — |
| … `GROUP BY bucket` (64 groups) | 10,640 µs | 9,792 µs | 64 | **−3.1%** |
| `SELECT bucket, qty FROM aggscan1024` | 11,173 µs | 10,785 µs | 20,000 | — |
| … `GROUP BY bucket` (1,024 groups) | 11,811 µs | 11,175 µs | 1,024 | **+5.7%** |

At 2 and 64 groups the fold is *faster* than its twin: it walks the same
20,000 rows and serialises a handful instead of all of them, and that saving
exceeds what the map costs. Only at 1,024 groups does the fold turn positive.

## The global form — the same walk with no map

| statement | mean | p50 | rows back | vs plain |
|---|---:|---:|---:|---:|
| `SELECT qty FROM aggscan1024` | 9,927 µs | 8,980 µs | 20,000 | — |
| `SELECT COUNT(*), SUM(qty) FROM aggscan1024` | 7,834 µs | 7,736 µs | 1 | **−21.1%** |

The no-GROUP-BY path encodes no key, computes no hash and probes no map
(spec §5). It is the cheapest way to read a whole relation this engine has.

## DISTINCT

| statement | mean | p50 | vs plain |
|---|---:|---:|---:|
| `SELECT COUNT(sym) FROM aggscan1024` | 7,704 µs | 7,633 µs | — |
| `SELECT COUNT(DISTINCT sym) FROM aggscan1024` | 9,814 µs | 9,373 µs | **+27.4%** |

16 distinct values over 20,000 rows, so all but 16 probes are hits — which
allocate nothing. The +27% is the per-row encode and set probe, not the
inserts, and it is the largest per-row cost in this feature.

## Grouped probe — btree, keyed chain

| statement | mean | p50 | rows back | vs plain |
|---|---:|---:|---:|---:|
| `SELECT qty FROM aggprobe WHERE id = 42` | 102.7 µs | 83.1 µs | 1 | — |
| `SELECT COUNT(*) FROM aggprobe WHERE id = 42` | 133.7 µs | 133.5 µs | 1 | **+30.1%** |
| `SELECT bucket, qty … BETWEEN 100 AND 200` | 193.9 µs | 184.6 µs | 101 | — |
| `SELECT bucket, COUNT(*) … GROUP BY bucket` | 249.9 µs | 236.0 µs | 64 | **+28.9%** |

**The point lookup is where the debug build lied worst.** It measured +0.4%
there and the release build measures +30.1% — not because the fold got
slower, but because the statement got 3× faster and the fold's *fixed* cost
(building the aggregator, founding one group, walking it at Finish) stopped
being hidden. In absolute terms it is ~31 µs on a statement that is ~103 µs.

## The finding: cost tracks *group count*, not row count

Same relation, same predicate, same 101 input rows, 100 reps — only the
number of groups moves:

| statement | groups | mean | p50 | vs plain |
|---|---:|---:|---:|---:|
| `SELECT bucket, qty … BETWEEN 100 AND 200` (plain) | — | 200.1 µs | 187.3 µs | — |
| `SELECT COUNT(*) …` | 1 | 185.1 µs | 172.4 µs | **−7.5%** |
| `SELECT sym, COUNT(*) … GROUP BY sym` | 16 | 206.3 µs | 200.1 µs | +3.1% |
| `SELECT bucket, COUNT(*) … GROUP BY bucket` | 64 | 246.6 µs | 243.1 µs | **+23.2%** |

101 rows throughout. The fold is **free at one group and pays per group
founded** — about **1 µs each** here, where the debug build's noise put it at
6–10 µs. So the +28.9% above is 64 foundings amortised over 101 rows, the
shape where a fold buys least: nearly unique groups, almost no aggregation
happening.

## Confirmation at 30× the size, on a real schema

`tools/scenario1_backtest.py` runs the same shapes as `agg-*` phases against
the backtest schema — 60,480 daily bars over 30 years, nine columns,
ingested through the ordinary write path. Same relation, same 60,480 rows,
only the group count moving (p50):

| phase | groups | p50 | vs 1 group |
|---|---:|---:|---:|
| `agg-global` | 1 | 31.6 ms | — |
| `agg-distinct` | 1, with a set | 32.6 ms | +3.2% |
| `agg-by-symbol` | 8 | 34.7 ms | **+9.8%** |
| `agg-by-session` | 7,560 | 46.2 ms | **+46.5%** |

7,560 groups is one per trading day — the high-cardinality end of what this
schema produces, and it costs 46%.

**And the finding only a real workload could produce**: on a scan-dominated
statement the fold is free.

| phase | p50 |
|---|---:|
| `read-day-slice` (FilterScan, no fold) | 12.12 ms |
| `agg-day-slice` (the same statement, folded) | 12.23 ms |

Identical relation, identical predicate, 8 rows out of 60,480 either way:
**+0.9%**, inside the noise of a statement that spends 12 ms finding 8 rows.

Read together with the point lookup's +30%, the two say the same thing from
opposite ends: **a fold costs group foundings and a small fixed setup, and
whether that is visible depends entirely on what the chain underneath it
costs.** A statement cheap enough for the fold to matter is a statement that
was already fast.

## Against PostgreSQL

`bench/results-scenario1-vs-pg.md` compares the `agg-*` phases against
PostgreSQL 17.10 on the same data. The short version, because it is not what
this file expected: ckdbs loses the low-cardinality folds by 2-3× and **wins
the high-cardinality one at 1.37×**, because PostgreSQL's HashAggregate
degrades with group count faster than this fold does.

## Where the time actually goes

Measured on the Release build, same 20,000-row walk, one connection. The
fold reads **no column** in any of these three:

| statement | mean | relation |
|---|---:|---|
| `SELECT COUNT(*) FROM narrow` | 4,474 µs | 2 columns |
| `SELECT COUNT(*) FROM wide` | 11,990 µs | 12 columns |
| `SELECT COUNT(*) FROM wide WHERE a = 1` | **3,898 µs** | 12 columns |

Two facts fall out. Width costs **2.7×** on a statement that reads no
column. And adding a predicate makes `COUNT(*)` **3.1× faster** than not
having one, while doing strictly more logical work.

`Step::filter_columns` is computed from the residual alone, and
`AcceptTupleAt` decodes that mask, tests the residual, then decodes
`~filter_columns` for every *surviving* row. An unfiltered `COUNT(*)` has an
empty residual, so every row survives and every row builds an `AstValue` for
all 12 columns to fold none of them. A predicate rejects 19,999 rows before
that pass and is therefore cheaper than no predicate at all.

**This is not aggregation's defect** — `SELECT a FROM wide` decodes 12
columns to emit 1 — but a fold is the consumer that makes it visible,
because it is the only one that can read zero columns. `docs/workplan-aggregate-perf.md`
`AP01` is the fix.

### AP01, measured

`Step::read_columns` — the columns any consumer of the row touches, as
opposed to the ones the filter needs — now bounds the post-residual decode.
Same relations, same statements, same Release build:

| statement | before | after | |
|---|---:|---:|---:|
| `SELECT COUNT(*) FROM narrow` (2 cols) | 4,474 µs | 3,259 µs | 1.37× |
| `SELECT COUNT(*) FROM wide` (12 cols) | 11,990 µs | **3,732 µs** | **3.21×** |
| `SELECT COUNT(*) FROM wide WHERE a = 1` | 3,898 µs | 3,764 µs | 1.04× |
| `SELECT SUM(a) FROM wide` | 11,370 µs | **5,691 µs** | **2.00×** |
| `SELECT a FROM wide` (1 of 12) | 13,022 µs | **7,672 µs** | **1.70×** |
| `SELECT * FROM wide` | 32,009 µs | 35,304 µs | 0.91× |

Three things to read out of it.

**The anomaly is gone.** `COUNT(*)` without a WHERE was 3.1× *slower* than
the same statement with one; it is now marginally faster, which is the only
defensible ordering — a predicate cannot make a scan cheaper than not having
one.

**Width almost stopped mattering** for a statement that reads no column:
`wide` was 2.7× `narrow` and is now 1.15×, and what remains is the wider
relation's pages, not its columns.

**A plain projection gained more than the fold did in absolute terms.**
`SELECT a FROM wide` is 1.70× faster, which is AP01 confirming it was never
an aggregation fix — the fold was only the consumer that made the waste
visible, being the one that can read zero columns.

`SELECT *` moved 9% the wrong way and is noise: its mask is `kAllColumns`
either way, so it executes the identical code path. It is reported rather
than dropped because a reader checking the table would otherwise wonder.

## AG11's defaults

**`aggregate_max_groups = 65,536` — ratified `[CONFIRMED]`.** Two reasons,
one measured and one arithmetic. Measured: cost is proportional to group
count, so a statement approaching this cap is already slow enough to be
visible to whoever ran it — the cap is a backstop, not a tuning knob.
Arithmetic: a group with one key and two items costs roughly 416 bytes
across its key vector, its state vector, its index node and its key string,
so the ceiling is about **27 MB per statement**.

**`aggregate_max_distinct = 1,048,576` — not ratified, stays
`[PROPOSED]`.** The same arithmetic puts its ceiling at roughly **84 MB per
statement** (about 80 bytes per entry: a set node plus a short-string
encoding), three times the group ceiling and the largest single allocation
any statement in this engine can ask for. Nothing measured here argues for a
number, because no scenario needs a million distinct values — and picking one
without data is exactly what the `[PROPOSED]` marker exists to prevent. What
would settle it is a workload with a genuinely high-cardinality
`COUNT(DISTINCT)`, measured for resident memory rather than latency.

## Reproducing

```
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && make -C build-release -j8
build-release/kds_server --config <conf with data_file on a block device>
python3 tools/aggregate_benchmark.py --port <port> --rows 20000 --reps 30
```

`--cardinalities` sweeps the group count; `--suffix` keeps repeated runs
against one data file from colliding. **Do not use `./build`** unless you
configured it Release — see the top of this file.
