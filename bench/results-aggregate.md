# What a fold costs

Measurements for `docs/feat-aggregate.md` (workplan AG10). Driver:
`tools/aggregate_benchmark.py`.

## Device and configuration

**A block device, not tmpfs.** `bench/results-cabin.md` records what
happens otherwise: with fsync free the scan becomes the bottleneck and
anything touching a scan measures as a much larger win than it is. That is
a rule now, and this run obeys it.

| | |
|---|---|
| device | `/dev/nvme0n1p1` — Amazon EBS gp3, non-rotational, 8 GB |
| data file | `/home/ec2-user/aggbench.db` on that device |
| kernel | 6.18.38-73.137.amzn2023.x86_64 |
| cores | 2 (server `cores = 1`) |
| memory | 7 GB |
| durability | `group` (default) |
| build | default CMake build, gcc 11.5 |
| client | one connection, `tools/aggregate_benchmark.py`, 30 reps/statement |

Latencies include the Python client's own socket cost. That is the floor
this harness can resolve, so **differences under ~50 µs are noise** and the
per-group constant derived below is an order of magnitude, not a figure.

## What is being measured

A *difference*, not a throughput. AG1 places the fold outside the executor
and compiles the same chain either way, so the only question the numbers
can answer is what folding a row costs on top of producing it. Every
statement is therefore run against its **unaggregated twin** — same chain,
same rows, same access kind.

## Grouped scan — 20,000 rows, heap relation

One relation per group count, so the row count does not move with the
variable.

| statement | mean | p50 | rows back | vs plain |
|---|---|---|---|---|
| `SELECT bucket, qty FROM aggscan2` | 155.5 ms | 155.1 ms | 20,000 | — |
| `SELECT bucket, COUNT(*), SUM(qty) … GROUP BY bucket` (2 groups) | 167.8 ms | 165.5 ms | 2 | **+7.9%** |
| `SELECT bucket, qty FROM aggscan64` | 156.9 ms | 154.8 ms | 20,000 | — |
| … `GROUP BY bucket` (64 groups) | 171.0 ms | 169.8 ms | 64 | **+9.0%** |
| `SELECT bucket, qty FROM aggscan1024` | 155.5 ms | 154.8 ms | 20,000 | — |
| … `GROUP BY bucket` (1024 groups) | 180.3 ms | 178.3 ms | 1,024 | **+16.0%** |

## The global form — the same walk with no map

| statement | mean | p50 | rows back | vs plain |
|---|---|---|---|---|
| `SELECT qty FROM aggscan1024` | 153.3 ms | 150.8 ms | 20,000 | — |
| `SELECT COUNT(*), SUM(qty) FROM aggscan1024` | 148.5 ms | 146.7 ms | 1 | **−3.2%** |

The no-GROUP-BY path encodes no key, computes no hash and probes no map
(spec §5), and it is the only configuration here that is *faster* than its
twin — it folds 20,000 rows and serialises one.

## DISTINCT

| statement | mean | p50 | vs plain |
|---|---|---|---|
| `SELECT COUNT(sym) FROM aggscan1024` | 145.9 ms | 145.1 ms | — |
| `SELECT COUNT(DISTINCT sym) FROM aggscan1024` | 166.4 ms | 164.4 ms | **+14.1%** |

16 distinct values over 20,000 rows, so all but 16 probes are hits — which
allocate nothing. The +14% is the per-row encode and set probe, not the
inserts.

## Grouped probe — btree, keyed chain

| statement | mean | p50 | rows back | vs plain |
|---|---|---|---|---|
| `SELECT qty FROM aggprobe WHERE id = 42` | 299.5 µs | 277.8 µs | 1 | — |
| `SELECT COUNT(*) FROM aggprobe WHERE id = 42` | 300.7 µs | 287.3 µs | 1 | **+0.4%** |
| `SELECT bucket, qty … WHERE id BETWEEN 100 AND 200` | 1,274 µs | 1,259 µs | 101 | — |
| `SELECT bucket, COUNT(*) … GROUP BY bucket` | 1,836 µs | 1,811 µs | 64 | **+44.1%** |

A point lookup is unchanged at noise level, which is AG1 behaving as
specified: one row in, one row out, one state folded.

The +44% is the headline number and it needed a second measurement to
explain, because a 44% regression on a keyed chain would be a design
problem if it were about rows.

## The finding: cost tracks *group count*, not row count

Same relation, same predicate, same 101 input rows, 60 reps — only the
number of groups moves:

| statement | groups | mean | p50 | vs plain |
|---|---|---|---|---|
| `SELECT bucket, qty … BETWEEN 100 AND 200` (plain) | — | 1,349 µs | 1,266 µs | — |
| `SELECT COUNT(*) …` | 1 | 1,247 µs | 1,209 µs | **−7.6%** |
| `SELECT sym, COUNT(*) … GROUP BY sym` | 16 | 1,565 µs | 1,463 µs | +16.0% |
| `SELECT bucket, COUNT(*) … GROUP BY bucket` | 64 | 1,848 µs | 1,813 µs | +37.0% |

101 rows throughout. The fold is **free at one group and pays per group
founded** — roughly 6–10 µs each at this scale, which this harness cannot
resolve more precisely than that. So the +44% above is not a per-row cost
that would scale with the relation; it is 64 group foundings amortised over
only 101 rows, which is the shape where a fold buys the least: nearly
unique groups, so almost no aggregation is happening.

The same effect explains the scan sweep. Going from 2 to 1,024 groups over
a fixed 20,000 rows costs +12.5 ms, which is both the foundings and the
worse probe locality of a larger map — the per-row arithmetic did not
change.

Nothing here is a surprise about the *fold*: it hashes a key, probes a map
and folds a state in place, with zero allocations for a row landing in an
existing group (pinned by `tests/aggregate_test.cpp`). What the numbers add
is which of those dominates, and it is the map.

## AG11's defaults

The workplan asks this file to ratify or amend them.

**`aggregate_max_groups = 65,536` — ratified `[CONFIRMED]`.** Two reasons,
one measured and one arithmetic. Measured: cost is proportional to group
count, so a statement approaching this cap is already slow enough to be
visible to whoever ran it — the cap is a backstop, not a tuning knob.
Arithmetic: a group with one key and two items costs roughly 416 bytes
across its key vector, its state vector, its index node and its key string,
so the ceiling is about **27 MB per statement**. That is a defensible
worst case for one statement on an OLTP engine.

**`aggregate_max_distinct = 1,048,576` — not ratified, stays
`[PROPOSED]`.** The same arithmetic puts its ceiling at roughly **84 MB per
statement** (about 80 bytes per entry: a set node plus a short-string
encoding), which is three times the group ceiling and the largest single
allocation any statement in this engine can ask for. Nothing measured here
argues for a number, because no scenario in this bench needs a million
distinct values — and picking one without data is exactly what the
`[PROPOSED]` marker exists to prevent. What would settle it is a workload
with a genuinely high-cardinality `COUNT(DISTINCT)`, measured for resident
memory rather than latency. Until then the default stands and the exposure
is written down here.

## Reproducing

```
kds_server --config <conf with data_file on a block device>
python3 tools/aggregate_benchmark.py --port <port> --rows 20000 --reps 30
```

`--cardinalities` sweeps the group count; `--suffix` keeps repeated runs
against one data file from colliding.
