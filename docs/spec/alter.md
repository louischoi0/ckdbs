# ALTER TABLE — catalog-only mutations

Decisions AL1-AL11. `ALTER TABLE` changes catalog facts and no tuple bytes;
a form outside that class is refused at the earliest layer that knows the
byte. AL11 (`ADD`/`DROP COLUMN`) is BW's
(`instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`). `DROP TABLE` is its own feature (`docs/spec/drop-table.md`) and shares
AL4's RESTRICT predicate.

## 1. AL1 — The class: mutations that change catalog facts and no tuple bytes

Four statements:

    ALTER TABLE <t> RENAME TO <new>
    ALTER TABLE <t> RENAME COLUMN <old> TO <new>
    ALTER TABLE <t> ADD [COLUMN] <column definition>     (AL11)
    ALTER TABLE <t> DROP [COLUMN] <name>                  (AL11)

`ADD` and `DROP COLUMN` stay inside the class because the row size never
moves: an added column takes a slot every row already has, and a dropped
column keeps its bytes (AL11). Everything else spelled under `ALTER TABLE`
is understood and declined with a position and the reason. The
row-rewriting forms answer **`NotImplemented`**, not `Unsupported`: they
cost a relation rewrite this engine does not do, and `Unsupported` is
reserved for what the architecture forbids (`include/kds/base/status.hpp`,
`docs/spec/protocol.md` §11).

- **Column type changes, and an `ADD` with no free slot.** Invariant 13
  makes a relation's row size a schema constant; changing it means every
  existing tuple *is* `Corruption` by the codec's own rule. Doing it right
  is a relation rewrite — a mover, page allocation, and the reader-horizon
  question — which is the physical optimizer's gated territory
  (`physical-optimizer.md` §6), not a catalog edit. Refused, not
  deferred-and-half-done.
- **Widening (`ALTER ... TYPE varchar(n)` / `char(n)` in any spelling).**
  Refused for the same reason: a cell's width is part of the row-size
  constant (invariant 13; `rule-fixed-length-tuple.md` §4 gives
  `varchar(N)` its per-column width), so changing it rewrites every tuple
  of the relation. Refusing it is not a statement about whether per-column
  widths exist.
- **Constraint and default surfaces** (`ADD CONSTRAINT`, `SET DEFAULT`,
  …): each belongs to the feature that owns the object (`CREATE
  ASSERTION` exists; defaults do not), and a second spelling of an
  existing surface is refused on I13's argument.

## 2. AL2 — Identity is the oid; a name is a label

Every cross-object reference in the engine is by oid, never by name:
`sys.fkeys` stores parent/child oids, `sys.indexes` its relation's oid,
`sys.cabins` likewise, and the assertion registry keys on the oid. So a
rename **dangles nothing**: FK enforcement, index maintenance and serving, Cabin
observation and the write hook, and every compiled chain in flight keep
working, unmodified, because none of them ever read the name again after
resolution. The tests exist to pin that claim, not to hope it.

What a rename does affect is **text**:

## 3. AL3 — Patterns are allowed to die

A fingerprint hashes the statement's tokens, names included. After
`RENAME TO`, traffic written against the new name is a different shape:
stored patterns and their Waystone trails for old-name statements simply
stop matching. That costs replay speed and nothing else — invariant 8 is
the whole reason this is acceptable — and it is self-healing: new-name
traffic registers new patterns on the ordinary n=2 path. **No pattern
migration is attempted**: rewriting stored SQL text would make the catalog
a second parser, and a wrong rewrite is a wrong canon forever.

## 4. AL4 — Assertions RESTRICT a rename

`sys.assertions` stores the declaration's `source_text` as the one canon
(AS10), and the mount-time registry rebuild re-parses it
(`src/exec/assertion_catalog.cpp`). A renamed relation would leave an
*enforcing* constraint whose canon names a table that no longer exists:
unlike a pattern, an assertion is not allowed to die quietly. So a rename
of a relation with assertions - and a `DROP COLUMN` on it (AL11) - is
**refused**; `ADD COLUMN` is not, since it moves no name or position a
canon reads. The refusal names the first
assertion — `exec::AssertionsOnRelation()`, the predicate `DROP TABLE`
shares. Drop the assertion, rename, re-declare against the new name: three
honest statements instead of one that silently breaks a constraint's canon.

The same argument does **not** restrict `RENAME COLUMN` for patterns
(AL3's class), but does for assertions: a `GROUP BY` or `SUM` column
named in an assertion's canon is the same canon problem, so the refusal
checks column renames against assertion text too — conservatively, by
relation, not by parsing the text to see whether the column matters. A
coarser refusal is never wrong.

## 5. AL5 — Coherency: one bump, no exceptions

A rename ends with `Catalog::BumpVersion()` — the one invalidation choke
point — so every cached `name → oid` mapping, `TableAccess`, and bound
statement stamped with an older `catalog_version` drops or revalidates.
The in-place-update exception (`ClaimPatternWaystoneRoot`) does not
apply: its argument is "read by nothing else", and a name is read by
*resolution itself*. Cross-core, the
schema version word carries the bump and every other core's next boundary
re-reads (`catalog.md` CT2); this spec adds no new coherency mechanism.

## 6. AL6 — Logged and durable, not undone by ROLLBACK

**For the two renames.** `ADD` and `DROP COLUMN` are transactional (AL11).

A rename is a catalog write: WAL-logged like every catalog write, and its
records are synced before the acknowledgement
(`CommandDispatcher::AwaitDdlDurability`, `docs/spec/ddl-transactional.md`
§7), because a transactionless DDL statement has no commit record for the
durability class to ride on. It is **not** undone by `ROLLBACK`: `ALTER
TABLE` is outside the transactional-DDL set (`ddl-transactional.md` §5).
It is admitted inside an explicit transaction, and the rename stands
whichever way that transaction ends.

## 7. AL7 — Grammar and refusal bytes

`ALTER` joins the statement heads (the dispatcher's "unknown SQL
keyword" list grows by one word). Nothing is reserved: `alter`, `rename`,
`to` and `column` are ordinary identifiers matched by text at clause
position, `kFingerprintVersion` does not move, and — `ALTER` not being a
patternable leading word — every corpus line for it carries `-` hashes.

Refusals, each with a byte: a missing or non-identifier name is
`InvalidArgument`; `MODIFY`/`SET` after the table name is `NotImplemented`
with AL1's reason, and any verb but `RENAME`, `ADD` and `DROP` there is
`InvalidArgument` ("expected RENAME, ADD or DROP"); `ADD` and `DROP` take
`COLUMN` optionally, and `ADD`'s definition is `CREATE TABLE`'s column
grammar (`Parser::ParseColumnDef`); `RENAME` to an existing name is
`AlreadyExists`, to a name another transaction's uncommitted `DROP TABLE`
holds is `TxnConflict`, and to one the renaming transaction's own open drop
holds is `Unsupported` (commit the drop first); renaming a column to a sibling's name is
`AlreadyExists`; a `sys.*` relation is refused outright (the catalog's
names are load-bearing for bootstrap and are nobody's to change).

## 8. AL8 — What a rename must check, and what it must not

- New table name: non-empty, fits `kCatalogNameMax`, no existing relation
  carries it, and no drop that has not committed holds it - a rename lands
  for good, so a drop's rollback would restore the name beside it; that
  holds for the renaming transaction's own open drop too. The check and the
  rewrite are one hold of `sys.objects`' root page, which is what makes
  them one act across cores (`catalog.md` CT7, AT-S17).
- New column name: same checks against the relation's own columns, under
  one hold of `sys.columns`' root page (CT7).
- **The pk column may be renamed.** Identity is the Keystone word and
  position 0 (invariant 11), not the spelling; `CheckKeystoneColumn` is
  positional and does not re-run.
- No content validation re-runs: a rename changes no value, so FK, index,
  Cabin and assertion *data* checks have nothing to say (and AL4 already
  refused the assertion case on canon grounds).

## 9. AL9 — Observability

`sys.tables` / `sys.columns` / `DESCRIBE` / `SHOW *` answer with the new
name immediately (they read the catalog, AL5 invalidated it). No history
is kept: the engine does not remember old names, deliberately — a rename
alias table would be a second resolution path, and the first one is the
one every bug in this class hides behind.

## 10. Out of scope, named so nothing drifts in

`DROP TABLE` (`docs/spec/drop-table.md`), any row-rewriting ALTER (AL1),
cleaning a dropped column's bytes or slot (AL11), `DEFAULT` and a `NOT
NULL` added column, rollback of a rename (AL6), pattern migration (AL3),
and renaming `sys.*` (AL7).

## 11. AL11 — `ADD COLUMN` and `DROP COLUMN` over reserved slots (BW)

**The slots.** `CREATE TABLE ... SPARE n` (`0 <= n <= 64`, default 0)
reserves `n` 16-byte column slots, recorded on the `sys.tables` row
(`spare_slots`) and laid out by `RowLayout::Build` between the declared
columns and the null bitmap; slot `j`'s null bit is the declared nullable
columns' count plus `j`, held or not. So the row size, every offset and the
bitmap's size are fixed at `CREATE` for the relation's life, and invariant
13 holds through every `ADD` and `DROP` (`rule-fixed-length-tuple.md` §2,
`null.md` §2.1). A relation with no slot lays out byte-identical to one
from before slots existed. Its var-heap chain exists from `CREATE` when it
has a slot, so an added `varchar(16)` allocates nothing.

**A vacant slot is NULL in every row** `[quiet-wrong]`. `EncodeRow` - the
one producer of a row from nothing - sets every vacant slot's bit
(`SetVacantSlotBits`), and `EncodeAssigned` copies the bits with the
before-image, so a column a later `ADD` places there reads NULL in every
row that predates it. Without the bit the zero-filled slot would read as a
present `0`.

**`ADD [COLUMN] <col> <type> NULL`** places the column at the next
position, in the lowest free slot, its `sys.columns` row flagged
`kColumnInSlot`.
- It must be declared `NULL` (`null.md` §2.3's default is `NOT NULL`, and
  the rows that predate it hold no value); there is no `DEFAULT`.
- Its cell is at most 16 bytes - every fixed-width type, `char(N <= 16)`,
  and `varchar(16)`, which holds any length and spills past 13 bytes; a
  wider cell is `NotImplemented` naming the slot, never narrowed.
- `REFERENCES` and a cabin clause are `NotImplemented` (CREATE TABLE's).
- No free slot - none reserved, or all held - is `NotImplemented`. A
  dropped column keeps its slot.

**`DROP [COLUMN] <name>`** sets `kColumnDropped` on the column's row. The
column keeps its position, so every stored position (an index's columns, a
foreign key's, a Cabin's, a Transition's) stays valid, and its bytes stay
in every tuple.
- Storage walks it: the layout, `RowSpills` (purge still releases its
  spills) and the `UPDATE` copy. `EncodeRow` writes it zero, and the
  decoders read it as NULL and never interpret its cell.
- The SQL surface does not see it: name resolution, `SELECT *`,
  `DESCRIBE`, `sys.columns`, the wire and KWP load descriptors, and
  positional `VALUES`, which count live columns (`RowLayout::live_columns`)
  and are expanded to physical width behind the writer's `IX`.
- Its name is free for an `ADD` or a `RENAME` once the drop's transaction
  is no longer in flight - and at once for an `ADD` inside that
  transaction, which alone can pass the `X`; its slot is never reused.
- Refused: the pk, and a column an index keys or covers, a foreign key
  names, a Transition governs or a Cabin is on - each named - and every
  column of a relation an assertion is declared on (AL4's predicate).

**Positional rows after a change** (BW-Q9 (a)) `[quiet-wrong]`. On a
relation whose column set has changed (`RowLayout::column_set_changed`), a
positional row carries every live column, its key `NULL` to have one
issued; the short form is refused, since after an `ADD` its length is that
of a row written for the old columns with its key named, and admitting it
would put every value one column over. **What the count does not catch**
(`known-gaps.md`): after a `DROP`, a row written for the old columns with
its key left out has the full form's length and is admitted, its first
value read as the key; and after a `DROP` and an `ADD` of one column each,
an old full row has the new full form's length. SQL text carries no shape
to check, so a statement written before a change is the writer's to
re-read. A `NULL` key at full width asks for an issued id on every
relation, which is how a KWP load row has always asked (`bulkinsert.md`
BI15); the load passes it through, so a load described before a change is
refused by the same count. **And by its shape** (BW-Q10 (a)) `[quiet-wrong]`:
a load records the relation's oid and column shape at LOAD BEGIN, and a
chunk is refused behind its `IX` unless the relation is still that one in
that shape - a `DROP` and an `ADD` of equal net width would leave the count
right and the values in other columns.

**Isolation and atomicity: `CREATE INDEX`'s shape** (`ddl-transactional.md`
§5e). Both run inside the statement's DDL transaction and take the
relation `X` before their first catalog write, waiting for open writers
and positioned readers (the 1 s fault net refuses `TxnConflict`). `ADD`
inserts its row and `DROP` overwrites its row as the transaction's
(`OverwriteForDdl`, the prior image to the undo log), so `ROLLBACK` and
mount recovery undo either, and the decide moves the schema word before it
releases. **Readers are not isolated** (BW-Q8 (a)): the row is rewritten
in place, so another session's read can see an undecided column set - a
column missing from `SELECT *`, a name refused, an added column reading
NULL - before the decide; no value it shows is wrong.

**What is not built.** Cleaning (reclaiming a dropped column's bytes and
slot, resizing rows) - `known-gaps.md`.
