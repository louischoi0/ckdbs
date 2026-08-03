# Access kinds and access statistics, measured

Measured 2026-08-03, on the build that added `kFilterScan`, made `kRange`
emitted, and landed `sys.access_stats`.

Two questions: what does collecting the statistics cost, and does the range
walk's `min_key` pruning actually save anything.

---

## Method

Single connection over the newline protocol, per-request wall clock around
one send + one recv, so every number carries Python's own socket cost
(~200 µs floor — `tools/bench_common.py`). Read the *differences*. 2000 rows,
one hot statement repeated 2000 times after a 20-execution warm-up, p50.

Two servers from the same binary, differing only in `access_statistics`.
Waystone recording and replay are **on for both**, so the point-lookup rows
below are replay-served on either side and the delta is the statistics alone.

The harness asserts that `CREATE TABLE` returns `CREATED` on both servers —
after three separate measurements in this project were quietly taken against
a stale server with accumulated rows, that check is worth more than it costs.

## What the statistics cost

| phase | stats off | stats on | change |
|---|---|---|---|
| btree point lookup | 236.7 µs | 239.6 µs | **+1.2%** |
| heap point lookup | 236.1 µs | 240.6 µs | **+1.9%** |
| btree range (`BETWEEN 5 AND 15`) | 1059.6 µs | 1062.2 µs | +0.2% |
| btree filter scan (`v = 1500`) | 7780.7 µs | 7770.1 µs | −0.1% |

One catalog row update per step per successful statement, written through
rather than batched. At 1–2% on the fastest thing the engine does, and
nothing measurable on anything slower, **the write-through design stands**.

The plan had a fallback ready — accumulate in memory, flush on a cadence —
and it is not needed. Worth recording that it was considered and rejected on
a number, because the in-memory version costs a flush mechanism, a
partial-loss story on crash, and an approximate `last_seen`, and none of that
buys anything at this price.

## What the range pruning saves

From the same run, on the same 2000-row relation:

| access | p50 | reads |
|---|---|---|
| `WHERE id BETWEEN 5 AND 15` (`kRange`) | 1059.6 µs | stops after the range |
| `WHERE v = 1500` (`kFilterScan`) | 7780.7 µs | every row |

**~7.3× on a range near the start of the relation.** Both walk the same
storage; the difference is entirely `VisitControl::kStop` firing at the first
page whose `min_key` passes the high bound.

**This is the favourable case and the number should be read as such.**
Pruning is a *tail* optimization: a range at the end of a relation reads
everything before it and saves nothing, which `range_scan_test.cpp` pins
directly rather than leaving to be discovered. Skipping leading pages needs a
seek to the first qualifying leaf — `BtreeLookup` descends to a key and
leaves are sibling-linked, so the primitive is close, but it is not built.

## What is not measured

Whether any of this is *useful*. `sys.access_stats` is the input to a
physical optimizer that does not exist (`docs/heap-and-tuple.md` §7), and
nothing reads it but `SHOW ACCESS`. The cost above is what it takes to have a
history when something arrives to consume one; the benefit is unmeasurable
until then, and claiming otherwise would be inventing a number.
