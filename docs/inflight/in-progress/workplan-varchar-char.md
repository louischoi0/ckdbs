# Workplan — `char(N)`, `varchar(N)`, and the var-heap's undo and reclaim

The `docs/` output of `instructions/v2.5.0/varchar-char-workorder.md` (the
order) and `instructions/v2.5.0/varchar-char-architecture.md` (the design).
Opened 2026-08-28 in worktree `hotfix-varchar` on branch
`varchar-char-milestone` at `6910b66`, which is the commit that landed the
two instruction files.

This file is VC-0's output: the ratification record, the reader census,
and the fingerprint premise. Task state lives in §4; when every row is
built this file is **deleted**, per `CLAUDE.md`'s 2026-08-26 rule, with
each decision moved into the spec that owns it.

---

## 1. Ratification record — D1 through D5

**The operator's word, 2026-08-28: *"go ahead for this milestone"*, and
then, when CLA recorded its reading of it, *"I will follow recommendation
from CLA."*** So every D below stands at its RECOMMENDED option, as
`instructions/v2.5.0/varchar-char-architecture.md` §7 states them — and
the second message is what makes that a ratification rather than an
inference. The recommendations were drafted before the go-ahead and are
carried here unchanged; the operator's assent is to the drafted text, not
to a choice CLA made afterwards, which is why §7 stays the authority on
each option's alternatives and what refusing it would reopen.

| D | Question | Ratified answer |
|---|---|---|
| **D1** | Is `varchar(N)`'s `N` a cell width or SQL's length cap? | **A cell width.** A value longer than `N − 3` spills; it is not refused. The only length refusal stays 8144 bytes (`varheap::kMaxValueSize`). |
| **D2** | Where do `varchar(N)`'s bounds come from? | **`storage::CheckInlineCellWidth`**, unchanged: `[16, 4096]`. `varchar(8)` is refused, and that wart is accepted rather than patched with a second validator. |
| **D3** | Is the deferred-release queue durable? | **Memory-resident**, the shape `workplan-undo-purge.md` D2 ratified for undo. A crash leaks what it held; VC-C7's mount-time pass is the deferred remedy; clean shutdown drains first. |
| **D4** | Where does a released var-heap page go? | **Recycled in place, inside its chain.** No free-map call; `page.md` D9 and `physical-optimizer.md` §6 gate 3 stay closed. |
| **D5** | A `char` value containing a NUL byte? | **Refused** (`InvalidArgument`). The decoder stops at the first NUL, so accepting one would store a value the engine reads back differently. |

**The constraint that outranks all five**, stated by the operator twice
and last on 2026-08-28: *"max_inline_char_size를 따로 두지 말고 기존
inline_cell_width와 통합해."* No row may add a configuration key, catalog
field, constant, variable or `SHOW META` field naming a per-column spill
threshold. The threshold **is** `inline_cell_width` — instance-scoped for
a bare `varchar`, column-scoped for `varchar(N)` — validated by
`CheckInlineCellWidth` and nothing else. A review that finds a second name
for one number has found a defect.

---

## 2. The `inline_cell_width` reader census — and what it changed

Run at `6910b66`: `grep -rn "inline_cell_width" src/ include/ sim/ tests/`,
read in full. The result **shrinks VC-A3 to one line**, and that is worth
stating because the order predicted a wider change.

### 2.1 What the order expected

> *"`RowLayout` … gains the per-column cell width each reader needs —
> derivable from `offsets`, but carried explicitly so a reader never has to
> know that the null bitmap follows the last column."*

### 2.2 What the code already does

**The row codec never reads `layout.inline_cell_width` for a cell.** It
derives every cell's span from the offsets, through two helpers that
already handle the last column's bitmap boundary:

- `CellOf` / `MutableCellOf`, `src/exec/row_codec.cpp:367-386`: a cell runs
  from `offsets[i]` to `offsets[i+1]`, or for the last column to
  `row_size - null_bitmap_bytes`.
- Every arm of the encode and decode switches takes that span. `char` reads
  `cell.size()`; `varchar` hands the span to `EncodeInlineCell`, whose
  capacity is `span.size() - 3`.

So a per-column width needs **no new field and no `widths` vector**: the
offsets are already the per-column widths, and the one function that turns
a `SysColumnRow` into a width is `RowLayout::ColumnWidth`. Changing that
one function's `varchar` arm to prefer `col.len` propagates to every
reader.

### 2.3 The readers, and what each needs

| site | reads | needs |
|---|---|---|
| `src/catalog/row_layout.cpp:25` | the instance width, for `varchar` | **the change**: `col.len != 0 ? col.len : inline_cell_width` |
| `src/exec/index_ddl.cpp:258`, `src/exec/index_maintain.cpp:119`, `src/exec/step_vm.cpp:1354` | `ColumnWidth(col, layout.inline_cell_width)` | **nothing** — they pass the instance width as the *fallback*, which is exactly its new meaning |
| `src/exec/row_codec.cpp` (`EncodeRow`, `DecodeRow`, `DecodeRowInto`) | the offsets, via `CellOf`/`MutableCellOf` | **nothing** |
| `src/catalog/catalog.cpp:736/840/1208/1956` | `RowLayout::Build(schema, inline_cell_width_)` | **nothing** — the instance width is still Build's input |
| `src/bootstrap/bootstrap.cpp`, `src/server/superblock.cpp`, `src/server/expeditor.cpp`, `src/server/core_runtime.cpp` | the instance setting itself | **nothing** — the superblock pin is untouched |
| `sim/integrity.cpp:309` | `access.layout.inline_cell_width` **as a cell width**, directly | **the one real fix**, taken with VC-A3 rather than deferred to VC-C4: it is a defect the moment a `varchar(N)` column exists, since the walk would read a narrow column's neighbour and report the corruption it invented |
| `tests/row_codec_test.cpp:521` | the same, in a test | **no change needed** — the column there is a bare `varchar`, so the instance width *is* its width. Fragile rather than wrong, and left alone rather than churned |

**Verdict.** VC-A3 is one line in `ColumnWidth` plus the one direct reader
in `sim/`; `RowLayout` gains no field. This is a deviation from the order's
VC-A3 text, recorded here rather than silently taken, and it makes the
change strictly smaller than what was reviewed.

**It also falsifies a claim in the v1 decision this version reverses.**
`rule-fixed-length-tuple.md` §4 justified a global-only width partly by
"one codec path instead of per-column widths threaded through every layout
computation". The threading cost was imaginary: the codec never read the
instance width per cell. The amendment records this, because a reversal
that does not say which of the original arguments failed teaches nothing.

---

## 3. The fingerprint premise — checked, not assumed

The order's VC-0 asked whether `varchar(64)` in a `CREATE TABLE` moves a
**stored** hash, which would owe a `kFingerprintVersion` bump
(`fingerprint.hpp:174`, currently 1).

It does not, and the reason is stronger than "DDL is not recorded":

1. **The live fingerprint is taken only on the SELECT path.**
   `CommandDispatcher`'s recording site (`src/server/command_dispatcher.cpp:5526-5533`)
   constructs a `parser::Parser`, parses, and **refuses anything that is
   not a `SelectStmt`** before a fingerprint is taken from the parse.
2. **A `CREATE PATTERN` body is a `SelectStmt` by type.**
   `parser::CreatePatternStmt::body` is a `std::shared_ptr<SelectStmt>`
   (`include/kds/parser/ast.hpp:704`), and `FingerprintOf(stmt.body_text)`
   (`src/exec/pattern_ddl.cpp:427`) is reached only after that body has
   parsed and compiled. A `CREATE TABLE` cannot be a pattern body.

So no `CREATE TABLE` hash can reach `sys.patterns`, and the bump rule —
which protects what is *stored* (`src/parser/fingerprint.cpp:166-172`,
the `kNumLit` argument) — is not engaged. **`kFingerprintVersion` stays
1**, and this version owes no re-registration of the golden corpus.

### 3a. One finding VC-0 turned up and is deliberately not fixing

**`decimal128(p, s)` is not declarable through the parser.** The paren
production accepted arguments only for a type named exactly `DECIMAL`, so
`decimal128(24, 6)` was refused as "takes no arguments" — while
`src/server/command_dispatcher.cpp`'s own comment states that "writing
`decimal128(p, s)` names the wide type directly and its bounds refuse
p <= 18", describing a path nothing can reach.
`tests/types_contract_test.cpp:211` lists `decimal128(10, 2)` among its
refusals and passes for the wrong reason. Nothing is lost in practice —
the wide type is reachable by promotion, `decimal(24, 6)`, which is what
`tests/types_e2e_test.cpp` exercises — so this is a **stale claim, not a
defect**.

**Left alone by this version, on purpose.** The fix is a change to the
same parser production VC-A1 touches, which would put an unasked type
change inside a review of a different feature. It belongs in
`known-gaps.md`'s stale-claims section, and VC-A7 puts it there.

---

## 4. Task state

Legend: ☐ not started · ◐ in progress · ☑ built, reviewed, suite green.

### Phase A — the declarations (worktree `varchar-char-milestone`)

| row | what | state |
|---|---|---|
| VC-0 | this file: ratification, census, fingerprint premise | ☑ |
| VC-A1 | parser: one argument for `CHAR`/`VARCHAR`, two for `DECIMAL` | ☑ |
| VC-A2 | the catalog door: `len` from the declaration, bounds checked | ☑ |
| VC-A3 | `ColumnWidth` prefers `col.len` for `varchar` (§2.3) | ☑ |
| VC-A4 | codec: `char`'s NUL refusal; the per-column span (already true) | ☑ |
| VC-A5 | `ColumnTypeText`: `varchar(N)` / `varchar` | ☑ |
| VC-A6 | end to end, and the contract suites | ☑ suite green at **2867** (+30); `ckdbs-sim` seeds 7 and 24 green. **Overhead not measured**, per the 2026-08-24 amendment |
| VC-A7 | phase A prose | ☑ `rule-fixed-length-tuple.md` §2/§3/§4, `heap-and-tuple.md` §3.3, `types.md` §2b/§4a, `alter.md`, `manual/sql/sql.md` §2, `known-gaps.md` (two stale claims), `CLAUDE.md` (row, invariant 13, Open Decisions) |

### Phase A's review, and what it changed

`critics-developer`, run against the finished phase A. It verified the §2
census independently — including index covered-column packing, the site
CLA was least sure of, which is sound because all three callers pass the
instance width as `ColumnWidth`'s *fallback* — and confirmed no second
name for the threshold exists anywhere in the diff. Findings applied:

- **C-1, the one defect.** `Parser::ExpectToken` never appended the
  token's byte, so two of VC-A1's five refusals (`char(8, 1)`,
  `varchar(32, 2)`) shipped without the position `CLAUDE.md` requires of
  every refusal. Fixed **in `ExpectToken` itself** rather than at the call
  sites, which also fixes `decimal(10)` — a production this feature never
  touched and which had the same gap. Both spellings are now pinned by
  tests that assert the byte, not merely the failure.
- **C-3.** `CheckDeclarableColumnTypes` had no unit test; it has three,
  including one that records what it deliberately does *not* check.
- **S-1, accepted.** The `char len == 0` arm added to
  `CheckDeclarableColumnTypes` was dead for its only caller —
  `RowLayout::Build` runs on the same schema seven lines later and already
  refuses it — so it was a third wording of one condition. Deleted; the
  varchar arm stays, because nothing else catches an 8-byte cell.
- **S-2, S-4, accepted.** `ColumnTypeText`'s two identical arms folded
  into one; three comment blocks that restated the spec trimmed.
- **S-6, accepted.** Two e2e assertions rewritten to state their claim.
  One of them was **wrong, not merely weak**: it expected a real newline
  as the row separator, and a reply's separator is the two-character
  escape, because the wire protocol is newline-delimited.
- **The shape note, accepted.** The dispatcher tested `has_precision`
  before the type arms and `has_width` after them. Both are hoisted above,
  so a third argument-taking type finds one place to extend.
- **S-3, partially.** The `sim/integrity` guard over an impossible
  `ColumnWidth` failure is kept — a checker may not call `.value()`
  unchecked — but collapsed from eight lines to one.
- **S-5, resolved by VC-A4.** `FirstPayload` now has two callers, the
  second being the `N−3`/`N−2` boundary test at both ends of the legal
  width range, so the fixture method earned its place.

### Phase B — undo

| row | what | state |
|---|---|---|
| VC-B1 | `kVarHeapAppend` (undo, trail), `kVarHeapRelease` (WAL) + the `kMaxAssignedRecordType` move | ☑ |
| VC-B2 | `PageRelease`, `PageLiveSlots`, `NotFound` on a tombstone, redo | ☑ |
| VC-B3 | the append's undo record and trail entry; `Compensate`; `RecoveryUndo` | ☑ |
| VC-B4 | the ownership fact, pinned | ☑ `tests/varheap_lifetime_test.cpp` |
| VC-B5 | phase B prose | ☑ `txn.md` §3.3/§6, `wal.md` §11a, `rule-fixed-length-tuple.md` §5, `heap-and-tuple.md` §3.4, `recovery_undo.hpp`, `varheap.hpp`, `undo_page.hpp` |

### What phase B found that the order did not predict

**A second whitelist, and the sim found it.** `UndoRecordType` had its
accepted-type list written out twice — once in `UndoPageAppend`, once in
`UndoPageRead` — so `kVarHeapAppend` was refused on write ("undo record
type 4 is not a writable type", `ckdbs-sim` seed 4, op 28), and after that
was fixed in isolation the *decoder* refused the record the engine had just
written, failing a recovery. This is `wal::kMaxAssignedRecordType`'s
failure one layer down, and it is now one predicate,
`IsWritableUndoRecordType`, consulted by both sides, with a test asserting
they agree rather than asserting either list.

**The UPDATE path collected spills only when there was a WAL.** The trail
entry a live `Abort` reads is owed whether or not anything is logged, so
an unlogged dispatcher would have leaked every value a rolled-back UPDATE
spilled. The collector's condition is now `wal_ != nullptr || scope.txn != nullptr`.

**A stated gap, not closed by phase B**: a spill logged with `kNoTxnId` —
`LogChainInsert`'s path, used by `sys.pattern_defs` and the assertion
catalog — has no transaction to chain an undo record to, so a rolled-back
`CREATE PATTERN`'s spilled body text still leaks. It leaked before this
work too; VC-C7's mount-time sweep is what would collect it.

### Phase B's review, and what it changed

`critics-developer`, run against the finished phase B and asked
specifically to attack the ownership premise. **It could not break it** —
`EncodeRow` never dedups, UPDATE resolves spilled cells to full strings
before re-encoding, the bulk fast path is gated by
`varheap_page_id == kInvalidPageId`, leaf division and relayout move a
version rather than duplicating it, and statement shipping re-parses text
on the owner. No double-free path exists. What it did find:

- **A silent heap-corruption hole in `PageRelease`** (fixed by the
  reviewer). A heap page carries `nr_slots` at the *same body offset* the
  var-heap does, so a release naming a heap page passed the slot bound and
  wrote two zero bytes over `min_key` or a slot pointer — invariant 2,
  broken silently, in the one undo type that skips the pk identity check
  saving every other type from a mis-addressed record. `PageWriteAt` had
  the identical hole and now carries the same guard.
- **The ordering test was vacuous** (fixed by the reviewer, then extended).
  `CREATE TABLE` writes four `UNDO_WRITE`s of its own, so a search from the
  log's start satisfied "UNDO_WRITE before VARHEAP_APPEND" using records
  the INSERT never wrote — proven by deleting `NoteSpills` and watching it
  still pass. It is scoped to the statement's own records now, with a count
  assertion, because the ordering alone is still satisfiable by the
  insert's own record. The UPDATE path gained the equivalent test.
- **`heap-and-tuple.md` §3.4 still carried the claim phase B falsified.**
  Its rules-file sibling was amended and the spec's own ordering bullet was
  not — and `CLAUDE.md` says the spec outranks the rules file, so the
  authoritative document contradicted both the code and its own rules file.
- **A mount failure the sim structurally cannot catch** (finding 4, fixed
  here). Phase B's own ordering puts the `UNDO_WRITE` *before* the
  `PAGE_INIT`/`VARHEAP_APPEND`, so a log whose readable prefix ends between
  them leaves the loser's chain naming a slot redo never created, and
  `PageRelease`'s Corruption propagated to a refused mount. An append that
  was never redone has nothing to undo: it counts as `already_done_`. The
  `kInsert` twin of this is **left alone and recorded in `known-gaps.md`** —
  same shape, but changing recovery semantics for a record type that
  predates this work does not belong inside its review.
- **The INSERT path's failure window**, narrowed: `NoteSpills` sat between
  the tuple landing in the page and its own `AppendUndo`/`NoteInsert`, so a
  failure there left a row no rollback would undo. It runs after
  `NoteInsert` now; the only ordering it owes is "before `LogSpills`".

Simplifications applied: `VarHeapReleasePayload` lost its `reserved` field
(2 bytes, `SLOT_RETIRE`-shaped, and nothing follows it to align); the
release-and-log block, written twice and about to be written a third time
by phase C's drain, is one `txn::ReleaseVarHeapSlot`; the undo record moved
into `TransactionManager::NoteVarHeapAppend`, so the chain link lives in
the class that owns the chain; ~40 lines of comment that restated one
claim in six places reduced to citations. Kept against the review's
suggestion: nothing — every finding was applied or recorded.

### Phase C landmines, recorded before they are hit

Found by phase B's review; each is a rule phase C must state or a check it
must carry.

1. **Index covered-column entries hold spill pointers.** Phase C multiplies
   the dangling-pointer case from "rolled-back rows" to "every superseded
   version". Harmless while nothing follows them; `docs/spec/index.md` §13
   now records it as a second gate on the index-only scan.
2. **`PageRelease` refuses `slot >= nr_slots`.** A recycle resets
   `nr_slots` to 0, so any queued release outstanding across a recycle of
   its page becomes a failure. **The queue must be purged of a page's
   entries when that page is recycled** — with `ReleaseOutcome`'s
   `kNothingToRelease` as the fallback if one is missed.
3. **`PageLiveSlots() == 0` is true of a fresh page too.** The recycle
   predicate is `PageSlotCount() > 0 && PageLiveSlots() == 0`; the header
   says so and the function cannot.
4. **`kOverwrite`'s undo image carries the old version's spill pointer.** A
   superseded slot dies when its undo record becomes unreachable, which
   coincides with the writer falling below `ReadHorizon()` **because of**
   the undo purge's soundness fact — cite `workplan-undo-purge.md`, do not
   re-derive it.

### Phase C — reclaim

| row | what | state |
|---|---|---|
| VC-C1 | the release queue | ☐ |
| VC-C2 | the drain and its two triggers | ☐ |
| VC-C3 | the in-place page recycle | ☐ |
| VC-C4 | `SHOW META` counters; `sim/` integrity and generator | ☐ |
| VC-C5 | the numbers (ck-tester) | ☐ |
| VC-C6 | phase C prose | ☐ |
| VC-C7 | the crash leak's mount-time remedy, gated on its own measurement | ☐ |

---

## 5. Where to pick this up

At the first ☐ in phase order. Each row's tests are named in the order's
§3; a row is landed only with a `critics-developer` review, the named
tests present and passing, and the full suite green in `build-release`.
The order's §5 lists the two stops particular to this version — a second
name for `inline_cell_width`, and VC-C2 landing without its
reader-above-the-writer test.
