# ALTER TABLE — catalog-only mutations

Decisions AL1-AL11. `ALTER TABLE` changes catalog facts and no tuple bytes;
a form outside that class is refused at the earliest layer that knows the
byte. AL11 (`ADD`/`DROP COLUMN`) is BW's
(`instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`); cleaning a
dropped column and reusing its slot are CD's
(`instructions/v3.2.0/workorder-cd-dropped-column-cleaning.md`), and the one data rewrite
here is that clean, of bytes no statement can read. `DROP TABLE` is its own feature (`docs/spec/drop-table.md`) and shares
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

**A Cabin is not a pattern, and a rename keeps it** (AQ-Q2, AR1-V3). An
expression or predicate Cabin's key function is stored and identified by
column **positions** (`sys.cabin_exprs`, `docs/spec/cabin.md` §10), not
names, so `RENAME TO` and `RENAME COLUMN` move nothing it reads, and the
same `F` respelled with the new name is the same Cabin. The two answer
differently because they cost differently: a pattern lost to a rename is a
trail rebuilt on the n=2 path for free, while a Cabin is a declared
structure with a maintained witness whose loss is the operator's to
decide. `DROP COLUMN` of an input is refused (AL11, BW-R8).

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

## 4a. AL4a — A rename takes the relation `X`

Both renames take the relation's `X` (the operator's ruling of 2026-10-21),
under a transaction of the statement's own - `CREATE ASSERTION`'s holder
(`assertion.md` §8.1, `ddl-transactional.md` §5f): it writes nothing and is
rolled back when the statement ends, so the rename stays transactionless
(AL6), and the `X` is held until then, across `AwaitDdlDurability`. The
grant is a moment no writer, positioned reader or declaration build of the
relation is mid-statement: a rename waits for every open transaction that
wrote the relation, and a writer arriving during it waits for the rename.
AL4's check and the column lookup run under the `X`, against a memo made
current at the grant (`Catalog::Revalidate`), and - when the rename took a
holder of its own - the name is resolved again there: one that moved to
another oid while the rename waited is refused `TxnConflict`, one that is
gone `NotFound`.

**The wait is the lock family's**: under `DispatchAsync` the rename parks
on the relation's slot and re-runs whole, and a holder that stays silent
for the 1 s fault net - one that reports no progress, or stopped advancing
(`txn.md` §5) - refuses it `TxnConflict`, retryable; a build that keeps
advancing is waited out. On the synchronous path it is refused at once. It
waits for the relation's open writers, every open child writer or creator
holding the parent's `IS` (D9(a) for a child writer, BF-Q11 (a) for a
child's `CREATE TABLE`; `foreign-keys.md` §2c), a positioned reader for its statement's
length, `PURGE DROPPED COLUMNS`, a KWP load, and an index, assertion or
transition build. The rename reports no progress, so a writer waiting for
it has the plain 1 s net.

**Why.** A `CREATE ASSERTION` building on the relation has no
`sys.assertions` row for AL4 to read until it publishes. A rename landing
inside the build would leave the published canon naming a vanished column,
and a chain of renames could make it name another column at the next mount -
a constraint silently different from the one declared.

**Inside an explicit transaction that holds the relation's `X`** - an open
DDL of its own that took it (`ADD` or `DROP COLUMN`, `CREATE` or `DROP
INDEX`) - the rename takes no second holder: the fence is already the
session's. **One that holds a weaker mode** - it wrote the relation (`IX`),
or wrote or created a child relation referencing it (the parent's `IS`,
D9(a) or BF-Q11 (a)) - is refused `TxnConflict` at once, retryable once
that transaction has ended: a statement cannot wait for its own session's
transaction. The refusal poisons nothing: `HandleAlter` is not inside
`InDdlStatement`. A positioned reader's `IS` is a read borrow, not the
transaction's, so a rename waits for that reader's statement rather than
being refused.

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
It is admitted inside an explicit transaction - unless that transaction
holds the relation in a mode weaker than `X` (AL4a) - and the rename stands
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
  one hold of `sys.columns`' root page (CT7), **compared ignoring case**:
  a name equal ignoring case to another live column's - or to one whose
  drop is undecided - is refused `AlreadyExists` at the new name's byte,
  naming that column as declared. The column being renamed collides only
  with itself, so `RENAME COLUMN a TO A` lands; `RENAME COLUMN a TO a` is
  `AlreadyExists`. Each refusal carries its token's byte: a missing old
  column the old name's, a collision or a name longer than 63 bytes the
  new name's.
- **The tuple key column may be renamed.** Identity is the Keystone word and
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
resizing rows after a drop (AL11), `DEFAULT` and a `NOT
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
- With no vacant slot it takes the lowest-position dropped slot column
  flagged clean (below), at that position and so in that slot (CD-S4); with
  neither - none reserved, or all held - it is `NotImplemented`, naming
  `PURGE DROPPED COLUMNS` when dropped columns hold slots. A dropped
  declared column has no slot to give.
- A name equal ignoring case to a live column's is `AlreadyExists`, naming
  that column as declared (`catalog.md` CT7's column rule); a dropped
  column's name is free in any case. A name longer than 63 bytes is
  `InvalidArgument` at its byte, never stored truncated.

**`DROP [COLUMN] <name>`** sets `kColumnDropped` on the column's row. The
column keeps its position, so every stored position (an index's columns, a
foreign key's, a Cabin's, a Transition's) stays valid, and its bytes stay
in every tuple.
- Storage walks it: the layout, `RowSpills` (purge still releases its
  spills) and the `UPDATE` copy. `EncodeRow` writes it zero with its null
  bit set where it has one (every slot column has one) since CD-S1, so a
  row written after the drop is born clean. A row written before the drop
  keeps its cell and bit, and one written after it by an older binary
  holds the bit clear; either reads NULL and its cell is never
  interpreted.
- The SQL surface does not see it: name resolution, `SELECT *`,
  `DESCRIBE`, `sys.columns`, the wire and KWP load descriptors, and
  positional `VALUES`, which count live columns (`RowLayout::live_columns`)
  and are expanded to physical width behind the writer's `IX`.
- Its name is free for an `ADD` or a `RENAME` once the drop's transaction
  is no longer in flight - and at once for an `ADD` inside that
  transaction, which alone can pass the `X`; its slot is reused only once
  `PURGE DROPPED COLUMNS` has flagged it clean.
- Refused: the tuple key, and a column an index keys or covers, a foreign key
  names, a Transition governs or a Cabin is on - each named - and every
  column of a relation an assertion is declared on (AL4's predicate).

**A heap's hidden tuple key** (`heap-and-tuple.md` §3.1b) is flagged
`kColumnHidden` (`0x04`) and skipped by every surface site that skips a
dropped column, the KWP load descriptor excepted; no `DROP` or `RENAME
COLUMN` can name it. Hidden is not dropped: storage reads it as the tuple
key. On a heap a positional row carries exactly the declared live columns,
whatever `ALTER` changed, and its key is always issued, so the count
question below does not arise there. A heap's last declared column is not
dropped, `InvalidArgument` at its byte: without it no statement could write
or read the relation (BZ-Q3).

**Positional rows after a change** (BW-Q9 (a)) `[quiet-wrong]`. On a
relation whose column set has changed (`RowLayout::column_set_changed`), a
positional row carries every live column, its key `NULL` to have one
issued; the short form is refused, since after an `ADD` its length is that
of a row written for the old columns with its key named, and admitting it
would put every value one column over. **What the count does not catch**
(`known-gaps.md`): after a `DROP`, a row written for the old columns with
its key left out has the full form's length and is admitted, its first
value read as the key; and after a `DROP` and an `ADD` of one column each,
an old full row has the new full form's length. Positional SQL text carries no
shape to check, so a positional statement written before a change is the
writer's to re-read. **A column list closes it** (BW-S6, BW-R13 of
`instructions/v3.2.0/workorder-bw-instant-add-drop-column.md`): `INSERT
INTO t (c1, c2, ...) VALUES (...)` names its columns, so no column change
can shift its values. Names match ignoring case, as every column name does
(`parser-v2.md`; `instructions/v3.2.0/raft-marks-2026-10-15.md` §1); an unknown
or dropped name and a repeated one are refused at their byte; a nullable
column the list leaves out is NULL - every added column is nullable, so a
list written before an `ADD` stays valid after it - and a NOT NULL one left
out refuses the statement at the list's `(`; the tuple key left out or
`NULL` is issued; each row has one value per listed name. BW-Q9's rule
never applies to a listed row. A `NULL` key at full width asks for an issued id on every
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

**`PURGE DROPPED COLUMNS FROM <t>`** (CD, `instructions/v3.2.0/workorder-cd-dropped-column-cleaning.md`)
cleans every dropped column of the relation in place and flags it.
- Autocommit, admin, under the relation's `IX`, in a DDL transaction of its
  own: readers and writers go on; a column DDL waits. A heap, a
  Transition-governed relation and an FK child are admitted - it frees no
  key. A relation with no dropped column, a `WHERE`, and an explicit
  transaction are `NotImplemented`, each at its byte.
- **The statement epoch** `[quiet-wrong]`: a statement whose `IS` a drop's
  `X` refused may read on with the layout it bound, in which the column is
  live, so nothing is cleaned until every core's running statements
  revalidated at or after the word this one's head published (W): the
  drop moved the word before it released its `X` (`catalog.md` CT2).
- **Per row, under the leaf's exclusive hold**: a row clean already is left;
  a row whose header writer is not resolved for every reader is left and
  waited for; otherwise every dropped cell is zeroed, its null bit set where
  it has one, and the row rewritten in place as `HEAP_OVERWRITE` at
  `kNoTxnId` keeping the header's writer and undo pointer - no version, no
  undo, no lock - and after the hold the spills those cells named are
  released. A resolved head has no reachable older version, and no writer
  but this pass turns a dirty image clean (an `UPDATE` copies the dropped
  cells, a rollback restores what its writer copied).
- **The waits** - the epoch, an undecided writer, an older snapshot - poll
  in PURGE's loop for up to 1 s, then refuse `TXN_CONFLICT` - a row wait
  naming the key, the rows still blocked and the rows cleaned, the epoch's
  saying nothing was cleaned; the cleaned rows stay clean
  and a re-run finishes. It does not yield its core (CD-OI-11 (a)).
- **On an attempt that leaves no dirty row** it drops every column Cabin on
  a dropped position (a column `CREATE CABIN` takes no relation lock, so one
  can land there and no statement can name it), and flags each dropped
  column `kColumnSlotClean` in its transaction (superblock 32). Since CD-S1
  `EncodeRow` writes every new row clean.
- **Reply**: `PURGED DROPPED COLUMNS <t>: <k> column(s) clean, <n> row(s)
  cleaned, <s> spill(s) released`, then `, cabin <id> dropped` per swept
  Cabin.

**Reuse** (CD-S4). The `ADD` that takes a clean slot rewrites the dropped
column's `sys.columns` row in place - its oid, which is its Keystone word,
and its position kept, the new definition, `generation` + 1 - so the new
column has the dropped one's offset and bit, every older row reads NULL,
and a rollback restores the dropped clean row. Every `ADD` asks what names
the position it takes: a column Cabin there names no column and is dropped,
anything else refuses. A KWP load compares a digest of the relation's
column rows - position, oid, generation, flags less the clean bit, type,
width, name - at
every chunk, since the counts return to their old values across a drop, a
clean and a reuse.

**What is not built.** Resizing rows after a drop, so a declared column's
dropped bytes and a `SPARE 0` relation's are never reusable - `known-gaps.md`.
