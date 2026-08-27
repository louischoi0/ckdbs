# ALTER TABLE v1 — catalog-only mutations

Decisions AL1-AL9. Workplan: `docs/workplan-alter.md` (`ALT01`-`ALT05`).
Written 2026-08-10, before any code, on the model every recent feature
followed: the spec is the argument, the workplan is the sequence, and a
form outside the supported class is refused at the earliest layer that
knows the byte.

## 1. AL1 — The class: mutations that change catalog facts and no tuple bytes

v1 is exactly two statements:

    ALTER TABLE <t> RENAME TO <new>
    ALTER TABLE <t> RENAME COLUMN <old> TO <new>

Everything else spelled under `ALTER TABLE` is a form this engine
understands and declines — `Unsupported`, with a position and the reason:

- **`ADD COLUMN` / `DROP COLUMN` / column type changes.** Invariant 13
  makes a relation's row size a schema constant; changing the column set
  changes the constant, and every existing tuple then *is* `Corruption` by
  the codec's own rule. Doing it right is a relation rewrite — a mover,
  page allocation, and the reader-horizon question — which is the physical
  optimizer's gated territory (`physical-optimizer.md` §6), not a
  catalog edit. Refused, not deferred-and-half-done.
- **Widening (`ALTER ... TYPE varchar(n)` in any spelling).** Permanently
  out, not open: the tagged cell has no per-column width to widen
  (`rule-fixed-length-tuple.md` — no `VARCHAR(n)`/`ALTER WIDEN` surface
  at all is a design decision this spec does not get to reopen).
- **Constraint and default surfaces** (`ADD CONSTRAINT`, `SET DEFAULT`,
  …): each belongs to the feature that owns the object (`CREATE
  ASSERTION` exists; defaults do not), and a second spelling of an
  existing surface is refused on I13's argument.

## 2. AL2 — Identity is the oid; a name is a label

Every cross-object reference in the engine is by oid, never by name:
`sys.fkeys` stores parent/child oids, `sys.indexes` its relation's oid,
`sys.cabins` likewise, the assertion registry keys on the oid, and
`owner_core` rides the relation row itself. So a rename **dangles
nothing**: FK enforcement, index maintenance and serving, Cabin
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
traffic registers new patterns on the ordinary n=2 path. A declared
pattern (`sys.pattern_defs`) keeps its old-name `source_text`; boot-time
re-registration of such a pattern fails its relation-resolution check and
the pattern is skipped, which is a performance event, logged, never an
error. **No pattern migration is attempted**: rewriting stored SQL text
would make the catalog a second parser, and a wrong rewrite is a wrong
canon forever.

## 4. AL4 — Assertions RESTRICT a rename

`sys.assertions` stores the declaration's `source_text` as the one canon
(AS10), and the recovery-side registry rebuild — unbuilt, but owed — will
re-parse it. A renamed relation would leave an *enforcing* constraint
whose canon names a table that no longer exists: unlike a pattern, an
assertion is not allowed to die quietly. So `ALTER TABLE` on a relation
with assertions is **refused**, naming the first assertion —
`exec::AssertionsOnRelation()`'s first live call site (the predicate
§8.3 built for a `DROP TABLE` that never came). Drop the assertion,
rename, re-declare against the new name: three honest statements instead
of one that silently breaks a constraint's canon.

The same argument does **not** restrict `RENAME COLUMN` for patterns
(AL3's class), but does for assertions: a `GROUP BY` or `SUM` column
named in an assertion's canon is the same canon problem, so the refusal
checks column renames against assertion text too — conservatively, by
relation, not by parsing the text to see whether the column matters. A
finer check is a later relaxation; a coarser refusal is never wrong.

## 5. AL5 — Coherency: one bump, no exceptions

A rename ends with `Catalog::BumpVersion()` — the one invalidation choke
point — so every cached `name → oid` mapping, `TableAccess`, and bound
statement stamped with an older `catalog_version` drops or revalidates.
The in-place-update exceptions (`SetPatternWaystoneRoot`,
`SetPatternOrigin`) do not apply: their argument was "read by nothing
else", and a name is read by *resolution itself*. Cross-core, the built
P6 machinery (flush + `kCatalogInvalidate` broadcast) carries the bump;
this spec adds no new coherency mechanism.

## 6. AL6 — Unlogged and non-transactional, like all DDL

A rename is a catalog write: unlogged, not undone by `ROLLBACK`
(`txn.md` §7's standing state), and lost by a crash after the fact like
every other catalog mutation — `docs/inflight/known-gaps.md`'s class, not a new
gap. It is admitted inside an explicit transaction exactly as `CREATE
TABLE` is, and with the same caveat. Making DDL transactional is one
decision for all DDL and does not start here.

## 7. AL7 — Grammar and refusal bytes

`ALTER` joins the statement heads (the dispatcher's "unknown SQL
keyword" list grows by one word). Nothing is reserved: `alter`, `rename`,
`to` and `column` are ordinary identifiers matched by text at clause
position, `kFingerprintVersion` does not move, and — `ALTER` not being a
patternable leading word — every corpus line for it carries `-` hashes.

Refusals, each with a byte: a missing or non-identifier name is
`InvalidArgument`; `ADD`/`DROP`/`MODIFY`/`ALTER`/`SET` after the table
name is `Unsupported` with AL1's reason; `RENAME` to an existing name is
`AlreadyExists`; renaming a column to a sibling's name is
`AlreadyExists`; a `sys.*` relation is refused outright (the catalog's
names are load-bearing for bootstrap and are nobody's to change).

## 8. AL8 — What a rename must check, and what it must not

- New table name: non-empty, fits `kCatalogNameMax`, no existing relation
  carries it. The check and the write happen on the same core (DDL is
  core 0's), so check-then-write is atomic by the event loop, CB's D3
  argument.
- New column name: same checks against the relation's own columns.
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

`DROP TABLE` (its own feature: page-chain reclamation, the free-map
question, the RESTRICT sweep across fkeys/indexes/cabins/assertions —
AL4's predicate is shared with it, which is why it was built consultable),
`ADD`/`DROP COLUMN` (AL1), any data-moving ALTER (AL1), transactional DDL
(AL6), pattern migration (AL3), and renaming `sys.*` (AL7).
