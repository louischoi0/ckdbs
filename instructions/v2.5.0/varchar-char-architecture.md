# v2.5.0 — `char(N)`, `varchar(N)`, and the var-heap's lifetime: architecture overview

Drafted 2026-08-28 in worktree `hotfix-varchar` against `main` at `814c568`
(`v2.2.1-89-g814c568`). Companion: `instructions/v2.5.0/varchar-char-workorder.md`,
which turns this overview into task rows. This file says *what the design
is and why*; the work order says *what to build, in which order, and when
it is done*.

> **Version named by the operator, 2026-08-28.** The operator's words:
> *"v2에서는 char(N)과 varchar(N)을 모두 지원해야해. varchar(N)의 경우 선언된
> max_inline_char_size 만큼 넘지 않으면 그대로 cell에 값을 두고, 이값을 넘으면
> 포인터 형식으로 var-heap에 저장해야해. undo/reclaim 도 구현되어야 하고."* —
> and, mid-drafting: *"max_inline_char_size를 따로 두지 말고 기존
> inline_cell_width와 통합해."* The second message governs §3.2 below:
> **there is no `max_inline_char_size`.** The number `varchar(N)` declares
> *is* `inline_cell_width`, scoped to one column. The tag on `main` is
> `v2.2.1`; nothing here mints `v2.5.0`, and every measurement names itself
> by `git describe --tags` until the operator pushes the tag.

Discipline, unchanged from `instructions/v2.2.0-stmtshipping.md`: every
claim below is **source-read** (`path:line` at `814c568`) or marked as a
decision for the operator. Nothing is measured yet; §8 says what will be.

---

## 1. What exists today, and the three gaps

The engine below the parser already handles a `char` of any width and a
`varchar` that spills. What is missing is the declaration surface for the
first, a per-column width for the second, and any way for a spilled value
to die.

| Fact | Where |
|---|---|
| A type argument is recognised by the paren, not the name, and refused for every type but `DECIMAL`: *"type 'X' takes no arguments (byte N)"* | `src/parser/parser.cpp:430-436` |
| `CREATE TABLE` copies `sys.types.len` into the column: `char` is **1 byte**, `varchar` is **0** | `src/server/command_dispatcher.cpp:3176-3180`; `src/catalog/catalog.cpp:630-631` |
| `RowLayout::ColumnWidth` already reads `col.len` for `char` and the instance-pinned `inline_cell_width` for `varchar` | `src/catalog/row_layout.cpp:23-25` |
| The `char` codec zero-fills an `N`-byte cell and refuses a longer value; the decoder stops at the first NUL | `src/exec/row_codec.cpp:170-187`, `:468-479` |
| The `varchar` codec inlines at `len <= W - 3` and otherwise appends to the relation's chain and writes a `kSpilled` descriptor — the cell's *tag* changes, never its size | `src/exec/row_codec.cpp:188-229`; `include/kds/storage/tagged_cell.hpp` (`InlineCapacity(W) = W - 3`, `kCellSpilledSize = 13`) |
| `DESCRIBE` can already print `char(8)`, through `ColumnTypeText` — a spelling no `CREATE TABLE` can produce | `src/catalog/rows.cpp:349-362`; `tests/catalog_row_test.cpp:496` |
| The var-heap has **no `Free()` by design**, its slot has **no dead state**, and its header says reclamation "rides on purge", which "does not exist" | `include/kds/storage/varheap.hpp` (file header; slot directory comment); `docs/spec/page.md` §5a last bullet |
| The undo purge that landed 2026-08-19 recycles **undo** pages only and names var-heap reclamation as out of its scope | `docs/inflight/in-progress/workplan-undo-purge.md` "Not in scope" |
| The per-column width was **rejected on purpose** in v1: "no `VARCHAR(n)` grammar, no `ALTER … WIDEN` question" | `docs/rules/rule-fixed-length-tuple.md` §4; `docs/spec/heap-and-tuple.md` §3.3 |

The three gaps, then: **(G1)** `char(N)` is not declarable; **(G2)**
`varchar(N)` is not declarable, by a v1 decision this version reverses;
**(G3)** a spilled value is never released — not on rollback, not when its
version dies, not ever. G3 is the one with engineering in it. G1 and G2
are declaration plumbing over machinery that exists.

---

## 2. The design in one paragraph

`char(N)` is a fixed `N`-byte cell with no tag, exactly what the engine
stores for `char` today with `N` made declarable. `varchar(N)` is the
tagged cell the engine stores today with its width made a **column**
property instead of an **instance** property: `N` is that column's
`inline_cell_width`, validated by the same function, bounded by the same
`[16, 4096]`, holding `N − 3` bytes inline and spilling past that to the
relation's var-heap through the unchanged `kSpilled` path; a bare
`varchar` keeps the instance-pinned width, so every existing file mounts
unchanged and no format bumps. The var-heap gains the one operation its
header refused — a **release** — and gains it under a lifetime model that
is already true in the code: **each tuple version owns its spilled slots
exclusively**, so a version's death is its slots' death. A version dies in
three ways the engine already sees — rollback of the write that installed
it, supersession by a writer that has settled below `ReadHorizon()`, a
delete-mark whose writer has settled — and each becomes a logged
`VARHEAP_RELEASE` of the slots it owned. A page whose every slot is
released is reformatted in place and becomes the chain's next append
target, so the chain stops growing without touching the free map.

---

## 3. The two types

### 3.1 `char(N)` — the fixed cell

- **Storage.** `N` bytes at the column's offset, no tag, no length. A
  value shorter than `N` is zero-padded (`row_codec.cpp:181-185` already
  zero-fills before writing, for the same reason `EncodeInlineCell` does:
  an overwrite must leave no stale tail). A value longer than `N` is
  `InvalidArgument` naming `N` and the column (`:174-178`, message
  unchanged in kind, `N` now the declared number rather than 1).
- **Read-back.** The decoder returns bytes up to the first NUL
  (`:468-479`). That makes an **embedded NUL byte** non-round-trippable,
  so the encoder gains one refusal: a `char` value containing `0x00` is
  `InvalidArgument`. Truthfulness over convenience — the engine must not
  accept a spelling and store something it will read back differently.
- **Comparison and keys.** Byte-wise over the unpadded value: `'ab'` and
  `'ab '` are different values. **No SQL space-padding semantics** — a
  stated divergence for the manual, in the same register as
  `docs/spec/null.md` D1. Index keys already spend `min(N, 32)` bytes
  for a `char` (`src/exec/index_key.cpp:124-129`); nothing changes there.
- **`char` with no argument is `char(1)`**, which is what it has always
  been (`sys.types` row `{kTypeChar, "char", kTypeValChar, 1}`) and what
  SQL says. Not refused like a bare `decimal`: that refusal guards a
  silent decision about a stored value's *meaning*; `char(1)` is the
  standard's own default and changes no value's meaning.
- **Bounds.** `N >= 1` (a zero-width column is already refused by
  `RowLayout::Build`, `row_layout.cpp:76-83`). No ceiling of its own: the
  row-size check against `heap::kMaxTuplePayloadSize` (8115,
  `row_layout.cpp:101-106`) is the ceiling, and it names the row's size
  rather than inventing a second number.
- **NULL.** Through the bitmap, as every column (`null.md` §3). A NULL
  `char` cell is zeros; the bitmap is the authority; there is no tag to
  disagree with it.
- **Wire.** Length-prefixed bytes, unchanged: `src/wire/row_codec.cpp:104-107`
  already says a `char`'s stored width is a schema fact the wire does not
  leak.

### 3.2 `varchar(N)` — the tagged cell, with `N` as its `inline_cell_width`

**One concept, two scopes.** Today `kds.inline_cell_width` is one number
for the instance (`tagged_cell.hpp`: default 64, `[16, 4096]`,
superblock-pinned, validated at every mount by `CheckInlineCellWidth`).
After this version it is the **default** a column takes when declared as
bare `varchar`, and `varchar(N)` **overrides it for that column**. Same
unit (bytes of cell, tag and length included), same validator, same
capacity formula `N − 3`, same cell layout:

| tag | after the tag | when |
|---|---|---|
| `kNull` | zeros | bitmap says NULL (`null.md` §3: the tag is filler, the bitmap decides) |
| `kInline` | `len u16`, bytes, zero padding | `len <= N − 3` |
| `kSpilled` | `len u32`, `varheap_ptr u64` | `len > N − 3` — bytes in the relation's var-heap |

Consequences, each a rule:

- **`varchar(64)` and a bare `varchar` at the default are the same column.**
  Same width, same capacity (61), same bytes on disk. Only the catalog
  row differs (`len = 64` versus `len = 0`), and only `DESCRIBE` shows
  it.
- **`N` is a width, not a length cap.** A value longer than `N − 3` is
  not refused; it spills, exactly as a long value spills today. The only
  refusal a `varchar` value meets is the one it meets today — larger than
  one var-heap page (`varheap::kMaxValueSize == 8144`, `Unsupported`) —
  and that cap stays the `[OPEN]` decision `rule-fixed-length-tuple.md`
  §9 says it is. (Reading `N` as SQL's length cap is D1 in §7, refused
  by the operator's phrasing — "넘으면 포인터 형식으로 var-heap에 저장" —
  and recorded there so nobody re-derives it.)
- **Bounds are inherited, not invented.** `varchar(N)` passes `N`
  through `CheckInlineCellWidth`, so `N < 16` and `N > 4096` are refused
  with the same message the config file's value gets, plus the byte
  position of the argument. `varchar(8)` is therefore refused: the
  narrowest cell that can still hold a spilled descriptor is 16 bytes
  (`kCellSpilledSize = 13`, rounded to 8 — `tagged_cell.hpp`'s stated
  reasoning). This is a usability wart the unification accepts on
  purpose: a second, smaller floor for the column form would be a second
  concept, which is what the operator's amendment removed.
- **The instance setting stays pinned and stays mandatory.** Bare
  `varchar` columns, every bootstrap relation (`sys.pattern_defs`, the
  assertion catalog, `sys.patterns`), and every column in an existing
  file read at `len = 0` and take the superblock's width. A file with a
  different pinned width still refuses to mount against a disagreeing
  config, exactly as today.
- **`SchemaCanSpill` is unchanged**: any `varchar`, of any `N`, can
  spill, so every relation with one still gets its chain at `CREATE TABLE`
  (`src/catalog/catalog.cpp:1273-1283`) and every gate keyed on
  `SchemaCanSpill` — the range split gate (`docs/spec/crosscore.md`
  §6a), `range_eligible.cpp:41`, the bulk-insert fast path
  (`command_dispatcher.cpp:4331`) — reads exactly as before.
- **Index keys stay at the 32-byte prefix** (`index_key.cpp:133`,
  `kIndexStringKeyBytes`). A `varchar(N)` value can be longer than `N`,
  so `N` bounds nothing a key could use; the prefix rule is unchanged.
- **Widening stays refused.** `ALTER … TYPE varchar(M)` is a rewrite of
  every row (invariant 13), so it remains permanently out — but
  `docs/spec/alter.md:26-28`'s *reason* ("the tagged cell has no
  per-column width to widen") becomes false and must be rewritten to the
  true one.

### 3.3 The catalog encoding — `len`, no format bump

`SysColumnRow::len` (`include/kds/catalog/rows.hpp:176-197`) already means
"the declared width" for `char` and "the packed `(p, s)`" for `decimal`,
and is dead weight for every other type — which is how TY9 fit a
decimal's precision into it with no superblock bump
(`docs/spec/types.md` §4a). This version uses the same field the same
way, for the last type that was not reading it:

| `type_val` | `len` | read by |
|---|---|---|
| `kTypeValChar` | the declared `N` (unchanged) | `RowLayout::ColumnWidth`, `IndexKeyColumnWidth`, `ColumnTypeText` |
| `kTypeValVarchar` | **the declared `N`, or `0` for a bare `varchar`** | `RowLayout::ColumnWidth` (`len ? len : inline_cell_width`), `ColumnTypeText` (`varchar(N)` when `len > 0`, `varchar` otherwise) |

`0` is the compatibility value: every `varchar` column written before this
version carries it, and it reads as "the instance width" — which is what
those columns have always been. No `SysColumnRow` field is added, no
offset moves, no superblock version bumps, and a pre-v2.5.0 file mounts
byte-identical. `DESCRIBE` and `sys.columns` render through the one
function TY9 routed them through, so they cannot disagree.

### 3.4 The layout — per-column widths, one reader at a time

`RowLayout` today carries a single `inline_cell_width` and its readers
assume every tagged cell has it: `DecodeRow` and `EncodeRow`
(`src/exec/row_codec.cpp`), `sim/integrity.cpp:309`
(`access.layout.inline_cell_width`), the assertion catalog and
`pattern_defs` sinks. The layout keeps `inline_cell_width` (the instance
default is still a fact every layout is built under) and gains the
per-column cell width each reader needs — derivable from `offsets`, but
carried explicitly so a reader never has to know that the null bitmap
follows the last column. Every reader is listed in the work order; the
review's first question is whether any was missed.

---

## 4. The var-heap's lifetime model — undo and reclaim

### 4.1 The ownership fact the whole model rests on

`UPDATE` decodes the old row into values and **re-encodes the whole row**
through `EncodeRow` with a `VarHeapSink` (`command_dispatcher.cpp:6338`,
`:6365-6370`). A spilled value that the `SET` did not touch is therefore
**appended again** — a fresh copy, a fresh slot — and the new version's
cell points at the copy, never at the old version's slot. So, at
`814c568`, **no two versions of a tuple share a var-heap slot.** A slot
has exactly one owner, and the owner is a version.

This is a fact about the current code, not a decision anyone wrote down,
and the model below *depends* on it: "a version dies → its slots die" is
exact only while ownership is exclusive. Two consequences:

- It gets a **test** (VC-B4 in the work order), so a future "reuse the
  old slot when the value is unchanged" optimisation cannot land without
  first replacing the model.
- Its **cost is named**: every `UPDATE` of a row holding a spilled value
  copies that value. That cost exists today; this version does not add
  it, but it is the reason a churn-heavy string workload will show a
  visible `varheap_slots_released` rate in `SHOW META`.

### 4.2 When a version dies

A tuple version becomes unreachable by every live and future traversal in
exactly the situations the undo purge already reasons about
(`workplan-undo-purge.md`, "The one soundness fact"):

| death | when the slots are dead | who knows |
|---|---|---|
| **(a) rollback** of the write that installed the version — live `Abort`, or recovery's undo phase for a loser | immediately: nothing but the aborting transaction ever pointed at them | the writing transaction, through its trail and its undo chain |
| **(b) supersession** — an `UPDATE` wrote a newer version | when the **superseding writer** drops below `ReadHorizon()`: the old version lives only in the superseder's before-image, which the soundness fact says is unreachable from then on | the superseder, at `UPDATE` time, holding the old row decoded |
| **(c) deletion** — a delete-mark | when the **marking writer** drops below `ReadHorizon()`: no view can see the pre-delete version after that | the deleter, at `DELETE` time |
| **(d) `DROP TABLE`** | the whole chain | out of scope — the chain orphans exactly as every page of a dropped relation does (`docs/spec/drop-table.md`), behind the same reclamation gate |

(a) is **undo**. (b) and (c) are **reclaim**. (d) is not this version's.

### 4.3 Undo — the append becomes a link in the transaction's chain

Every var-heap append gets what every other page mutation already has:
a **trail entry** for the live rollback and an **undo record** for
recovery.

- `TrailAction::kVarHeapAppend` (`include/kds/txn/manager.hpp:92-97`):
  `(rel_oid, varheap page_id, slot, pk)`. Compensated by a release.
- `UndoRecordType::kVarHeapAppend` (`include/kds/txn/undo_page.hpp:122-144`):
  image empty, `target_page_id`/`target_slot` name the **var-heap** slot,
  `pk` the row. A link in `txn_prev_undo_ptr`'s chain, so recovery's undo
  phase reaches it however far below the redo start it lies — RV10's
  argument for `kInsert`, verbatim.
- **Ordering: the `UNDO_WRITE` precedes the `VARHEAP_APPEND`.** RV3's
  rule for catalog writes, for the same reason: redo alone must never
  resurrect an append the undo phase has no record to release. `LogSpills`
  (`src/exec/wal_row_log.cpp:14-63`) is the one place every spill is
  logged, so it is the one place the record is written.
- **Compensation is the release** (§4.5), and it is **idempotent**:
  releasing a released slot is a no-op, which is the property every
  compensation must have for recovery to be crash-restartable
  (`include/kds/txn/recovery_undo.hpp`, "no CLR"). The no-locator identity
  check (`Compensate`, `src/txn/manager.cpp:236-263`) does not apply —
  the target is a var-heap slot, not a row — and the type says so.

What this closes: `rule-fixed-length-tuple.md` §5 line 61 and
`heap-and-tuple.md` §3.4 both hand a crash *between* `VARHEAP_APPEND` and
the tuple write to "purge's reclamation sweep". With the append in the
loser's chain, recovery's undo phase releases it like any other loser
write, and **no sweep exists to be owed**.

### 4.4 Reclaim — the deferred-release queue, drained at the horizon

(b) and (c) cannot release at the moment they happen: a reader above the
superseder may still be walking into the old version. They release when
the writer settles. The undo purge solved the identical problem for undo
pages with a **memory-resident side table keyed by writer id**
(`undo_log.hpp`, `TrackedPage::max_trx_id`; `workplan-undo-purge.md` D2's
ratified sub-decision: "in-memory for v1"). This version copies the
shape:

- **A per-core `varheap::ReleaseQueue`** owned by the `TransactionManager`
  (it owns `ReadHorizon()` and sees `Abort`), entries
  `{writer trx_id, rel_oid, VarHeapPtr}`.
- **Fed at `UPDATE` and `DELETE`** with the *old* version's `kSpilled`
  pointers. `UPDATE` already has them: `DecodeRow` records every spilled
  cell as a `PendingSpill` (`command_dispatcher.cpp:6360`'s `spills`).
  `DELETE` reads the marked tuple's `varchar` cells off the page in hand
  (`DecodeCell` per column, no fetch — the pointer is in the cell).
- **`Abort` drops the aborting writer's entries**: the old version
  survived, so its slots are not dead. `Commit` does nothing — the entry
  waits for the horizon, not for the commit.
- **The drain** releases every entry whose `trx_id < ReadHorizon()`,
  logging each (§4.5). Triggered where UP3 triggers the undo purge —
  **on chain growth, before allocating** — and at checkpoint, so a quiet
  instance still settles. A drain is a purge pass: it may release
  another relation's entries while relation R grows, which is correct
  and cheaper than a per-relation queue.
- **Why memory-resident, and what it costs.** The queue is
  reconstructible bookkeeping, the FPI-exemption argument in reverse;
  logging it would touch `wal.md` §4.1 for a structure a crash can only
  *lose*, never corrupt. A crash forgets the queue, and every slot it
  held **leaks** — the same class of leak UP4 records for undo pages, with
  the same remedy deferred: a mount-time mark pass (work order VC-C7)
  that walks a relation's live tuples and releases every slot no live
  cell names. Clean shutdown drains the queue first, so the leak is a
  crash-only fact.

### 4.5 The release — one record, one tombstone, no moved byte

- **`RecordType::kVarHeapRelease`**, the next unassigned number after
  `kTxnPrepare = 27` (`include/kds/wal/record.hpp`; append-only, never
  reused; adding one is a format-version event per the enum's rule — RP1's
  `kTxnPrepare` is the precedent to copy). Payload `{slot u16}`; envelope
  `page_id` the var-heap page, `txn_id` the releasing transaction for a
  rollback compensation and `kNoTxnId` for a drain — the same split
  `txn.md` §6 states for `SLOT_RETIRE`.
- **`varheap::PageRelease(page, slot)`**: sets the slot's `offset` to
  `0`. A live value's offset is never below `kVarHeapHeaderOffset +
  kHeaderSize`, so `0` is an unambiguous tombstone that **adds no field
  to `VarHeapSlotFields`** and changes no page's format; `length` is
  kept, for diagnostics. The value's bytes are not touched: invariant 14
  (values immutable, pages never relocated) holds to the letter — a
  released value is *dead*, not *moved*.
- **`PageRead` of a tombstone answers `NotFound`**, not `Corruption`.
  Under the model no live traversal can reach one; the distinct status
  is for `sim/integrity`, which walks delete-marked tuples and must be
  able to say "released, as expected" rather than "corrupt".
- **Idempotent** by construction: tombstoning a tombstone writes the
  same zero. Redo applies it through the ordinary `page_lsn` gate.

### 4.6 Page recycle — in place, in the chain

A page whose `nr_slots > 0` and whose every slot is a tombstone holds
nothing any traversal can reach. It is **reformatted in place**
(`FormatPage` + `PAGE_INIT{kVarHeap}`, the record and applier that
already exist for chain growth) and becomes an append target. Nothing
else changes:

- **The chain's links are untouched.** No unlink, no predecessor edit,
  no full page image. The page keeps its position; it is simply empty
  again. The root (`sys.tables.varheap_page_id`) is a cached catalog fact
  and is recycled like any other page but never *freed*.
- **The free map is untouched.** `page.md` D9 (the map stays unlogged)
  and `physical-optimizer.md` §6 gate 3 (cross-relation page reuse) are
  not this version's fight, exactly as `workplan-undo-purge.md` D2(i)
  said of undo. A relation's var-heap footprint high-water-marks instead
  of growing.
- **The appender targets the first page with room**, which a recycled
  page is. `ChainAppend` already walks root → tail on every append
  (`src/storage/varheap.cpp:232-247`; the O(chain) walk is a named gap,
  `known-gaps.md:309-315`) and fetches every page on the way, so
  "first page with room" costs nothing the walk does not already pay —
  and an all-dead page met on the walk can be recycled on the spot,
  which is what makes the recycle set need no side list and survive a
  crash for free. Values have no ordering property (`varheap.hpp`,
  "Chain"), so a value landing in a middle page is as reachable as one
  in the tail. A per-relation append hint (the heap's `heap_tail_hint`
  shape) would close the O(chain) gap as a rider; the work order names
  it as optional and measured, not required.
- **Stale pointers into a recycled page** exist only in dead versions —
  before-images below the horizon, delete-marked tuples below the
  horizon — which the soundness fact makes unreachable by every live
  traversal, and which recovery cannot need (a loser is active, so
  nothing it references has settled). `sim/integrity` is the one walker
  outside that argument; it skips spilled-cell resolution for
  delete-marked tuples and treats `NotFound` as the expected answer
  there.

### 4.7 Redo, recovery, checkpoint — nothing new to invent

| event | record(s) | applier |
|---|---|---|
| spill | `UNDO_WRITE{kVarHeapAppend}` → (`PAGE_INIT{kVarHeap}` + `FULL_PAGE_IMAGE` on growth) → `VARHEAP_APPEND` | existing, plus the undo record type |
| release (rollback or drain) | `VARHEAP_RELEASE` | `PageRelease`, idempotent |
| recycle | `PAGE_INIT{kVarHeap}` on the same page id | `wal::ApplyPageInit`, existing; the next `VARHEAP_APPEND` lands at slot 0, and `PageWriteAt`'s dense-slot rule holds because the page is empty |
| loser's spill | its `kVarHeapAppend` chain record | recovery undo → release |
| checkpoint | `CHECKPOINT_BEGIN`'s active-transaction undo heads already cover the chain; the checkpointer's flush covers recycled pages as it covers every page | existing |

### 4.8 Observability — fields on `SHOW META`, per core

`varheap_slots_released`, `varheap_pages_recycled`,
`varheap_release_pending` (queue length), beside `undo_pages_live` /
`undo_pages_recycled` (`command_dispatcher.cpp:870`). The plateau test in
the work order reads the second and the data file's size, exactly as UP5
read the undo pair.

### 4.9 What the model deliberately does not do

- **Heap slot retirement.** A deleted tuple's slot stays; only its
  spilled bytes are released. Slot reclamation has its own owner
  (`workplan-undo-purge.md` "Not in scope").
- **Chain-level release on `DROP TABLE`.** The chain orphans behind the
  gate every dropped page is behind.
- **Multi-page values.** The 8144 cap stays `[OPEN]`.
- **Var-heap partition under a range split.** The `crosscore.md` §6a
  gate stays.
- **Sharing a slot between versions.** Refused by the model (§4.1);
  reopening it means replacing the model, not patching it.
- **The prior-run leak.** A crash loses the queue; VC-C7's mount-time
  pass is the remedy and is the last row, not the first.

---

## 5. Interactions with confirmed design

- **Fixed-length rule (invariant 13).** Untouched in substance: a row's
  size is still a schema constant; what changes is that the constant is
  now computed from per-column widths for `varchar` as it already was for
  `char`. `rule-fixed-length-tuple.md` §4's global-over-per-column
  rationale is **reversed by this version** and the section is rewritten
  to say so, dated, keeping the original argument as history.
- **Invariant 14.** Holds verbatim: a release writes a slot's `offset`,
  never a value's bytes, and no page moves.
- **MVCC.** An old-version reader still follows an old pointer to bytes
  that cannot have changed — until the version is dead by the soundness
  fact, after which no reader follows it. The `ReaderLease` that bounds
  `ReadHorizon()` (T01-T14) is what makes "dead" a fact rather than a
  hope, and the model is unsound on any core where a snapshot can outlive
  a park without one.
- **Recovery.** One new undo record type, one new WAL record type; the
  analysis, redo and undo phases change only by learning them.
- **Indexes, Cabin, assertions, patterns.** Key on the value's bytes,
  never on where they live; unchanged. `pattern_defs.cpp:154-161`'s body
  cap is `varheap::kMaxValueSize` and stays.
- **Statement shipping, peer writes, range gates.** Carry text or key on
  `SchemaCanSpill`; unchanged.
- **Fingerprint.** `varchar(64)` in a `CREATE TABLE` adds a paren and an
  int literal to a DDL token stream. `kFingerprintVersion` stays 1 by the
  argument `src/parser/fingerprint.cpp:166-172` gives for `kNumLit` — the
  bump rule protects what is *stored*, and a DDL hash never reaches
  `sys.patterns` — but VC-0 verifies that premise rather than assuming
  it.
- **Eviction, buffer pool.** A drain fetches the pages it releases into
  and pins them like any writer; nothing new.

---

## 6. Divergences from SQL, for the manual

Stated once here so `manual/sql/sql.md` §2 can carry them in D1's
register:

1. `char(N)` pads with zeros, not spaces, and compares byte-wise on the
   unpadded value — `'ab' <> 'ab '`.
2. `varchar(N)` declares a **cell width**, not a maximum length. A longer
   value is stored, out of line. The only length refusal is 8144 bytes.
3. `varchar(N)` accepts `N` in `[16, 4096]` and nothing else; `varchar`
   alone means the instance's `inline_cell_width`.
4. `char` alone is `char(1)`.
5. A `char` value may not contain a NUL byte.

---

## 7. Decisions for the operator — `[OPEN — ratify]`

Each has a recommendation and is the drafted basis; the work order's
first row is the gate that records the answers.

- **D1 — `varchar(N)`'s `N` is a cell width, not a length cap.**
  RECOMMENDED and drafted. The alternative — SQL's cap, refusing a longer
  value — would need a *second* number to say where the cell ends, which
  is the separate concept the operator's amendment removed. Refusing D1
  reopens §3.2 whole.
- **D2 — bounds are inherited from `CheckInlineCellWidth`** (`[16,
  4096]`), so `varchar(8)` is refused with the byte position.
  RECOMMENDED. The alternative — a lower floor for the column form —
  is a second validator for one concept.
- **D3 — the deferred-release queue is memory-resident**, crash-leaky,
  with the mount-time pass as the deferred remedy. RECOMMENDED; it is
  the shape D2 of the undo purge ratified. The alternative — a durable
  `kVarHeapRelease` undo record written at `UPDATE`/`DELETE` and
  *cancelled* on abort — needs an in-place edit of an undo record, which
  the append-only undo page has no operation for, and is rejected on
  that ground rather than deferred.
- **D4 — a released page recycles in place, in its chain**, never to
  the free map. RECOMMENDED; D9 and gate 3 are not reopened.
- **D5 — `char` embedded NUL is refused.** RECOMMENDED. The alternative
  — carrying a length for `char` — makes it a `varchar` with a different
  name.

---

## 8. What will be measured, and what will not

The operator's 2026-08-24 amendment suspends the interleaved A/B overhead
measurement for v2-stage work; the work order's measurement rows say so
where they apply and every landed row carries "overhead not measured" as a
stated fact. Two numbers are asked for anyway, because each is the
feature's own claim rather than a regression guard: **the footprint
plateau** under a spilling `UPDATE` loop (chain length flat,
`varheap_pages_recycled` climbing — UP5's shape), and **the per-spill cost
of the undo record** (one `UNDO_WRITE` more per spilled cell, priced in
bytes and in microseconds, `build-release`, `git describe --tags` on
every number).
