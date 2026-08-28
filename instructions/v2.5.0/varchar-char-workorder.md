# Work order VC — `char(N)`, `varchar(N)`, and the var-heap's undo and reclaim

Drafted 2026-08-28 in worktree `hotfix-varchar` against `main` at `814c568`
(`v2.2.1-89-g814c568`).

The build half of `instructions/v2.5.0/varchar-char-architecture.md`. That
file owns the design and its rationale (§3 the two types, §4 the lifetime
model, §7 the decisions D1–D5); this one owns the rows, their order, their
tests, and what "done" means. **It does not restate the design** — a row
that needs a reason cites the section that carries it.

> **Version named by the operator, 2026-08-28**: this work lands under
> `instructions/v2.5.0/`, beside the cross-owner protocol order already
> there (two orders under one version, as `v2.4.0` holds two). The tag on
> `main` is `v2.2.1`; nothing here mints `v2.5.0`, every measurement names
> itself by `git describe --tags`, and results land under `bench/v2.5.0/`.

> **The one constraint the operator stated twice.** *"max_inline_char_size를
> 따로 두지 말고 기존 inline_cell_width와 통합해."* No row below may
> introduce a configuration key, a catalog field, a constant, or a
> variable named for a per-column spill threshold. The threshold **is**
> `inline_cell_width` — instance-scoped when a column says `varchar`,
> column-scoped when it says `varchar(N)` — and it is validated by
> `CheckInlineCellWidth` and nothing else. A review that finds a second
> name for the same number has found a defect.

Discipline unchanged from `instructions/v2.2.0-stmtshipping.md`: every
claim **source-read** (`path:line` + commit) or **measured** (with its
invocation); `build-release` for numbers, never `./build`; task rows that
land state their worktree and cite their `critics-developer` review; the
full suite gates every row. The operator's 2026-08-24 amendment holds —
the interleaved A/B overhead measurement is suspended — and the two rows
that ask for a number anyway (VC-C5) say what they measure and what they
do not.

---

## 0. Why this version, and why in this order

Three gaps, one substrate (architecture §1): `char(N)` is not declarable
(G1); `varchar(N)` is not declarable, by a v1 decision this version
reverses (G2); a spilled value is never released (G3). G1 and G2 are
plumbing over machinery that exists at `814c568` — `RowLayout::ColumnWidth`
already reads `col.len` for `char` (`src/catalog/row_layout.cpp:23`) and
`ColumnTypeText` already prints `char(8)` (`src/catalog/rows.cpp:355`).
G3 is the engineering, and its two halves are separable: **undo** (a
rolled-back append is released) needs nothing from **reclaim** (a settled
version's slots are released), but reclaim needs undo's release primitive.

So three phases, in this order, each landable alone:

| phase | closes | depends on | worktree (suggested) |
|---|---|---|---|
| **A** — the declarations | G1, G2 | nothing | `varchar-char-decl` |
| **B** — undo | G3 (a) | nothing (works on today's bare `varchar`) | `varheap-undo` |
| **C** — reclaim | G3 (b), (c) | B's `VARHEAP_RELEASE` and `PageRelease` | `varheap-reclaim` |

A before B because it is small, user-visible, and its review teaches the
per-column-width reader census that C's `sim/integrity` change reuses. B
before C because C's drain is a caller of B's primitive. This order was
drafted in `hotfix-varchar`; that name says "hotfix" and this is not one,
so the build opens the worktrees above rather than continuing here.

---

## 1. What this version delivers

| deliverable | shape | proof |
|---|---|---|
| `char(N)` declarable | `CREATE TABLE t (id int64, code char(8))`; `char` alone is `char(1)`; `DESCRIBE` prints `char(8)` | VC-A6's e2e; `ACharColumnKeepsItsDeclaredWidth` already pins the layout |
| `varchar(N)` declarable | `N` is the column's `inline_cell_width`, `[16, 4096]`, capacity `N − 3`, spills past it; `varchar` alone is the instance width; `DESCRIBE` prints `varchar(N)` or `varchar` | VC-A6's e2e; VC-A3's compatibility test |
| No format bump | a pre-v2.5.0 file mounts byte-identical; `len = 0` reads as the instance width | VC-A3's `ALegacyVarcharColumnReadsAtTheInstanceWidth` |
| A rolled-back spill is released | live `Abort` and recovery's undo phase both release every slot the loser appended, including one no tuple ever pointed at | VC-B3's `ALoserThatCrashedBetweenTheAppendAndTheTupleWriteLeavesNoOrphan` |
| A dead version's spills are released | superseded and deleted versions release when their writer settles below `ReadHorizon()`; an aborted writer releases nothing | VC-C2's four tests |
| The chain stops growing | a fully released page is reformatted in place and reused | VC-C3's `AFullyReleasedPageIsReusedBeforeTheChainGrows`; VC-C5's plateau |
| Observable | `SHOW META`: `varheap_slots_released`, `varheap_pages_recycled`, `varheap_release_pending` | VC-C4 |

---

## 2. Decisions — the gate

Architecture §7 lists **D1–D5** with a recommendation each. **VC-0 records
the operator's answer to each before VC-A1 starts**, in the workplan it
creates. The drafted basis is the recommendation in every case; a
different answer to D1 reopens architecture §3.2 whole and stops the
order at VC-0. D3 and D4 gate phase C only; D5 gates VC-A4 only.

---

## 3. Task rows

Every row: a `critics-developer` review (correctness first; then whether
the row added a second name for one number), the full suite green in
`build-release`, and the row's own tests named here present and passing.
A row that cannot say all three is not landed.

### VC-0 — the ratification record and the reader census

Creates `docs/inflight/in-progress/workplan-varchar-char.md` (the
`docs/` output of this `instructions/` input, per `CLAUDE.md`'s rule) with:

1. **D1–D5's answers**, operator's words quoted, dated.
2. **The `inline_cell_width` reader census**: every site that reads
   `RowLayout::inline_cell_width` or assumes one width for every tagged
   cell. Known at `814c568`: `src/exec/row_codec.cpp` (`EncodeRow`,
   `DecodeRow`, `DecodeRowInto`, and the last-column span rule at
   `:366-384`), `sim/integrity.cpp:309`, `src/exec/assertion_catalog.cpp`
   around `:270`, `src/stats/pattern_defs.cpp` around `:188`,
   `src/catalog/catalog.cpp:736/840/1208/1956` (`RowLayout::Build`
   callers — these stay, the instance width is still Build's input). The
   census is `grep -rn "inline_cell_width" src/ include/ sim/ tests/`
   run and read, not recalled.
3. **The fingerprint premise checked**: that no `CREATE TABLE` hash is
   storable (architecture §5), stated with the code path that proves it
   or, failing that, the `kFingerprintVersion` bump this version then
   owes.

Done when the file exists with all three and VC-A1 can cite it.

### Phase A — the declarations

**VC-A1 — parser.** `src/parser/parser.cpp:426-466`: the paren production
takes **one** argument for `CHAR`/`VARCHAR` (`ParseTypeArgument("width")`),
**two** for `DECIMAL`, and refuses the paren for every other type with
the existing message (`:432-435`). `parser::ColumnDef` gains `has_width`
and `width` — **not** an overload of `precision`, because a reader of the
AST must not need the type name to know what a number means. Refusals,
each with the byte position of the offending token: `char()`,
`varchar(1,2)`, `char(8,1)`, `int64(4)`, `decimal(8)` (unchanged).
`CREATE PATTERN` parameter types (`:671-673`) are **unchanged** — a
pattern parameter names a type for coercion only (`pattern_ddl.cpp:312`),
and a width there is refused as it is today.
*Tests:* the parser suite gains `char(8)`, `varchar(32)`, the bare forms,
and each refusal above with its byte.

**VC-A2 — the catalog door.** `src/server/command_dispatcher.cpp:3176-3260`:
after `ResolveTypeByName`, `char` takes `row.len = has_width ? width : 1`
and refuses `0` with the argument's byte; `varchar` takes `row.len =
has_width ? width : 0` and passes a declared width through
`storage::CheckInlineCellWidth`, appending the byte to its message on
refusal; any other type with `has_width` meets the `:3254` refusal,
reworded from "takes no precision or scale" to "takes no arguments".
`catalog::CheckDeclarableColumnTypes` (`src/catalog/schema.cpp:39`)
gains the same two checks, so a schema arriving by another door is
refused at the catalog's own door with the TY2 wording pattern.
*Tests:* `tests/fixed_length_tuple_test.cpp` gains `AVarcharWidthOutsideTheInstanceBoundsIsRefusedAtCreateTable`
(16 and 4096 accepted, 15 and 4097 refused, message names both bounds
and the byte) and `ACharOfZeroWidthIsRefusedAtCreateTable`; a
`CheckDeclarableColumnTypes` unit test with a hand-built `len = 8`
varchar row.

**VC-A3 — the layout.** `RowLayout::ColumnWidth` (`row_layout.cpp:25`)
returns `col.len != 0 ? col.len : inline_cell_width` for `varchar`.
`RowLayout` gains a per-column cell width (`widths`, one per column,
alongside `offsets`), and **every site in VC-0's census** reads it instead
of `inline_cell_width`. `inline_cell_width` stays on the layout — it is
still the number Build was built under and the default a `len = 0` column
takes — but after this row the only reader of it inside the codec is
`ColumnWidth`.
*Tests:* `tests/row_layout_test.cpp`: `AVarcharWithADeclaredWidthCostsThatWidth`,
`TwoVarcharsOfDifferentWidthsInOneRow` (offsets, row size, null-bitmap
position), `ALegacyVarcharColumnReadsAtTheInstanceWidth` — a `len = 0`
column and a `len = 64` column under instance width 64 produce
**identical** offsets and row size, which is the compatibility claim in
one assertion. `AVarcharCostsTheSameWhateverTheWidthIsSetTo` (`:74`)
keeps its name and its premise for `len = 0`.

**VC-A4 — the codec.** `src/exec/row_codec.cpp`: `char` refuses an
embedded NUL (`InvalidArgument`, D5) and its too-long message names the
declared width; `varchar` hands `EncodeInlineCell` the column's own span,
so capacity is per column with **no change to `tagged_cell.cpp`** — the
width was always the span's size. `DecodeRow`'s spill recording and
`ResolveSpills` are unchanged in kind.
*Tests:* `tests/row_codec_test.cpp` and `tests/fixed_length_tuple_test.cpp`:
inline/spill boundary at `N − 3` and `N − 2` for `N = 16` and `N = 4096`;
`TupleAddressesSurviveUpdatesAcrossTheSpillBoundary` parametrised over
two widths in one row; `ACharValueWithAnEmbeddedNulIsRefused`;
`AShortUpdateOverALongValueLeavesNoStaleTail` for `char(N)`; wire
byte-identity of inline versus spilled storage of one logical value at
two widths (`rule-fixed-length-tuple.md` §8.6, still owed at `814c568`);
NULL in `char(N)` and `varchar(N)` through the bitmap; an index on
`char(40)` (prefix 32) and on `varchar(16)` (prefix 32, spilled key
value included).

**VC-A5 — `DESCRIBE`, `sys.columns`.** `ColumnTypeText`
(`src/catalog/rows.cpp:349-362`) prints `varchar(N)` for `len > 0` and
`varchar` for `len = 0`. `src/wire/row_codec.cpp:121-125`'s comment
("a char column's len is a storage width") extends to `varchar`; the
description still does not leak it.
*Tests:* `tests/catalog_row_test.cpp:496`'s neighbour gains the two
`varchar` cases; an e2e `DESCRIBE` round trip for
`char(8) / varchar(32) / varchar / decimal(10,2)` in one relation.

**VC-A6 — end to end, and the contract suites.** `tests/types_e2e_test.cpp`
shape: `CREATE TABLE` with `char(8)`, `varchar(32)`, `varchar(4096)`,
bare `varchar`; `INSERT`/`SELECT`/`UPDATE` across each boundary; every
refusal from A1 and A2 at the wire with its byte. The contract suites
(waystone, index, cabin, types, assertion) byte-for-byte green — a
`varchar(N)` column must produce the same results as a bare `varchar`
holding the same values under every configuration they compare.
`scripts/sim.sh` green with the generator **unchanged** (C4 extends it).

**VC-A7 — phase A prose.** Amend at the source, dated, keeping the old
argument as history where the design reversed it:

- `docs/spec/heap-and-tuple.md` §3.3 — line 77's "no `VARCHAR(n)` …
  surface at all" reversed; the per-column override stated; §4.1-style
  wording.
- `docs/rules/rule-fixed-length-tuple.md` §4 — "The Global Constant"
  becomes "The instance default and the column override"; the
  global-over-per-column rationale kept as the v1 record; V5 (widening
  `Unsupported`) restated with its true reason.
- `docs/spec/types.md` — a `char(N)`/`varchar(N)` section; §4a's `len`
  table gains the `varchar` row.
- `docs/spec/alter.md:26-28` — the reason rewritten (architecture §3.2).
- `manual/sql/sql.md` §2 table and §7 — the five divergences of
  architecture §6, in D1's register.
- `include/kds/catalog/rows.hpp:176-197` (`len`'s comment),
  `include/kds/storage/tagged_cell.hpp` (instance-pinned → default),
  `include/kds/exec/row_codec.hpp` (file comment's width sentence).
- `CLAUDE.md`: the Types row; Open Decisions → Storage keeps
  "`inline_cell_width` default" open (it is now the default for a bare
  `varchar`, which sharpens rather than closes the question).

### Phase B — undo

**VC-B1 — the record types.** `UndoRecordType::kVarHeapAppend = 4`
(`include/kds/txn/undo_page.hpp:122`), `TrailAction::kVarHeapAppend = 4`
(`include/kds/txn/manager.hpp:92`), `RecordType::kVarHeapRelease` at the
next unassigned number after `kTxnPrepare = 27` **and
`kMaxAssignedRecordType` moved with it** — `known-gaps.md:345-350` records
`HEAP_DELETE_UNMARK` landing as 23 with the bound left at 22, so every
attempt to log one answered "unassigned record type"; the test below is
that lesson. `wal::VarHeapReleasePayload{slot u16, reserved u16}` with
`Encode`/`Decode` and size checks in `payload.hpp/.cpp`, and
`RecordTypeName` → `"VARHEAP_RELEASE"`.
*Tests:* payload round trip; **a record encoded through `WalManager`**,
not only through the payload codec.

**VC-B2 — the primitive.** `include/kds/storage/varheap.hpp`:
`PageRelease(page, slot)` sets the slot's `offset` to `0`
(architecture §4.5); `PageLiveSlots(page)` counts non-tombstones;
`PageRead` of a tombstone answers `NotFound`. `src/wal/redo.cpp` applies
`kVarHeapRelease` beside `kVarHeapAppend` (`:404`). The file header's
"There is deliberately no Update() and no Free()" becomes "no Update();
Release() is the one way a value dies, and §4 of the architecture says
when".
*Tests:* `tests/varheap_test.cpp`: `ReleaseTombstonesExactlyOneSlot`,
`AReleasedSlotReadsAsNotFoundNotCorruption`, `ReleaseIsIdempotent`,
`APageWithEverySlotReleasedReportsNoLiveSlots`,
`AReleaseLeavesTheValueBytesUntouched` (invariant 14, literally: the
page's value region is byte-identical before and after); a redo test
that replays `VARHEAP_APPEND` then `VARHEAP_RELEASE` and reads
`NotFound`.

**VC-B3 — the append's undo, live and recovered.** `exec::LogSpills`
(`src/exec/wal_row_log.cpp:14-63`) writes, per spill, the
`kVarHeapAppend` undo record (`UndoLog::Append` with target = the
var-heap `(page_id, slot)`, image empty, `pk` = the row) **before** the
`PAGE_INIT`/`FULL_PAGE_IMAGE`/`VARHEAP_APPEND` it writes today, and
pushes the trail entry — which means `LogSpills` gains the transaction as
a parameter, and its three callers (`INSERT` at
`command_dispatcher.cpp:4531`, `UPDATE` at `:6365`, and whatever the
assertion-catalog and `pattern_defs` sinks route through since RV3 put
DDL under a real transaction) are each shown to pass it or census'd out
with a reason. `TransactionManager::Compensate` (`src/txn/manager.cpp:220`)
handles the new action: `PageRelease` + `VARHEAP_RELEASE` under the
aborting transaction's id, **skipping the row-identity probe at
`:236-263`** because the target is not a row. `txn::RecoveryUndo`'s chain
walk does the same for the record type.
*Tests:* `tests/insert_wal_test.cpp`: `ASpilledValuesUndoRecordPrecedesItsAppend`
(record order pinned, the shape of `:346`);
`tests/recovery_undo_test.cpp`: `ALosersSpillIsReleased`,
`ALoserThatCrashedBetweenTheAppendAndTheTupleWriteLeavesNoOrphan` — log
`UNDO_WRITE` + `VARHEAP_APPEND` and **no** `HEAP_INSERT`, crash, recover,
assert the slot is a tombstone (the case `rule-fixed-length-tuple.md` §5
line 61 handed to a sweep); `RunningTwiceIsAByteForByteNoOp` (`:183`)
extended to include a released slot; `AWinnersSpillIsLeftAlone`.

**VC-B4 — the ownership fact, pinned.** `tests/fixed_length_tuple_test.cpp`:
`AnUpdateOfAnotherColumnGivesTheSpilledValueANewSlot` (architecture §4.1
— the model's premise, so an optimisation cannot remove it silently);
`ARolledBackSpillingInsertReleasesExactlyItsSlots` (oracle: the count of
spilled cells; every other slot in the chain untouched);
`ARolledBackUpdateReleasesTheNewCopyAndKeepsTheOld` (the old version's
slot still reads).

**VC-B5 — phase B prose.** `docs/spec/txn.md` §3.3 (the record type), §6
(the compensation table gains the row; the `txn_id` split restated for
`VARHEAP_RELEASE`); `docs/spec/wal.md` §5.2 (record catalog) and §11a
(the `INSERT`/`UPDATE` rows gain `UNDO_WRITE{kVarHeapAppend}` before
`VARHEAP_APPEND`; rollback gains `VARHEAP_RELEASE`);
`docs/rules/rule-fixed-length-tuple.md` §5 line 61 (the sweep sentence
replaced by the chain record); `docs/spec/heap-and-tuple.md` §3.4 line 89
(the "two limits" sentence loses "nothing reclaims" *for the rollback
case*; the settled case waits for C6); `include/kds/txn/undo_page.hpp`'s
type comment; `include/kds/txn/recovery_undo.hpp`'s identity-check
paragraph (the exception named).

### Phase C — reclaim

**VC-C1 — the queue.** `txn::VarHeapReleaseQueue`
(`include/kds/txn/varheap_release_queue.hpp`, beside `undo_log.hpp`,
owned by `TransactionManager` because it needs `ReadHorizon()` and sees
`Abort`): entries `{writer trx_id, rel_oid, VarHeapPtr}`; `Push`,
`DropWriter(trx_id)` (called from `Abort`), `Drain(horizon, releaser)`
returning what it released. Fed at **`UPDATE`** with the old version's
`kSpilled` pointers — already decoded into `spills` at
`command_dispatcher.cpp:6360` — and at **`DELETE`** by reading the marked
tuple's `varchar` cells off the page in hand (`DecodeCell`, no fetch).
Memory-resident by D3; the header says so and names VC-C7 as the leak's
remedy.
*Tests:* unit: an entry below the horizon drains, one at or above does
not; `DropWriter` removes exactly one writer's entries; a drained entry
is gone.

**VC-C2 — the drain and its triggers.** Two triggers, both existing
shapes: **chain growth** — before `ChainAppend` allocates
(`src/storage/varheap.cpp:248-256`'s `OutOfSpace` arm), the way
`UndoLog::TailFor` reclaims before it allocates — reached through the
`VarHeapSink`, which gains the queue (or a drain callback) beside its
`appended` collector; and **checkpoint** (`src/wal/checkpointer.cpp`),
so a quiet instance settles. Each release logs `VARHEAP_RELEASE` with
`kNoTxnId` and stamps the page. Clean shutdown drains once more.
*Tests:* `ASettledSupersededValueIsReleasedOnTheNextGrowth`;
`AReaderAboveTheWriterKeepsTheOldValueAlive` — a `ReaderLease` held
across the drain leaves the slot readable; releasing it and draining
again tombstones it (`rule-fixed-length-tuple.md` §8.3's "purge reclaims
exactly the dead values", with the oracle count);
`ADeletedRowsSpilledValueIsReleasedWhenTheMarkSettles`;
`AnAbortedUpdateReleasesNothingFromTheQueue`; a checkpoint-triggered
drain observed through `SHOW META`.

**VC-C3 — the recycle.** In `ChainAppend`'s walk
(`src/storage/varheap.cpp:232-247`): a page with room takes the append
(first fit, not tail); a page with `nr_slots > 0` and no live slot is
reformatted in place (`FormatPage`, then `PAGE_INIT{kVarHeap}` through the
existing `LogPageInit` path — `ChainAppendResult` reports it as the page
this append **(re)initialised**, so `LogSpills` logs it exactly as it
logs a created page) and takes the append at slot 0. No link changes, no
free-map call, the root recycled but never unlinked (D4).
*Tests:* `tests/varheap_test.cpp`:
`AFullyReleasedPageIsReusedBeforeTheChainGrows` (`ChainLength` flat
across release → append), `ARecycledPageReplaysToTheSameBytes` (WAL
replay of `PAGE_INIT` + `VARHEAP_APPEND` onto a page image that held
values), `TheRootIsRecycledButNeverUnlinked`,
`AMiddlePageWithRoomTakesTheAppend`.
*Optional rider, measured not required:* a per-relation append hint on
`TableAccess` (the `heap_tail_hint` shape) closing `known-gaps.md:309-315`'s
O(chain) walk. If taken, its number goes in VC-C5's file; if not, the
gap entry stays and says this order left it.

**VC-C4 — observability and the harness.** `SHOW META` gains
`varheap_slots_released`, `varheap_pages_recycled`,
`varheap_release_pending` beside `undo_pages_live`
(`command_dispatcher.cpp:870`), per core. `sim/integrity.cpp`: the cell
walk reads the per-column width (VC-A3's census), **skips spilled-cell
resolution for delete-marked tuples**, and classifies `NotFound` on a
live tuple's pointer as the defect it is. The `sim/` generator gains
`varchar(N)` columns at two widths and ops that cross the spill boundary
by `UPDATE`, delete spilled rows, and roll back spilling statements.
`scripts/sim.sh` green over the existing cells plus a **crash cell whose
seed lands between an append and its tuple write**, found by sweep and
then pinned as a test the way `SimLoop.ALongRunRollsASegmentAndStillRecoversEveryAcknowledgedRow`
pins seed 24.

**VC-C5 — the numbers (ck-tester).** `build-release`, every number
stamped `git describe --tags`, file
`bench/v2.5.0/results-varchar-<git describe>.md`, raw driver JSON under
`bench/v2.5.0/archive/`:

1. **The plateau** (UP5's shape): 30k `UPDATE`s of a spilled value on
   one row, then on 1k rows. Report `ChainLength`, the data file's size
   and `varheap_pages_recycled` at 16 samples; the claim is flat after
   warm-up with the recycle counter climbing. State the caveat UP5
   states: with no concurrent reader the steady state is a ping-pong by
   construction; a long-running reader grows the chain for its lifetime
   and that is D3's accepted cost, not a defect the run can see.
2. **The per-spill undo cost.** WAL bytes per spilling `INSERT` before
   and after VC-B3 (one `UNDO_WRITE` more: `kUndoWriteFixedSize` + the
   envelope), and a single-arm p50/p99 — **not** an interleaved A/B,
   which the 2026-08-24 amendment suspends; say so in the file, and
   carry "overhead not measured" on the row.
3. **The drain at growth**, if the rider in C3 was taken: p99 of the
   spilling `INSERT` with and without the hint, same caveat.

PostgreSQL twin skipped by decision, as UP5 skipped it: the analogue is
TOAST plus VACUUM, a different mechanism on a different schedule.

**VC-C6 — phase C prose.** `docs/spec/page.md` §5a (last bullet:
reclamation exists, what triggers it, what a crash loses);
`include/kds/storage/varheap.hpp` header ("Reclamation rides on purge"
→ the queue, the drain, the recycle, the leak); `docs/inflight/known-gaps.md`
reclamation section (the var-heap entry rewritten: what releases, the
crash-only leak until VC-C7, the O(chain) walk's status);
`docs/inflight/in-progress/workplan-undo-purge.md` "Not in scope" (the
var-heap clause now points here); `docs/spec/heap-and-tuple.md` §3.4 and
§9 (purge-cadence metric now has counters); `docs/spec/txn.md` §9;
`CLAUDE.md` — the Pages row (var-heap), the Transactions row (var-heap
reclaim in, with the leak named), the Storage line of Open Decisions
(purge cadence). Then `workplan-varchar-char.md` is **deleted**, per the
2026-08-26 rule, once every row above is built — its D1–D5 record moves
into the spec that owns each answer first.

**VC-C7 — the crash leak's remedy (last, and gated).** A mount-time
mark pass: for each relation with a chain, walk its live tuples (after
recovery no reader exists, so only the latest version of a
non-delete-marked tuple is live), mark every slot a `kSpilled` cell
names, release every other slot, and recycle pages that end up empty.
Reported in `SHOW META`'s recovery block as `varheap_slots_swept`.
**Gate:** the pass costs O(relation + chain) once per unclean mount;
measure it on the 1k- and 10k-row spilling relations of VC-C5 before
deciding it runs by default. If the cost is not acceptable, it ships
behind a flag or not at all, and `known-gaps.md` keeps the leak as UP4
keeps its own — stated, bounded, owned.

---

## 4. Out of scope, by decision

- **A length cap for `varchar(N)`** — D1; the only refusal is 8144 bytes.
- **A floor below 16 for `varchar(N)`** — D2.
- **Heap slot retirement** for deleted tuples (`workplan-undo-purge.md`
  "Not in scope").
- **`DROP TABLE` chain release** (`docs/spec/drop-table.md`'s gate).
- **Multi-page values**; the cap stays `[OPEN]`.
- **Var-heap partition under a range split** (`crosscore.md` §6a gate).
- **`ALTER … TYPE varchar(M)`**, any spelling — permanently out, with its
  reason corrected (VC-A7).
- **SQL space-padding for `char`** — architecture §6 (1).
- **Sharing a var-heap slot between versions** — refused by the model,
  pinned by VC-B4.
- **A durable release queue** — D3's rejected alternative.

---

## 5. What stops the chain

Per `CLAUDE.md`'s Session Workflow, unchanged: a failed review, a red
suite, or an unrun measurement stops the row before the merge, and the
report says which. Two stops particular to this order:

- **A second name for `inline_cell_width`** anywhere in the diff — a
  config key, a catalog field, a constant, a `SHOW META` field, a
  message — is a review finding that blocks the row.
- **VC-C2 landing without `AReaderAboveTheWriterKeepsTheOldValueAlive`
  green** is a soundness hole, not a missing test: a release the horizon
  did not license is a committed value lost.

---

## 6. Done when

1. VC-0 through VC-C6 built, reviewed, tested, merged; VC-C7 built or
   gated with its measurement in `bench/v2.5.0/`.
2. `bench/v2.5.0/results-varchar-<git describe>.md` exists with the
   plateau, the per-spill cost, and the suspension named.
3. Every doc in VC-A7, VC-B5, VC-C6 amended at the source; no claim in
   `docs/spec/` or `manual/` still says "no `VARCHAR(n)`", "no `Free()`",
   or "nothing reclaims" without a date and a pointer to what replaced it.
4. `docs/inflight/known-gaps.md` states, per core, exactly what a crash
   still leaks and what a clean shutdown does not.
5. The operator names the tag. Nothing here does.

---

## 7. The gaps this version leaves, stated now

- **A crash leaks the queue** until VC-C7 lands or is refused; clean
  shutdown does not leak.
- **A deleted tuple's heap slot stays**; only its spilled bytes go.
- **A dropped relation's chain orphans**, as every dropped page does.
- **`varchar(8)` is refused** — the unification's stated wart (D2).
- **Every `UPDATE` of a row with a spilled value copies it** — the
  ownership fact's cost, present at `814c568`, now named and pinned.
- **Overhead not measured** on every row, per the 2026-08-24 amendment;
  the two numbers VC-C5 reports are the feature's claims, not a
  regression guard.
- **The O(chain) append walk** stays unless VC-C3's rider is taken.
