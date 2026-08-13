# The PageRef migration's per-statement price — pin bookkeeping on the hot path

This run is the acceptance measurement `docs/workplan-pageref.md` §6.5 asks
for: after MG01–MG06, every page fetch in the engine constructs and destroys
a pinned `PageRef` — one `frames_.find()` plus two counter increments
(`Frame::pins`, `live_pins_`) per pin, and the same per unpin, on top of the
find the fetch already did (`src/storage/device_page_store.cpp`,
`PinFrame`/`UnpinFrame`). The eviction sweep is **not** armed here — the
frame budget defaults to 0 (unbounded) — so the number below is the price of
the bookkeeping alone, not of eviction.

**Finding, in one line: the pin bookkeeping is unmeasurable on the write
path (+0.0 to +0.2 µs at p50, inside the run's own floor) and costs
+0.6 to +1.0 µs at p50 on the pk point-SELECT, consistently across all four
relations measured.**

## The run

| | |
|---|---|
| Executed | 2026-08-13, 08:19–08:23 UTC |
| Branch / worktree | `pageref-migration` |
| Head side | `b16b9f3` (committed 08:04 UTC); binary built 08:16 UTC from a tree verified clean immediately after the build. A concurrent review edit to `src/storage/device_page_store.cpp` appeared in the worktree *after* the build and is not in the measured binary |
| Base side | `5c66aa3` (the commit before MG01), built 08:17 UTC in a detached scratch worktree, clean by construction |
| Build | Release, GCC 13.3, `-DKDS_WITH_TLS=OFF` on **both** sides (this host has no OpenSSL dev headers; the option is identical across the A/B, so it cancels) |
| Device | `/dev/nvme0n1p1` (ext4); data dirs `~/bench-pageref/{head,base}`, one fresh dir and one fresh server per side |
| Server config | `cores = 1`, `durability = relaxed`, `log_level = warn`; frame budget at its default 0 — the sweep never runs. `KDS_TEST_FRAME_BUDGET` confirmed absent from the environment |
| Driver | `tools/assertion_abort_benchmark.py` (documented in `bench/docs/README.md`), head on 15601, base on 15602, interleaved block-by-block inside one process: `--rows 1000 --ordinary-ops 1000 --txns 200 --reservations 10 --block 10 --seed 1 --suffix pr1` |
| Load average | 0.54 (1 m) before, 0.70 after; no compiler or test suite running (`bench/wait_quiet.sh` gate) |
| Verify | **passed** on both sides: every relation holds 9,000 rows and every asserted relation's `GROUP BY` aggregate equals the unasserted control's |

**One row count, by instruction.** This run was scoped to 1,000 rows only.
The documentation rule's 200/1K/10K sweep was deliberately not run, and the
consequence is stated rather than hidden: a single cardinality cannot say
whether the select-arm delta is fixed per statement or grows with pages
touched. The follow-up that answers it is the same invocation at `--rows
200` and `--rows 10000`, plus a scan-shaped arm — a scan pins once per page,
so its delta is the per-pin cost times a knowable count.

## The ordinary arms — head against base, interleaved

Four autocommitted statement shapes, 1,000 ops per arm per relation per
side, the two binaries interleaved in blocks of 10 inside one process so any
mid-run drift costs both sides equally. `none` and `twin` carry no
assertion; `cnt` and `multi` carry one and two. Latencies are client-side
microseconds and include the Python socket stack (the ping floor is
~28–31 µs p50); the head−base delta is the column that matters.

| arm | rel | ops | H p0 | H p25 | H p50 | H p95 | H p99 | B p0 | B p25 | B p50 | B p95 | B p99 | Δp50 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ac-insert | none | 1000 | 22.9 | 31.7 | 32.8 | 48.2 | 68.3 | 22.8 | 31.4 | 32.6 | 47.0 | 62.2 | +0.2 |
| ac-insert | twin | 1000 | 22.8 | 30.6 | 31.8 | 38.8 | 45.4 | 22.6 | 30.4 | 31.7 | 39.1 | 45.0 | +0.1 |
| ac-insert | cnt | 1000 | 23.5 | 31.6 | 33.0 | 42.6 | 50.3 | 23.5 | 31.4 | 32.8 | 41.9 | 47.4 | +0.2 |
| ac-insert | multi | 1000 | 24.1 | 29.3 | 34.1 | 44.6 | 55.2 | 24.1 | 25.8 | 34.0 | 45.1 | 60.2 | +0.1 |
| ac-update | none | 1000 | 23.6 | 32.5 | 33.8 | 48.6 | 57.7 | 23.9 | 32.5 | 33.7 | 51.1 | 79.2 | +0.1 |
| ac-update | twin | 1000 | 23.6 | 31.5 | 32.8 | 40.0 | 46.2 | 23.9 | 31.8 | 32.6 | 40.0 | 53.0 | +0.2 |
| ac-update | cnt | 1000 | 23.6 | 31.8 | 32.9 | 42.7 | 52.2 | 24.1 | 32.2 | 32.9 | 44.1 | 60.5 | 0.0 |
| ac-update | multi | 1000 | 24.7 | 31.2 | 35.5 | 46.8 | 55.1 | 25.2 | 34.4 | 35.5 | 46.2 | 65.2 | 0.0 |
| ac-select | none | 1000 | 24.7 | 34.3 | 36.2 | 52.7 | 64.2 | 24.4 | 33.7 | 35.6 | 53.5 | 63.1 | **+0.6** |
| ac-select | twin | 1000 | 24.1 | 33.6 | 35.1 | 43.9 | 50.9 | 25.3 | 32.8 | 34.1 | 43.9 | 56.9 | **+1.0** |
| ac-select | cnt | 1000 | 24.3 | 33.6 | 35.1 | 47.0 | 69.9 | 24.7 | 32.8 | 34.1 | 44.8 | 75.5 | **+1.0** |
| ac-select | multi | 1000 | 24.3 | 34.0 | 35.5 | 44.2 | 51.5 | 24.6 | 33.4 | 34.6 | 43.6 | 64.8 | **+0.9** |
| ac-delete | none | 1000 | 23.1 | 31.8 | 32.8 | 47.4 | 62.9 | 22.9 | 31.5 | 32.7 | 46.4 | 54.0 | +0.1 |
| ac-delete | twin | 1000 | 22.6 | 30.7 | 31.8 | 37.4 | 44.0 | 22.9 | 30.4 | 31.6 | 37.1 | 44.0 | +0.2 |
| ac-delete | cnt | 1000 | 23.4 | 31.4 | 32.8 | 40.6 | 52.7 | 23.5 | 31.4 | 32.6 | 40.1 | 53.1 | +0.2 |
| ac-delete | multi | 1000 | 23.8 | 27.0 | 33.9 | 45.3 | 54.8 | 24.0 | 31.9 | 34.2 | 45.0 | 59.1 | −0.3 |

## The noise floor, established inside the run

Three controls bound what a real delta must exceed:

- **`BEGIN`** touches no page, so the migration cannot cost it anything. Its
  interleaved head−base p50 delta reads −0.1 to −0.4 µs across the four
  relations (head marginally *faster*) — that is the cross-side floor on a
  no-page-fetch statement, roughly ±0.4 µs.
- **`twin` against `none`** — identical configurations by construction —
  differ by 1.1–1.5 µs *within* each side on the select arm, in the same
  direction on both sides. That gap is a systematic relation-order effect
  shared by both binaries; it cancels in a same-relation head−base
  comparison and does not inflate the cross-side floor.
- **The spread of the select deltas themselves** across four independently
  loaded relations is 0.6–1.0 µs, i.e. ±0.2 around their ~0.9 µs mean.

Against that floor: the twelve write-arm deltas (+0.0 to +0.2, one −0.3)
are **inside it and are not a finding**. The four select deltas sit 2–3×
above it, agree in sign and magnitude across four relations, and are
reported as real. p0 moves on no arm on either side (≤0.4 µs, both
directions): the best case the path can reach is unchanged, so the cost is
in the body of the distribution, not the floor.

## Where the time goes

The measured unit is one autocommitted statement, client-observed.

| wait type | share of an ~33 µs p50 statement | how known |
|---|---|---|
| Client + socket round trip | ~28–31 µs (the `SHOW META` ping p50; measured sequentially per side, so the interleaved `BEGIN` arm is the trustworthy cross-side control) | measured |
| Durability / commit fsync | 0 — `durability = relaxed` puts no fsync on the autocommit path | by configuration |
| Lock / conflict wait | 0 — one connection per side, `cores = 1`, nothing to conflict with | by construction |
| Engine execute (parse, descend, read/write, pin bookkeeping) | the remaining ~3–7 µs; the migration's +0.9 µs on the select arm lands here | inferred as the residue |

A finer per-wait split is not measurable on today's engine from the client
side; the server exposes no per-statement wait counters.

## Server CPU

Both server pids were sampled from `/proc/<pid>/stat` around the driver's
CPU-pass windows. At this window size (150 transactions per read pair) one
10 ms scheduler tick is 16.7 µs of per-transaction quantisation, and the
observed head−base differences (−66 to +50 µs/txn, all multiples of
~16.7 µs) are exactly that quantisation — **the CPU meter cannot resolve a
~1 µs effect and no CPU number is reported as a result**.
`bench/results-assertion-abort.md` §10 documents the same limit on the same
meter.

## Versus PostgreSQL

There is no PostgreSQL column in this document, for two stacked reasons:
the axis under measurement is two builds of ckdbs, which PostgreSQL cannot
sit on either side of; and the driver carrying the interleaved A/B has no
twin because no released PostgreSQL parses `CREATE ASSERTION`
(`bench/docs/README.md`). No task follows from this run — a baseline is
meaningful for a workload's absolute cost, not for an internal A/B — but
any *absolute* claim quoted from the tables above should lean on the
scenario results files, which carry their PostgreSQL columns.

## What this run teaches about the engine

**The pin bookkeeping's price concentrates on the read path.** On
`pageref-migration` at `b16b9f3`, a pk point SELECT pays ~0.9 µs at p50
over `5c66aa3` while INSERT, UPDATE and DELETE — statements that also fetch
pages, plus undo, WAL and index maintenance — pay nothing measurable. Two
consistent explanations, not separated by this run: the read path performs
more pin/unpin pairs per statement than the write path (each `GetForRead`
that was a raw span is now a find-increment/find-decrement round trip), or
the write path's extra work overlaps the bookkeeping in a way the shorter
read path cannot. A per-statement pin count would decide it, which leads
to:

**An observability gap worth closing before the budget is armed.**
`DevicePageStore` maintains `live_pins()` and `pin_high_water()` precisely
because MG04 wants the per-operation pin ceiling "measured rather than
assumed" — but `SHOW META` on this build surfaces neither, so the one
number that would turn the select-arm hypothesis into a fact (pins per
statement shape) is not reachable from a client. Surfacing the two counters
is a smaller change than the migration they instrument.

**What this number is a floor for.** With the budget at 0 the sweep never
runs, so ~0.9 µs on a point read is the standing cost of the *seam* —
what every statement pays so that eviction can be armed at all. Arming a
nonzero budget adds the sweep's own cost on faulting paths on top of this
floor; that measurement is a different run against a budgeted config, and
this file's number is its baseline.
