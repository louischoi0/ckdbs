# Work order BM — the scan row: a walk that owns its slot loop, and a compiled filter that rejects before it decodes

Written 2026-10-12 **in the main checkout on `main` at `ade5bc20`**
(`v2.7.0-843-gade5bc20`), with no worktree: a document-only change, pushed
to `main` directly on the operator's W2 below. It follows the operator's
words of 2026-10-12, recorded in English (`raft-marks-2026-10-12.md`
§1-§3):

- **W0:** *"The scan is slow right now. Propose ways to improve it, and a
  direction for scans."*
- **W1:** *"Is the scan kernel the same concept as the scan ring we
  reviewed before?"* CLA answered no: the scan ring (`eviction.md` EV6) and
  BG's scan queue decide which frames a scan's pages occupy and in what
  order they are reclaimed, which is the buffer pool's question; the scan
  kernel is the executor's per-row CPU on pages already resident. Neither
  moves the other's number (§1.8).
- **W2:** *"Write the workplan document for that improvement and push
  it."*

**Opened 2026-10-13** on `worktree-bm-scan-kernel-open` from `fa80da9d`, on
the operator's words, recorded in English (`raft-marks-2026-10-13.md`
§2-§3): *"Open the scan kernel milestone."* (W3) and *"Adopt the BM-Q
items as CLA proposed."* (W4). BM-Q0..Q12 are marked as proposed, so
every ruling in §2 is adopted. **The `critics-developer` review BM-S0
owed ran at the opening**, after W3 and while W4 was given, so it did not
gate the marks; its findings are applied (§6, "BM opened"), and where one
revised an adopted proposal the revision is recorded as adopted on the
operator's standing go-ahead of 2026-10-13 (go-ahead-achieving-milestone).
**BM runs to its close on that go-ahead** (`raft-marks-2026-10-13.md` §4).

**Claim tags:**
- `[source-read]` with `path:line` at `fa80da9d` (re-read at the opening;
  written at `ade5bc20`), read and not run;
- `[measured]` with the results file that carries the invocation;
- `[design]` for CLA's reasoning about code not yet written;
- `[quiet-wrong]` marks an item where a wrong choice turns a correct answer
  into a wrong one with no error;
- `[provisional]` marks a threshold set before any measurement, to be
  replaced by the measured value.

The operator's words, and a rule that restates them, are decisions rather
than claims, so they carry no tag.

**The letter.** `[source-read]` BL is the last letter on `main`
(`workorder-bl-session-balancer-priority-affinity.md`); no file under
`instructions/` or `docs/` uses BM, and no remote branch carries it. The
operator named no letter; BM is CLA's choice and BM-Q11 puts it to the
operator.

**BM against other orders.**
- **BN, the read and update paths** (`workorder-bn-opt-read-update-path.md`,
  closed 2026-10-11). BN-R4 was *"a scan's per-row cost ... split into
  parts, and the largest part is cut"*. BN cut 3 ns of the row and left the
  rest to `known-gaps.md`'s BN section (*"A scan row costs twice
  PostgreSQL's, and no single part of it dominates"*). That entry rests on
  BN-S1's re-profile, which found no part above a quarter
  (`bench/v3.0.0/archive/bn-s1-census-v2.7.0-769-gb70f79d8/profB1r-reports.txt`).
  BM is that remainder. Its premise is that a distributed
  cost is cut by changing the unit of per-row work, not by another point
  cut.
- **BG, scan resistance** (`workorder-bg-scan-queue.md`, open, BG-S2
  next). A different layer (W1, §1.8). BM changes no access mode: the
  outermost walk still faults `kScan` (BE-R5), and BM-R3's span rule is the
  one EV6 already states, so BG-S2's queue and BM compose in either order.
- **AP01 and AP02** (closed). AP01 is why a rejected row decodes only its
  filter columns (`filter_columns`); AP02 is why `CheckDecodeInputs` builds
  no `Status` on success. BM takes both one step further: a rejected row
  decodes nothing, and a check that cannot change within a walk runs once.
- Not part of AR0 §8's chain.

---

## 0. What BM is

**The intent, in one sentence:** a row the `WHERE` clause rejects costs the
engine a visibility decision and one typed comparison on the bytes already
in the frame - no copy, no `AstValue`, no interpreted residual, no indirect
call per slot - while every row it keeps goes through the residual exactly
as today.

**Three goals:**
- **G1 — the scan row's CPU.** The net per-row server CPU of an unindexed
  scan falls, measured on the BN shape and on the wider shapes of §5.1.
- **G2 — the same answer, the same order, the same counts.** Rows, their
  emission order, `ANALYZE`'s `examined=` and `sel=`, the row-touch budget,
  Waystone trails, Cabin recording and the inner build are unchanged
  (BM-R2). What may change is work: decodes, copies, spill fetches.
- **G3 — the floor stated, not promised.** Every cell is reported against
  the PostgreSQL 18 twin on the same host. Reaching it is not a bar
  (BM-Q12): the profile prices the parts BM replaces, not what replaces
  them.

**Non-goals, stated so no stage grows into them:**
- **Cold reads.** A miss is a synchronous `pread` on the reactor
  (`device_page_store.cpp:999`) and nothing in the engine reads ahead
  (§1.8). That is an I/O order, and it needs the open I/O-backend decision
  (`include/kds/sched/io_backend.hpp:13`).
- **The buffer pool's own scan cost**: BG's queue, EV6's ring, and the
  resident-scan regression of about 270 ns a page in the pool's hit path
  (`known-gaps.md`, Eviction).
- **Skipping pages**: per-page summaries, zone maps, skip bitmaps.
- **Intra-statement parallelism**: a statement runs on one core
  (`crosscore.md` CC1).
- **A code generator.** The kernel is a fixed set of typed comparators
  chosen at bind, not code emitted at run time.
- **New predicate shapes.** `parser-v2.md` I10's filter scope is
  unchanged; BM evaluates what the grammar already admits.

### 0.1 What CLA told the operator before this order

Answering W0, CLA reported the measurement of §1.1 and the per-row
repetitions of §1.2-§1.6, and proposed four directions in order:
- **(A)** the kernel of this order;
- **(B)** a recent row's visibility, measured first;
- **(C)** cold-scan read-ahead, measured first and gated on the I/O-backend
  decision;
- **(D)** not now: page summaries and intra-statement parallelism.

W2 asks for the workplan of that proposal. BM therefore carries (A) as its
body and (B) as a conditional stage. (C) and (D) are named as exclusions
(BM-Q10).

---

## 1. Survey at `ade5bc20`

### 1.1 What BN measured

`[measured]` `bench/v3.0.0/results-bn-close-v2.7.0-787-g8679367e.md` §4,
at `v2.7.0-787-g8679367e`:
- **Shape.** `SELECT * FROM s WHERE w = ?` over 10,000 rows of
  `s (id int64, v int64, w int64)` (`tools/bn_census.py:178`), one row
  matching.
- **Configuration.** `cores = 1`, `relaxed`, `buffer_pool_frames = 65536`,
  everything resident.
- **Result.** Net of a point statement, a scanned row costs 67.2 ns of
  server CPU (literal path) and 67.0 ns (prepared). PostgreSQL 18.6 costs
  34.3 ns on the same host and pinning, a ratio of 1.96. BN moved the row
  by 2.9 ns.

`[measured]` The same file's profile of B (1,394 samples, 708.6 µs per
statement, about ±5 µs per 100 samples; a frame-pointer build) puts 93 % of
the statement in the row walk. Per row, self time:

| function | per row | what it is |
|---|---|---|
| `ChainRunner::AcceptTupleAt` | 11.7 ns | the per-row body below |
| `DecodeColumnsInto` | 8.0 ns | the mask loop |
| `std::_Function_handler::_M_invoke` | 6.6 ns | the walk's callback |
| `BtreeVisitLeafPage` | 6.5 ns | the slot loop |
| `DecodeOneValueInto` | 5.6 ns | one int64 into an `AstValue` |
| `EvaluatePredicate` | 4.3 ns | the conjunct |
| `PageView::ReadTuple` | 4.1 ns | the slot and header read |
| `DecodeAndResolve` | 3.3 ns | the decode's entry |
| `CheckDecodeInputs` | 2.8 ns | four checks |
| `EvaluateAll` | 2.6 ns | the conjunct loop |
| `CompareValues` | 2.5 ns | the comparison itself |
| `EvaluateConjunct` | 2.4 ns | the malformed-chain checks |
| `ExecStats::For` | 1.9 ns | a counter lookup |
| `memcpy` | 1.8 ns | the row copy |

Inclusive, the decode is 19.9 ns and the residual 11.5 ns. With the
callback, those three are 38.0 ns of the 67 ns.

**What that says.** The filter reads one int64 column. Turning that column
into a comparison costs 31 ns (decode and residual), and `CompareValues`,
the comparison itself, is 2.5 ns of it. The rest is the shape of the path,
not the data. `known-gaps.md`'s *"no single part of it dominates"* is the
same finding seen from the other side.

`[measured]` The shape is not a corner case.
`git show 36449d28:bench/v3.0.0/results-kds-vs-pg18-summary-v2.7.0-545-gf2f1ee7.md` §4
(deleted from the tree in `26faa557`) measured twelve
library shapes at engine `v2.7.0-531`, with another driver and run on the
same host model (its §1). Its findings:
- unindexed, KDS ran at 0.60x to 0.87x of PostgreSQL, falling with rows;
- at 10,000 rows it ran at 0.62x on one core and 0.60x on eight;
- the cause it named is the row walk, about 70 ns a row against 34 ns.

### 1.2 The walk, per slot

`[source-read]`
- The outermost walk steps pages itself (`step_vm.cpp:2089-2091`), but
  hands each page to `btree::BtreeVisitLeafPage`.
  - That function fetches the leaf once and calls the visitor once per
    slot (`btree.cpp:1291-1314`).
  - It goes through a `const std::function<StatusOr<VisitControl>(...)>&`
    (`:1293`), whose result is checked per slot by `ResolveVisit`
    (`:1306`, `include/kds/storage/visit.hpp:50-58`).
- The visitor is converted to that `std::function` once per walk, not per
  page (`step_vm.cpp:1995-2001`). So the cost is not an allocation but an
  indirect call and a `StatusOr` per slot.
- `StatusOr<T>` is a `Status` and a `std::optional<T>`
  (`include/kds/base/status.hpp:313-314`), and `Status` carries a
  `std::string` (`:276`).
- Per slot, the visitor itself does the following:
  - tests the stop (`step_vm.cpp:1909-1911`);
  - tests a page transition, to count `pages_fetched`;
  - tests the range bound;
  - calls `accept`, which calls `AcceptTupleAt` (`:1938-1944`).
- `kds::FunctionRef` (`include/kds/base/function_ref.hpp:7-11`) removes
  `std::function`'s allocation, not its indirect call: a call through it
  still goes through a thunk. So it does not answer this cost. A template
  visitor, or a slot loop inlined into the VM, does (BM-S2).

### 1.3 `AcceptTupleAt`, per row

`[source-read]` `step_vm.cpp:2199-2558`, for every slot of every page:
- `spills_.clear()`.
- A `PageSpanGuard` and `page.RelayoutEpoch()` (`:2235-2236`).
- `page.ReadTuple(slot)` (`:2237`):
  - it re-reads the page header on every call and returns
    `StatusOr<Tuple>` (`heap_page.cpp:232-270`);
  - `PayloadAt` reads the header once per page (`heap_page.hpp:301-322`).
    The walk's per-row read does not use it; only JB6's keyed mark does
    (`step_vm.cpp:1952`).
  - `PayloadAt` also omits two of `ReadTuple`'s checks: BF-R10's slot bound
    and the payload-within-tuple check (`heap_page.cpp:248-253`, `:264-267`,
    against `:272-292`). It returns no MVCC header either. So it is the
    pattern BM-S2 follows, not the call it makes.
  - A slot `ReadTuple` refuses (out of range, dead, or a `Corruption` for a
    slot leaving the page) is no row today. `if (tuple.ok())` (`:2238`)
    leaves `decoded` false (`:2298`). BM keeps that as it is.
- `txn::Classify` (`:2239`).
- For a visible version, **the whole row is copied into `version_`
  before anything is tested** (`:2265-2266`). The comment there says why:
  the decode happens after the span is released.
- `DecodeAndResolve` (`:2327`), then counters through `stats_.For`
  (`:2332-2334`), then `budget_.ChargeRow()` (`:2404`), then the residual
  (`:2420-2423`).

### 1.4 The decode

`[source-read]`
- `DecodeAndResolve` calls `DecodeColumnsInto` (`src/exec/row_codec.cpp:1172-1184`;
  every `row_codec.cpp` below is this file, not `src/wire/row_codec.cpp`).
- **`CheckDecodeInputs` runs four checks on every call**
  (`row_codec.cpp:980-1012`):
  - the Keystone column is an integer;
  - the layout matches the schema;
  - the target span's size;
  - the payload's length.

  The first three depend only on the schema and layout the walk bound,
  which `Catalog::MemoPin` keeps alive across the walk
  (`step_vm.cpp:1873-1877`). Only the fourth depends on the row
  (invariant 13).
- Each decoded column becomes an 80-byte `parser::AstValue`. Two
  `std::string` clears are paid even for an int64 (`row_codec.cpp:486-503`).

### 1.5 The residual

`[source-read]` `EvaluateAll → EvaluateConjunct → EvaluatePredicate →
CompareValues` (`chain_frame.cpp:29-110`, `row_codec.cpp:1244-1320`):
- per conjunct and per row:
  - three `StatusOr<bool>` returns;
  - the `$param` check;
  - `SchemaFor`;
  - a `CanResolve` check (two when the right side is a column) and a
    column bound check;
  - `CompareValues`' run-time dispatch on the value kinds.
- Every check but the comparison depends only on the chain and the frame's
  shape.

`[source-read]` **What makes a typed kernel possible is already done at
compile.**
- A literal is coerced to its column's type before the chain runs
  (`CoerceLiteralToColumn`, `row_codec.cpp:662`): a decimal to the
  column's scale, a date to its epoch day.
- The file says why: *"Per-row evaluation stays an **int64 comparison**"*
  (`:656`).
- `CompareValues` then reduces every type to one of four comparisons:
  - `uint64` through `ValueAsUint64`;
  - narrow and wide decimal on equal kind and scale;
  - every integer-encoded type (the int widths, bool, date, timestamp) as
    `int64`;
  - strings as `string_view`.

  Any other kind pair is a non-match (`:1260-1319`).
- Column offsets are schema constants (`CellOf`, `row_codec.cpp:408-415`;
  invariant 13).

`[source-read]` **NULL.** `parser-v2.md` I16 says the evaluator collapses
UNKNOWN to false *"in exactly one place ... one function, one call site"*.
That place is `CompareValues`' NULL arm (`row_codec.cpp:1257-1259`). It is
one function, but no longer one call site (`chain_frame.cpp:45`, `:51`,
`:90`, `:96`, `:173`; `step_vm.cpp:2357`). Under the six relational
operators a NULL operand keeps no row. So which path such a row takes
changes no answer, and BM-Q3 is a question of I16's structure and of cost.

### 1.6 A recent row's visibility

`[source-read]` `ReadView::Visible` (`read_view.hpp:122-131`):
- answers `true` from an atomic floor for a writer below it;
- for anything at or above the floor, calls
  `InstanceVisibility::LookupCommit`, which takes the instance's one
  `window_latch_` and searches a `std::unordered_map`
  (`instance_visibility.cpp:322-334`, `instance_visibility.hpp:642`);
- a scan over rows written since the floor therefore takes a latch shared
  by every core, once per row - **above one core only**: since BP-H2
  (`workorder-bp-per-core-residue.md`, BP-S3) the latch is unarmed at
  `cores = 1` (`instance_visibility.hpp:317-320`), and there the lookup is
  the map search alone. BM-Q8's writer cell runs at `cores = 2`, so it
  measures the latched form.

`[measured]` BN's shape loaded its rows before the arms ran, at
`cores = 1`. `LookupCommit` is in none of the profile's top entries, so
that shape most likely never left the floor path. **No measurement exists
of a scan under concurrent writers.**

`[source-read]` What would make a memo sound: `instance_visibility.hpp:519-523`
states that every commit at or below a snapshot's ceiling is already in the
window, *"so a view minted here never changes its answer for any
transaction"* (AN-R9). The floor only rises (`:550-552`).

`[source-read]` A memo needs a second premise: the floor must not rise past
a writer this view cannot see. That is `Reclaim`'s bound,
`min(HorizonLsn(), PendingCommitBound())`, read under the window latch
(`instance_visibility.cpp:350-371`). It binds only a view that is published
to the horizon.

### 1.7 What BM builds on

`[source-read]`
- **The read hold.** A read fetch pins its frame shared, and
  `DevicePageStore` latches it in that mode when armed
  (`page_store.hpp:150-158`).
  - `BtreeVisitLeafPage` holds that `PageRef` across every slot of the
    leaf (`btree.cpp:1295-1314`).
  - So any byte the kernel reads on the page is read under the same hold
    the copy at `step_vm.cpp:2265` is taken under today.
- **The commutation argument**, already written for the inner build's split
  (`step_vm.cpp:2407-2415`): on a well-formed chain, *"AND over total
  predicates commutes"*, so evaluating a subset of conjuncts first changes
  no verdict.
- **The read-path switch pattern**: `indexes = on|off` steers execution,
  never the compiled chain, and the contract suites compare replies byte
  for byte across it (`kds.conf.sample:490-505`).
- **The census and twin drivers** BN left: `tools/bn_census.py`,
  `tools/bn_pg_scan.py`, `tools/kds_sampler.c` with
  `tools/kds_sampler_report.py` and `tools/bn_parts.json`.

### 1.8 What is not BM's (W1)

`[source-read]`
- **The scan ring** (`eviction.md` §5, EV6) is a small set of frames a bulk
  reader cycles through. Its pages bypass retention and never bump usage.
  - The executor does not use it, by decision (`known-gaps.md`, Eviction:
    `OpenScanRing`'s callers are `relayout_planner.cpp` and
    `cabin_optimizer_exec.cpp`).
  - A `SELECT`'s outermost walk faults cold instead (BE-R5,
    `step_vm.cpp:2073-2080`).
- **BG's scan queue** reorders reclamation so a scan larger than the pool
  stops lapping the hot set (`workorder-bg-scan-queue.md` §1.1).
- `[measured]` Both act only when the scan and the working set compete for
  frames. BN's 67 ns was measured with every page resident, so neither
  moves it, and BM moves neither re-fault rate.
- **Cold reads** are a third question: residency is the pool's, latency on
  a miss is the device path's (§0's non-goals).

---

## 2. Rulings

CLA's proposals, adopted with BM-Q0.

### BM-R1 — The kernel never decides a row in `[quiet-wrong]`

The kernel is a pre-filter.
- It may **reject** a row only when the residual would reject it.
- Every row it does not reject is filtered by the residual, unchanged,
  exactly as today.

The only wrong answer the kernel can introduce is therefore a row rejected
that the residual would keep. Every kernel cell aims at that case (BM-S3,
BM-S4). BM-Q2 (b), the kernel as the authority for its conjuncts, is the
alternative.

### BM-R2 — The same rows, in the same order, with the same counts

What stays as it is:
- the rows emitted and their order (`parser-v2.md` I12);
- `pages_fetched`, which `ANALYZE` prints as `pages=`
  (`plan_printer.cpp:396`). It counts a page on which the visitor saw a
  slot (`step_vm.cpp:1913-1921`), so an empty relation or an empty leaf
  still reads 0;
- `rows_examined` and `rows_matched`, and so `ANALYZE`'s `examined=` and
  `sel=` (`step_vm.hpp:77-78`). A kernel-rejected row is examined and not
  matched. The field's comment is restated, because "decoded" stops being
  true of it;
- the row-touch budget, charged once per visible row in the same order
  (`budget.hpp:107-111`, `parser-v2.md` §7), so `ResourceExhausted` falls
  on the same row;
- Waystone recording, Cabin recording and the inner build's bucketing.

What may fall is work:
- decodes and copies;
- spill fetches (`ANALYZE`'s `spills=`);
- var-heap faults.

A row the kernel rejects is never decoded. So a corrupt cell in a column
only the residual reads is not reported for that row. AP01's partial
decode already accepts this for columns no filter reads
(`step_vm.cpp:2300-2308`). The payload-length check stays per row and comes
first (BM-R3).

### BM-R3 — The kernel reads only what the walk already holds `[quiet-wrong]`

- **A visible version** (`txn::Visibility::kVisible`): the kernel reads
  the tuple's payload on the page, inside the span, under the read hold of
  §1.7, before the copy. A rejected row is never copied.
- **A version reconstructed through undo** (`kNeedsUndoWalk`): the kernel
  reads `version_` after `ResolveThroughUndo`, never the page.
  - The page holds a version this reader may not see.
  - Reading it there is the `[quiet-wrong]` this rule exists for.
- **Never a fetch under the span** (`parser-v2.md` I15 R1). A spilled value
  is not compared by the kernel. It is passed to the residual.
- **The payload length is checked before the kernel reads an offset**
  (invariant 13). A disagreeing length is the `Corruption` it is today.

### BM-R4 — What the kernel may hold `[quiet-wrong]`

A conjunct is compiled into the kernel only if all of the following hold;
anything else stays residual-only:
- its left side is a column of the step's own relation (`lhs.up == 0`,
  `lhs.rel_slot` the step's own);
- its operator is one of `=`, `<>`, `<`, `<=`, `>`, `>=`;
- its right side is a literal of the comparison class `CompareValues` would
  use for that column after coercion (§1.5);
- it is not a `$param`.

Comparisons are made in the domain the decoder produces:
- `int64` after sign extension at the cell's width: the int widths, date,
  timestamp and narrow decimal;
- bool, zero-extended (`row_codec.cpp:524-531`);
- `uint64`;
- the 128-bit pair as `Int128FromHalves` (`:1305`), the high half signed
  and the low half unsigned;
- bytes: after `char(N)`'s NUL trim, or from a `varchar` cell read through
  `DecodeCell` (BM-S4).

**Column 0 is the Keystone id, never its cell.**
- Its cell is the whole 8-byte Keystone word, whose bytes 6 and 7 are a
  flags byte and a reserved byte (`include/kds/storage/keystone.hpp:11-16`).
- Its value is the 48-bit id that `DecodeKeystoneInto` takes through
  `KeystoneIdOfPayload` (`row_codec.cpp:1067-1069`,
  `src/storage/keystone.cpp:44-49`).
- A pk bound stays in a range walk's residual (`step_vm.cpp:1851-1853`), so
  it is the first conjunct the kernel would hold.
- The engine writes the flags and reserved bytes as 0 on every row
  (`keystone.hpp:29-35`), so only a hand-built page can tell the id from the
  cell (BM-S3).

**A literal is never narrowed to the cell's width.** An integer literal
outside an `int32` column's range reaches the residual uncoerced
(`step_compiler.cpp:148`, `row_codec.cpp:839-841`) and is compared as an
int64 (`:1313-1314`). So `c32 < 2^40` keeps every row today, and the kernel
must keep it too.

Two non-matches never reach the kernel, because the compiler refuses them:
a negative or oversized literal against a `uint64` column
(`row_codec.cpp:828-836`) and a decimal of another scale (`:755-761`).

A literal of another value kind is not compiled, for example a string
against an int column (`:839-841`, then `:1319`). The residual answers it
as the non-match it is.

**Two consumers restrict the kernel:**
- **Cabin recording.** While the step's Cabin recording is live, the
  kernel is not consulted. A row the other conjuncts reject must still be
  recorded if its key matches (`step_vm.cpp:2336-2350`).
- **The inner build.** The kernel never holds the build's correlated
  conjunct (`building_->residual_pos`). Since BM compiles only literal
  right sides, this holds by construction, and BM-S3 pins it.

### BM-R5 — The walk owns its slot loop

- One fetch and one header read per page.
- No `std::function` call and no `StatusOr` per slot.
- A stop is still a `VisitControl`, never a `Status` (I15 R4). A stop
  touches no page past the one it stopped on.
- No allocation per row (`parser-v2.md` §7).
- `pages_fetched`, the range-bound stop and JB6's resume marks keep their
  per-slot meaning.
- Counters stay in `StepStats`, through `ExecStats::For` - one size
  compare and an index, inlined. *Revised at BM-S2's review and adopted on
  the operator's standing go-ahead of 2026-10-13
  (go-ahead-achieving-milestone):* locals flushed per page were the
  proposal, and a reference cache built in their place cost more compares
  than the lookup it replaced; neither is a measured part of the row.

### BM-R6 — What cannot change within a walk is checked once

The schema and layout half of `CheckDecodeInputs`, and `EvaluateConjunct`'s
malformed-chain checks, run once per step per statement. The flag is kept
per step and set inside `AcceptTupleAt`, so every access kind that reaches
it - a walk, a descent, the probe memo, a Waystone replay, a Cabin resolve
(`step_vm.cpp:2212-2216`) - runs the check before its first row, not the
walk alone. They run at the first visible row, before the kernel reads any
offset, with today's messages. So a chain is still refused at today's
first row even when the kernel would reject every row.
- On a row broken twice - a payload of the wrong length under a malformed
  chain - the message changes: today the decode's length `Corruption` fires
  first (`row_codec.cpp:1006`), after BM-R6 the chain's.
- An empty relation is still not refused.
- **A malformed later conjunct is refused at the first row.** Today it can
  be masked by an earlier conjunct that is false on every row.
- This changes behaviour only on a chain the compiler should never build,
  and only toward refusing (BM-Q7).

### BM-R7 — A recent row's verdict is memoized only if measured `[quiet-wrong]`

If BM-Q8's gate opens BM-S5, it builds two things:
- **The floor read once per page**, as a lower bound. A writer below an
  earlier floor is below the current one (§1.6).
- **A per-walk memo of `LookupCommit` verdicts keyed by transaction id**,
  under the walk's one read view, never shared across statements. It rests
  on two premises:
  - AN-R9;
  - the walk's view being published to the horizon for the walk's life,
    which is what keeps `Reclaim` below a writer the view cannot see
    (§1.6). This is `ReadView::Visible`'s own premise (AN-R12), which the
    memo inherits rather than adds.

  BM-S5 re-reads both at its own commit before building.

### BM-R8 — Measure first

BM-S1 measures the premise before any code stage. Its gate is BM-Q1. The
overhead of the whole change is measured once at the close (Session
Workflow step 3).

---

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BM-S0 | **The order** | <ul><li>This file</li><li>Its review (§6)</li><li>The index row</li><li>`raft-marks-2026-10-12.md` §1-§3</li></ul> | S |
| BM-S1 | **The premise, measured** (BM-R8) | <ul><li>**Code: instrumentation only, no behaviour change:**<ul><li>a count of `LookupCommit` calls, a `Tally` in `base/contention.hpp`: `SHOW META` prints it summed as `contention_visibility_window_lookups` and, above one core, per core as `load_c<k>_visibility_window_lookups` (one relaxed add on the asking core's own slot per lookup, which the writer cell carries on both sides of any A/B);</li><li>`tools/bn_parts.json` split so the row walk's parts (loop and callback, visibility, decode, residual, copy) are separate parts rather than one `descent and page access`.</li></ul></li><li>**The drivers:** `tools/bn_census.py` and `tools/bn_pg_scan.py` extended with §5.1's shapes and their PostgreSQL twins.</li><li>**The measurement** (§5.1), into `bench/v3.0.0/results-bm-s1-scan-row-<describe>.md`.</li><li>**The gates** (BM-Q1, BM-Q8).</li><li>**The suite green.**</li></ul> | M |
| BM-S2 | **The walk owns its slot loop; the walk's invariants checked once** (BM-R5, BM-R6). No result can change | <ul><li>**Code:**<ul><li>the VM owns the leaf's slot loop for a btree relation: it fetches the leaf, checks its type and walks its slots itself, so the shared primitive `btree::BtreeVisitLeafPage` - which `UPDATE` and `DELETE` also reach (`command_dispatcher.cpp:6509`) - and the heap's `heap::ChainVisitOnePage` (`heap_chain.cpp:315`, served for pre-SUS-1 relations) keep their `std::function` form and their callers (not `FunctionRef`, §1.2; chosen at the opening review over a templated primitive, which would have changed both write paths);</li><li>`ReadTuple`'s checks with the header read once per page: BF-R10's bound and the payload check kept, and a refused slot no row, as today (§1.3);</li><li>the once-per-walk check of BM-R6.</li></ul></li><li>**Cells:**<ul><li>a three-leaf relation emits in slot order across both boundaries;</li><li>`LIMIT 1` and a true `EXISTS` stopping mid-page leave `pages=` at 1 and `examined=` at the stopping row's ordinal plus one;</li><li>an empty relation and an empty leaf leave `pages=` at 0;</li><li>a slot whose offset leaves the page is no row, as today;</li><li>a hand-built chain with an out-of-range `col_pos` is refused `Corruption` with today's message at the first row, and not on an empty relation - through a walk and through a lookup step;</li><li>an inner-build prefix resuming mid-page (JB6, `step_vm.cpp:1946-1981`) emits each row once;</li><li>a range walk stops on the first slot past `range_high` with `range_pages_pruned` at 1 (`:1927-1929`);</li><li>a payload of the wrong length is refused as today.</li></ul></li><li>**Mutations, each repeated:**<ul><li>the stop deferred to the page's end (killed by the range cell's `range_pages_pruned` and the wrong-length cell's rows: a stop changes no counter the sink-stop cell reads);</li><li>the once-per-statement check skipped (killed by the malformed-chain cell);</li><li>the check's flag set by the walk only (killed by the lookup-step arm of that cell).</li></ul></li><li>**The contract suites byte-identical** (waystone, index, cabin, inner-build) and the suite green.</li></ul> | M |
| BM-S3 | **The kernel, integer-encoded types** (BM-R1..R4): the int widths, `uint64`, bool, date, timestamp, decimal, `decimal128` | <ul><li>**Code:**<ul><li>the kernel compiled per step at bind from the residual (BM-R4);</li><li>evaluated in `AcceptTupleAt`, the one place every read access kind reaches (`UPDATE` and `DELETE` filter through `EvaluateConjuncts`, `step_vm.cpp:2874`, `command_dispatcher.cpp:8419-8420`, and are not BM's), before the copy for a visible version and after the undo walk for a reconstructed one (BM-R3);</li><li>a rejected row examined and charged (BM-R2);</li><li>the switch, as BM-Q5 marks.</li></ul></li><li>**Cells:**<ul><li>**the differential matrix**: for every kernel type × the six operators × literals at each boundary (the type's min and max, ±1, 0, `INT64_MAX` and above for `uint64`, negative and positive `decimal128` halves), the kernel's verdict equals `CompareValues`' for every non-NULL operand, and it rejects no row whose operand is NULL (BM-Q3);</li><li>a literal above an `int32` column's range answers as today's int64 comparison under all six operators: every row under `<`, `<=` and `<>`, none under `=`, `>` and `>=`;</li><li>a pk-range conjunct on column 0 over a hand-built leaf whose Keystone flags or reserved byte is nonzero (the engine never writes one, `keystone.hpp:29-35`), and over an `int32` and an `int16` pk;</li><li>`decimal128` values that differ only in the low half, across 2^63;</li><li>a malformed non-kernel conjunct, on a relation whose every row the kernel rejects, is still refused at the first row (BM-R6);</li><li>**MVCC**: a committed row with `w = 5` updated to `w = 6` by a writer the reader cannot see is returned by `WHERE w = 5`;</li><li>**recording**: under a live recording of `sym`, a row with `sym = 'A'` that `qty > 5` rejects enters the set, and a later served `WHERE sym = 'A'` returns it;</li><li>**budget**: with `max_rows_touched` below the relation and every row kernel-rejected, `ResourceExhausted` falls on the same row as with the kernel off;</li><li>`ANALYZE`'s `examined=` and `sel=` equal on and off;</li><li>the inner build's correlated conjunct is never in a kernel.</li></ul></li><li>**Mutations, each repeated:**<ul><li>`uint64` compared signed (killed by the `INT64_MAX` cell);</li><li>`<=` compiled as `<` (killed by the boundary cell);</li><li>column 0 read as its 8-byte cell (killed by the flags cell);</li><li>column 0 sign-extended at its declared width (killed by the narrow-pk arm of the flags cell);</li><li>a literal truncated to an `int32` column's width (killed by the above-range cell);</li><li>sign extension omitted on a narrow int (killed by the matrix's minimum values);</li><li>the `decimal128` low half compared signed (killed by the low-half cell);</li><li>the kernel reading the page for a reconstructed version (killed by the MVCC cell);</li><li>the kernel consulted during recording (killed by the recording cell);</li><li>a kernel-rejected row not charged (killed by the budget cell).</li></ul></li><li>**The contract suites byte-identical with the switch on and off**, and the suite green.</li></ul> | M |
| BM-S4 | **The kernel, strings**: `char(N)` and an inline `varchar` cell | <ul><li>**Code:**<ul><li>`char(N)` compared after the NUL trim `DecodeOneValueInto` applies;</li><li>a `varchar` cell compared only through `DecodeCell` (`src/storage/tagged_cell.cpp:99-123`), and only when it answers an inline value. Any other answer (spilled, NULL-tagged, or a refusal) goes to the residual, which answers or refuses it as today.</li></ul></li><li>**Cells:**<ul><li>values shorter than N and exactly N;</li><li>a literal longer than N;</li><li>a spilled value equal to the literal is returned;</li><li>a NULL-tagged cell under a present bit is still `Corruption`;</li><li>an inline cell whose length exceeds its capacity is still `Corruption`.</li></ul></li><li>**Mutations, each repeated:**<ul><li>the trim removed (killed by the short-value cell);</li><li>a spilled cell compared by its inline bytes (killed by the spilled cell);</li><li>`DecodeCell` bypassed and the inline length trusted (killed by the over-capacity cell).</li></ul></li><li>**The suite green.**</li></ul> | S |
| BM-S5 | **A recent row's visibility** (BM-R7), **only if BM-Q8's gate opens it** - **struck at BM-S1** (3.8 % against 10 %, §6) | <ul><li>**Code:**<ul><li>the floor read once per page;</li><li>the per-walk verdict memo.</li></ul></li><li>**Cells** (two-core rig):<ul><li>a writer committing above the reader's snapshot mid-walk stays invisible for every row of it;</li><li>a floor rising mid-walk changes no verdict;</li><li>a second statement after the commit sees it;</li><li>a page holding the rows of two writers, one visible and one not, answers each row by its own writer.</li></ul></li><li>**Mutations, each repeated:**<ul><li>the memo kept across statements (killed by the second-statement cell);</li><li>the memo keyed by page rather than by transaction (killed by a page holding two writers' rows).</li></ul></li><li>**The suite green** and the sim corpus.</li></ul> | S |
| BM-S6 | **The close** | <ul><li>**The measurement** (§5.2).</li><li>**Text:**<ul><li>`parser-v2.md` I15 and §7 (the kernel, BM-R1..R5);</li><li>`kds.conf.sample` for the switch;</li><li>`step_vm.hpp`'s `rows_examined` comment;</li><li>CLAUDE.md's query-language row.</li></ul></li><li>**Docs:**<ul><li>`known-gaps.md`'s BN scan entry restated with the close's number, or deleted if it no longer holds;</li><li>what BM did not cut, named there.</li></ul></li></ul> | S |

## 4. Items for the operator

Every item marked 2026-10-13 as CLA proposed (`raft-marks-2026-10-13.md`
§2-§3). Every proposal is `[design]` unless a cell says otherwise.

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BM-Q0 | **Open BM**, with BM-S0..S6 and BM-R1..R8 as written | process | Yes, sequenced as BM-Q9 marks | Opened 2026-10-13 (`raft-marks-2026-10-13.md` §2) |
| BM-Q1 | **BM-S1's premise gate.** BM stops for a ruling when, on the BN shape and on §5.1's wide shape, the parts BM-S2..S4 replace are under one third `[provisional]` of the net row. Those parts are the loop and callback, the per-row checks, the copy, the decode of a rejected row, and the residual on a rejected row | process | As written. At BN's close they were 38 ns or more of 67 (§1.1). The gate asks whether that holds on wider rows and at other selectivities | As written |
| BM-Q2 | **The kernel's authority.**<br>(a) A pre-filter: it only rejects, and the residual decides every row it passes (BM-R1).<br>(b) Authoritative: a row passing every kernel conjunct skips them in the residual | design, **[quiet-wrong] under (b)** | (a). Under (a) a defect can only drop a row, and the differential matrix is aimed at exactly that. Under (b) a defect can also admit one. (b) also saves nothing on a selective scan: the residual only runs on the rows the kernel passes. §5.1's 100 % cell prices what (a) costs when every row passes. (b) is reconsidered only if that cost is material | (a) |
| BM-Q3 | **A NULL operand.**<br>(a) The kernel passes a row whose operand's NULL bit is set to the residual, so I16's single collapse stays the only one.<br>(b) The kernel rejects it through a helper shared with `CompareValues`, and I16 is amended to one function and two call sites | design | (a). The two are result-equivalent: a NULL operand keeps no row under the six operators (§1.5). (a) keeps the collapse in `CompareValues` alone, at the cost of a residual call on each NULL row, which §5.1's nullable cell measures. (b) is taken only if that cost is material | (a) |
| BM-Q4 | **Counters and the budget** (BM-R2): a kernel-rejected row is examined, unmatched and charged once, so `examined=`, `sel=` and `ResourceExhausted` are unchanged. `spills=` may fall | design | As written. Charging per page instead would move the row a budget refuses on | As written |
| BM-Q5 | **The switch.**<br>(a) `scan_kernel = on\|off`, a read-path key like `indexes`, on by default. The contract suites run both ways.<br>(b) No key; a test-only build flag | user-visible | (a). It is the tree's own pattern for an accelerator that must not change an answer, and it gives the close an A/B on one binary. The name is the operator's to change | (a), `scan_kernel` |
| BM-Q6 | **The type waves.** BM-S3: the integer-encoded types. BM-S4: `char(N)` and inline `varchar`. A spilled value never | scope | As written. BM-S3's types are one `CompareValues` arm each and need no allocation. Strings come second because their trim and tag rules are where a drop would hide | As written |
| BM-Q7 | **The once-per-walk check** (BM-R6): a malformed later conjunct is refused at the first row, even where an earlier false conjunct masks it today | design | Yes. It changes only a chain the compiler should never build, and only toward refusal | Yes |
| BM-Q8 | **BM-S5's gate.** BM-S5 is built only if §5.1's writer cell shows `LookupCommit` and its latch at more than 10 % `[provisional]` of the scan's server CPU, by the sampler | process | As written. Otherwise BM-R7 is recorded as measured-not-material and BM-S5 is struck | As written |
| BM-Q9 | **Sequencing.**<br>(a) BM opens now, in its own worktree; BM-S1's gate decides whether S2..S5 follow.<br>(b) BM waits for BL | process | (a). BI, the open milestone this order was first sequenced against, closed in code at `ec827a19`. BL is not opened, and BL and BM share no ruling | (a) |
| BM-Q10 | **Out of BM**:<ul><li>cold-scan read-ahead (an I/O order, after the I/O-backend decision);</li><li>page summaries and zone maps;</li><li>intra-statement parallelism;</li><li>the pool hit path's 270 ns a page</li></ul> | scope | Out. Each is its own order if measured material. A page summary also needs an MVCC argument BM does not make: a summary over the page's current versions does not cover the older versions an older snapshot reads through undo | Out |
| BM-Q11 | **The letter** BM, chosen by CLA | naming | BM | BM |
| BM-Q12 | **The close** (§5.2) and its bar:<ul><li>no pass/fail against PostgreSQL;</li><li>the 100 % output scan, the point `SELECT` and the every-row `UPDATE` each within the spread between A and A', a rebuild of A's identical code</li></ul> | measurement | As written, revised at the opening review: "run-to-run spread of A" became the A-vs-A' spread, because identical code rebuilt has read 1.5-2.3 % apart on the `cores = 1` every-row `UPDATE` and BP-S8's open question puts 3-6 % on code layout alone (`known-gaps.md`, "Per-core structures kept (BP)"). G3 states the floor, and the bar guards what BM must not cost. Under BM-S2's VM-owned loop the every-row `UPDATE` reaches neither the loop nor the kernel, so it is a control for layout, not a guard of S2 | As written; revised at the opening review and adopted on the standing go-ahead of 2026-10-13 (go-ahead-achieving-milestone) |

## 5. Measurement

### 5.1 BM-S1

`[design]` Where every cell is run:
- In `build-release`, rebuilt at BM-S1's instrumented commit and named by
  `git describe --tags`. `bench/README.md`'s five rules apply.
- On one host, the server and driver pinned to separate CPUs as BN pinned
  them.
- Each cell on a fresh server and data file, with the literal and prepared
  driver paths, interleaved, eight runs.
- PostgreSQL 18 as the twin for every cell but the writer cell.

**Shapes:**
- **BN's shape**: `SELECT * FROM s WHERE w = ?`, 10,000 rows, for
  continuity with §1.1.
- **Rows**: the same at 1,000, 10,000 and 100,000. This separates the
  fixed cost from the per-row cost, which BN's close did not sweep.
- **Selectivity**: `WHERE w < ?` at 0 %, 1 %, 10 % and 100 % of rows
  emitted. The 100 % cell is the all-pass cost of BM-Q2 (a) and BM-Q12's
  guard.
- **Width**: a 12-column relation of int32, int64, `uint64`, bool, date,
  timestamp, `decimal(12,2)`, `decimal(30,4)`, `char(16)`, an inline
  `varchar`, a nullable int64 and a spilled `varchar`:
  - filtered on its last fixed-width column;
  - with two conjuncts, one of them on the `char(16)`;
  - one equality per kernel type family.
- **Nullable**: a filter on the nullable column at 0 % and 50 % NULL
  (BM-Q3's cost).
- **Writers** (BM-Q8): `cores = 2`, `group`, one session updating the
  scanned relation's non-key column at its own pace while the scan runs.
  - Reported: `visibility_window_lookups` per scanned row, and the scan's
    p50 and p99 against the same scan with no writer.
  - No twin: the cell measures KDS against itself.

**Profile.** `tools/kds_sampler.c` on BN's shape, on the width shape, and
in the writer cell, with BM-S1's split parts.

**Report, per cell:**
- server CPU per statement, and the net row `(scan - point) / rows`;
- the PostgreSQL twin's net row and the ratio;
- wall-time p50, p90 and p99;
- the parts table per row, and the BM-Q1 and BM-Q8 readings.

**Host.** `/proc/loadavg`, the competing-process list and `df -T`, per
cell.

### 5.2 BM-S6

`[design]` The close, per CLAUDE.md's Session Workflow step 3:
- A is BM's opening commit and B its close, interleaved, over §5.1's cells.
- B is also run with `scan_kernel = off`. This separates BM-S3 and BM-S4
  from BM-S2 on one binary.
- A' is A's commit rebuilt from a second export, run beside A, so the
  bar's spread includes code layout (BM-Q12).
- The point `SELECT`, the every-row `UPDATE` and the 100 % scan are
  BM-Q12's guard. Every other cell is reported with no bar.
- If BM-S5 was built, the writer cell is repeated against A.

---

## 6. Row status

### BM-S0 — written 2026-10-12

Written in the main checkout on `main` at `ade5bc20`
(`v2.7.0-843-gade5bc20`), with no worktree, as a document-only change:
- No file under `src/`, `include/` or `tests/` moved, and no suite ran.
- The pre-push hook did not run, because it is not enabled in this clone
  (`core.hooksPath` unset). A documentation-only push is the case the
  hook's own text names as legitimate for `--no-verify`
  (`scripts/githooks/pre-push:24-28`). No build and no suite ran.
- The survey was first read at `7383a394`, and the order drafted and
  reviewed at `5126ddd0`, with `src/` and `include/` unchanged between them.
- BI's close (`ec827a19`) then reached `main` before the push. It changed
  two cited files: `device_page_store.cpp`, where `:1021` still holds, and
  `command_dispatcher.cpp`, where `EvaluateConjuncts`' call moved from
  `:8302` to `:8312`. CLA re-read both.
- No other cited file changed. Every `[source-read]` citation holds at
  `ade5bc20`, not run.
- The text that sequenced BM after BI was rewritten (BM-Q9).

With it:
- `instructions/v3.0.0/index.md` gains BM's row;
- `raft-marks-2026-10-12.md` records the operator's words W0-W2.

**Review before the push.** An independent agent that had not seen the
drafting checked the citations, numbers, the `[quiet-wrong]` arguments and
the cells against `5126ddd0`: one blocker (column 0 compared as its 8-byte
cell), thirteen should-fix and eight nits, all applied before the push.

### BM opened 2026-10-13

On `worktree-bm-scan-kernel-open` from `fa80da9d`, on W3 and W4
(`raft-marks-2026-10-13.md` §2-§4). The `critics-developer` review BM-S0
owed ran on the order against `fa80da9d`: two blockers, seven should-fix,
eight nits, five cuts.
- **Applied:**
  - B1, the header said the review gated BM-Q0 after the marks;
  - B2, the citations re-pointed from `ade5bc20` to `fa80da9d` (most moved:
    `step_vm.cpp` +6/+7, `row_codec.cpp` -7 with `FitsSigned` deleted);
  - S1, the window latch is unarmed at `cores = 1` since BP-H2 (§1.6);
  - S2, BM-S1's counter restated as built (summed and per core, not "the
    session's core"); `known-gaps.md`'s BP entry no longer says no
    `LookupCommit` counter exists;
  - S3, BM-Q12's spread taken across A and a rebuilt A';
  - S4, BM-S2 is the VM-owned btree loop, not a templated primitive;
  - S5, BM-R6's flag per step per statement, set in `AcceptTupleAt`;
  - S6, the Keystone flags cell needs a hand-built leaf, plus a narrow-pk
    mutation;
  - S7, cells for a mid-page resume and the range stop; the "flushed on an
    error" clause dropped, since a refused statement prints no counters;
  - every nit, and cuts 1 (this section's earlier changelog) and 2 (the BI
    and BL bullets).
- **Rejected:** cut 3 (§0.1), because it records what CLA told the operator
  before the order and the header does not; cut 4 (BM-R4's domain list),
  because §1.5 is the survey and BM-R4 is the rule, and the rule must be
  readable alone; cut 5 (one-line BM-Q4, Q7, Q8), because those rows are
  what the operator marked and are kept as marked.

### BM-S1 — the premise measured; BM-Q1 passes, BM-Q8 strikes BM-S5

On `worktree-bm-scan-kernel-open`. The instruments landed at `f5a7efb2`
(`v2.7.0-942-gf5a7efb2`): `visibility_window_lookups` and the row walk's
parts split in `tools/bn_parts.json`, with `chain:` parts anchored to the
walk. Every binary measured was built from `git archive f5a7efb2`. The
results file is `bench/v3.0.0/results-bm-s1-scan-row-v2.7.0-942-gf5a7efb2.md`,
and its drivers are `tools/bm_scan.py`, `tools/bm_scan_table.py`,
`tools/bm_waystone_control.py` and `tools/bn_pg_scan.py --shapes bm`.
- `[measured]` **The bn net row** is 65.5 ns (literal) and 65.3 ns
  (prepared), against BN's close at 67.2 and 67.0 ns. PostgreSQL 18.6
  costs 29.3 ns on a vacuumed table (2.24x) and 34.9 ns un-vacuumed (1.88x).
  The row cost is flat in the row count: 66.2, 65.5 and 65.0 ns at 1k, 10k
  and 100k rows. The wide relation costs 82-102 ns a row, whichever type
  the filter reads.
- `[measured]` **BM-Q1 passes.** The parts BM-S2..S4 replace are 96.3 % of
  the net row on bn, 91.6 % on `h = ?` and 93.2 % on `h = ? AND k = ?`,
  against a stop gate of one third. The inline floor-path visibility
  decision is 2.2 ns of the row.
- `[measured]` **BM-Q8 does not open BM-S5, so BM-S5 is struck** and BM-R7
  is recorded as measured, not material. With a point writer beside the
  scan, as the cell specifies, `LookupCommit` and its latch are 3.8 % of the
  scan core's CPU, against the 10 % gate. The share rises with the
  fraction of rows on the window path: 10.7 % beside an every-row writer,
  and 15.6 % with every row on the window path and no writer. A lookup
  costs 12-15 ns. Those two readings fall outside the cell the gate names;
  they go to `known-gaps.md` at the close.
- **Not run:** a second round, stopped on the operator's word of
  2026-10-13 (*"Finish it with the data you have now."*), was meant to add
  a 200-row cell, `pool_misses` per arm, and a second writers cell and
  profile. No number in the results file comes from it. Also not run: the
  twin for the writers cell, the twin's prepared path, any census at
  `cores > 1`, and `strict` in the writers cell.
- The drivers' review rides with BM-S2's.

### BM-S2 — the walk owns its btree slot loop; a step's shape checked once

On `worktree-bm-scan-kernel-open` from `23dfdbb8`.
- **Code:**
  - the outermost walk fetches each btree leaf through
    `btree::BtreeFetchLeaf` (the type check `BtreeVisitLeafPage` makes, now
    shared by both) and calls its slot visitor directly, with no
    `std::function` and no `StatusOr` per slot; a failed row is parked in
    `inner` and answered as a stop. `BtreeVisitLeafPage`, which UPDATE and
    DELETE reach, and the heap's `ChainVisitOnePage` keep their form;
  - `PageView::ReadTupleIn(slot, nr_slots)`, `ReadTuple`'s checks with the
    slot count the walk read once per leaf;
  - `CheckDecodeShape` and `DecodeChecks::kPayloadOnly` in the codec, and
    `CheckConjunct` / `EvaluateAllChecked` split out of `EvaluateConjunct`,
    with every message unchanged;
  - `CheckStepShapeOnce` in `AcceptTupleAt`, flagged per step id in the
    statement's `StatementStore`, so every access kind runs it and a
    sub-chain rebuilt per outer row does not re-check or re-allocate.
- **Cells** (`tests/scan_row_walk_test.cpp`): three leaves in slot order
  with one `pages_fetched` each; a sink stop mid-leaf (`pages` 1, `examined`
  7) - the stop `LIMIT` uses; an empty relation (no page, no refusal of a
  malformed chain); the range stop (`range_pages_pruned` 1, `pages` 2); a
  column past the frame refused with today's message; a `$param` refused
  through a walk and through a lookup step; BM-Q7's masked later conjunct
  refused; a wrong-length payload refused on its own row after five good
  ones; a slot whose offset leaves the page is no row; a true uncorrelated
  `EXISTS` stopping its walk mid-leaf. JB6's mid-page resume with the slot
  count is `exec_chain_test.cpp`'s
  `AResumedPrefixCoversEveryInnerRowAcrossADivideThatDropsARetiredSlot`.
  **Not built:** an empty non-root leaf, which no SQL path leaves standing
  without a hand-built tree.
- **Mutations**, each run twice, all killed: the stop deferred to the
  leaf's end (the range and wrong-length cells); the check skipped (five
  cells); the check's flag set by the walk only (the lookup cell).
- **Review** (`critics-developer`): no blocker; rows, order, counters and
  refusals unchanged but for BM-Q7. Applied: the flag moved from the
  runner to `StatementStore` (a correlated sub-chain allocated it per
  outer row); the counter cache deleted and BM-R5 revised to keep
  `ExecStats::For`; the slot-count sentinel replaced by an overload;
  `ReadTuple` reads `slot_count()` alone; mutation (a)'s kill re-attributed;
  the lookup cell made to fail without an out-of-bounds read; the slot,
  `$param` and `EXISTS` cells added. The drivers' findings (lookups per row
  read from the instance total rather than the scan core's slot, a
  `SHOW META`'s own lookups not subtracted, unused `Cell.pg_sql` and
  `rowfn`, a second server class) are taken at the close, where the drivers
  run again; none moves either gate, since each errs upward.
