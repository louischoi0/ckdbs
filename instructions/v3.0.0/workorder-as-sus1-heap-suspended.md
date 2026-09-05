# AS — SUS-1: heap relations suspended

Status: **ordered by the operator, 2026-09-05. AS-S0 and AS-S1 both built
the same day**; the one item AS-S1 does not deliver is the `heap-suspended`
label's *application*, which waits on AS-Q4 (there is no nightly runner in
this tree, so labelling would remove heap cells from the only thing that
runs them). The mechanism ships; the labelled set is empty.
Author: CLA, on `worktree-ar2-borrow-model-2` at `07c94c7`, built through
`ecf17cc`
Size: **S, and now measured** — AS-3 C.
Scope: `CREATE TABLE`'s storage clause and its default; the test corpus's
storage form; `heap-and-tuple.md` §3.1b; `CLAUDE.md`'s milestone table;
`manual/sql/sql.md`. **No storage, catalog, recovery or executor code.**
Claim tags: `[source-read]` with `path:line` at `07c94c7`; the rest
`[design]`. Nothing `[measured]`.

---

## AS-1 — The order, and what it is not

The operator's words of 2026-09-05, which this document records rather than
interprets:

> Heap relations suspended. `CREATE TABLE … HEAP` is refused with
> `Unsupported` naming SUS-1; existing heap relations still mount and
> serve; the storage-form default, if it is HEAP, flips to BTREE (client
> manual updated). Code and tests: nothing deleted. Heap-only cells move to
> ctest label `heap-suspended`, excluded from the gate, run nightly;
> fixtures keep a test-only bypass so the paths stay exercised. Work
> orders: AM/AN/AO/AQ cells that name a heap arm keep it as non-gating; new
> stages need not add heap cells. A heap defect found meanwhile is filed in
> `bugs/`, not fixed. Prose: `heap-and-tuple.md` §3.1b gets a SUSPENDED
> banner, no deletion; `CLAUDE.md` milestone table gains the row. Resume
> condition, as a test: `CREATE TABLE … HEAP` succeeds and parser-v2 §8's
> three-way equivalence corpus is green at `cores = 4` under tsan.

**What it is not.** It is not a removal, not a deprecation with a horizon,
and not a statement that the heap is wrong. Nothing below the parser
changes: `ClusteredType::kHeap` stays a live value, a heap volume mounts,
its chain walks, its redo and undo replay, and `AdmitExplicitRowId` still
refuses a key below the high-water mark for §3.1b's reasons. A reader who
takes it for a removal will delete code this order says to keep.

**But "a refusal at creation and a default, and nothing else" is not true of
the tree, and the first draft of this section said it was.** Three built
subsystems are heap-*gated*, so suspending heap creation makes them
unreachable for every relation a user can now make:

- **The relayout planner** builds plans only for `kHeap && !system_relation`
  (`src/stats/relayout_planner.cpp:89`, `:238`). On a fresh volume
  `SHOW RELAYOUT` can never propose a plan for anything — the whole of the
  physical optimizer's Part I is dark.
- **The bulk sorted-fill fast path** requires `clustered_type == kHeap`
  (`src/server/command_dispatcher.cpp:7416`), so `bulkinsert.md`'s T3-2 gate
  is dead for new relations.
- **Range routing** keys on `heap_omitting_pk`
  (`src/server/command_dispatcher.cpp:7224`), so **no new relation can ever
  be range-split** — which reaches the K-series and every crosscore claim
  that rests on a relation having more than one range.

None of `physical-optimizer.md`, `bulkinsert.md` or `crosscore.md` carries
the §3.1b banner, and `CLAUDE.md`'s milestone rows for all three still read
as unqualified "Built". **That is a documentation debt this order created
and did not pay**, and it is larger than the suspension itself: an operator
reading the milestone table would believe three subsystems work on relations
they can no longer make. AS-Q5 is where the operator's word on it goes.

One more thing the sentence hid: the catalog still creates heap relations at
bootstrap (`src/catalog/catalog.cpp:601`, `:765` — every `sys.*` table), so
"no new heap relation is created" is true of `CREATE TABLE` only.

It is also not a milestone of AR0's chain. AS depends on no other work
order and blocks none, which is why it can land between AO stages.

## AS-2 — Where this sits

| against | relation |
|---|---|
| AR0 §8's milestone chain | **outside it.** AS is not M0–M3 and gates nothing in them |
| AO (M2, the lock family) | AO-S3b's `MidWalkWaitTest` is the first fixture that would silently lose coverage to the flip — see AS-R5. Nothing else in AO names a storage form |
| AR2's borrow model | untouched: the unit table is keyed on relation/range/page/slice/tuple, not on storage form |
| `docs/inflight/bugs/` | takes any heap defect found while suspended, per the order — the entry names the commit it was verified at, as every entry there does |
| the letter `AS` | claimed here. **AR0-5's body used `AS-P1` and `AS-E` speculatively for stages that did not exist**; the `critics-developer` pass on that document found both undefined. `AS-P1` is now retired as a name — `page.md` §6's rewrite is AM-S5's (`workorder-am-m1-shared-pool.md:228`) — and `AS-E` was a mis-citation of D10's weight gate. Neither belongs to this work order |

## AS-3 — The survey at `07c94c7`

**A. Where the storage word is parsed.** `src/parser/parser.cpp:634-661`,
inside the trailing-word loop that also takes `EXPLICIT` and `ASSIGNED`.
The loop already carries the exact precedent this order needs three cases
down: **`ASSIGNED` is refused `Unsupported` with its byte offset**, argued
from `CLAUDE.md`'s truthfulness rule — "accepting it would be accepting a
spelling and enforcing something other than what was written". `HEAP` takes
the same shape and the same place.

**B. The default is spelled *three* times, and only one of them is the one
that runs.** The first draft of this section said twice, and the miss was
found by review, not by the flip — which is the argument for counting rather
than asserting. The third is `src/server/command_dispatcher.cpp:2679`,
`HandleCreateTable`'s bare `CREATE TABLE <name>` debug form, which hardcoded
`kHeap`. It is inert today (the schema is empty and `CreateTable` refuses
"no columns" first), but a `kHeap` sitting on a *creation* path is exactly
what this order forbids, and it goes live the moment that refusal moves.
The two that matter: `include/kds/parser/ast.hpp:415` sets
`clustered = ClusteredType::kHeap`, and
`src/server/command_dispatcher.cpp:5430-5431` then ignores it:

```
const catalog::ClusteredType clustered =
    stmt.clustered_given ? stmt.clustered : catalog::ClusteredType::kHeap;
```

So on the dispatch path the AST default is dead and the ternary decides.
`clustered_given` (`ast.hpp:428`) already exists and already distinguishes
"the writer asked for a heap" from "the writer said nothing", which is the
field a flip would otherwise have had to add. **A flip that changed only
one of the two spellings would be silently half-applied**, and which half
depends on the caller — which is AS-R2.

**C. ~525 of 759 test relations ride the default — but only 41 cells care.**
`CREATE TABLE` appears **759** times under `tests/` and `sim/`; **208** name
`BTREE` and **26** name `HEAP`, leaving roughly 525 on the default. Those
525 all become btree when the default flips, which is what made the sizing
look like a 500-file edit.

**Measured rather than guessed** (2026-09-05, at `ecf17cc`): flipping both
spellings of the default and running the suite fails **41 cells in 13
fixtures** — `RangeEquivalenceTest` 17, `RelayoutPlannerTest` 4,
`RangeChainTest` 4, `CommandDispatcherTest` 3, then `SuppliedKeySqlTest`,
`ShowRelayoutTest`, `InsertWalTest` and `BulkInsertTest` at 2 each, and one
each in `WalGoldenLog`, `RangeScanTest`, `IndexDdlTest`, `ForeignKeyTest`
and `DispatcherLogTest`. The other ~484 relations never asserted anything
about their storage form, which is why the flip is **S** rather than M: the
bypass is applied per *fixture*, and there are thirteen of them.

The measurement was an accident worth recording, because it was first
reported as a failed probe: the run collided with a concurrent subagent
holding the build directory, and the flip then survived a blanket
`git checkout` into a later suite run, where its 41 failures were briefly
read as a regression in unrelated work. The number was always there; the
first reading of it was not.

**D. Some fixtures bypass the dispatcher already.** Eight-plus test files
read `ct.clustered` straight off the parsed statement and hand it to a
catalog helper (`tests/step_vm_mvcc_test.cpp:62`,
`tests/aggregate_contract_test.cpp:99`, `tests/exec_chain_test.cpp:71`,
`tests/inner_build_fixture.hpp:81`, and others). These never reach
`command_dispatcher.cpp:5431`, so **for them the `ast.hpp` default is
live**. Both spellings therefore have real callers, which is why AS-R2
collapses them rather than picking one.

**E. ctest can label without moving a cell.** `tests/CMakeLists.txt:211` is
a single `gtest_discover_tests(kds_tests)`. CMake here is **3.28.3**, and
`TEST_FILTER` on `gtest_discover_tests` landed in 3.22 — so two discovery
calls over the one binary, split by a gtest name filter, give the label
with no target split and no cell moved. AS-R4.

**F. The resume condition cites a section that no longer holds it.** The
order says "parser-v2 §8's three-way equivalence corpus". At `07c94c7`
`docs/spec/parser-v2.md` §8 is **"Open decisions — do not assume"** and its
body is one line: "The decisions this section listed are unrecorded here."
The corpus is **§9 item 8**, "Execution equivalence: every case runs
heap×heap, heap×btree and btree×btree with identical rows in identical
order". The numbering moved in the 2026-09-02 compaction. A resume
condition pointing at an empty section is not executable, so AS-R6 restates
it — and note the corpus **cannot be green while heap is suspended**, since
two of its three arms create heap relations. That is coherent rather than a
contradiction: it is the resume test, and it runs through the bypass or
after the un-suspension, never in the gate meanwhile.

## AS-4 — Rulings (CLA's proposals unless marked)

**AS-R1 — `Unsupported`, and the message names SUS-1. [operator]** Given in
the order. Recorded here with the tension stated rather than smoothed over:
`status.hpp`'s test is that `Unsupported` is "what the architecture cannot
admit" and `NotImplemented` is "what nobody built", and heap is **built**,
so neither reading is exact and the operator has chosen the first. The
message must therefore carry *why*, naming SUS-1 and this file, or a reader
meeting it will conclude the engine cannot do something it plainly can. The
byte offset of the offending token is required as for every refusal.

**AS-R2 — One spelling of the default, not two.** Set `ast.hpp:415` to
`kBtree` **and** change `command_dispatcher.cpp:5431`'s fallback to
`kBtree`, and rewrite `ast.hpp:426-427`'s comment, which currently says
kHeap is both "the writer asked for a heap" and "the writer said nothing" —
true today and false after the flip. Preferred over deleting one spelling:
AS-3 D shows both have live callers, and a single shared constant is the
follow-up this order does not need to take.

**AS-R3 — The bypass is parser-level, and unreachable from the wire.** The
refusal is at parse, so the bypass must be too. A setter on the parser (or
on the dispatcher, forwarded), defaulting to refusing, set only by test
fixtures. **Three things it must not be**: a config key, anything readable
from KWP or the debug text port, or `#ifdef NDEBUG`-gated — release test
builds need it, and a debug-only bypass would make the gate and the nightly
run different engines. It is a test seam, and the header says so.

**AS-R4 — The label by filter, not by target.** Two
`gtest_discover_tests(kds_tests …)` calls: the gate's with a negative
`TEST_FILTER`, the suspended one with the positive filter and
`PROPERTIES LABELS heap-suspended`. This needs a **naming convention** on
the suspended cells, which is the cost — propose a `HeapSuspended` fixture
or cell-name infix, chosen because it is greppable and survives a rename
better than a line-numbered list in CMake.

**AS-R5 — A fixture keeps the heap path only where the heap path is what it
tests.** The bypass is not for every fixture that happens to default. Two
classes, and they get opposite treatment:
- Cells that *are about heap storage* (tail append, the below-the-mark
  refusal, chain walks) take the bypass **and** the label: still exercised,
  not gating.
- Cells that merely needed *a table* take the flip and become btree, with
  no bypass. Most of the ~525 are these.

The worked example is AO-S3b's `MidWalkWaitTest`
(`tests/txn_2pc_protocol_test.cpp`): it creates `t` on the default and `tb`
explicitly `BTREE`, precisely so the position-resume and key-resume arms
are both covered. After the flip `t` is btree and the two tables test the
same walk twice, silently. `t` must be created through the bypass, or
`WalkHeapChains` loses its only executing cells.

**AS-R6 — The resume condition, restated so it is executable.** Both halves
must hold, at `cores = 4` under tsan: (a) `CREATE TABLE … HEAP` succeeds
without the bypass, and (b) **`docs/spec/parser-v2.md` §9 item 8**'s
three-way corpus — heap×heap, heap×btree, btree×btree — is green. §8 is
where the order pointed and is empty since the compaction. Written as a
labelled, skipped cell so the condition is a test rather than a paragraph.

**AS-R7 — A heap defect found meanwhile is filed, not fixed. [operator]**
`docs/inflight/bugs/`, naming the commit it was verified at.

## AS-5 — Stages

| stage | deliverable | cells | size |
|---|---|---|---|
| **AS-S0** | This document; `heap-and-tuple.md` §3.1b's SUSPENDED banner (no deletion); `CLAUDE.md`'s milestone row; `index.md`'s row | none — prose only, no engine code, and the suite is untouched, which is what makes this landable on its own | the hour |
| **AS-S1** | The refusal (AS-R1); the default flip, both spellings and the comment (AS-R2); the bypass (AS-R3); the label and its naming convention (AS-R4); the fixtures that keep heap (AS-R5); `manual/sql/sql.md`; the resume cell (AS-R6) | `CREATE TABLE … HEAP` refused `Unsupported`, naming SUS-1, with the token's byte; an unqualified `CREATE TABLE` produces a **btree** relation, asserted through `DESCRIBE`; a volume created before the flip still mounts, reads and writes its heap relations; the bypass produces a heap relation and is reachable from no wire path; `WalkHeapChains` still has an executing cell (AS-R5's example); `ctest -L heap-suspended` selects exactly the suspended set and the gate excludes it | **S, measured.** AS-3 C: the flip alone fails 41 cells in 13 fixtures, not the ~525 that ride the default, so the bypass is applied thirteen times |

## AS-6 — Row status (CLA, appended as rows land)

| stage | status |
|---|---|
| AS-S0 | **written 2026-09-05** on `worktree-ar2-borrow-model-2` at `07c94c7`; the survey is source-read at that commit; no engine code changed; the suite was not run for this stage and is not claimed |
| AS-S1 | **Built 2026-09-05** on `worktree-ar2-borrow-model-2` from `ecf17cc`, and **substantially corrected by its `critics-developer` pass** — the corrections are the useful part of this row. **The mechanism**: the refusal sits in the trailing-word loop beside `ASSIGNED`'s (`src/parser/parser.cpp`) and carries the token's byte, the string `SUS-1` and this file's path, because `Unsupported` on something that is *built* otherwise reads as "the engine cannot do this" (AS-R1). The default moved in all **three** spellings (AS-R2, and see AS-3 B — the first draft said two). The seam is `parser::SetHeapStorageAllowedForTest` with a `SuspendHeapStorageForTest` guard, armed binary-wide by `tests/heap_suspension_env.cpp` and, since the review, by `sim/sim_main.cpp` as well. **Three defects a green suite could not have shown, all found by review and all fixed.** (a) **`ckdbs-sim` was dead.** `sim/workload.cpp:196` emits the storage word itself, the sim is a separate executable that does not link the test seam, and ~70% of seeds died at op 0 — an iteration `sim/loop.cpp:640` says "verified *nothing*". The gate stayed green because `sim_loop_test` runs the corpus **inside** `kds_tests`, where the seam is armed: the gate passed while the binary an operator sweeps with was broken. (b) **The golden parser corpus was silently disarmed**, recording a verdict no client can observe; re-arming it (`SuspendHeapStorageForTest` around the two corpus-driven cells) immediately caught a verdict change nobody had noticed — `CREATE TABLE t (id int) HEAP BTREE` was `InvalidArgument` and is now `Unsupported`, because `HEAP` is refused where it is read and the duplicate-storage refusal is never reached. `pattern_id`/`arg_hash` did not move, so `kFingerprintVersion` is untouched. (c) **AS-R5's own worked example was not done.** The ruling names `MidWalkWaitTest` as the fixture that would lose coverage; after the flip AO-S3b's **position-resume arm — the one that shipped `UPDATED 0` at `07c94c7` — executed nowhere**, and four more heap/btree twin fixtures went the same way, two of them the **waystone and cabin contract suites** `CLAUDE.md` names as comparing configurations byte-for-byte. Each halved what it compares with no cell failing. **The refusal cell was mutation-proven weak**: it asserted `"ERR"`, so changing `Unsupported` to `NotImplemented` left it passing — against a ruling whose entire subject is which of those two codes this is. It now asserts the token and the code. **The regex was 4x too broad.** 114 pins; stripping all of them fails exactly **40** cells, and the rest cost coverage rather than buying it — `insert_wal_test.cpp` had pinned the whole INSERT-WAL contract to heap, the one form nobody can create, leaving the form everybody now gets covered nowhere. Cut back by the same empirical method: strip, run, re-pin only what fails. Two cells moved to **btree** rather than being pinned, because their subject was the `DESCRIBE` line rather than the storage form. **Not delivered.** The `heap-suspended` label is a mechanism applied to nothing (AS-R4): with the seam armed every heap cell passes in the gate, and this tree has no nightly runner (AS-Q4), so labelling would remove those cells from the only thing that runs them. **An earlier version of this row claimed the gate excludes the label; it does not** — `scripts/test.sh` carries no `-LE`, which the review caught and which is now fixed in that script. The resume condition ships as a `DISABLED_` cell asserting the half that is expressible, against `parser-v2` **§9 item 8**. **Two claims this stage made and had to withdraw**: the default is spelled three times, not two; and "a refusal at creation and a default, and nothing else" is false — the relayout planner, the bulk sorted fill and range splitting are all heap-gated and now unreachable for every new relation, which is AS-Q5 and is larger than SUS-1 itself. **Suite: 3344/3344; `ckdbs-sim` verified on four seed/mode/fault combinations.** Overhead not measured |

## AS-0 — Items for the operator

| # | item | CLA proposal |
|---|---|---|
| AS-Q1 | The order says the default flips "if it is HEAP". **It is** (`ast.hpp:415`, and the dispatcher's fallback at `command_dispatcher.cpp:5431`), so the flip is in scope — confirming because the conditional phrasing suggests the operator was not certain | flip both, per AS-R2 |
| AS-Q2 | The resume condition's citation is wrong (§8 → §9 item 8, AS-3 F) | correct it in the order's record; AS-R6 carries the corrected form |
| AS-Q3 | `Unsupported` for something that is built and suspended stretches `status.hpp`'s test (AS-R1). A third code is **not** proposed — `CLAUDE.md` forbids a second name for a quantity an existing one expresses — but the tension is real and the message is what carries it | keep `Unsupported`, make the message name SUS-1 |
| AS-Q4 | Nightly is named as the cadence for the `heap-suspended` label, but this repository has no nightly runner in the tree | say where nightly runs, or accept that the label means "excluded from the gate, run on request" until one exists |
| AS-Q5 | **Three built subsystems go dark for every new relation** (AS-1): the relayout planner, the bulk sorted fill, and range splitting. Their specs carry no banner and `CLAUDE.md` calls all three "Built" | banner each spec and qualify the three milestone rows, in the same shape §3.1b took. This is bigger than SUS-1 itself and CLA did not do it unasked — the operator's order named `heap-and-tuple.md` §3.1b and the milestone table, not these |
| AS-Q6 | **`tools/benchmark.py`'s `--clustered heap` is the default and emits no storage word**, so it now measures a **btree** and labels every `bench/v3.0.0/` header heap. Six more tools share the shape; three others emit explicit `HEAP` and will simply be refused | make the clause unconditional so `--clustered heap` fails loudly with the SUS-1 message rather than mislabelling a result. `bench/`'s rules are the operator's, so CLA left it |
