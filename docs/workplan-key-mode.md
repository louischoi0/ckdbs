# Key mode (`EXPLICIT` pk) — workplan

Tasks `PK01`-`PK08` for `docs/heap-and-tuple.md` §4.1 (the 2026-08-11
amendment to invariant 11). The spec owns the argument and the refusal
codes; this file owns only the order and the acceptance.

**PK01-PK07 are done, 2026-08-11, built and green.** PK08 (docs) is this
file and the set it lists. One piece is deliberately unbuilt and carried
below as remaining work: **dividing a full btree internal node.**

**The shape of the work changed once, and the change is worth reading
before touching any task below.** This workplan was drafted against a
narrower decision — *the caller may supply a pk, but it must still ascend*
— and opened with "every task is additive: no storage path changes, and
the two insert paths are not edited at all." The final decision dropped the
ascent for explicit relations, and that made `BtreeInsert` the centre of
the work rather than a file left alone: with descending ids, the cursor
proves nothing about uniqueness and the descent has to prove it instead.
Every task description below has been corrected to what shipped. Where a
task's original text made an argument that is now false, the correction
says so rather than being quietly rewritten.

## PK01 — Catalog: `KeyMode`, the row field, the DDL parameter

`catalog::KeyMode {kAssigned = 0, kExplicit = 1}` in `well_known.hpp` beside
`ClusteredType`. `SysTableRow::key_mode` appended after `owner_core` with
`kKeyModeOffset`; `kOnDiskSize` grows one byte, **which takes the superblock
from 13 to 14** — a growing catalog row is a format-version event here, six
times over, and `SysTableRow::Decode` refusing any other size is what turns
the omission into an opaque first-read failure instead of a refusal at the
door. (This task was drafted claiming the opposite, on `docs/page.md` §16's
"no shipped format"; that sentence is about the page layout, not the catalog
rows.) `Catalog::CreateTable` grows a `KeyMode` parameter, and **every
caller passes it explicitly** — the bootstrap tables and the legacy
`HandleCreateTable` pass `kAssigned` by name rather than by default, because
a defaulted mode is how the wrong one reaches a relation without anyone
reading the line. `TableAccess` caches it: a DDL-only fact, the same class
as `clustered_type` (`catalog_cache.hpp`'s rule).

`Catalog::CreateTable` also carries the **`EXPLICIT` ⇒ `BTREE`** refusal
(`Unsupported`), which PK01 did not originally have. It landed here rather
than only at the statement layer because this is where a relation comes into
being, and a relation that can accept no `INSERT` should not be creatable
through any path — the catalog can be driven directly.

Files: `well_known.hpp`, `rows.hpp`, `rows.cpp`, `schema.hpp`, `catalog.hpp`,
`catalog.cpp`, `superblock.hpp`, `command_dispatcher.cpp` (both CreateTable
call sites), `tests/catalog_row_test.cpp` (round-trip, byte isolation, the
pinned layout), `tests/catalog_test.cpp` (per-relation, survives a reopen,
reaches TableAccess), and the mechanical call-site updates the required
parameter forces across `tests/` and `bench/`.

**[DONE 2026-08-11.]** Built and run. Every data file predating the bump
stops mounting, which is the bump working as intended.

## PK02 — Catalog: `AdmitExplicitRowId(oid, id)`

**Corrected — this task shipped with one of its three rules deleted, not
implemented.** As drafted the gate was:

1. `id < kFirstRowId || id > kMaxKeystoneId` → `InvalidArgument`.
2. `id < row.next_id` → `OutOfRange` ("the relation's id sequence has gone
   backwards").
3. otherwise `row.next_id = id + 1`.

**Rule 2 does not exist.** With descending ids permitted, `next_id` stops
being evidence about what is in use — a relation whose mark is 1000 may have
nothing at 500, or may have had 500 since its first insert — so rejecting
below the mark would refuse correct inserts while proving nothing about the
ones it admits. What shipped:

1. `id < kFirstRowId || id > kMaxKeystoneId` → `InvalidArgument`. **The only
   check**, and it is spellability: an id outside the Keystone field cannot
   be stored by any path, so it is answered before the catalog page is
   touched at all.
2. wrong mode (`row.key_mode != kExplicit`) → `Unsupported`, the mirror of
   `AllocateRowId`'s refusal of a `kExplicit` relation. Proving at the callee
   that neither is reachable in the wrong mode is cheaper than trusting the
   call sites.
3. `row.next_id = max(row.next_id, id + 1)`, persisted **before returning** —
   `AllocateRowId`'s ordering verbatim, so a crash between the mark and the
   insert leaves a ceiling that is too high (K3 calls a burned id free)
   rather than one too low. An id **below** the mark writes nothing at all,
   which is what keeps a backfill of old ids from touching the catalog page
   once per row.

The mark is therefore a **high-water mark** for K4's budget and the 40-bit
exhaustion check, never a uniqueness gate. Uniqueness moved to PK04a's
descent.

`AdmitExplicitRowId` never draws from `row_id_leases_`, and no lease can
exist for an explicit relation: a lease is carved through
`AllocateRowIdRange`, which refuses `kExplicit`.

Files: `catalog.hpp`, `catalog.cpp`, `tests/catalog_test.cpp`.

**[DONE 2026-08-11.]**

## PK03 — Parser: the key-mode word

`ParseCreateTable`'s optional trailing slot loops over bare identifiers
instead of peeking one. `HEAP|BTREE` sets storage, `ASSIGNED|EXPLICIT`
sets the mode, each category accepted at most once (a repeat is
`InvalidArgument` with the byte), and anything else still falls through to
the top-level trailing-garbage check unchanged. `CreateTableStmt::key_mode`
and `key_mode_byte_offset` — the offset exists so the statement-layer
`EXPLICIT` ⇒ `BTREE` refusal can name the offending word. Both words stay
**identifiers**, never reserved.

Corpus lines for each word and for both orders. The test asserts
`kFingerprintVersion == 1` explicitly rather than leaving it to memory:
this is new syntax, no accepted statement's hash moves, and the version
therefore does not (§4.1, `parser/fingerprint.hpp`'s bump rule).

Independent of PK02 — ran beside it.

Files: `parser.cpp`, `ast.hpp`, `tests/testdata/parser_corpus.txt`,
`tests/parser_test.cpp`, `tests/fingerprint_test.cpp`.

**[DONE 2026-08-11.]**

## PK04 — Dispatcher: single-row `INSERT`

Arity flips on the mode. `kAssigned` keeps today's two refusals verbatim.
`kExplicit` requires `values.size() == ncols` and refuses `ncols - 1` with
the mirror message, naming the pk column the same way. Any other arity is
refused with a message naming the rule rather than the count, before the id
is settled, so a refused row burns nothing (BI9).

The pk comes from `values[0]`, must be an **integer literal** (anything else
refused with the value's byte — the gate runs before placement and must not
depend on evaluation), must not be negative (also refused with its byte),
and goes through `AdmitExplicitRowId` **at exactly the position
`AllocateRowId` occupies on the assigned path**: after
`enforcer_.AdmitInsert`, before `EncodeRow`. That keeps the assertion
admission check ahead of the burn, which is the property FK and BI9 both
already rest on.

The pk is split off `values[0]` once, into a body vector holding the
remaining columns, so every consumer downstream — the FK forward check,
assertion admission, `EncodeRow`, the Cabin witness, index maintenance —
keeps receiving the shape it already expected: the columns *after* the key.
`EncodeRow` still takes the id as its own argument, so the Keystone word
stays the only copy of the key (§4's third bullet).

The statement-layer `EXPLICIT` ⇒ `BTREE` refusal lives in
`HandleCreateTableSql`, carrying `key_mode_byte_offset`. It is a statement
about where rows can be put rather than about how the words go together,
which is why it is not in the parser: the grammar takes the two trailing
words in either order and neither one's meaning depends on the other.

Files: `command_dispatcher.cpp`.

**[DONE 2026-08-11.]**

## PK04a — Btree: uniqueness by descent, and dividing a full leaf

**This task did not exist in the original plan, and it is where the work
actually was.** The drafted workplan asserted `BtreeInsert` would not be
edited at all. Dropping the ascent made that false in three places:

1. **Uniqueness.** `BtreeInsert`'s duplicate check stops being "a sanity
   check on the id sequence" and becomes the proof: the descent lands on
   the one leaf that may hold the key, the leaf's live slots are scanned,
   and a hit is `AlreadyExists` naming page and slot. A delete-marked
   version still holds its key — a `DELETED` slot is live until retirement,
   and nothing retires — so a deleted pk cannot be re-supplied. That is K1
   holding for explicit relations by the same mechanism it holds for
   assigned ones.
2. **`SplitLeafAndInsert`.** An id that sorts *inside* a full leaf used to
   be refused with `OutOfSpace` citing the open split policy. It now
   divides: live versions copied out, sorted by key, cut at the median. The
   old leaf keeps its `min_key` (invariant 2) and is **rebuilt rather than
   edited**, because `RetireSlot` marks slots dead without reclaiming their
   bytes — retiring the moved half would leave the page just as full and the
   division would make room for nothing. The new leaf's `min_key` is the
   split key; both halves are at or above their own bound (invariant 3).
   The old page's `relayout_epoch` becomes `old + 1` (§3.1a's pairing rule,
   restored to one past the old value rather than bumped from the zero the
   reformat left); delete marks travel with the version they belong to;
   sibling links are re-established at both ends; secondary indexes need
   nothing, because an entry's sort key is `key || pk` and never a location.
   A leaf with fewer than two live tuples is `OutOfSpace` naming why.
3. **`FindSlotForId`.** A leaf's slots are no longer in key order, so the
   binary search became an optimization over a linear fallback rather than
   the whole search.

Files: `src/storage/btree/btree.cpp`, `tests/btree_test.cpp` (the division
tests: reachability after a division, page-wise key ordering preserved,
`min_key` unchanged, the epoch bump, a moved delete mark, the oversized-row
refusal, and a fully descending 400-id load).

**[DONE 2026-08-11.]**

## PK05 — Dispatcher: bulk `INSERT`

**Corrected.** As drafted this task said "each row calls
`AdmitExplicitRowId` in statement order, which is what makes non-ascending
rows inside one statement fail on the offending row with `OutOfRange`".
There is no such failure: rows inside one statement may name keys in any
order, and `tests/key_mode_test.cpp` pins that they all land.

What shipped is simpler than the draft: bulk `INSERT` runs every row through
`InsertOneRow`, the same single-row pipeline, in statement order (BI2), so
the mode-conditional arity, the literal check and the admission gate are
reached once per row with no bulk-specific copy of any of them. BI4's
whole-statement atomicity and BI9's burn rule are unchanged.

`AllocateRowIdRange` is not called on an explicit relation — it refuses
`kExplicit` outright — and **the sorted-fill fast path excludes explicit
relations** (`SortedFillEligible`): it carves one contiguous id range up
front and appends in order, which is the wrong shape for ids the caller
names. The exclusion is stated on `key_mode` rather than inherited from
`kHeap`, even though an explicit relation cannot reach that path through
DDL, so the coupling cannot be broken silently from the other end.

The draft's note on `ChainAppendBatch` is moot: it serves heap relations,
and no explicit relation is a heap relation.

Files: `command_dispatcher.cpp`.

**[DONE 2026-08-11.]**

## PK06 — `DESCRIBE`

`key_mode=ASSIGNED|EXPLICIT` after `clustered_type=` on the summary line.
The per-column `autoincrement=` line reads the mode rather than `is_pk`
alone: on an explicit relation the pk column is not autoincrement, and
saying so is the difference between the output being true and being
convenient (CLAUDE.md's truthfulness rule).

`SHOW BUDGET` needs no change — it already derives from `next_id`, which
the high-water advance keeps truthful.

Files: `command_dispatcher.cpp`, `tests/command_dispatcher_test.cpp`.

**[DONE 2026-08-11.]**

## PK07 — Tests

`tests/key_mode_test.cpp` is the end-to-end file, through SQL, the way a
caller meets the feature; the unit-level pieces stay where they live (the
catalog gate in `catalog_test.cpp`, the leaf division in `btree_test.cpp`,
the grammar in `parser_test.cpp`). What it pins:

- The claim itself: a caller-named key is the row's identity; a **descending**
  key is accepted; a fully descending load stays whole and findable; a range
  scan is correct after descending inserts; interleaved ascending and
  descending keys all land; a bulk statement may name keys in any order.
- The refusals that protect it: a duplicate key, and a duplicate of a
  descending key; a key outside the id space; a non-integer key with its
  byte; omitting the key on an `EXPLICIT` relation; supplying it on an
  `ASSIGNED` one; an `EXPLICIT HEAP` relation refused at `CREATE`; the key
  still not updatable.
- The mode as a property of the relation: two modes coexisting in one
  database, and the mode surviving across dispatchers.

**Corrected — the drafted acceptance criterion is gone.** The draft made the
suite's centrepiece a **byte-for-byte** comparison of an explicit load
against an assigned load reaching the same ids, on the argument that the
mode "changes who names the id and nothing physical". That argument died
with the ascent: an explicit relation is btree-clustered by force, its
leaves divide, and its pages are consequently *not* byte-identical to
anything an assigned load produces. The claim under test is now the one
`key_mode_test.cpp` states at the top — *a caller may name a relation's
primary keys, those keys need not ascend, and nothing else about the engine
changes* — and it is proved by reachability, ordering and refusal, not by
equal pages.

**[DONE 2026-08-11.]**

## PK08 — Docs

`docs/heap-and-tuple.md` §4 and §4.1 (rewritten), §3.1b, §5, invariant 11
in §8, and §9's open-decisions entries. `manual/sql/sql.md` — the CREATE
TABLE grammar and its trailing words, the Keystone-contract bullets, the
INSERT arity rule per mode, `DESCRIBE`'s output, and the error table.
`CLAUDE.md` — invariant 11, the milestone row, the Open Decisions entry.
`README.md` — the pk bullets under the Keystone constraints and the
invariant table. `docs/keystoneid-invariant.md` — K3 and §2's explicit-id
bullet. `docs/known-gaps.md` — the internal-node division. 
`docs/waystone-concpets.md` §8 and §9.

**[DONE 2026-08-11.]**

---

## Remaining work

**PK09 — dividing a full btree internal node.** Not implemented, and the
only piece of the feature that is not.

When a leaf division promotes a separator into a parent that is already
full, `PromoteSeparator` right-splits with no movement: a new internal node
whose only child is the new subtree, with the separator promoted another
level. **That is sound only when the separator sorts above every separator
the node already holds.** Under monotonic ids the argument was free — a
split always happened at the rightmost edge — and a caller-supplied id
removes it. The trick is kept, because it is correct and cheap in the case
it covers, and it is now **guarded by an explicit check**: a separator
sorting below the node's highest is refused with `OutOfSpace` naming this
task. Checked rather than assumed, because promoting an interior separator
that way would strand every subtree above it — silent data loss, not a
wrong answer anyone would notice.

Reaching the refusal needs roughly **678 leaf divisions under one parent**
(`kInternalMaxEntries` is 678, `btree_page.hpp`), i.e. a large descending or
interleaved load into one key region. Remote, not impossible.

Building it is an internal-node redistribution: cut the entry array at its
median, move the upper half to a new node, and promote the median separator
rather than the incoming one — the leaf division's shape, one level up, and
without the payload copying that made the leaf case need a rebuild. It has
no dependency on anything unbuilt.

**Not planned, and not a gap:** relaxing `EXPLICIT` onto heap relations.
That is the heap page split policy (`heap-and-tuple.md` §3.1b), and a heap
chain has no descent to prove uniqueness with — it is a different decision
with a different owner, listed open in §9.
