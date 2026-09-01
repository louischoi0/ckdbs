# OPT-004 measured: the entry's own "low single digits" prediction is what this harness can, and mostly cannot, resolve

OPT-004's own text predicted a small win and said so in advance: *"AP02
measured 19-35% on this class; here expect low single digits, but on ~5
lines that cannot regress."* This run tests that prediction with two
pairs across three row counts. The finding is not a number to quote as a
win — it is that **no shape reachable by `DecodeRowInto` shows a
reproducible, correctly-directioned, size-scaling effect**, in either
pair, at any of the three sizes, and the one shape that *does* show a
large, consistent, same-direction delta (`update` in pair 2) is one
`DecodeRowInto` is barely called in at all under OPT-001's masking — so
that delta cannot be OPT-004's. Read plainly: **inside the noise floor
at every size this harness can resolve**, which is the honest reading
rule 8 of this role asks for when a low-single-digit prediction meets a
harness whose floor is comparable in size to the prediction itself.

## Stamp

| | |
|---|---|
| Executed | 2026-09-01 01:53–02:44 UTC (a ~40 min gap, 01:56–02:38, is a wait for a concurrent build on another worktree to clear — see "Contamination" below) |
| Worktree (orchestration only) | `/home/cdkbs/ckdbs/.claude/worktrees/path-optimizer`, currently `opt-003-walk-read-access` at `53797fd` (`v2.7.0-42-g53797fd`) — **irrelevant to what was measured**: all four arms are `git archive` exports built in scratch source trees, never built from this worktree |
| Build | `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=<scratch ossl>` (sandbox has no `libssl-dev`), `cmake --build build-release --target kds_server -j8`. All four arms configured and linked clean — the same two pre-existing, unrelated warnings every prior CIP run in this directory has seen (`waker.cpp`'s unused `read()`, `spsc_ring.cpp`'s switch) |
| Device | Data dirs under `$HOME/bench-opt004/{pair1,pair2}-{a,b}-<rows>`, `/dev/root`, **ext4** (`df -T`), not tmpfs |
| Server config | `cores = 1`, `durability = relaxed` (`bench/run_ab_server.sh`), fresh server + fresh data dir per (arm, row count) — 12 server instances total across both pairs |
| Host | 8 vCPUs, Ubuntu 24.04.4 LTS; quiet verified before every stage with `pgrep -x cc1plus`/`cmake`/`make`/`ctest` (the exact-name form, which does not self-match a monitoring command's own text the way `pgrep -f` does) plus `uptime` |

### Pair 1 — the change in isolation, on the tree it was written against

| | |
|---|---|
| Arm A (baseline) | `55d2c0b`, `v2.7.0-30-g55d2c0b`, committed 2026-08-31T23:12:00Z. Has OPT-002, not OPT-004 |
| Arm B (change) | `b05925f`, `v2.7.0-31-gb05925f`, committed 2026-08-31T23:38:02Z. OPT-004 added |
| Diff | `git diff --stat 55d2c0b b05925f` → one file, `src/exec/row_codec.cpp`, `+8/-17` — the whole change is `DecodeRowInto`'s four preconditions routed through `CheckDecodeInputs` |
| Binary provenance | A: `build-release/kds_server` mtime 2026-09-01 01:37:26 UTC (2h25m after commit). B: mtime 2026-09-01 01:38:15 UTC (2h00m after commit). Both post-date their own commit, as expected for binaries built this session |
| Binary run | A copy sha256 `c5db2b1a1054a4f0e6329219f1419a7979cd80a43f2d03b988e0917766fadba2`; B copy sha256 `76773147d22e7f651e81d8f1484ae71239baebd8cbf0f119e320352522221541`. Both copied once, never rebuilt; every server in this pair started from these copies (verified unchanged by re-hashing after the run) |
| Sanity | Both are **unmodified, historical commits** already verified independently: OPT-004's own `proposal.md` records "Release suite 3088/3088 green" at `b05925f`, and this run adds a fresh 6-for-6 byte-identical correctness check (below) on top |

### Pair 2 — what OPT-004 is worth today, after OPT-001 cut `DecodeRowInto`'s call volume on a point `UPDATE`

| | |
|---|---|
| Arm A (reverted) | `ea1d9d0`'s tree (`v2.7.0-38-gea1d9d0`, committed 2026-09-01T00:16:07Z) with **one hand edit**: `DecodeRowInto`'s `CheckDecodeInputs` call restored to the pre-OPT-004 two-checker form verbatim from `55d2c0b`. **This is a modified tree, not a commit** — cited as such throughout, never as a bare hash. Exact diff: `CIP/OPT-004-decoderowinto-preconditions/archive/pair2-arm-a-revert.diff` (touches only `src/exec/row_codec.cpp`, 15 lines, confirmed by `diff -rq` against arm B's tree: the two trees differ in exactly this one file) |
| Arm B (unmodified) | `ea1d9d0`, `v2.7.0-38-gea1d9d0`, unmodified — current OPT-004 code |
| Binary provenance | A (modified): mtime 2026-09-01 01:39:00 UTC (built ~1h23m after `ea1d9d0`'s own commit time, from a tree edited after export). B: mtime 2026-09-01 01:41:51 UTC (1h26m after commit) |
| Binary run | A copy sha256 `723a72b9713c7adbd33c2f2ed25909cf620bd7ecedd645d5b0a3b004ca003bf4`; B copy sha256 `d04132cb53b07cde6ddcad5a22094778b427820a1e42620f742938e7c972ab8f` |
| Sanity | Arm A's modified tree is the one genuine code change this session made, so it gets the suite: **`kds_tests` built and run, 3091/3091 pass**, including `RowCodecKeystoneTest.ASchemaWithNoColumnsIsRefused` — the test that drives `DecodeRow`→`DecodeRowInto` through the exact reverted precondition block and checks the refusal `Status`. Full log tail and the `RowCodecKeystoneTest` lines are archived at `archive/pair2-arm-a-suite-tail.log` / `archive/pair2-arm-a-suite-rowcodec.log` |

## What changed, and why the two pairs measure different things

`DecodeRowInto` (`src/exec/row_codec.cpp`) is the whole-row (`kAllColumns`)
decoder, reached through `DecodeAndResolve` whenever a step's
`filter_columns` is still `Step::kAllColumns` — always true for a plain
`SELECT *`/`ANALYZE SELECT *` (no `WHERE` to narrow it), and forced true
for any step carrying a sub-chain (`step_compiler.cpp`'s
`CompileWhere`: `maskable = out.sub_chains.empty() && ...`). OPT-004
replaces its four hand-written preconditions —
`catalog::CheckKeystoneColumn` (cross-TU, constructs a `Status` carrying a
`std::string` even on success) and `CheckLayoutMatches`, called
unconditionally, plus two inline checks — with one call to
`CheckDecodeInputs`, the same predicate-first helper AP02 already gave the
sibling `DecodeColumnsInto`.

**Pair 1 predates OPT-001**: a point `UPDATE`/sub-chain-carrying `UPDATE`
walks the whole relation and calls `DecodeRowInto` (via `DecodeRow` and
directly) on *every scanned row*, matched or not — the shape where
`DecodeRowInto`'s call volume, and therefore any per-call saving, is
largest. **Pair 2 has OPT-001**: a point `UPDATE`'s `filter_columns` masks
down to the `WHERE`'s own column, so `DecodeAndResolve` calls
`DecodeColumnsInto` (not `DecodeRowInto`) for the scan/test phase, and
`DecodeRowInto` runs only once, for the ≤1 matched row — call volume is
now **O(1) per statement, not O(N)**. `full-scan`/`analyze-scan` (no
`WHERE`) and `subchain` (subquery forces `kAllColumns`) are unaffected by
OPT-001's mask either way, so they exercise `DecodeRowInto` at O(N) in
*both* pairs — these are the shapes that can actually show a per-row
effect if one exists.

Four shapes, each row-count-swept at 200 / 1,000 / 10,000 rows (rule 9):
`full-scan` (`SELECT * FROM t_main`, render+wire-bound — OPT-002's own run
showed this shape cannot see a decode saving, included as a flat
control), `analyze-scan` (`ANALYZE SELECT * FROM t_main`, the decode-only
instrument), `update` (`UPDATE t_main SET c_int=x WHERE id=pid`), and
`subchain` (`UPDATE t_main SET c_int=x WHERE id IN (SELECT target_id FROM
t_pool WHERE target_id=target)`). Full methodology, schema and every
parameter's reasoning: `archive/opt004_ab.py`'s own docstring.

## The noise floor, and why it understates the true floor here

Per-cell, the floor is arm A's own CPU-pass samples split at the run's
midpoint (10 rounds vs 10 rounds), reported as `|Δus/op|` between the
halves — the mechanism rule 8 asks for, and the one every prior CIP run
in this directory uses. It is tight where tick volume is high (**256
identical ticks** on both halves for pair 1's `analyze-scan` at 10,000
rows — a real, well-resolved 0.00% floor, not a fluke of few samples;
raw tick counts for every cell are in the archived JSON's `cpu_floor`
block).

**That floor measures only within-run, within-binary variance. It
does not measure the variance between two separately-linked
binaries**, and this run has direct evidence that the second kind is
real and comparable in size to OPT-004's own predicted effect:

- Pair 1's `analyze-scan` at 10,000 rows shows B **9.06% slower** than A,
  clearing a tight 0.00%-ticked floor — a large, well-resolved,
  wrong-direction result. Pair 2's `analyze-scan` at 10,000 rows — the
  **identical `CheckDecodeInputs` code**, compiled into a different pair
  of binaries — shows **-0.33%**, inside its own floor. If OPT-004's
  code shape itself caused a regression here (say, `CheckDecodeInputs`
  now being shared by two call sites and not inlined into either, where
  before at least the in-TU `CheckLayoutMatches` might have been), both
  pairs would show it. Only one does. The best-supported reading is
  inter-binary code-layout noise specific to the `55d2c0b`/`b05925f`
  linked pair — the same class of artifact
  `results-opt001-update-decode-order-v2.7.0-38-gea1d9d0.md` named for
  its own unexplained `select` outlier (`-10.9%` at 10,000 rows, "no
  logical channel" to move through).
- Pair 2's `update` shows a **large, consistent, same-direction** delta
  at every size — -8.82% (200), -6.56% (1,000), -10.46% (10,000), B
  slower every time, all clearing their floors. This looks like a
  reproducible finding until the mechanism is checked: under OPT-001,
  `update`'s masked scan/test phase calls `DecodeColumnsInto`, never
  `DecodeRowInto` — the ≤1 matched row per statement is the *only* call
  `DecodeRowInto` gets. A per-row cost cannot produce a delta this size,
  this consistent, and this insensitive to row count from a call site
  that fires **once per statement regardless of N**. Whatever produces
  this delta, it is not OPT-004's changed code — it is something else
  about this specific pair of linked binaries.

Both observations point the same way: the true floor for a paired-binary
comparison at this magnitude is wider than the within-run split alone
reports, and wide enough to swallow a "low single digits" prediction.
The cross-check between two pairs measuring the same code is what
exposes this — a single pair's floor would not have.

## Pair 1 — server-CPU throughput (derived QPS, `1e6/us-per-op`)

| rows | shape | A (`55d2c0b`) | B (`b05925f`) | Δ | floor | verdict |
|---:|---|---:|---:|---:|---:|---|
| 200 | full-scan | 9,524 | 10,256 | +7.69% | 10.00% | inside floor |
| 200 | analyze-scan | 12,973 | 13,333 | +2.78% | 5.56% | inside floor |
| 200 | update | 10,526 | 11,111 | +5.56% | 6.52% | inside floor |
| 200 | subchain | 4,000 | 3,922 | -1.96% | 0.00%† | at tick resolution (~1.3%), not a finding |
| 1,000 | full-scan | 3,556 | 3,265 | -8.16% | 14.29% | inside floor |
| 1,000 | analyze-scan | 6,486 | 6,250 | -3.65% | 1.09% | above floor, wrong direction, small |
| 1,000 | update | 4,082 | 4,117 | +0.86% | 0.68% | above floor, correct direction, tiny |
| 1,000 | subchain | 988 | 980 | -0.82% | 4.20% | inside floor |
| 10,000 | full-scan | 355 | 379 | +6.82% | 10.45% | inside floor |
| 10,000 | analyze-scan | 977 | 888 | **-9.06%** | 0.00%† | **above a real, well-resolved floor, wrong direction — does not reproduce in pair 2, see above** |
| 10,000 | update | 506 | 510 | +0.77% | 0.17% | above floor, correct direction, tiny — the best-supported positive signal in this run (largest `DecodeRowInto` call volume of any pair-1 shape, correctly directioned at both 1,000 and 10,000 rows) |
| 10,000 | subchain | 106 | 106 | +0.27% | 3.23% | inside floor |

† A "0.00%" floor means both halves landed on the identical tick count
(real, not a rounding artifact where tick volume is high — see the
noise-floor section) but should be read as "at or below the tick
resolution," not literally zero.

## Pair 2 — server-CPU throughput (derived QPS)

| rows | shape | A (reverted) | B (`ea1d9d0`) | Δ | floor | verdict |
|---:|---|---:|---:|---:|---:|---|
| 200 | full-scan | 9,524 | 9,091 | -4.55% | 0.00%† | at tick resolution, not a finding |
| 200 | analyze-scan | 12,632 | 11,940 | -5.47% | 2.13% | above floor, wrong direction, small |
| 200 | update | 16,129 | 14,706 | -8.82% | 3.28% | above floor, wrong direction — **not attributable to OPT-004** (see above) |
| 200 | subchain | 4,110 | 4,286 | +4.29% | 0.00%† | at tick resolution, not a finding |
| 1,000 | full-scan | 3,265 | 3,077 | -5.77% | 13.04% | inside floor |
| 1,000 | analyze-scan | 5,581 | 5,854 | +4.88% | 0.93% | above floor, correct direction, small |
| 1,000 | update | 8,421 | 7,869 | -6.56% | 5.04% | above floor, wrong direction — not attributable to OPT-004 |
| 1,000 | subchain | 1,062 | 1,048 | -1.31% | 0.00%† | at tick resolution, not a finding |
| 10,000 | full-scan | 345 | 357 | +3.57% | 4.23% | inside floor |
| 10,000 | analyze-scan | 828 | 825 | -0.33% | 2.01% | **inside floor — the largest, best-resolved size, and the shape closest to a pure decode instrument, shows nothing** |
| 10,000 | update | 1,460 | 1,307 | **-10.46%** | 0.49% | above floor, largest wrong-direction delta in the dataset — **mechanistically impossible for OPT-004 to cause** (`DecodeRowInto` runs ≤1×/statement here, not O(N)) |
| 10,000 | subchain | 115 | 117 | +1.46% | 0.00%† | at tick resolution, not a finding |

† Same reading as pair 1's table.

## Client latency at 10,000 rows (µs, rule 6's five percentiles)

The size-blind instrument alongside the CPU pass — 200/1,000-row
percentiles are in the archived JSON, omitted here per rule 8 (a finding
is led with, not buried under three sizes of a table that says the same
thing each time).

| pair:arm:shape | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|---:|
| p1 A:analyze-scan | 300 | 1167.6 | 1054.2 | 1072.1 | 1086.5 | 1762.1 | 1798.0 |
| p1 B:analyze-scan | 300 | 1178.9 | 1135.8 | 1156.0 | 1166.3 | 1252.8 | 1371.2 |
| p1 A:update | 300 | 2048.9 | 1963.0 | 1985.8 | 2016.0 | 2109.2 | 3427.4 |
| p1 B:update | 300 | 2001.4 | 1931.9 | 1958.3 | 1973.7 | 2043.6 | 2668.9 |
| p2 A:analyze-scan | 300 | 1285.9 | 1233.7 | 1252.6 | 1270.9 | 1403.1 | 1498.6 |
| p2 B:analyze-scan | 300 | 1264.7 | 1229.3 | 1242.4 | 1252.7 | 1380.3 | 1420.1 |
| p2 A:update | 300 | 728.2 | 686.7 | 705.5 | 721.9 | 769.4 | 802.6 |
| p2 B:update | 300 | 809.4 | 773.5 | 795.0 | 801.0 | 849.9 | 1002.7 |

Pair 2's `update` client-latency delta (728→809µs, +11.2%) corroborates
its CPU-pass delta (-10.46%) — both instruments agree on the *size* of
the anomaly, which rules out "an artifact of the CPU-pass instrument
specifically" but does not change the mechanistic argument above: the
call this change touches does not run per row in this shape under
OPT-001's mask, so this delta cannot be attributed to it regardless of
which instrument shows it.

## Wait accounting (rule 3)

`durability`/commit wait: not applicable — `relaxed` on every arm, and
the target of this measurement is CPU-bound decode, matching the
precedent OPT-001's and OPT-002's own results files set for the same
reason. `lock/conflict wait`: not applicable — one connection per arm,
no concurrent writer. `client/socket round trip`: excluded by
construction — server CPU via `/proc/<pid>/stat` is the primary
instrument precisely so this measurement does not have to net it out of
a latency number (§"client latency" above is corroborating, not
primary). `write-statement`/`read` wait: not a meaningful decomposition
at this granularity — the quantity under test is a few nanoseconds
inside one decode call, smaller than any wait category this engine's
instruments can separately name. The honest statement is that this
measurement has **no further decomposition below "server CPU, all of
it"** — which is exactly why noise a within-run floor can't see (the
inter-binary kind, above) is the dominant hazard here rather than any
named wait.

## Correctness

Every one of 6 runs (2 pairs × 3 sizes) passed both checks, both before
and after the writes: `SELECT * FROM t_main ORDER BY id` (sha256 of the
reply), `SELECT COUNT(*) FROM t_main`, and — the check that also proves
no `ANALYZE` counter moved — the sha256 of the **entire** `ANALYZE SELECT
* FROM t_main` reply (plan included; deterministic, since
`plan_printer.cpp`'s per-step stats carry no wall-clock field).

| pair | rows | t_main hash | count | ANALYZE reply hash |
|---|---:|---|---|---|
| 1 | 200 / 1,000 / 10,000 | match | match | match |
| 2 | 200 / 1,000 / 10,000 | match | match | match |

Both arms of both pairs are driven by the identical RNG-seeded statement
sequence, so exact equality is the bar, and it held in all 12
before/after checks. Combined with pair 2 arm A's 3091/3091 suite pass
(including the direct `DecodeRowInto` refusal-path test named above),
this closes the "no behaviour may change" requirement: the checks and
their messages are unchanged by construction, and every empirical probe
this session ran agrees.

## Contamination (stated plainly, per this role's own rule 3)

Pair 1's **first** attempt (all three row counts) was discarded entirely:
a build in a different worktree (`xf`, unrelated to this task) ran
throughout, verified by real `cc1plus` invocations naming that
worktree's own source paths, not merely an elevated load average.
Symptom: floors of 10-14% at 200/1,000 rows where a clean run later
showed 5-10%.

Pair 1's **second** attempt (200 and 1,000 rows) ran host-quiet
throughout, verified repeatedly with `pgrep -x cc1plus`/`cmake`/`make`
(the exact-name form — `pgrep -f` matches this harness's own wrapped
command line and reports false busy). Its `rows=10000` stage was hit
by a **second**, later build from the same `xf` worktree partway
through the CPU pass and was discarded too (floors 20-44%, the second
half of every shape's CPU pass 20-43% slower than the first — the exact
signature this role's own standing warning names). Both discarded JSON
files are **not** in this document's tables; the second is archived as
`pair1-run10000-CONTAMINATED.json/.log` for the record, never for a
finding.

Pair 1's `rows=10000` **used in this document** is a third, separate
run, started only after `pgrep -x cc1plus`/`cmake`/`make` all reported
zero and confirmed again immediately before launch. One brief `cc1plus`
process (from `xf` again) was observed roughly 78 seconds into this
run's ~102-second driver-internal duration (setup+latency+CPU pass) and
was gone on the next check; this run's own CPU-pass duration (86.05s)
was **faster** than the discarded contaminated run's (112.27s), and its
floors are tight (≤10.45%, several ≤1%, one at 256-tick resolution) —
both facts argue against material impact, stated as evidence rather than
assumed. Pair 2's full three-size sweep ran with zero `cc1plus`/`cmake`/
`make` detected at any check, throughout.

## What this run teaches

**The entry's mechanism is real but the entry's own instrument (this
harness, at these row counts) cannot resolve it separately from
inter-binary noise of comparable size.** AP02's argument — avoid
constructing a `Status` (which owns a `std::string`) on the passing path
of a cross-TU call — is sound in principle and unchanged by this run.
What this run adds is the scale: at the one place with the most call
volume to show it (pair 1's `update`, `DecodeRowInto` called 2×/scanned
row, pre-OPT-001), the correctly-directioned signal is **+0.77-0.86%**,
not the "low single digits" the entry itself predicted as a floor. Every
shape that stays reachable at O(N) in *both* pairs (`analyze-scan`,
`subchain`) shows no size-scaling, no cross-pair-reproducible signal at
all — direction flips between pairs and between row counts for the
identical compiled logic.

**A single pair's within-run floor is not enough evidence at this
magnitude — the two-pair design is what caught it.** Pair 1's
`analyze-scan`/10,000-row cell would have read as a real, floor-clearing,
9% *regression* if reported alone. It took pair 2 measuring the
byte-identical `CheckDecodeInputs` code, in a different linked binary,
showing 0.33%, to make clear the first number was about the binary pair,
not the code. Any future CIP entry measuring a change this small should
budget for a second, independently-built pair as the actual noise-floor
instrument, not the within-run split alone — the within-run split
answers "is arm A stable against itself," not "do these two binaries
differ for the reason I think they differ."

**OPT-001 already extracted almost everything there was to extract from
this call site.** Pair 2 exists because OPT-001 cut `DecodeRowInto`'s
call volume on a point `UPDATE` from O(N) to O(1); that is *why*
OPT-004's already-small effect has nothing left to act on in that shape
today. This is the same relationship OPT-002's own results file
described for its own `update` number ("a measure of OPT-001's defect,
and it will shrink when OPT-001 lands") — OPT-004 is a second, smaller
instance of the same pattern: a per-row fix's value is capped by how
many rows still take that path, and R-series and OPT-001 work upstream
of this file keep shrinking that count.

**Verdict for the CIP entry**: inside the noise floor at every size this
harness can resolve, with one small, correctly-directioned, floor-
clearing exception (pair 1's `update`, +0.77-0.86% at scale — consistent
with "low single digits" only if read at its very bottom edge). The
entry should say this rather than reach for a number the data does not
carry; it should **not** be read as evidence the change is wrong — the
suite (3091/3091, including the refusal-path test) and 12/12
byte-identical correctness checks stand, and the mechanism's direction
(never regress, per rule "same answers, same errors, fewer cycles") is
consistent with the one signal that does clear a floor cleanly.

## Reproduction

Driver and orchestrator: `archive/opt004_ab.py`, `archive/run_pair.sh`
(`run_pair.sh pair1|pair2 <binary-a> <binary-b> <out-dir>` — fresh server
and data dir per row count, per this role's own rule 6). Raw JSON and
logs for every cell used in this document, plus the discarded
contaminated run (clearly labelled) and pair 2 arm A's revert diff and
suite output, are under `archive/`.
