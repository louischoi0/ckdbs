# View - a named, stored SELECT a statement's block absorbs by text

Owning spec for `CREATE VIEW`, `DROP VIEW` and reading a view
(`instructions/v3.2.0/workorder-dp-view.md`, DP). It states what is built.
The work order carries the items (DP-Q0..Q23), the rules (DP-R1..R11), the
census and the stage log.

## VW1 - the object

A view is a `kTypeView` (36) row in `sys.objects` - a name in its
namespace's one relation name space beside the tables (NS6): a table and a
view never share a name, and `CREATE TABLE`, `RENAME TO` and `CREATE VIEW`
each refuse a name the other holds (`Catalog::CheckNameFree`). Its text,
column names, whether it is absorbable and what it reads are its rows in
`sys.views` (oid 168, root page 19, superblock 40): fixed-width
`SysViewRow`s - a header, the text in 192-byte chunks, one row a column
name, one row a dependency - written under the DDL's transaction
(`include/kds/catalog/rows.hpp`). A body is at most 16,384 bytes.

**A view's body never changes under its oid** (DP-R3). There is no
`CREATE OR REPLACE VIEW` (refused `NotImplemented` at `OR`); a new body is
`DROP VIEW` and `CREATE VIEW`, which issues a new oid. A dropped view's row
is retyped to `kTypeDroppedView` (37) and never retired, so its oid is never
reissued (DT2), and every plan, trail and statistics row made through the
old body names an oid that no longer resolves.

`CREATE MATERIALIZED VIEW` is refused `NotImplemented`: no view stores
rows.

## VW2 - CREATE VIEW and DROP VIEW

```
CREATE VIEW [<ns>.]<name> [(<column>, ...)] AS <select>
DROP VIEW [<ns>.]<name>
```

Both are `admin`'s (every DDL is) and refused in a `READ ONLY`
transaction. **Both are transactional** as a relation's DDL is: the rows
are placed or delete-marked under the transaction, a rollback takes them
back, a crash loser's are rolled back at the mount, and a view another
transaction has created but not committed resolves for no other reader
(DT3).

- **Columns** (DP-Q15). The column list names the view's columns; without
  one, each is named by its select-list item - a column's name, or a query
  layer item's alias. Two columns of one name, or an item with no name a
  reader could write, are refused `InvalidArgument`, naming the column list
  as the remedy. A list of the wrong length is refused at its `(`.
- **A body's unqualified names mean the view's namespace** (DP-Q21),
  whoever reads it.
- **`*`** (DP-Q11). A native body's `*` is the columns live at the create,
  stored so: the text is written with them in its place, and a column added
  later does not change the view. A query-layer body's `*` (`SELECT
  DISTINCT *`, a set operation) is not expanded; no column may then be
  added under it (VW6).
- **Validated at its create.** An absorbable body (VW3) is compiled; any
  other is compiled by the query layer without running (`exec::
  DescribeQuery`). A wrong body is refused there, its message naming the
  body's start byte.
- **What it reads.** The create holds `IS` on every relation and view the
  body binds, held to its decide, and reads the body again under those
  grants; a second reading that binds other relations is refused
  `TxnConflict`. A relation dropped before its grant is refused.
- **Nesting** (DP-Q8). A view may read views to depth 8, its depth one more
  than the deepest view it reads at any position, a subquery's and a
  derived table's included. A ninth level is refused `Unsupported` at its
  create. `8` is measured (DP-R11): about 3 us a level, linear - a point
  read through a chain eight deep costs 2.1x its hand-written expansion
  (`bench/v3.5.0/results-dp-close-v3.5.0-84-g78a32c4f.md` §6).
- **`DROP VIEW`** takes the view oid's `X` (the lock family's 1 s fault
  net), which a reader's bind and a `CREATE VIEW` over it hold `IS` on. It is
  RESTRICTed by a view that reads it and by an enabled administrator's plan
  made through it (VW5). Once it commits, the captured plans, disabled
  administrator's plans, statistics and Waystone opt-ins made through the
  view are forgotten.

`CREATE VIEW` answers `CREATED VIEW <name> oid=<n>`; a non-absorbable body's
answer carries `WARNING: its body is not absorbable (<why>) - every read
runs the whole body as a derived table`.

## VW3 - absorption

**A body is absorbable** (DP-Q1) when the native grammar reads it as one
block with no aggregate, `GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT`,
`OFFSET` or `FOR UPDATE`: one relation or an inner equi-join chain, views
among them, conjuncts and boolean trees, subquery predicates, a plain
select list. Moving an outer conjunct into such a block changes no answer,
because an inner join's ON and WHERE are one conjunct list.

**A view an absorbable block reads is replaced by its body**
(`exec::AbsorbViews`, `include/kds/exec/view_absorb.hpp`): the body's
relations at the view's position, the ON that attached the view kept with
the body's first relation, the body's conjuncts appended, and the view's
columns read through its binding. It runs at `CompileBlock`'s head, before
the bind, with no `PlanOptions` in hand - a class (a) compile-time item of
`parser-v2.md` I12's inventory (DP-R2) - in every block, subquery blocks
included; nested views absorb inside out. The block that leaves is the
statement's expansion written out by hand, and equality propagation, the
access ladder, key sets, the inner build, Waystone, Cabin and every plan
source run on it as written.

**A body's names are its own** (DP-Q23). Every name and relation of a
spliced body carries a fresh lens, and a name resolves only against
relations and views of its own lens: a reader reaches a view's columns - by
the view's binding or bare - and never its base relations; a body's names
never reach the reader's relations.

**A body binds only what it was created over** (DP-Q22 (b)): a body name
that resolves to an oid its `sys.views` dependencies do not hold - a
relation created later under a name the body read - is refused
`InvalidArgument`, never read.

`SELECT *` from a view is the view's columns under its names, a view over a
join included (DP-Q12).

## VW4 - a non-absorbable body

A view whose body is not absorbable is read by the query layer as a
derived table (DP-Q2): the body compiled over its own text and namespace,
**nothing of the reader pushed into it** (DP-R6), so the whole body is
folded whatever the reader's filter. A `SELECT` through one reaches the
query layer through the native compile's `NotImplemented`. Its cost is a
product limit, said by the create's warning and by `sys.views.absorbable =
0`.

## VW5 - shape identity and managed plans

- **The `fetch_id` is the written text's** (DP-Q5): a statement through a
  view and its hand-written expansion are two shapes, with two trails and
  two plans.
- **The binding** (DP-R5) declares the view's oid before its body's first
  relation, depth first, the body's relations with no unqualified
  namespace of the reader's; `sys.statement_stats.relations` lists the view
  then its relations.
- **The plan key** (DP-R4, DP-Q17) keeps `n` and `oids` as the block's base
  relations - every order and pin is measured by them - and carries the
  views the block absorbed, in declaration order, at most 16
  (`CapturedKey::views`), persisted as `views=` in `sys.plans`' text. A
  captured order or an administrator's plan made through a view applies to
  neither the hand-written expansion, nor another view with the same body,
  nor a view created again under the name.
- **A plan names a view's inner relation by the view's path** (DP-Q7):
  `<view binding>.<inner binding>`, depth first (`v.w.o`), in `ORDER (...)`,
  `ACCESS`, `BUILD`, `sys.plans` and `ANALYZE` (`o AS v.o`).
- An enabled administrator's plan made through a view RESTRICTs its `DROP
  VIEW`, naming the plan (DP-Q6); a disabled one reads `invalid` once the
  view is gone.

## VW6 - what a view RESTRICTs

What a view's text names must keep its name, so each of these is refused,
naming the view: `DROP TABLE` of a relation it reads; `ALTER TABLE ...
RENAME TO`; `RENAME COLUMN` or `DROP COLUMN` of a column it names - its
text names any column whose name it writes, and a `*` body every column;
`ADD COLUMN`, or `RENAME COLUMN` to a name its text writes, on a relation
it reads, since a column of that name could capture a name the body
resolves elsewhere today (DP-Q16 `[quiet-wrong]`); any `ADD COLUMN` under a
query-layer `*` body; `DROP NAMESPACE` of a namespace holding it; `DROP
VIEW` of a view it reads.

## VW7 - what names a view

- **Read**: a `SELECT`'s `FROM` and `JOIN`, a subquery's, a write's `FROM`
  (`UPDATE ... FROM`, `DELETE ... USING`, `INSERT ... SELECT`), `ANALYZE`,
  `WAYSTONE ON`/`OFF`, `CREATE PLAN`, `ADVISE PLAN`.
- **`FOR UPDATE`** (DP-Q10): admitted where the view absorbs to one
  relation - a locking read of that relation's rows, its WHERE read through
  the views to the relation's own columns, the body's conjuncts among it;
  refused `NotImplemented` through a view over a join or one the query
  layer answers.
- **Nothing else** (DP-Q9): `INSERT`, `UPDATE`, `DELETE`, `PURGE`, the KWP
  load, `CREATE INDEX`, a Cabin, an Assertion, a Transition, a foreign key's
  parent and every `ALTER` naming a view are refused `NotImplemented`
  (`Catalog::ResolveRelation`'s gate).
- **`DESCRIBE <view>`** and `sys.columns` list a view's columns, named as the
  view names them and typed as its body returns them now.

## VW8 - introspection

`sys.views` (and `SHOW VIEWS`): `oid`, `namespace_oid`, `namespace`, `name`,
`absorbable`, `columns`, `reads` (the oids it reads), `source_text`. Read
under the session's view, as every schema-object view is.

## VW9 - cost (DP-R10)

While no view has existed on the instance (`InstanceWords::views_seen`), a
statement asks nothing of views. Once one has, each `FROM` name of every
block is asked as a view first - a lookup in the catalog cache's whole set
of view names - and a block a `SELECT` absorbed is not asked again
(`SelectStmt::views_settled`). Measured at DP-S4
(`bench/v3.5.0/results-dp-close-v3.5.0-84-g78a32c4f.md`): a statement over
no view moves no standard cell outside its floor (one, `pkmix`, at the
edge of its own: -0.79 % throughput, about 0.3 us of 30 us, inside the
server-CPU control); a view that exists and is not read costs at most
0.4 us a point read, inside its floor; **reading through an absorbable
view costs a fixed 3.0-3.7 us a level** - the body parsed and absorbed
at every compile, since no compiled chain is cached (CX-S6) - flat in the
rows read.

## What a view does not do

No materialized view, no write through a view, no predicate pushed into a
non-absorbable view, no `CREATE OR REPLACE`, no security semantics (definer's
rights, a security barrier) - with no per-relation grant there is nothing a
view could hide - no view-level plan, no outer join in a body or around a
view (I9).
