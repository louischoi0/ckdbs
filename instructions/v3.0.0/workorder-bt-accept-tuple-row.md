# Work order BT — the rejected row's fixed cost: what `AcceptTupleAt` pays per row the kernel rejects

Written and opened 2026-10-13 on `worktree-bt-accept-tuple-row` from
`eeb88f47` (`v2.7.0-992-geeb88f47`), on the operator's words, recorded in
English (`raft-marks-2026-10-13.md` §7-§8):

- **W0**: `$!a` - the operator's only words - choosing CLA's option (a)
  after BM's close, which CLA had written as: a new milestone on
  `AcceptTupleAt`'s per-row cost, a read-only survey first, then a work
  order with its premise measured first.
- **W1**, the `go-ahead-achieving-milestone` skill invoked on it: *"go
  ahead until achieving milestone, I will follow CLA proposal if decision
  needed, keep going in this session."*

BT-Q1..Q3 below were put to the operator with CLA's proposal beside each
before W1; W1 came without a per-item mark, so **every BT-Q item is adopted
on the operator's standing go-ahead of 2026-10-13
(go-ahead-achieving-milestone)**, not marked by the operator.

**Claim tags** (BM's): `[source-read]` with `path:line` at `eeb88f47`;
`[measured]` with the results file; `[design]` for reasoning about code
not yet written; `[quiet-wrong]` where a wrong choice turns a correct
answer into a wrong one with no error; `[provisional]` for a threshold set
before a measurement.

**The letter.** `[source-read]` BS is the last letter under
`instructions/v3.0.0/` (`workorder-bs-test-hardening.md`); no file under
`instructions/` or `docs/` uses BT. CLA's choice.

**BT against other orders.** BT is the remainder BM's close recorded in
`known-gaps.md`, *"The scan row (BM)"*: after BM a scan row costs 31.0 ns
against PostgreSQL 18's 29.1, and `AcceptTupleAt`'s own body is the
largest part left (`bench/v3.0.0/results-bm-close-v2.7.0-946-g6b771ec1.md`
§6). BT changes no rule BM made: BM-R1..R4's kernel, BM-R5's loop and
BM-R6's once-per-statement check stand as written. Not part of AR0 §8's
chain.

---

## 0. What BT is

**The intent:** a row the scan kernel rejects pays a visibility decision,
the kernel's compare and the counts `ANALYZE` reports - and nothing that is
the same for every row of the statement.

- **G1 — the rejected row's CPU.** The net per-row server CPU of `bn`'s
  scan falls by what the premise measures (BT-Q1).
- **G2 — the same answer, order and counts.** BM-R2 unchanged: rows, order,
  `examined=`, the kernel-rejected count, the row-touch budget falling on the same
  row, Waystone trails and Cabin recording.
- **G3 — less code where the order can delete it** (C4): the per-row
  conjunct well-formedness test leaves the engine.

**Non-goals.** The kernel's own compare (7.4 ns, BM's), the slot loop
(BM-R5's), a recent row's visibility (BM-R7, struck), anything a kept row
pays after the kernel, `ExecStats`' representation (BN-S3 inlined `For`;
BM-R5 kept it).

---

## 1. Survey at `eeb88f47`

`[source-read]` throughout, read and not run. `src/exec/` is unchanged
from BM's close (`2d33a442`) but for `index_maintain.cpp`, which BT does
not touch.

### 1.1 The rejected row, per row

`AcceptTupleAt` (`src/exec/step_vm.cpp:2245`), for a visible row the armed
kernel rejects - every row of `bn` but its matches:

| # | per row | row-invariant? |
|---|---|---|
| 1 | `spills_.clear()` (`:2254`) | unused on this path |
| 2 | `ArmedKernel(step, recording_here)` (`:2287`, body `:2693`): `budget_.scan_kernel()`, the `recording_` test, `statement_->shape_checked`'s `vector<bool>` bit, `statement_->kernels[step_id]`, `empty()` | yes, once the step's first full-path row has run (BM-R6) |
| 3 | `PageSpanGuard` (`:55`): a `thread_local` increment and decrement and a `released_` flag, compiled into Release | no: R1's guard, which BT keeps by choice - it is the only check that a fetch never runs under a live span |
| 4 | `page.RelayoutEpoch()` (`:2297`): a u64 load whose three readers (`:2456`, `:2551`, `:2644`) all run after the rejected return | unused on this path |
| 5 | `page.ReadTupleIn(slot, nr_slots)` (`src/storage/heap/heap_page.cpp:236`): out of line, returning `StatusOr<Tuple>`; a `Status` holds a `std::string` (`include/kds/base/status.hpp:277`) | the checks are per row (BF-R10); the `StatusOr` is not needed on success |
| 6 | `txn::Classify` (`include/kds/txn/visibility.hpp:66`): four tests and `Floor()`'s acquire load | no |
| 7 | `stats_.For(step.step_id)` (`include/kds/exec/step_vm.hpp:231`): a size test, a resize branch, two increments | yes: every entry point pre-sizes `stats_.steps` to the statement's highest step id (`step_vm.cpp:3030`, `:3075`), so a `StepStats&` is stable for the whole execution |
| 8 | `budget_.ChargeRow()` (`include/kds/exec/budget.hpp:115`): a `Status` built and returned on success | the `Status` is |

A ninth cost is the call's own return: `AcceptTupleAt` returns a `Status`, which holds a `std::string` and so comes back through memory, not registers, to the walk's `accept` lambda on every row.

BM's close profile priced #1-#8 together with the frame at 13.8 ns of the
29.3 ns row: the body 9.1, `ReadTupleIn` 3.4, `For` 1.4
(`results-bm-close-v2.7.0-946-g6b771ec1.md` §6). It cannot split the body:
the sampler sees frames, and #1-#4 and #6-#8 are inlined into it.

### 1.2 UPDATE's and DELETE's conjuncts

`EvaluateConjuncts` (`step_vm.cpp:3014`) runs `EvaluateAll`
(`src/exec/chain_frame.cpp:139`), which asks `ConjunctWellFormed`
(`chain_frame.cpp:60`) of every conjunct of every row, for its two callers:
UPDATE's walk (`src/server/command_dispatcher.cpp:8494`) and DELETE's
(`:10449`). BM-S6's remedy made that test build no `Status` on success;
its own deferred note (`known-gaps.md`, *"The scan row (BM)"*) is that
checking once per statement deletes it. `EvaluateConjunct`'s other callers
- the inner build's bucketing (`step_vm.cpp:2518`, via `EvaluateAllExcept`)
and its correlated conjunct (`:2570`) - run inside `AcceptTupleAt` after
`CheckStepShapeOnce` has checked every conjunct of the step, so they need
the comparison alone.

---

## 2. Rulings

### BT-R1 — Visibility stays in `AcceptTupleAt` `[quiet-wrong]`

Every change of this order is made inside `AcceptTupleAt` or below it. No
access kind decides a row's visibility, or rejects a row, before
`AcceptTupleAt` is called: Waystone §3.1 rule 2 is structural because
there is one place (`step_vm.cpp:2258`'s comment), and a second
`Classify` beside it is how the two drift. (BT-Q2 (a).) `AcceptTupleAt`
is every *chain* access kind's one call; UPDATE's and DELETE's row loops,
which compile to no chain, already classify for themselves
(`command_dispatcher.cpp:8458`, `:10436`), and BT adds no third.

### BT-R2 — A hoisted value is re-read where it can change `[quiet-wrong]`

A value BT stops computing per row (#2, #7) is computed where it can
change and nowhere else: the kernel's arming at the step's first
full-path row and whenever `recording_` changes (`WalkAndRecord`,
`step_vm.cpp:993`/`:1000`), and a held `const ScanKernel*` after any
`CheckStepShapeOnce` of any step - a descent or a sub-chain's first row
resizes `statement_->kernels` (`:2675`) and moves every kernel in it, unless
that vector is pre-sized at statement entry as `stats_.steps` already is. A
stale armed kernel while a Cabin records drops entries from an
authoritative set (BM-R4); a dangling one reads freed memory. A `StepStats&`
needs no re-read: `stats_.steps` never grows during an execution (#7).
Where proving that is not local, the value stays per row.

### BT-R3 — BM-R2 unchanged

The same rows, order, `examined=`, the kernel-rejected count, `sel=`, and the
row-touch budget refusing on the same row. Counts are not batched.

### BT-R4 — A `Status` is built only for a failure

On the rejected row's path neither the tuple read nor the row charge
builds a `Status` on success (AP02's rule, BM-S6's remedy's). The check
stays per row and its wording stays in one body: the failure path calls
the existing worded function.

### BT-R5 — UPDATE's and DELETE's conjuncts are checked once per statement

`EvaluateConjuncts` takes a check BM-R6's way: at the walk's first row
that reaches the conjuncts, so an empty relation still refuses nothing,
then the comparisons alone. The once is the caller's: `EvaluateConjuncts` is a free function called per row, so UPDATE's and DELETE's walks each hold a bool and call a separate check at their first such row. `ConjunctWellFormed` and every per-row check
it fronted are deleted; the inner build's two sites take the comparison
alone, as `CheckStepShapeOnce` already covers them. (BT-Q3 (a).)
`[source-read]` What the check reads is per statement: the frame is
opened once per statement (`command_dispatcher.cpp:8321`, `:10412`) and
`CanResolve` reads its shape, never a value; no caller in `src/` or
`tests/` other than the four above reaches `EvaluateConjunct` or
`EvaluateAll`. One observable change, BM-Q7's: a malformed conjunct that an
earlier conjunct false on every row used to mask is now refused. A
well-formed chain sees none.

### BT-R6 — Measure first

BT-S1 measures the premise by ablation before any code stage; its gate is
BT-Q1. The whole change's overhead is measured once at the close.

---

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BT-S0 | **The order** | This file, its review, the index row, `raft-marks-2026-10-13.md` §7-§8 | S |
| BT-S1 | **The premise** (BT-R6) | Release builds of A (`eeb88f47`), A' (A's bytes started as a second server, the spread) and five ablations - D1 (#1, #2, #4 hoisted or dropped), D2 (#5 inline, no `StatusOr`), D3 (#7, #8 skipped on the rejected path), D-all (D1-D3), D4 (D-all plus the walk rejecting before the call: the floor, which prices #9 and is wrong by BT-R1) - each a measurement-only patch in one Release build directory, never committed; `tools/bm_scan.py census` on `bn`, 5 runs, within 20 minutes; results file; the gate BT-Q1 | S |
| BT-S2 | **The tuple read** (C2) | `PageView` gains an inline read returning the tuple or its reason, no `Status`; `ReadTupleIn` words a failure through it (one body); `AcceptTupleAt` uses it; suite green | S |
| BT-S3 | **Row invariants and the charge** (C1, C3) | #1 moved to its reader, #4 read under the span on the kept paths only (its readers run after the release, `:2279`'s comment); #2 hoisted under BT-R2; the rejected path charges and counts with no `Status` on success (BT-R4); suite green | M |
| BT-S4 | **UPDATE's and DELETE's conjuncts once** (C4) | BT-R5; `ConjunctWellFormed` deleted; suite green | S |
| BT-S5 | **The close** | Interleaved A/B, `eeb88f47` against the closing commit, in `build-release`, ~1 h: `bm_scan` `bn` and `wide-h`, `bn_census` every-row UPDATE and point SELECT; texts moved to their owners; `origin/main` merged | M |

A stage the premise strikes is struck in §6, not built.

---

## 4. Items for the operator

| item | question | options | CLA's proposal | mark |
|---|---|---|---|---|
| BT-Q0 | Open BT | yes / no | yes | **yes** - W0 |
| BT-Q1 | BT-S1's gate `[provisional]` | (a) build S2/S3 only if D-all (or D4) saves at least 3.0 ns a row (about a tenth of 31.0), and each stage only if its own ablation saves at least 1.0 ns and more than the run's median A-against-A' gap; (b) no gate | (a): BM's premise rule; a candidate inside the run's noise is not built. **Revised at the opening review**: the per-stage bar was 0.5 ns, below BM's measured A-against-A' gap (-3.2 ns on `bn`, `results-bm-close-v2.7.0-946-g6b771ec1.md` §8), so an A' arm was added and the bar raised | **(a) as revised**, adopted on the operator's standing go-ahead of 2026-10-13 (go-ahead-achieving-milestone) |
| BT-Q2 | Where the rejected row is decided `[quiet-wrong]` | (a) inside `AcceptTupleAt`, the walk passing at most what it already holds (BT-R1); (b) a reject-only fast path in the walk's `visit_slot` | (a): (b) puts a second `Classify` beside the one place visibility is applied | **(a)**, adopted on the operator's standing go-ahead of 2026-10-13 (go-ahead-achieving-milestone); premise re-read at `eeb88f47`: `AcceptTupleAt` is still every chain access kind's one call (UPDATE's and DELETE's loops classify outside it, as before) |
| BT-Q3 | C4's place | (a) in BT (BT-S4); (b) a separate order | (a): it deletes code, and the close's every-row UPDATE cell measures it | **(a)**, adopted on the operator's standing go-ahead of 2026-10-13 (go-ahead-achieving-milestone) |
| BT-Q4 | A push, a merge to `main`, a tag | the operator's word | none | open |

---

## 5. Measurement

### 5.1 BT-S1

`tools/bm_scan.py census` with `--cells bn`, arms A, A', D1, D2, D3, D-all, D4,
5 runs, `cores = 1`, `relaxed`, every page resident, servers pinned on CPU
2, the driver on CPU 4 (BM's setup). Each ablation is a patch over
`git archive eeb88f47` built `Release`, target `kds_server`. An ablation
may be wrong (D3 counts nothing): it prices a part, it is not a candidate.
The reading is each arm's net row against A's, paired by run.

### 5.2 BT-S5

`eeb88f47` against the closing commit. `bm_scan` `bn` and `wide-h`;
`bn_census` `update-range` at 1,000 rows (BM-S6's guard cell, A against A'
for the spread) and the point `SELECT`. About an hour, building included.

---

## 6. Row status

### BT-S0 — written and opened 2026-10-13

On `worktree-bt-accept-tuple-row` from `eeb88f47`. The suite at
`eeb88f47` before any BT change: 3691/3691 (Debug, `-LE heap-suspended`).

**The `critics-developer` review** found every cited `path:line` right
and four claims wrong, each corrected in place: #7's `StepStats&` is
stable for the execution (`stats_.steps` is pre-sized), so BT-R2's hazard
is the held `ScanKernel*` that `CheckStepShapeOnce` can move; BT-R1's
"every access kind" is every *chain* access kind (UPDATE's and DELETE's
loops classify for themselves); BT-S3 would have moved #4 out from under
the span. It recommended, and CLA applied: an A' arm and BT-Q1's per-stage
bar raised above the run's A-against-A' gap (revised proposal adopted on
the standing go-ahead); #9, `AcceptTupleAt`'s `Status` return, priced by
D4; where BT-R5's once lives; #3 kept by choice. **Rejected:** collapsing
the adoption wording in §4 into one "per the header" mark - the
go-ahead's rule is that every adopted item carries who decided and how.

### BT-S1 — the premise measured; BT-Q1 passes for S2 and S3

`[measured]` `bench/v3.0.0/results-bt-s1-premise-v2.7.0-992-geeb88f47.md`,
2026-10-13 07:36-07:42 UTC, `bm_scan` `bn`, 5 runs, seven arms. The net
row at `eeb88f47` is 32.0 ns literal and 32.4 prepared; A' is +0.1 and
-0.2 ns from A. D1 saves 2.6 / 2.5 ns, D2 3.1 / 3.3, D3 3.2 / 3.2, D-all
7.1 / 8.1 - every one below A in 5 of 5 runs on both paths but D1 literal
(4 of 5). **BT-Q1 (a) as revised passes**: D-all is over 3.0 ns and each
stage's ablation over 1.0 ns and the A-against-A' gap, so BT-S2 and BT-S3
are built. D4 - D-all, `AcceptTupleAt`'s `Status` return, reads 0.7-1.0
ns: at the bar and forbidden by BT-R1, not built. **What the gate does not
promise:** D-all is a ceiling - D3 counts nothing and BT-R3 keeps every
count, so C3 can recover only part of D3; D1 mixes three parts. Rows and
digest identical across the seven arms; only D3, D-all and D4 change
`ANALYZE`'s counts, by design.

### BT-S4 — UPDATE's and DELETE's conjuncts checked once

Built while BT-S1 measured, ahead of S2 and S3: BT-Q1 does not gate it.
UPDATE's and DELETE's walks hold a `conjuncts_checked` bool and call
`CheckConjuncts` at their first row reaching the `WHERE`;
`EvaluateConjuncts` compares with `EvaluateAllChecked`. Deleted:
`ConjunctWellFormed`, `EvaluateConjunct`, `EvaluateAll`, the `StatusOr`
`EvaluateAllExcept` (now `EvaluateAllExceptChecked`, beside
`EvaluateConjunctChecked` for the inner build's two sites), the dead
`EvaluatePredicate`, and `CheckConjunct` from the header (file-local;
`CheckStepShapeOnce` calls `CheckConjuncts`). **The `critics-developer`
review** found no correctness defect: every caller of the checked forms is
preceded by a check against the same frame shape - UPDATE's probed and
scanned paths both through `apply`, a resumed walk re-entering with the
flag false, the inner build's sites after `CheckStepShapeOnce` on every
access kind - and the one behaviour change is BM-Q7's. Applied: the dead
`EvaluatePredicate` deleted, `CheckConjunct` made file-local, a comment
rewrapped, DELETE's comment made explicit, `tools/bn_parts.json`'s
residual pattern renamed. Kept: the seven-line once-block at both sites (a
helper saves nothing, as the review also judged). Suite 3691/3691 (Debug,
`-LE heap-suspended`). Overhead not measured; measured at the milestone's
close.

### BT-S2 — the tuple read builds no `Status`

`PageView::TryReadTupleIn` (inline, `heap_page.hpp`) makes every check
`ReadTupleIn` made - range, retired, BF-R10's slot leaving the page,
`data_len` past the slot - and returns a `TupleRead` reason;
`ReadTupleIn` is it plus the wording, so the checks have one body.
`ReadSlot` and `SlotOffset` moved into the header for it. `AcceptTupleAt`
reads through it, a reason other than `kRead` being no row as `!ok()` was.
A new cell pins each reason to `ReadTupleIn`'s code and message
(`HeapPageTest.TryReadTupleInGivesReadTupleInsReasonForEverySlot`).
**The `critics-developer` review** found no defect: the check order never
reads past the frame, codes and messages are identical, R1's span covers
every use of the payload, the cell fails on any swapped reason. Rejected:
returning `kOverlongPayload`'s `Corruption` inside the switch and ending
it with an unreachable marker - C++20 has no `std::unreachable`, and the
trailing return is the same statement with no marker. **Found, not fixed:**
`PayloadAt` makes none of BF-R10's checks and can read past the frame on a
stale or corrupt page - a pre-existing defect outside BT's scope, filed as
`docs/inflight/bugs/payloadat-reads-a-slot-that-leaves-the-page.md`.
Suite 3690/3692 with the new cell; the two failures
(`IdAllocationAcrossCores.TwoCoresWritingOneRelationIssueOneSequence`,
`SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement`) were
60 s two-core waits timing out under another session's load, and passed
3 of 3 each re-run alone on the same build. Overhead not measured;
measured at the milestone's close.

### BT-S3 — the arming in one lookup, the epoch on the kept paths

What the rejected row stops paying: **#2** the arming asks one vector
once - `StatementStore`'s `shape_checked` and `kernels` are one
`std::vector<std::optional<ScanKernel>>`, an entry present once its step's
check has passed, so "checked" and "has a kernel" cannot fall out of step;
**#4** the relayout epoch is read under the span on the two paths that
leave it with a copied row; **#1** `spills_.clear()` is deleted - each
decode clears its scratch itself (`row_codec.cpp`), so the call was
redundant on every path.

**Decided mid-stage, adopted on the operator's standing go-ahead of
2026-10-13 (go-ahead-achieving-milestone):** #2 is not hoisted out of
`AcceptTupleAt` as §3's row said. BT-R2 keeps a value per row "where
proving that is not local": `recording_` changes in `WalkAndRecord` and
`kernels` grows under a nested step's first row, both inside a walk, so the
arming stays per row and is made one lookup. **#8 is not built:** the
rejected path returns a `Status` either way, and `ChargeRow()` is inline
and builds `Status::OK()` straight into that return slot, so a bool form
generates the same code - BT-S1's D3 priced the counts and the charge
themselves, which BT-R3 keeps. #7 stays as it is (BT-R3).

**The `critics-developer` review** found no defect: no path reads
`spills_` before a decode clears it; `observed_epoch` is set on every path
that reaches a reader, under the span; a present kernel is a checked
step's. Applied: the clear deleted rather than moved, #8 reverted (a first
draft had `TryChargeRow`), the two vectors made one, the epoch comment
corrected. Suite 3690/3692 at the first draft and 3690/3692 here: the
failures were two-core waits under a host load of 15-19 from other
sessions - `SortedLeafRigTest.AppendsFromTwoCoresKeepTheTreeWholeAndDense`
(a load flake named in the BM handoff) and
`SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement`, which
also failed in BT-S2's run; re-run alone on this build, each passed 4 of 4
(1.7-2.1 s against the suite's 60 s wait). The close re-runs the suite.
Overhead not measured; measured at the milestone's close.
