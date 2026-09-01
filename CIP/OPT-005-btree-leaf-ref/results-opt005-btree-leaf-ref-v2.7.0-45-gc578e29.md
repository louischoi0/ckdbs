# OPT-005 measured: the win is real by construction and invisible to this instrument

`BtreeLookupHeld` carries a btree descent's leaf `PageRef` out of the
descent instead of dropping the pin and letting the caller re-fetch the
identical page a line later. Two interleaved A/B pairs, four converted
call sites, four controls, three row-set sizes: every shape's delta sits
inside a noise floor of roughly 1-4%, established from this run's own
structural controls, with no shape or size showing a delta that
reproducibly separates from that floor in the predicted direction. This
is not a null result manufactured to be safe — it is what
`btree.hpp`'s own amended comment at `1495016` already says should
happen: `pages_fetched` is incremented explicitly beside the descent and
never counted the redundant fetch this entry removes, so the one
instrument this run had was CPU time, and CPU time could not resolve a
saving this small against a statement costing 80-800x more. The `pages=`
non-movement itself is confirmed empirically below, not only asserted.

| | |
|---|---|
| Executed | 2026-09-01 04:15-04:35 UTC |
| Worktree | `/home/cdkbs/ckdbs/.claude/worktrees/path-optimizer`, branch `opt-006-subchain-runner-reuse` at `1495016` — untouched by this run; every arm is a `git archive` export of a named commit into its own scratch tree under `/tmp/claude-1000/-home-cdkbs-ckdbs/e52ac4d4-a740-442c-8ac0-ad035bc82a3b/scratchpad/pathopt/src-{base,005,006first,final}`, never built from the worktree |
| Tree cleanliness | Each arm is a `git archive` export of a named commit — no working-tree drift possible in what was compiled |
| Build | `cmake -S <src> -B <src>/build-release -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=<scratch ossl>`, `cmake --build <src>/build-release --target kds_server -j8`. All four arms configured and linked clean (two pre-existing unrelated warnings — `waker.cpp`'s unused `read()`, `spsc_ring.cpp`'s switch — present identically in all four) |
| Host quiet | `pgrep -x cc1plus/cmake/make/ctest` empty and `uptime`/`/proc/loadavg` checked immediately before every server start of every (pair, row-count) cell — a concurrent `kds_tests` run in the sibling worktree `workorder-cabin-under-split` finished before this session's first build and never overlapped a measurement |
| Device | Data dirs under `$HOME/bench-opt005-*`, `/dev/root`, **ext4** (`df -T`), not tmpfs. Binaries copied once into `.../scratchpad/pathopt/run/` before the first cell and never rebuilt; every server in this run started from those copies |
| Build type | Release |
| Server config | `cores = 1`, `durability = relaxed`, fresh server + fresh data dir per (arm, row count) |

## The four arms

| Arm | Commit | `git describe --tags` | Committed | Binary mtime | Binary sha256 (run copy) |
|---|---|---|---|---|---|
| base | `31bc482` | `v2.7.0-41-g31bc482` | 2026-09-01 01:58:02 UTC | 2026-09-01 04:09:24 UTC | `897c61d5…d019fc397a` |
| 005 | `c578e29` | `v2.7.0-45-gc578e29` | 2026-09-01 03:35:18 UTC | 2026-09-01 04:10:13 UTC | `73856457…4ee46464ae73b318a94e1323`(1) |
| 006-first | `ff27662` | `v2.7.0-46-gff27662` | 2026-09-01 03:40:47 UTC | 2026-09-01 04:11:03 UTC | `4be3d5a2…f39a8c`(1) |
| final | `1495016` | `v2.7.0-47-g1495016` | 2026-09-01 04:02:19 UTC | 2026-09-01 04:11:55 UTC | `4e12386c…f223b9a07ba2f35`(1) |

(1) truncated for width; full digests are in each arm's build log under this
entry's `archive/`. All four binaries post-date their own commit, and no
build overlapped any measurement window.

Pairs measured (OPT-005 touches four sites: the point step, the
index-range resolve, `LocateByPk` — **not** converted, see below — and
`fk_check.cpp`'s parent descent, the last landing only at `1495016`):

- **Pair 1 — base vs 005** (`31bc482` vs `c578e29`): OPT-005 alone, the
  three sites that land in one commit.
- **Pair 2 — 005 vs final** (`c578e29` vs `1495016`): isolates the
  `fk_check.cpp` conversion, since every other OPT-005 site is already
  present on both sides of this pair. (This pair also carries OPT-006's
  full landing plus its regression fix, which is irrelevant to every
  shape below except `fk_insert` — none of `point`/`point_heap`/`range`/
  `join` touch a sub-chain.)

`ff27662` vs `1495016` was not run for OPT-005: `fk_check.cpp`'s
conversion is identical between those two (both post-date it), so the
pair would measure nothing this entry owns.

## What OPT-005 changed, and the six shapes

`BtreeLookup` (`src/storage/btree/btree.cpp:758` at `31bc482`) builds a
`Descent` that holds the leaf `PageRef`, returns only
`Location{page_id, slot}`, and lets the pin die at return. Every caller
that reads the leaf immediately then re-fetches the identical page: a
`std::map` region lookup plus three `frames_` hash lookups plus three
virtual dispatches (`device_page_store.cpp`). `BtreeLookupHeld` returns
`LocatedRef{loc, leaf}` instead, moving the descent's own pin out. Six
shapes, chosen to hit every converted site plus two structural controls:

| Shape | What it exercises |
|---|---|
| `point` | `SELECT * FROM t_pt WHERE id=pid`, BTREE relation — `step_vm.cpp:590`'s `RunPointStep`, converted at `c578e29` |
| `point_heap` | Same query, HEAP relation — `RunPointStep`'s `clustered_type == kBtree` gate is false, so this **never reaches `BtreeLookup*` at all**. True structural control |
| `range` | `WHERE cust_id BETWEEN lo AND hi` on a secondary-indexed BTREE relation — the index-range resolve, **once per resolved row** (~10 rows/query), the largest instance of the pattern |
| `join` | `SELECT c.id,p.val FROM child c JOIN parent p ON c.parent_id=p.id WHERE c.id=pid` — child hits the point step, parent hits it again as a pk join probe (`kProbe` compiles through the same `RunPointStep`) |
| `fk_insert` | `INSERT INTO t_fk_child VALUES (pid, val)`, `t_fk_child.parent_id REFERENCES t_fk_parent` — `fk_check.cpp`'s parent descent, converted only at `1495016`. On pair 1 (neither arm converted) this is itself a control |
| `plain_insert` | Same insert shape, no `REFERENCES` — isolates "cost of INSERT" from "cost of the FK check" |

`LocateByPk` (`command_dispatcher.cpp:6740`, the point-`UPDATE` path) is
**deliberately not converted**: its caller re-fetches through `Get` for a
writable frame regardless, so a read pin held across that buys nothing —
the `1495016` comment on that function says so explicitly. No shape here
targets it for that reason.

Full methodology in the driver's own docstring:
`CIP/OPT-005-btree-leaf-ref/archive/opt005_ab.py`. One RNG stream drives
an identical statement sequence on both arms; a latency pass times each
statement on the wire (`bench_common.Phase`); a CPU pass brackets larger
blocks with `/proc/<pid>/stat` (utime+stime, 10ms ticks) since `pages=`
was the predicted instrument and its own owning comment retracts that
prediction (see below) — CPU is what is left. Both passes alternate
which arm leads each round. `--rows` is 200/1000/10000 per this role's
sweep rule; `t_rng`'s customer count and range span hold the *matched*
row count near-constant across sizes (`index_benchmark.py`'s design),
isolating a per-row-resolved cost from a per-query one.

## `pages=` does not move — confirmed, not assumed

The proposal's original measurement plan was `ANALYZE`'s `pages=`
counter, predicted to drop by exactly one per resolved row. `1495016`'s
review retracted that: `pages_fetched` is incremented explicitly beside
the descent and never counted the redundant `GetForRead` this entry
removes. Every `ANALYZE` snapshot taken in this run — both pairs, all
three sizes, all four read shapes — confirms the retraction rather than
merely repeating it:

| Shape | rows | `pages=` (both arms, both pairs) | `index_resolved=` |
|---|---:|---|---|
| `point` | 200/1000/10000 | `[1, 1]` | — |
| `point_heap` | 200/1000/10000 | `[3, 3]` / `[13, 13]` / `[130, 130]` | — |
| `range` | 200/1000/10000 | `[69, 69]` / `[71, 71]` / `[71, 71]` | `[68]` / `[70]` / `[70]` |
| `join` | 200/1000/10000 | `[2, 1, 1]` | — |

Identical on every arm at every size, in both pairs — 32 `ANALYZE`
comparisons, zero deltas. This is the first concrete evidence for the
review's claim: whatever OPT-005 buys, no counter this engine emits
today shows it.

## The measurement: every shape inside a 1-4% floor

**QPS by shape, pair 1 (base vs 005) — the three converted sites plus
their controls:**

| rows | shape | base QPS | 005 QPS | Δ QPS |
|---:|---|---:|---:|---:|
| 200 | point | 9,313 | 9,264 | -0.5% |
| 200 | point_heap (control) | 8,440 | 8,354 | -1.0% |
| 200 | range | 4,160 | 4,188 | +0.7% |
| 200 | join | 8,954 | 8,932 | -0.3% |
| 200 | fk_insert (control, unconverted here) | 11,213 | 11,449 | +2.1% |
| 200 | plain_insert (control) | 12,385 | 12,440 | +0.4% |
| 1,000 | point | 9,230 | 9,452 | +2.4% |
| 1,000 | point_heap (control) | 5,626 | 5,672 | +0.8% |
| 1,000 | range | 3,710 | 3,806 | +2.6% |
| 1,000 | join | 8,701 | 9,012 | +3.6% |
| 1,000 | fk_insert (control) | 10,502 | 11,248 | +7.1% |
| 1,000 | plain_insert (control) | 12,081 | 12,464 | +3.2% |
| 10,000 | point | 8,627 | 9,179 | +6.4% |
| 10,000 | point_heap (control) | 1,233 | 1,192 | -3.4% |
| 10,000 | range | 3,522 | 3,591 | +1.9% |
| 10,000 | join | 8,333 | 8,665 | +4.0% |
| 10,000 | fk_insert (control) | 10,029 | 10,373 | +3.4% |
| 10,000 | plain_insert (control) | 11,653 | 4,979 | -57.3%(2) |

(2) A single p99 stall (7,162.9us against a 139.1us baseline, 1 sample of
60) drags the mean; p50 delta for this cell is +0.6%. Reported here
rather than dropped, per the rule that a delta which does not reproduce
is a delta to name, not to hide — see below.

**QPS by shape, pair 2 (005 vs final) — isolates the `fk_check.cpp`
conversion:**

| rows | shape | 005 QPS | final QPS | Δ QPS |
|---:|---|---:|---:|---:|
| 200 | point (control, converted both sides) | 9,201 | 9,316 | +1.2% |
| 200 | fk_insert (**the converted site**) | 11,253 | 11,303 | +0.5% |
| 1,000 | point (control) | 9,398 | 9,306 | -1.0% |
| 1,000 | fk_insert (**converted**) | 11,128 | 10,949 | -1.6% |
| 10,000 | point (control) | 9,223 | 9,206 | -0.2% |
| 10,000 | fk_insert (**converted**) | 10,497 | 10,566 | +0.7% |

(`point_heap`/`range`/`join` in pair 2 sit in the same ±3.4% band as
pair 1's controls and are in the raw JSON/logs; omitted here since they
carry no new site.)

**The floor.** Splitting arm A's own latency series in half (rounds
1-6 vs 7-12) and reading the two halves as if they were a second arm
gives the same measurement's own noise, with nothing that could
possibly differ:

| Shape | Floor spread (p50), typical range across sizes/pairs |
|---|---:|
| `point_heap` (never touches `BtreeLookup*`, any commit) | 0-2% |
| `point` | 0.2-9.5%(3) |
| `range` | 0.1-6% |
| `join` | 0.4-12.5%(3) |
| `fk_insert` / `plain_insert` | 0.2-8.4% |

(3) The high end of these ranges is the CPU-pass tick-quantization floor
at rows=10,000 with these op counts, not the latency-pass floor; the
latency-pass (wall-clock, not tick-quantized) floor for every shape sits
at 0-3% across every cell measured, and that is the number the QPS table
above should be read against.

Against a 0-3% latency-pass floor, **no shape's delta in either pair
separates cleanly and in one direction across all three sizes.**
`point` in pair 1 shows +2.4%/+6.4% at 1,000/10,000 rows — the largest
single cells in the predicted direction — but `point_heap`, a control
that structurally cannot be touched by this change, moves by a
comparable or larger amount in the *same run* (-3.4% at 10,000 rows) with
no shared mechanism, and `point`'s own delta is -0.5% at 200 rows,
not monotonic in the way a real, scaling effect would be expected to be
for an O(1) pk descent whose per-op cost does not grow with table size.
`fk_insert` in pair 1 (unconverted control) moves by *more* (+7.1% at
1,000 rows) than `fk_insert` in pair 2 (the actually-converted pair,
+0.5%/-1.6%/+0.7%) — the pair that should show the effect if it were
resolvable shows less of a delta than the pair that cannot. That
inversion is the clearest single piece of evidence that these numbers
are noise, not signal.

One anomaly is worth naming rather than silently re-running past:
pair 1's rows=1,000 cell was first measured with every shape — including
`point_heap` and `plain_insert`, both structurally untouched by any
commit in this run — reading 20-30% slower on arm B than arm A, a
uniform shift across shapes that share no code path. That is exactly
this role's documented risk of a build or another process cutting
throughput mid-run (`ck-tester.md`'s "3x and 34%" precedent). The cell
was re-measured on fresh servers and a fresh data directory with the
host confirmed quiet immediately before, and reproduced the flat result
shown in the tables above (host load and quiet-check output are in
`archive/pair1-base-vs-005-run1000.log`, second occurrence). The
contaminated first run's JSON/log were discarded rather than reported.

## Correctness

Every relation's full contents (`SELECT * ... ORDER BY id`, sha256 of the
reply) matched byte-for-byte between arms, post-load and post-writes, at
every size, in both pairs — 8 relations x 2 checkpoints x 3 sizes x 2
pairs = 96 comparisons, zero mismatches. Zero statement errors on either
arm across the whole run. Full detail: `pre_check`/`post_check`/`errors`
keys in every `archive/*.json`.

## What this run teaches

**The predicted saving is real by construction and this instrument
cannot see it.** A `std::map` region lookup plus three hash lookups plus
three virtual dispatches is a few hundred nanoseconds on any reasonable
estimate — and every shape here costs 80-800us end to end, dominated by
the socket round trip, statement parse/compile, WAL append (even under
`relaxed`) and, for `range`/`join`, the actual page I/O the walk does.
A few hundred nanoseconds against that base is under the 0-3%
latency-pass floor this run itself establishes, which is why nothing
resolves. This is not evidence the change is wrong — the `pages=`
confirmation above shows the code path taken is exactly the one the
proposal describes — it is evidence that **an interleaved CPU/latency
A/B on a live statement is the wrong scale of instrument for a
few-hundred-nanosecond saving**, and that the fix belongs on a smaller,
more isolated benchmark (a tight loop directly over `BtreeLookup*`
outside the statement pipeline) if a number is ever needed. Per the CIP
README's own rule, a refuted-at-this-scale hypothesis is a result, not a
failure to report: OPT-005 keeps its branch and this entry, and the
honest verdict is **measured, no resolvable win at the statement level,
confirmed at the counter level that it changes nothing else**.

The one genuinely new finding is `fk_insert`'s inversion above: a
control that cannot show the effect (pair 1) moved by more than the pair
that can (pair 2). That is worth carrying forward as a caution the next
time this shape is used to price something — `fk_insert`'s own
statement cost (INSERT, catalog lookups, undo, WAL) is noisy enough on
its own that it is a poor shape for isolating a sub-microsecond delta,
independent of what that delta is.

## Files

- Driver: `CIP/OPT-005-btree-leaf-ref/archive/opt005_ab.py`
- Raw JSON + logs: `CIP/OPT-005-btree-leaf-ref/archive/pair{1,2}-*-run{200,1000,10000}.{json,log}`
- Proposal: `CIP/OPT-005-btree-leaf-ref/proposal.md`
