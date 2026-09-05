# AS — SUS-1: heap relations suspended

Status: **ordered by the operator, 2026-09-05.** AS-S0 is this document and
the prose that goes with it; AS-S1 is the code and has not been built.
Author: CLA, on `worktree-ar2-borrow-model-2` at `07c94c7`
Size: **S**, with the one thing that could make that wrong named in AS-3.
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
refuses a key below the high-water mark for §3.1b's reasons. **The
suspension is a refusal at creation and a default, and nothing else.** A
reader who takes it for a removal will delete code this order says to keep.

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

**B. The default is spelled twice, and only one of them is the one that
runs.** `include/kds/parser/ast.hpp:415` sets
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

**C. ~525 of 759 test relations ride the default.** `CREATE TABLE` appears
**759** times under `tests/` and `sim/`; **208** name `BTREE` and **26**
name `HEAP`, leaving roughly 525 on the default. **This is the one number
that could make the S sizing wrong**: those relations become btree the
moment the default flips, and any of them asserting tail-append order, the
below-the-mark `OutOfRange`, or chain-walk page counts will fail. The
operator's "fixtures keep a test-only bypass" is therefore not a
convenience — it is what makes the flip landable as one change rather than
a 500-file edit.

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
| **AS-S1** | The refusal (AS-R1); the default flip, both spellings and the comment (AS-R2); the bypass (AS-R3); the label and its naming convention (AS-R4); the fixtures that keep heap (AS-R5); `manual/sql/sql.md`; the resume cell (AS-R6) | `CREATE TABLE … HEAP` refused `Unsupported`, naming SUS-1, with the token's byte; an unqualified `CREATE TABLE` produces a **btree** relation, asserted through `DESCRIBE`; a volume created before the flip still mounts, reads and writes its heap relations; the bypass produces a heap relation and is reachable from no wire path; `WalkHeapChains` still has an executing cell (AS-R5's example); `ctest -L heap-suspended` selects exactly the suspended set and the gate excludes it | **S–M** — S if AS-3 C's ~525 default relations turn out not to assert heap behaviour, M if they do. **Measure this before promising the size**: flip the default alone on a scratch commit and read the failure count |

## AS-6 — Row status (CLA, appended as rows land)

| stage | status |
|---|---|
| AS-S0 | **written 2026-09-05** on `worktree-ar2-borrow-model-2` at `07c94c7`; the survey is source-read at that commit; no engine code changed; the suite was not run for this stage and is not claimed |
| AS-S1 | not started |

## AS-0 — Items for the operator

| # | item | CLA proposal |
|---|---|---|
| AS-Q1 | The order says the default flips "if it is HEAP". **It is** (`ast.hpp:415`, and the dispatcher's fallback at `command_dispatcher.cpp:5431`), so the flip is in scope — confirming because the conditional phrasing suggests the operator was not certain | flip both, per AS-R2 |
| AS-Q2 | The resume condition's citation is wrong (§8 → §9 item 8, AS-3 F) | correct it in the order's record; AS-R6 carries the corrected form |
| AS-Q3 | `Unsupported` for something that is built and suspended stretches `status.hpp`'s test (AS-R1). A third code is **not** proposed — `CLAUDE.md` forbids a second name for a quantity an existing one expresses — but the tension is real and the message is what carries it | keep `Unsupported`, make the message name SUS-1 |
| AS-Q4 | Nightly is named as the cadence for the `heap-suspended` label, but this repository has no nightly runner in the tree | say where nightly runs, or accept that the label means "excluded from the gate, run on request" until one exists |
