# Work order BK — a 48-bit Keystone id: the pk widens from 40 to 48 bits inside the one 64-bit word

Written 2026-10-11 on `worktree-bk-keystone-48` from `5979b5e7`
(`v2.7.0-763-g5979b5e7`), on the operator's words, translated from Korean:

- **W0:** *"Is it possible to change kdbs's pk width to 42 now? What would
  the scope of work be? Do not mind backward compatibility of the data."*
- **W1:** *"How about changing it to about 48?"*
- **W2:** *"Do it as 48 bits. Write a new milestone and ask me about the
  parts that need a decision."*

**Not opened.** BK-Q0 is unmarked. W2 asks to be asked, so no standing
go-ahead applies to this order: every item in §4 waits for the operator's
own mark, and nothing is adopted on CLA's proposal.

**What the words settle, and what they leave open.**

- **W0** asked for 42. CLA answered that 42 is possible and costs the
  byte alignment of `flags` (bits 40-47 today, 42-49 under 42), for no gain
  a wider width does not also give. No file was written on W0.
- **W1** asked about 48. CLA answered that 48 is the better width: the id
  takes bytes 0-5 whole, `flags` stays one whole byte, and the two bits of
  `reserved` the 42 would have kept become eight. **W2 chose it.**
- **W2 overturns one written decision**:
  `docs/rules/keystoneid-invariant.md:230-233` says *"Widening the id beyond
  40 bits was considered and rejected: it forfeits the fixed 64-bit
  super-column word (40+8+16) that the tuple header, meta-pool handle, and
  page arithmetic are built on."* BK keeps the 64-bit word, the meta-pool
  handle it names is retired (`keystone.hpp:11-14`), and no page arithmetic
  reads the id's width (§1.7). The sentence is restated by BK-S3 as a
  decision taken on 2026-10-11, not deleted (BK-Q2).
- **W0's "do not mind backward compatibility"** meets a standing order
  that already answers it: since 2026-10-07 a format change bumps the
  superblock version and refuses older volumes, never migrates
  (`raft-marks-2026-10-07.md` §11; BD-S2 took 19 → 20, BF-S3
  20 → 21). BK-R4 applies it: 21 → 22. It is a ruling here, not an item.

**BK is not part of AR0 §8's chain.** It touches one constant and what
derives from it. It shares a surface with BH (PURGE's pk window is judged
against `kIdSpaceEnd`, `command_dispatcher.cpp:9283, 9903`) and with BD
(the exhausted refusal, BD-R5), both of which follow the constant and need
no restatement of their rules. It blocks no open order and none blocks it;
BI (checkpoint) and BJ (expression `UPDATE`) touch no id width.

## 0. What BK is

**Today the Keystone word is `id:40 | flags:8 | reserved:16`**
(`include/kds/storage/keystone.hpp:37-39`), and the pk of every relation is
a 40-bit id: `[1, 2^40 - 1]` = `[1, 1099511627775]`, about 1.1 × 10^12 ids
issued over a relation's lifetime (K4). **After BK it is
`id:48 | flags:8 | reserved:8`**: `[1, 2^48 - 1]` = `[1, 281474976710655]`,
about 2.8 × 10^14. Nothing else about the word changes: it is eight bytes
at the front of every tuple, written and read as one `uint64_t`, by
shift and mask.

**Why the change is small.** The width is one `constexpr`
(`kKeystoneIdBits`) and two values derived from it (`kMaxKeystoneId`,
`kIdSpaceEnd`), which eleven files under `include/` and `src/` besides
`keystone.hpp`, and `sim/integrity.cpp`, read by name (§1.1, §1.4). Every
external store of an id - a btree key, `min_key`, an index entry, an undo
record, a Waystone or observational Cabin entry, `sys.tables.next_id` - is a
zero-extended `uint64_t` already, so no field widens. **One record format
does move**: the Cabin bound entry's word, on its pages and inside every
`ASSERT_RESERVE`/`ASSERT_BUILD` record, carries live flags in bits 40-47,
and they move to 48-55 (§1.2, §1.7). What BK has to find is the four places
that spelt the number instead of naming the constant (§1.2, §1.3), and the
one of them that would be *wrong* rather than *refusing* if missed.

**Why 48 and not 42 or 56.** 42 splits `flags` across a byte boundary for
2 bits of capacity. 56 would leave 8 bits for `flags` and `reserved`
together, and `flags` is a field invariant 5 names. 48 is the widest id
that keeps both named fields at least a byte wide and every field on a byte
boundary: id in bytes 0-5, `flags` byte 6, `reserved` byte 7.

**BK's rules, one line each:**

- **BK-R1.** The Keystone word is `id:48 | flags:8 | reserved:8`.
  `kKeystoneIdBits` 48, `kKeystoneReservedBits` 8, `Keystone::reserved`
  a `uint8_t`; `Encode` refuses an id above `kMaxKeystoneId` as it does
  today. Invariants 5, 7 and 11 are restated to 48 bits and an upper 16.
- **BK-R2.** One source for the width. The three constants that spell
  `1 << 40` on their own (`heap_page.cpp:18`, `btree_page.cpp:17`,
  `cabin_bound_page.hpp:176`) are deleted and their readers name
  `kMaxKeystoneId`; the Cabin bound entry's word is encoded and decoded
  with `kKeystoneIdBits`, not with the literal 40.
- **BK-R3.** The trail index's packed key shifts by `kKeystoneIdBits`,
  takes its `step_id` as the `uint16_t` the entry format stores, and
  `static_assert`s that 16 + `kKeystoneIdBits` ≤ 64; `Find` answers null
  for a step id above `0xFFFF`, which is today's miss kept. It is the one
  site whose miss is silent rather than a refusal, so two cells pin it.
- **BK-R4.** `kSuperBlockVersion` 21 → 22; a 21 volume is refused at mount
  with the version named; no migration and no reader of a 21 volume. The
  standing order of 2026-10-07 applies, and the bump is load-bearing: a 21
  volume's tuple words would read, its Cabin bound entries would not
  (§1.7).
- **BK-R5.** A refusal names the ceiling's value, never the width in
  words: every message that says "40-bit" states `kMaxKeystoneId` as a
  number, as the exhausted refusal already does (`command_dispatcher.cpp:
  5614-5618`), so the width is spelt in `keystone.hpp` alone.
- **BK-R6.** K4 is restated, not relaxed: the lifetime budget is
  `kMaxKeystoneId - kFirstRowId + 1` = 2^48 - 1 ids per relation, the rate
  table recomputed (§2), `kKeystoneBudgetWarnFraction` untouched, the
  period-partitioning guidance and K5's reservation kept (BK-Q2).
- **BK-R7.** What does not move: `kKeystoneWordSize` 8, the 20-byte tuple
  header, `trx_id:48` (a different field whose width now equals the id's by
  coincidence), every zero-extended `uint64_t` store, the wire's pk field,
  `kFingerprintVersion`, `sys.tables.next_id`, `IsIntegerTypeVal`.
- **BK-R8.** The sim judges by the engine's constants: `sim/oracle.cpp:86`'s
  own `kMaxKeystoneId` and `sim/workload.cpp:199`'s `1 << 40` go, the
  integrity check's message derives its number.
- **BK-R9.** The text: every sentence that says 40 bits, 2^40, an upper 24
  or `1099511627775` is restated, in the specs, the rules, the manual, the
  concept notes, the one bug entry and the source comments (§1.9).

**What BK leaves standing:**

- **K1 and BD-R4.** A committed key stays bound for the relation's life,
  `PURGE` (BH) is K1's one exception, and `AlreadyExists` is still the
  descent's answer. BK widens the space the rule ranges over; it changes
  no rule.
- **BD-R5's two reasons.** A named key is refused for its pk as a
  duplicate or as exhausted, and exhausted is still "outside
  `[kFirstRowId, kMaxKeystoneId]`", judged from the literal's digits.
- **The heap's mark gate** (BB-R7, BB-R12) and `sys.tables.next_id` as a
  high-water mark (BD-R7).
- **AO-R3.** `flags` stays a byte written as zero; the lock family keeps no
  persisted bit.

## 1. Survey at `5979b5e7`

Read, not run. Line numbers are the tree's at `5979b5e7`.

### 1.1 The word and its one encoder

- `include/kds/storage/keystone.hpp:37-39` defines the three widths and
  `:40` asserts they sum to 64; `:48` derives `kMaxKeystoneId`, `:53`
  `kIdSpaceEnd`. `:11` (the header's `id:40 | flags:8 | reserved:16`),
  `:47` and `:64` say 40 bits in comments.
- `src/storage/keystone.cpp:19-20` refuses an id above the max with the
  text *"Keystone id exceeds 40-bit range"*; `:24-25` shift `flags` and
  `reserved` by the named constants; `:31-34` decode by the same names,
  with the reserved mask spelt `0xFFFFu`. **That literal is harmless under
  BK**: a 64-bit word shifted right by 56 has eight bits left, so `& 0xFFFF`
  and `& 0xFF` agree. What must follow `kKeystoneReservedBits` is
  `reserved`'s **type** (`uint16_t` at `keystone.hpp:59, 66` and
  `keystone.cpp:18, 33`): left at 16 bits, `Encode` would silently shift a
  value's upper byte out of the word at `:25`, which BK-R1's `uint8_t`
  removes.
- **`Encode` has one caller**, `src/exec/row_codec.cpp:858`, passing
  `flags = 0, reserved = 0`. **`flags` is read by nothing**: the only
  readers of a decoded word take `.id` (`keystone.cpp:42`,
  `row_codec.cpp:915`, and through `KeystoneIdOfPayload` the btree, the
  heap chain, redo and the builds). `reserved`
  is read by one checker, `sim/integrity.cpp:271-275`, which requires it
  zero. So every Keystone word on every volume today has bits 40-63 zero
  (§1.7 draws the consequence).
- No site reads `flags` or `reserved` by byte offset: a grep for the
  word's byte 5 and for `kKeystoneWordSize` arithmetic finds the two size
  checks (`keystone.cpp:39`, `row_codec.cpp:912`) and the layout's start
  offset (`row_layout.cpp:73`) only, none of which names a field. **BK-S1 confirms this with a cell,
  not a grep** (§3).

### 1.2 Three constants that spell the number, and the Cabin entry word

| site | what it spells | its reader |
|---|---|---|
| `src/storage/heap/heap_page.cpp:18` | `kMaxTupleId = (1 << 40) - 1`, with `:14` saying it duplicates `kKeystoneIdBits` deliberately | `:73-74`, `min_key` refused above it, text "40-bit" |
| `src/storage/btree/btree_page.cpp:17` | the same constant, the same comment at `:14` | `:133-134`, a separator key refused above it, text "40-bit" |
| `include/kds/storage/cabin_bound_page.hpp:176` | `kMaxEntryPk = (1 << 40) - 1`, `:174-175` citing invariants 5 and 7, `:179` "does not fit 40 bits" | `src/storage/cabin_bound_page.cpp:79-81` refuses a pk above it, text "40-bit"; `:87` packs `flags << 40`; `:103-104` unpack with `& kMaxEntryPk` and `>> 40`; `:12` and `:84` say `pk:40 \| flags:8 \| reserved:16` |

The Cabin bound entry is an **on-disk** word (`cabin_bound_page.hpp:24`:
`pk (40 bits used) | flags (8) | reserved (16)`), packed by the Keystone
word's discipline, and **unlike the Keystone word its flags are live**:
`kEntryReserved`, `kEntryHintValid`, `kEntryDeparture`, `kEntryOrphaned`
(`cabin_bound_page.hpp:73-123`; `bound_cabin_test.cpp:64` round-trips
`kEntryReserved | kEntryHintValid`). The word is written to the bound
pages and, whole, into every `ASSERT_RESERVE`/`ASSERT_BUILD` record
(`assertion_build.cpp:118`), and replay decodes it there
(`assertion_replay.cpp:41-44`). If BK moved `kKeystoneIdBits` and left the
whole entry codec at 40, an entry for a pk ≥ 2^40 would be **refused** at
`:79` (`kMaxEntryPk` still 40-bit) - a refusal, not a wrong answer. **A
half move is quiet**: `kMaxEntryPk` replaced by `kMaxKeystoneId` with `:87`
left at `<< 40` ORs the flags into the pk's bits 40-47, and decode returns
*a different row's name* and wrong flags, which is what K1 forbids. BK-R2
removes the possibility by naming the constant in all three places.

### 1.3 The packed trail key - the one site whose miss is silent

`include/kds/exec/trail_replay.hpp:123-128`: `(step_id, pk)` packed into
one `uint64_t`, `step_id << 40 | (pk & 0xFF'FFFF'FFFF)`, with the comment
saying the pk is 40-bit and step ids 16-bit *"so the pair fits a uint64
with room to spare"*. `PackKey` takes `std::uint32_t step_id`; the entry
format stores `uint16_t` (`include/kds/stats/waystone.hpp:159`,
`trail_collector.hpp:71`). **The step id itself is a `uint32_t`**
(`Step::step_id`, `step_chain.hpp:461`, one statement-wide counter): the
one producer narrows it by an explicit cast (`step_vm.cpp:2482`), and
`Find`'s one caller passes it whole (`step_vm.cpp:683`, through
`trail_replay.cpp:75-76`).

Left at 40 under a 48-bit id, the mask would drop a pk's bits 40-47: two
rows whose pks differ only there would share a key, and a lookup for one
could return the other's recorded location. Moving the mask and leaving
the shift is worse in kind: those bits would land in `step_id`'s, and
`(1, k)` and `(0, k + 2^40)` would share a key across steps. **The consequence is bounded by invariant 9**: a trail location
is advisory, verified by the Keystone id at the page against the key the
step derived before it is used (`step_vm.cpp:698-703`,
`tuple_verify.cpp:52-53`), so the wrong location is a miss and a descent,
never a wrong row. It is still the one change in BK that no compiler, no
refusal and no existing cell would report,
which is why BK-R3 pins it with a `static_assert` and a cell. At 48 the
pair fits exactly: 16 + 48 = 64, no room to spare, so the `static_assert`
is `≤`, not `<`.

### 1.4 Sites that follow the constant by name

These read `kMaxKeystoneId` or `kIdSpaceEnd` and move with `keystone.hpp`,
or sit beside such a read with a comment that says 40 bits (each marked).
BK-S1 lists them to check that each one's **comment or message** moves too
(the number does on its own):

- **Admission of a key:** `src/catalog/catalog.cpp:2487` (a chunk's last id
  against the ceiling), `:2529`, `:2568-2571` (`CheckNamedRowIdSpellable`,
  text "40-bit"); `include/kds/catalog/catalog.hpp:80, 638, 685, 727`
  (comments); `src/server/command_dispatcher.cpp:5602-5628` (the exhausted
  refusal, judged from the digits, message naming the value - BK-R5's
  model), `:2068` (`SHOW BUDGET`'s comment), `:6353` (a negative literal is
  a miss), `:9283, 9375, 9903` (PURGE's window and the range lock's
  `kIdSpaceEnd`). **And one that names no constant and spells the number**:
  `:9822`, `PurgeConjunctRefusal`'s *"outside the Keystone id space [1,
  1099511627775]"* for a pk literal the lexer wrapped - the one refusal
  text in the engine that would state the old ceiling after BK-S2 unless
  it is moved by hand.
- **The budget:** `include/kds/catalog/keystone_budget.hpp:11, 23`
  (comments), `:47` (`kKeystoneBudgetCapacity`, derived);
  `src/catalog/keystone_budget.cpp:19-31`.
- **The executor:** `src/exec/step_vm.cpp:143` (comment), `:2005, 2049,
  2068` (`kIdSpaceEnd` borrows), `:2110` (comment); `include/kds/exec/
  step_vm.hpp:303`; `src/exec/step_compiler.cpp:16, 1598` (`:642, 1882`
  comments); `include/kds/server/read_borrow.hpp:77, 120`; and comments
  only at `src/exec/row_codec.cpp:1049`, `include/kds/exec/
  row_codec.hpp:239`, `src/exec/assertion_catalog.cpp:72` and
  `include/kds/parser/token.hpp:218` (V30's).
- **The log:** `src/wal/payload.cpp:70-71, 105-106` (PAGE_INIT's `min_key`
  refused above the max, text "40-bit", once `InvalidArgument` on encode
  and once `Corruption` on decode).
- **Comments that say "upper 24 bits" or "40-bit" and name no constant:**
  `include/kds/catalog/schema.hpp:25`, `include/kds/catalog/
  foreign_key.hpp:45`, `include/kds/txn/undo_page.hpp:214`,
  `include/kds/stats/waystone.hpp:122-123` ("never 5-byte-packed"),
  `include/kds/exec/
  trail_collector.hpp:52`, `include/kds/storage/index/index_page.hpp:46`,
  `include/kds/storage/btree/btree_page.hpp:38-41, 164`,
  `include/kds/storage/heap/heap_page.hpp:211`, `include/kds/storage/
  cabin_bound_page.hpp:133`.

### 1.5 Sites that look like the width and are not

- `include/kds/stats/decay.hpp:218`: `1ULL << 40` saturates a count of
  elapsed half-lives (`:47` is a comment bounding a Q8 product `< 2^40`);
  `tests/relayout_planner_test.cpp:60` passes `1 << 40` to
  `ShapeDecayedWeight` (`relayout_planner.cpp:129`) as a use count too
  large to scale. Neither is a Keystone constant; BK leaves all three
  alone.
- `include/kds/storage/heap/heap_page.hpp:172` `kMaxTrxId` and
  `include/kds/wal/record.hpp:329` `kMaxTxnId` are `(1 << 48) - 1`: the
  transaction id, a different field of the tuple header. After BK the two
  widths are equal, and **a hand-edit that replaces "40" by "48", or later
  "48" by anything, must not reach them**. BK-S1's census names them so
  the review can check they did not move.
- `tests/waystone_dir_test.cpp:115` `b = a | (1ull << 40)`: a bit above
  the pattern directory's depth in a **hash** key (`DirIndexAt`, the cell
  `BitsAboveTheDepthFoldRatherThanFail`), not a pk. BK leaves it alone, and
  BK-S1's census re-confirms the classification.

### 1.6 The superblock

`include/kds/server/superblock.hpp:266` `kSuperBlockVersion = 21` (BF-S3);
`src/server/superblock.cpp:34` writes it, `:69-82` refuses any other value
at mount, naming both. BK-R4 moves it to 22; the refusal needs no change.

### 1.7 The on-disk facts, and why the version is load-bearing

Every **tuple** Keystone word written so far has bits 40-63 zero (§1.1:
`flags` and `reserved` are written 0 by the one encoder, and the sim
checks `reserved`). Decoded under `id:48 | flags:8 | reserved:8`, such a
word yields the same id, `flags` 0 and `reserved` 0. **The Cabin bound
entry's word does not**: its flags are live in bits 40-47 (§1.2), so under
BK's layout a 21 entry decodes with those bits as the pk's 40-47 - a
different row's name - and flags 0, and a replayed 21 `ASSERT_RESERVE`
would be refused *"entry is not reserved"* (`assertion_replay.cpp:44-50`).
So a 21 volume does **not** read correctly under BK's layout, and the
refusal (BK-R4) is a layout incompatibility as well as the standing
order's. The converse holds too: a 22 volume can hold an id a 21 engine's
40-bit mask would truncate to another row's name. The version is what
stops either engine reading the other's volume, and `!=` already does
that.

Nothing in page arithmetic reads the id's width: slot directories, the
tuple header (`kTupleHeaderOnDiskSize` 20, `heap_page.hpp:162`), the row
layout and the fixed-length cells are all byte-sized from the schema. The
rejection at `keystoneid-invariant.md:230-233` names "page arithmetic" as
what the 40-bit word was built on; at `5979b5e7` nothing of the kind
exists.

### 1.8 The tests

Nineteen files, forty-four lines name the width (`grep -nE "<< ?40|
kMaxKeystoneId|1099511627775|40-bit|2\^40|kIdSpaceEnd" tests/*.cpp`).
Twenty-three of those lines, in eight files, read `kMaxKeystoneId` or
`kIdSpaceEnd` by name and move on their own; BK-S1's census lists them.
**The grep misses three kinds**: a number
derived from the ceiling (`keystone_budget_test.cpp:152`), a comment
saying "upper 24" (`btree_test.cpp:1450`), and seven cell **names** that
spell forty - `bound_cabin_test.cpp:84`, `btree_test.cpp:1441`,
`heap_page_test.cpp:56`, `keystone_budget_test.cpp:42`,
`keystone_test.cpp:39`, `row_codec_test.cpp:104`,
`wal_payload_test.cpp:74` (the `FortyEight` names at
`undo_page_test.cpp:341` and `wal_payload_test.cpp:252, 291` are the
transaction id's and stay). **Those whose meaning changes**, each a BK-S2
edit:

| cell | today | under BK |
|---|---|---|
| `fk_parent_hold_test.cpp:105` | `kForeign = 1 << 40`, a parent that can never exist | a valid key; must become `kIdSpaceEnd` or the cell asserts the wrong thing |
| `heap_page_test.cpp:58` | `CreateEmpty` with `min_key = 1 << 40` refused | placed; use `kIdSpaceEnd` |
| `bound_cabin_test.cpp:63, 89, 95` | the largest 40-bit pk, one past it, and the message containing "40-bit" | `kMaxKeystoneId`, `kIdSpaceEnd`, and the message's number (BK-R5) |
| `keystone_test.cpp:19, 25, 48` | `reserved = 0xFFFF` encodes | does not fit 8 bits; `0xFF`, and a cell that a wider value is refused or cannot be passed (by type, under BK-R1) |
| `sorted_leaf_named_keys_test.cpp:202, 207` | `1099511627776` exhausted, `1099511627775` placed | `281474976710656` exhausted, `281474976710655` placed |
| `supplied_key_test.cpp:672, 674` | past the field: `2^40` (`:674` spells `1099511627776`) | `2^48` |
| `keystone_budget_test.cpp:152` | `DESCRIBE` after two inserts reports `ids_remaining=1099511627773` | `ids_remaining=281474976710653` - red under BK as written today |
| `purge_key_test.cpp:326` | `PURGE ... WHERE id > 1099511627775` purges 0 | still 0 on that relation, but the premise (a window past the space) is gone; restate at `kMaxKeystoneId` |
| `waystone_page_test.cpp:32` | `0x000000FFEEDDCCBB`, "inside the 40-bit id space" | a value with bits 40-47 set, to show they round-trip |
| `step_compile_test.cpp:219`, `lexer_test.cpp:266`, `row_codec_test.cpp:160`, `keystone_id_test.cpp:200`, `keystone_budget_test.cpp:43, 59-60, 115`, `sorted_leaf_named_keys_test.cpp:22`, `btree_test.cpp:1450` | comments | restated |
| the seven cell names above | "40Bits", "40Bit", "FortyBits", "TwoToTheForty" | renamed to the property, not the width |

### 1.9 The text

- `CLAUDE.md:147` (invariant 5), `:149` (7), `:153` (11), `:74` (the pk
  row's `[1, 2^40 - 1]`).
- `docs/spec/heap-and-tuple.md:105` (the field table, "40 bits", the
  capacity), `:107` (the same table's `reserved | 16 bits`, which BK-S3's
  grep does not catch), `:109`, `:124`, `:130` ("upper 24 bits zero", "never 5-byte-
  packed"), `:156`, `:174`, `:220`, `:298` (PURGE's literal), `:365`,
  `:367`, `:371` (§8's invariants).
- `docs/rules/rules.md:60, 63`; `docs/rules/keystoneid-invariant.md:12, 56,
  208, 212-217` (§3's rate table), `:226`, `:230-233` (the overturned
  rejection), `:332`.
- **The Cabin bound entry's layout, specified twice**: `docs/spec/assertion.md:222-224`
  (§5.1, `pk | 40 bit`, `reserved | 16 bit`) and `docs/spec/cabin.md:719-721`
  (`pk | 40 bit of a u64`, `reserved | 16 bit`). Neither matches BK-S3's
  grep; both are on-disk format statements BK changes (§1.2).
- `docs/spec/parser-v2.md:111-112` (I6, "range-checked `< 2^40`"; it also
  says the refusal is `InvalidArgument`, where a named `INSERT` key is
  refused `OutOfRange` since BD-R5, `command_dispatcher.cpp:5615`),
  `docs/spec/aggregate.md:153`, `docs/spec/foreign-keys.md:17`,
  `docs/spec/bulkinsert.md:32` (BI9), `docs/spec/waystone-concpets.md:119`.
- `manual/sql/sql.md:196, 198, 533, 1132` (`:198` and `:1132` spell
  `1099511627775`).
- `docs/conceptnotes/cn3-supersede-and-chain-id-succession.md:196`,
  `docs/conceptnotes/cn5-commit-outcome-and-idempotency.md:84`.
- `docs/inflight/bugs/a-negative-foreign-key-value-skips-the-parent-check.md:
  10, 46, 48` - a bug entry, verified at its own commit; BK-S3 restates
  its range and keeps its commit.

## 2. Rulings — CLA's proposals, unmarked

### BK-R1 — The layout

`id:48 | flags:8 | reserved:8`. In `keystone.hpp`: `kKeystoneIdBits = 48`,
`kKeystoneReservedBits = 8`, the two `static_assert`s unchanged in form.
`Keystone::reserved` becomes `std::uint8_t`, so a value that does not fit
is narrowed at the call rather than shifted out of the word inside
`Encode`, as `flags` already is (an implicit conversion: a constant draws
a warning under `-Wall -Wextra`, a variable nothing); `Decode`'s reserved mask is
derived from `kKeystoneReservedBits`, not spelt. `Encode` refuses
`id > kMaxKeystoneId` as today. The word's byte picture, for the header
comment: bytes 0-5 the id, byte 6 `flags`, byte 7 `reserved`.

BK-Q1 offers two other layouts of the upper 16 bits; this is (a).

### BK-R2 — One source

`heap_page.cpp:18` and `btree_page.cpp:17` delete their `kMaxTupleId` and
read `kMaxKeystoneId`; their `:14` comments, which say the duplication is
deliberate so the page layer need not know the Keystone struct, go with
them - `keystone.hpp` is one header of constants; neither file includes it
today, and it costs them nothing: it pulls in `<cstddef>`, `<cstdint>`,
`<span>` and `status.hpp`, which both page headers already include, and
`keystone.cpp` is in the same `kds` library (`CMakeLists.txt:23-28`).
`cabin_bound_page.hpp:176` deletes `kMaxEntryPk` for `kMaxKeystoneId`, and
`cabin_bound_page.cpp:87, 104` shift by `kKeystoneIdBits`; its `:24`
table, `:133`, `:174-175` and `:179` comments and the `.cpp`'s `:12` and
`:84` are restated. After BK-S2, `grep -rnE "<< ?40|>> ?40" include src sim` finds
only `decay.hpp` (§1.5).

### BK-R3 — The trail key

```
static_assert(16 + kKeystoneIdBits <= 64);
static constexpr std::uint64_t PackKey(std::uint16_t step_id, std::uint64_t pk) noexcept {
    return (static_cast<std::uint64_t>(step_id) << kKeystoneIdBits) | (pk & kMaxKeystoneId);
}
```

`Find` keeps its `std::uint32_t step_id` parameter and **answers null for a
value above `0xFFFF`** before it packs. Its one caller (`step_vm.cpp:683`)
passes `Step::step_id`, a `uint32_t` (`step_chain.hpp:461`); today such a
value packs into bits the recorded keys never set and misses, and the guard
keeps exactly that. Narrowing the parameter instead would let step 65,537
hit step 1's entries - bounded by the verifier, but a change of behaviour
BK has no reason to make. The producer at `step_vm.cpp:2482` narrows the
same counter by a truncating `static_cast<std::uint16_t>`, so a statement
of more than 65,536 steps would record under another step's id; BK-S1's
census records it as a pre-existing fact and BK does not touch it (no
statement shape approaches the count).

Two cells: an index built from two entries whose pks differ only in bits
40-47, each `Find` returning its own location (kills the mask left at 40);
and the cross-step pair `(1, k)` and `(0, k + 2^40)`, each found at its own
(kills the half move - mask on the constant, shift still 40 - which the
first cell alone lets live).

### BK-R4 — The superblock

`kSuperBlockVersion` 22. The refusal at `superblock.cpp:69` is unchanged.
No migration, no reader of a 21 volume, no flag. The standing order of
2026-10-07 is the authority, and §1.7 records that the refusal is also a
layout incompatibility: a 21 volume's tuple words would read under BK, its
Cabin bound entries - on their pages and in the log - would not.

### BK-R5 — Refusals name the value

The seven messages that say "40-bit" (`keystone.cpp:20`, `heap_page.cpp:74`,
`btree_page.cpp:134`, `cabin_bound_page.cpp:81`, `catalog.cpp:2570`,
`payload.cpp:71, 106`), and the one that spells `1099511627775`
(`command_dispatcher.cpp:9822`, §1.4), state `kMaxKeystoneId`'s value
instead - *"Keystone
id N exceeds the id space (max 281474976710655)"* - as
`command_dispatcher.cpp:5614-5618` already does. A later change of the
width then touches no message. `bound_cabin_test.cpp:95`, which matches
the substring "40-bit", matches the number.

BK-Q3 offers "48-bit" in words as the alternative.

### BK-R6 — K4 restated

Capacity `kMaxKeystoneId - kFirstRowId + 1` = 2^48 - 1 =
281,474,976,710,655 ids per relation, as `keystone_budget.hpp:47` already
derives it. The rate table at `keystoneid-invariant.md` §3 recomputed:

| sustained insert rate (one relation) | 40-bit budget exhausted in | 48-bit |
|---|---|---|
| 1,000 /s | ~35 years | ~8,900 years |
| 5,000 /s | ~7 years | ~1,800 years |
| 50,000 /s | ~8 months | ~180 years |
| 500,000 /s | ~25 days | ~18 years |
| 5,000,000 /s | ~2.5 days | ~1.8 years |

The product stance is restated with it: at 48 bits the budget is reachable
only by a relation sustaining hundreds of thousands of inserts per second
for years. Period partitioning stays the first sanctioned pattern and K5
(offline re-key) stays reserved, each now the answer to a rarer case
(BK-Q2). `kKeystoneBudgetWarnFraction` stays 0.90 and `[PROPOSED]`.

### BK-R7 — What does not move

Stated so the review can check each: `kKeystoneWordSize` 8;
`kTupleHeaderOnDiskSize` 20 and `trx_id:48` (invariant 12); `min_key`,
btree and index keys, undo `target` ids, Waystone and Cabin entry pks, all
`uint64_t` zero-extended - the upper **16** bits now zero (invariant 7
restated, "never 5-byte-packed" becomes "never 6-byte-packed"); the wire's
pk field, flagged `kFieldFlagKeystone` (`wire/row_codec.cpp:126`) and
**typed and sized as the column is declared** (`:117-118`, written at that
width by `:183-185`, against `protocol.md:93`'s "(u64)") - a 48-bit id
fits an `int64` or `uint64` pk as a 40-bit one did, and an `int8`-`int32`
pk is cut to its declared width on the binary wire at either width;
`kFingerprintVersion` (a pk literal is a value in the shape, its width is
not); `sys.tables.next_id` (a `uint64_t`); `IsIntegerTypeVal` and
`CheckKeystoneColumn` (the declared type is display metadata in storage,
`heap-and-tuple.md:124`, though not on the wire, above); `row_layout.cpp:73`.

### BK-R8 — The sim

`sim/oracle.cpp:86` deletes its local `kMaxKeystoneId` and includes
`keystone.hpp`; `sim/workload.cpp:199`'s exhausted key becomes
`kIdSpaceEnd`; `sim/integrity.cpp:280`'s *"id above 2^40-1"* states the
value; `sim/integrity.hpp:22` and `sim/oracle.hpp:169` are restated. The
corpus is re-run at BK-S2.

### BK-R9 — The text

§1.9's list, each sentence restated to what BK built. The rules document
keeps its §3 and says, dated, that the rejection it recorded was overturned
on 2026-10-11 and why its reason no longer held (§0).

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BK-S0 | **The order** | <ul><li>This file</li><li>Its review (§6)</li><li>The index row</li><li>`raft-marks-2026-10-11.md`'s section for the words</li></ul> | S |
| BK-S1 | **The census, and red first** | <ul><li>**The census** at the tip of `main`: §1.2-§1.5 and §1.8 re-run as greps, every site listed with its line, every `1 << 40` classified as Keystone or not (`decay.hpp`, `waystone_dir_test.cpp:115`), `kMaxTrxId`/`kMaxTxnId` named as the two 48-bit constants BK must not touch; the producer's truncating cast at `step_vm.cpp:2482` recorded as a pre-existing fact (BK-R3).</li><li>**The bug entry** for the narrow-pk wire truncation (BK-Q6), filed under `docs/inflight/bugs/` at the tip of `main` with the cell that shows it (an `int32` pk above 2^31 read back over KWP/1), whichever option BK-Q6 takes - under (b) the entry is deleted again when BK-S2b fixes it.</li><li>**Red cells**, each failing at `5979b5e7` for the right reason - refused where the tree refuses the value, the 40-bit answer where it computes one (the width, trail and flags-byte cells):<ul><li>`Keystone::Encode(kMaxKeystoneId, 0xFF, 0xFF)` round-trips with `kMaxKeystoneId == (1 << 48) - 1` - the width cell.</li><li>a named `INSERT` of key `2^40` is placed and read back by pk, and `2^48` is `OutOfRange` naming the value.</li><li>BK-R3's two trail cells: two pks differing only in bits 40-47, each found at its own location; and the cross-step pair `(1, k)` and `(0, k + 2^40)`, each found at its own.</li><li>a Cabin bound entry with pk `2^40 + 1` **and flags `0x3`** (a flag bit where the pk's bits 40-47 are clear) encodes and decodes to itself, pk and flags both compared - with flags `0x1` alone the half move of §1.2 would pass.</li><li>a `PAGE_INIT` with `min_key = 2^40 + 1` encodes, decodes and replays.</li><li>a heap page and a btree leaf created with `min_key = 2^40 + 1`, and a separator key `2^40 + 1` inserted in an inner node.</li><li>the flags byte: a word encoded with `flags = 0xA5` reads `0xA5` from `Decode` **and** from byte 6 of the payload, `reserved = 0x5A` from byte 7 - §1.1's grep made a claim; this is its cell.</li></ul></li><li>No engine file moved; the suite not run as a gate (the new cells are red by design).</li></ul> | S |
| BK-S2 | **The word** (BK-R1..R5, R7, R8) | <ul><li>**Code:** `keystone.hpp`/`.cpp` (BK-R1); the three duplicates deleted and the Cabin entry word on the constants (BK-R2); `PackKey` (BK-R3); `kSuperBlockVersion` 22 (BK-R4); the eight messages (BK-R5); the sim's constants (BK-R8); §1.4's comments and §1.8's cells restated, `keystone_test.cpp`'s `reserved` cells at 8 bits. **Two cuts taken while in the files**: `RowKeystoneId` (`row_codec.cpp:911-916`), a copy of `KeystoneIdOfPayload` with its own `LoadLe64`, forwards to it - `keystone.hpp:78-82` says that decode exists once; `keystone.hpp:11-22`'s header comment restated (its `id:40` and its "system-generated, callers do not supply it" sentence, stale since BD).</li><li>**BK-S2b, only under BK-Q6 (b):** the pk field on the binary wire is sent as 8 bytes, `u64`, whatever the column's declared type (`wire/row_codec.cpp:117-118, 183-185`), as `protocol.md:93` states; a cell reads an `int32` pk above 2^31 back whole; `protocol.md` §6 and `client-manual.md` say the pk field's width is fixed; the bug entry BK-S1 filed is deleted.</li><li>**Cells:** BK-S1's red cells green; a 21 volume is refused at mount naming 21 and 22 (the existing version cell, `superblock_test.cpp:257`, re-pointed: it iterates `kSuperBlockVersion ± 1` and so passes at either version until it spells 21); `SHOW BUDGET` reports `capacity=281474976710655` and `DESCRIBE` an `ids_remaining` derived from it (`keystone_budget_test.cpp:152`); `PURGE FROM t WHERE id > 281474976710655` purges 0 and `id > 1099511627775` is a live window.</li><li>**Mutations, each repeated:** `PackKey` left at 40 (killed by the first trail cell); `PackKey`'s half move - mask on the constant, shift at 40 (killed by the cross-step pair); `cabin_bound_page.cpp:87, 104` left at 40 with `kMaxEntryPk` replaced - §1.2's half move (killed by the entry cell's flags compare); `kKeystoneIdBits`/`kKeystoneReservedBits` left at 40/16 (killed by the width cell). `Decode`'s reserved mask left at `0xFFFF` is **not** a mutant: under BK it is equivalent (§1.1), and is derived for clarity only. `kSuperBlockVersion` left at 21 (killed by the mount cell).</li><li>**The grep:** `grep -rnE "<< ?40|>> ?40|40-bit|upper 24|1099511627775" include src sim tests` finds `decay.hpp`, `relayout_planner_test.cpp` and `waystone_dir_test.cpp:115` only (§1.5).</li><li>**The suite green** and the sim corpus (`scripts/sim.sh`) green, each stated with its count.</li></ul> | M |
| BK-S3 | **The text** (BK-R6, BK-R9) | <ul><li>§1.9's every line, `keystoneid-invariant.md` §3's table and its overturned sentence first.</li><li>`CLAUDE.md`: invariants 5, 7, 11; the pk row; this order's row in the milestone table is not added - the pk row carries the width.</li><li>The grep over `docs manual CLAUDE.md` for `2\^40|40-bit|40 bits|id:40|upper 24|1099511627775` finds only the rules document's dated overturn (this order and the marks file lie outside those paths); the three sites it cannot see - `heap-and-tuple.md:107`, `assertion.md:222-224`, `cabin.md:719-721` - checked by reading.</li></ul> | S |
| BK-S4 | **The close** | <ul><li>The measurement (§5), or the operator's waiver stated as such.</li><li>§6's row per stage; what BK carries, if anything, into `known-gaps.md`.</li><li>The index row restated.</li></ul> | S |

## 4. Items for the operator

Every item is **unmarked**. W2 asks that these be asked, and CLA's
proposal beside each is a proposal.

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BK-Q0 | **Open BK**, with BK-S0..S4 and BK-R1..R9 as written | process | Yes | |
| BK-Q1 | **The upper 16 bits.**<br>(a) `id:48 \| flags:8 \| reserved:8` (BK-R1): both named fields kept, each a byte.<br>(b) `id:48 \| flags:16`: `reserved` deleted, `flags` widened; invariant 5 names two fields.<br>(c) `id:48 \| reserved:16`: `flags` deleted, since its one writer passes 0 and nothing reads it; invariant 5 names two fields and `Encode` loses a parameter | design, on-disk | (a). It changes the least: the struct keeps its three fields, `Encode`'s one caller is untouched, the sim's reserved check holds, and CN-3 (`cn3-supersede-and-chain-id-succession.md:196`) still has the flag byte it names beside the id. (c) is the elimination the operator has preferred elsewhere (BA-Q14 (b), BB's `kUnordered`), and it is sound - AO-R3 says no persisted bit lives there - but it closes CN-3's door and rewrites invariant 5 twice over for no capacity. **The Cabin bound entry under each:** (a) keeps the entry word identical to the Keystone word, one layout from one set of constants (BK-R2); under (b) or (c) the two words diverge, because the entry's flags are live (§1.2) and cannot be deleted or widened with the Keystone word's, so the entry keeps `pk:48 \| flags:8 \| reserved:8` from its own constants derived from `kKeystoneIdBits`, and `assertion.md` §5.1 and `cabin.md`'s layout tables say the two words no longer match | |
| BK-Q2 | **The rules document's stance.** `keystoneid-invariant.md:230-233` rejected widening; §3's partitioning guidance and K5's offline re-key answer the 40-bit budget.<br>(a) The rejection restated as overturned, dated, with its reason; §3's table recomputed; partitioning and K5 kept (BK-R6).<br>(b) As (a), and K5's reservation struck as moot at 48 bits | rules text | (a). K5 reserves semantics nothing has built and costs nothing to keep; at 500,000 inserts per second a relation still exhausts in ~18 years, so the case it answers exists. Striking it is a separate decision the budget does not force | |
| BK-Q3 | **How a refusal names the ceiling.**<br>(a) The value, `max 281474976710655` (BK-R5), so a width change touches no message.<br>(b) The width in words, "48-bit", as today's "40-bit" | user-visible | (a). The exhausted refusal a client most often meets (`command_dispatcher.cpp:5614-5618`) already names the value, and the manual at `sql.md:1132` quotes it that way; (a) makes the eight others agree with it | |
| BK-Q4 | **The measurement at the close** (§5): the interleaved A/B in `build-release`, `5979b5e7` against the closing commit, insert and update shapes, expectation flat | process | As written. BK changes the value of a mask and nothing per row; a difference beyond the run-to-run noise is reported, not resolved, and gates nothing | |
| BK-Q5 | **A tag at the close.** The format version moves (BK-R4), and a tag message is a durability claim (`CLAUDE.md`, Version Management).<br>(a) No tag; `v2.7.0-*` continues, as at every close since AT (AT-close Q3).<br>(b) The operator names a version | process | (a). The operator names versions, and nothing since AT has been tagged; BK is one constant and does not change what the v3.0.0 tag will claim | |
| BK-Q6 | **A pre-existing wire defect the review found** (read, not run): a pk declared `int8`, `int16` or `int32` - admitted by `IsIntegerTypeVal`, and `row_codec_test.cpp:159-170` stores id 100000 under an `int8` pk - is written to the binary wire at its declared width (`wire/row_codec.cpp:117-118, 183-185`, `PutLE(int_val, width)`), while `protocol.md:93` promises `u64`; an id above the declared width reaches the client cut. It is the same at 40 and 48 bits and is not BK's, but BK restates invariant 11, whose "display metadata" sentence (`heap-and-tuple.md:124`) it contradicts.<br>(a) File it as a `docs/inflight/bugs/` entry in BK-S1 and leave the fix to its own letter.<br>(b) Fix it inside BK as BK-S2b: the pk field is always 8 bytes `u64` on the wire, as the protocol states.<br>(c) Refuse `int8`/`int16`/`int32` as a pk's declared type at `CREATE TABLE` | **`[quiet-wrong]` today**; scope | (a), with (b) as the fix when it is taken. (b) is small and brings the code to the written protocol, but it changes what a client receives for a pk it declared narrow, and `xrock`'s reading of the field descriptor has not been checked (its reports live outside this tree); CLA will not widen a wire field in an order about a storage constant without that read. (c) refuses a declaration the spec calls display metadata and would make existing relations' schemas unspellable | |

## 5. Measurement

Once, at BK's close, in `build-release`, per `CLAUDE.md`'s Session Workflow
step 3: `5979b5e7` (A) against the commit that closes BK (B), each named by
`git describe --tags`, the results in `bench/v3.0.0/`, `bench/README.md`'s
rules applied. The shapes are the insert and the hot-row update BE and BA
measured with, interleaved A/B, server CPU per statement. BK adds nothing
per row - `Encode`'s compare is against a larger constant and `Decode`'s
mask is a different constant - so the expectation is flat, and the number
is reported either way (BK-Q4).

## 6. Row status

### BK-S0 — written 2026-10-11

Written on `worktree-bk-keystone-48` from `5979b5e7`. No file under `src/`,
`include/`, `tests/` or `sim/` moved, and no suite ran. The survey (§1) is
a read of the tree at that commit, every line number pinned there.

**Its review** (`critics-developer`, the same day) checked §1 against the
tree and corrected it in place:
- **The claim that mattered most was wrong.** §1.7 said a 21 volume's
  bytes would read correctly under the new layout. The Cabin bound entry's
  flags are live in bits 40-47 (`cabin_bound_page.hpp:73-123`) and the
  word travels whole inside every `ASSERT_RESERVE`/`ASSERT_BUILD` record
  (`assertion_build.cpp:118`, `assertion_replay.cpp:41-44`), so a 21 entry
  decodes to another row's name and a replayed reserve is refused. BK-R4's
  bump is load-bearing; §0, §1.2, §1.7, BK-R4 and the index row say so.
  The proposal (22, refuse 21) is unchanged.
- **Citations:** `keystoneid-invariant.md:216-219` → `:230-233` in six
  places; `CLAUDE.md`'s four lines; `sql.md`'s four; the standing order's
  source (`raft-marks-2026-10-07.md` §11, not AY-Q6); `keystone.cpp:44` →
  `:42`, `row_codec.cpp:894` → `:915`, `step_vm.cpp:2403` → `:2482`;
  `decay.hpp:47` is a comment, not a constant; PURGE's window is checked
  against `kIdSpaceEnd`.
- **Claims corrected:** "18 files" → eleven under `include/` and `src/`
  plus `sim/integrity.cpp`; "no record format moves" → the Cabin entry's
  does; `Decode`'s `0xFFFFu` mask is harmless under BK (a word shifted by
  56 has eight bits left) and `reserved`'s **type** is what must move, so
  the mask is not a mutant; all four hand-spelt sites compile unchanged and
  what sets the trail key apart is that its miss is silent; `Find`'s one
  caller passes a `uint32_t` (BK-R3 now says what the guard keeps); neither
  page file includes `keystone.hpp` today; the red cells fail by the old
  answer, not by refusal; `DESCRIBE` prints `ids_remaining`, not
  `capacity`; `superblock_test.cpp:257` iterates `± 1` and so does not
  kill a version left at 21 until it spells it; the wire's pk field is
  typed as the column is declared (BK-R7, and BK-Q6).
- **Census additions:** `command_dispatcher.cpp:9822` spells
  `1099511627775` in a refusal; `keystone_budget_test.cpp:152` hard-codes
  `ids_remaining=1099511627773`; `btree_test.cpp:1450`'s comment; seven
  cell names that spell forty; `heap-and-tuple.md:107`,
  `assertion.md:222-224`, `cabin.md:719-721` (the entry's layout, twice);
  `parser-v2.md` I6's `InvalidArgument` where BD-R5 refuses `OutOfRange`;
  `waystone_dir_test.cpp:115` classified (a hash bit, not a pk).
- **Cells and mutants added:** the cross-step trail pair and the entry
  cell's flags compare, each the one that kills a half move; both half
  moves in BK-S2's list.
- **Two cuts for BK-S2:** `RowKeystoneId` duplicates `KeystoneIdOfPayload`;
  `keystone.hpp`'s header comment is stale since BD.

Two findings went past the text:
- **BK-Q6** is added unmarked: a pk declared narrower than 64 bits is cut
  to its declared width on the binary wire, against `protocol.md:93`.
- **BK-Q1** now says what each option does to the Cabin bound entry, which
  the order had assumed identical to the Keystone word under every option.

Applied from its duplication findings: the fixed-length bullet left "what
BK leaves standing" for BK-R7, BK-R9's `CLAUDE.md` sentence left for
BK-S3, and §1.8's by-name list became a count. Declined: none.
