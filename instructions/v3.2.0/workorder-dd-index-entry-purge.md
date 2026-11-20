# Work order DD - secondary index entries removed: an index entry purge, its premise measured before it is built

Status: **WRITTEN 2026-11-18, not opened.** Written on worktree
`db-fpi-cadence-dc-for-update` from `a4d2fb3b` (`v3.4.12-42-ga4d2fb3b`) on
the operator's word of 2026-11-18 (`raft-marks-2026-11-18.md` §24): *"Write
the work order for item 7, index entry removal."* (given in Korean;
recorded in English). No stage is started; DD-S0 waits for the operator's
word and for the marks on §4's items. Every `[source-read]` claim below was
read at `a4d2fb3b`, and the order was reviewed by `critics-developer`
before it was committed (§7).

**A note of 2026-11-20, to be read before DD-S0: DN gives DD a second
consumer, and several of this order's proposals do not serve it** (§0a,
on the operator's word, `raft-marks-2026-11-20.md` §16). Nothing below is
re-marked or rewritten by the note.

**Exploratory, and the premise is weak as measured.** The one measurement
the tree holds on a churned index argues against part of it (§1.1), so
DD-S0 can end the order with nothing built, or with one statement and no
background work, and either is a result.

**Its `[quiet-wrong]` items are DD-Q1, DD-Q2, DD-Q3 and the rules DD-R1 to
DD-R4**: an entry removed while a reader or a writer still needs it is a
row no probe finds, with no error.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured.

Related: `docs/spec/index.md` (IX1, IX2, IX4b, §7, §10.1-§10.3, §12, §12.2,
§13); `docs/spec/cabin.md` §5 (the two removal classes);
`docs/spec/txn.md` §4.1 (the horizon), §5 (the lock family), §6 (rollback);
`docs/spec/heap-and-tuple.md` §4.1c (`PURGE`, BH-S5's compaction);
`docs/spec/parser-v2.md` I15 (R1); `docs/spec/page.md` §6;
`instructions/v3.2.0/workorder-by-reindex-drop-index-reclaim.md` (BY: what a
rebuild keeps); `instructions/v3.2.0/workorder-bx-unique-index.md` (BX: the
run walk); `instructions/v3.2.0/workorder-ci-online-index-build.md` (CI:
appends compose); `instructions/v3.2.0/workorder-cd-dropped-column-cleaning.md`
(CD: a cleaning pass under `IX`, logged at `kNoTxnId`);
`instructions/v3.0.0/workorder-bh-purge-key.md` (BH: the horizon census);
`instructions/v3.2.0/workorder-db-torn-page-healing.md` (DB: the order that
touches the writers of the record kind DD logs by; DD adds one caller of
the existing writer and depends on nothing DB decides);
`bench/v3.2.0/results-by-close-v3.2.0-119-g9da66f0e.md`;
`bench/v3.2.0/results-ix4b-whole-entry-sort-key-v3.2.0-110-g4d10a613.md`.

## 0. What DD is

`[operator]` A work order for item 7 of CLA's survey of 2026-11-18: index
entries are appended per version and never removed, an update-heavy index
grows until a `REINDEX`, which holds the relation `X`; the missing
component is an entry removal tied to the purge horizon.

`[design]` An **index entry purge**: entries that no reader can reach and
no writer can need are taken out of a live tree, under the relation `IX`,
by the rule a rebuild already applies to decide what it keeps (DD-R1). The
write path does not change: `INSERT`, `UPDATE`, `DELETE` and rollback
still only append (IX2). DD proposes the purge as one statement first
(DD-Q4), and leaves a background driver to a gated stage.

**What the survey said, read against the tree.** Two statements stand:
nothing removes an entry from a live tree (§1.2 A), and the spec lists the
removal as a decision it has not taken (`index.md:1124-1127`). One does not
stand as said: **that the growth costs a statement anything is not
measured**, and the one reading there is says it did not (§1.1). **And the
driver CLA first weighed - a writer compacting its full leaf before it
divides, as the clustered tree does (BH-S5) - is not available as
stated**: the writer holds its clustered leaf while it maintains its
indexes, and deciding an entry is dead needs another clustered leaf
(§1.2 G). DD-Q4 shows it as the option it is.

## 0a. A note for the re-read: DN's consumer (2026-11-20)

`[operator]` *"Leave a note so that DD is looked at again from that
viewpoint."* (given in Korean; recorded in English) - after CLA's report
that DN (`workorder-dn-heap-rowid.md`) makes DD its prerequisite and that
DD's premise should be re-read with it.

Written in the main checkout on `main` at `9c7d3b2e`
(`v3.4.15-23-g9c7d3b2e`), documents only. **A note, not an amendment**: no
DD-Q is marked, no rule or stage is changed, and §1's claims stay as read
at `a4d2fb3b`. It lists what whoever opens DD-S0 reads again, and why.

**The viewpoint.** DD was written for one consumer: an index that grows.
Its premise is that the growth costs a statement something, which §1.1
does not show, so DD may end "do not build" (DD-Q0). DN adds a consumer of
another kind. In DN's heap an index entry names a rowid - a page and a
slot - and the operator's mark DN-D1 is that **a slot is reused only once
every index entry naming it is removed** (DN-R2, `[quiet-wrong]`: an entry
that outlives its slot names another row). So on a DN heap entry removal
is not about an index's size. **It is the only thing that ever frees a
deleted row's space.**

`[design]` What that changes in the reading, item by item. Each is a
question for DD-S0, not a finding.

1. **DD-Q0's verdict rule has no arm for it.** "Build", "narrow" and "do
   not build" are decided on a statement's CPU, a tree's pages and a
   `REINDEX` stall. None asks whether a consumer needs removal for
   correctness of reuse. Under DN a "do not build" verdict means a heap
   never reuses a slot. The rule needs a ground of its own for DN, or DD's
   verdict is said not to bind DN.
2. **DD-Q1 (a) keeps the entry DN most needs gone.** "A delete-marked row
   keeps its head entry ... until a row `PURGE` retires it" - and DN's
   heap admits `PURGE` only where the Keystone is the PK (DN-Q6 (a)). A
   deleted heap row's slot would then wait for an entry nothing removes.
   DD-Q1 (b) already names the case ("needed where rows cannot be purged
   (a heap, ...)"); for DN it is the case, not an edge.
3. **DD-Q3 (a) pins a slot per leaf.** "A leaf's first entry is never
   removed: at most one dead entry a leaf" costs an index a few bytes
   today. Under DN each such entry names a rowid, and that slot can never
   be reused: one slot leaked for every leaf of every index, for as long
   as the leaf stands. (b), the bound in the leaf header, and (c), the
   check from the parent's separator, do not have this cost. Either is
   also where DM's upper bound (`workorder-dm-leaf-upper-bound.md`, U2)
   would put the same fact.
4. **DD-Q4 (a) makes space reuse an administrator's statement.** With the
   purge "the statement alone in S1-S3", a heap reuses nothing until an
   admin runs `PURGE INDEX`. DN needs a driver that runs without being
   asked, which is DD-S4, "offered only on a **build** verdict".
5. **DD purges an index; DN needs to know a slot.** `PURGE INDEX <name>`
   (DD-Q6 (a)) removes one index's dead entries. A slot is free only when
   no entry of **any** index of the relation names it, as of every reader
   that could resolve one. Nothing in §2 says who knows that, or where it
   is recorded. DD-Q6 (c), a clause of `PURGE FROM t`, is the nearer
   shape; whether the knowledge is a pass over every index or a state on
   the slot is open, and is DN's to ask and DD's to answer.
6. **DD-R1's byte-equality is about another entry.** The judge compares
   the entry a version owes with the entry the write path appended, in
   bytes. On a DN heap the entry is `(index key, rowid)` and perhaps the
   Keystone (DN-Q3), not `(index key, tuple key)`. The premise is re-read
   for that entry before it is trusted there.
7. **DD-S0's cells measure no heap.** The seven cells price a churned
   index against a rebuilt one. For DN the number is how long a deleted
   row's slot waits, and how much of a churned heap is slots that wait.
8. **Since DD was written, more code leans on "no entry is removed".**
   DD-S0 already reads "every line that states or leans on" it. CV, which
   landed on 2026-11-20 (`fda024cf`), added one: the reverse ordered index
   walk's step, `IndexFetchLeafAtOrBelow`, whose comment says a leaf
   covers `[its separator, the right sibling's first entry)` "while no
   entry is removed" (`include/kds/storage/index/index_tree.hpp`). It is
   on the list whether or not DN opens.

**What does not change.** DD-R2 to DD-R7 are about a reader's and a
writer's safety while an entry goes, and DN adds nothing against them;
DN-R2 leans on them. The write path stays IX2's.

**Order.** DN's own mark is DD :>> DN-S2. This note adds: **DN-S0's
census (what DN must have from DD) :>> DD-S0**, so that DD measures and
decides once, with both consumers in front of it.

## 1. The tree as read

### 1.1 What is measured `[measured]`

| measurement | reading |
|---|---|
| BY's close, `v3.2.0-119-g9da66f0e`, `cores = 1`, `relaxed`, every page resident (`bench/v3.2.0/results-by-close-v3.2.0-119-g9da66f0e.md:6`, §1.2, §3.2, §8.2 of that file) | An index on `v` with 3n surplus entries - from rolled-back `INSERT`s of fresh `v`, or from `DELETE` + `PURGE` - holds 4n entries and 1,682 pages at n = 100,000; `REINDEX` leaves n and 421 (4.0x), at 0.64 us of build a row under the relation `X` (63-65 ms at that size). **A point probe costs the same before and after**: rebuilt twin minus untouched twin is -0.48 to +0.14 us over 10 cells, the control's own spread, including the cell whose height falls from 3 to 2. The file's own reading: with every page resident the probe is "protocol- and parse-bound ... and not tree-bound", and a pool smaller than the tree "is the cell to run next" |
| the same file, finding 4 (`:8`) and §1.4 | The `UPDATE`-churn row of that matrix **has no after-state**: a rebuild of a relation whose rows were updated twice was refused `Corruption` at both commits. The fix landed with BY-S4 (`workorder-by-reindex-drop-index-reclaim.md:431`); the cell was not re-run. **Cut there and never run since**: what a writer waiting behind a `REINDEX`'s `X` sees, and a cold or smaller pool |
| IX4b, `v3.2.0-110-g4d10a613` (`index.md:248-252`, `bench/v3.2.0/results-ix4b-whole-entry-sort-key-v3.2.0-110-g4d10a613.md`) | On `0479cb29`, before IX4b's whole-entry sort key, one row whose covered column moves was refused after 169 updates on a three-`int64` covering, 20 on a 4 x `varchar(128)` one (its run of equal sort keys outgrew a leaf); on the IX4b binary all 3,000 were accepted (that file's §5). Measured as a correctness boundary; what a probe of that key then costs was not |

**So the premise is weak as it stands.** BY's churn put its surplus under
key values no probe asked for. **Not measured anywhere in the tree:** an
index whose surplus shares the probed key's run - a low-cardinality key
rows move between (a status column), a key updated away and back; the
unique check's walk of such a run; a write into a churned tree against a
rebuilt one; any of it with the pool smaller than the tree; and the cost
only DD removes, a `REINDEX`'s `X` stalling the relation's writers.

### 1.2 What the tree does `[source-read]` at `a4d2fb3b`

**A. Nothing removes an entry from a live tree.** IX2: every maintenance
action is an append, a `DELETE` does nothing (`index.md:78-81`;
`include/kds/exec/index_maintain.hpp:20-33`). An `UPDATE` that moves no
key or covered column appends nothing (`index.md:85-88`). A rollback
leaves its entries (`txn.md:1210-1212`; `index.md:379-380`), and so does a
row `PURGE` (`heap-and-tuple.md:282`; `known-gaps.md:551-552`). "No index
purge was built" (`index.md:89-94`). The leaf view has an insert and a
split and no removal (`include/kds/storage/index/index_page.hpp:302`,
`:308`); the log has `INDEX_INSERT` and no kind that takes an entry out
(`include/kds/wal/record.hpp:86`). Only a rebuild drops entries, into a new
tree (`index.md:943-946`).

**B. What a surplus entry costs a read.** A probe collects the tuple keys
of every entry between its bounds (`src/exec/step_vm.cpp:1541-1601`), then
resolves each by a descent of the clustered tree; the key re-check against
the resolved version is what drops a stale one (IX1, `index.md:40-44`). So
a stale entry inside a probed run costs one tuple key descent when no other
entry in the run names its row, unless a covering filter drops it from the
entry (`index.md:350-353`); a further entry of a row already claimed costs
an entry visit and no descent (`step_vm.cpp:1585`, `:1595`), so a covered
column churned under one key costs visits and pages, not descents. `ANALYZE`
counts the entries walked (`index_entries_scanned`, `step_vm.cpp:1566`).
A unique index's check walks the key's whole run and resolves each distinct
row it names (`src/exec/index_maintain.cpp:325-392`; `known-gaps.md:1427-1431`
states the wait a stale entry can cost).

**C. Who reads index leaves, and how.** Three callers: the probe
(`step_vm.cpp:1543-1548`), the unique check (`index_maintain.cpp:332-335`)
and the `sys.indexes` entry count (`src/server/sys_views.cpp:569`). Each
walk holds one leaf at a time, reads its entry count once and visits every
entry under that one hold (`src/storage/index/index_tree.cpp:588-619`), and
keeps tuple keys, never a slot: "an index position is not a stable address
and nothing stores one" (`include/kds/storage/index/index_tree.hpp:112-115`).
An entry carries no location hint (`index.md:125-126`), and an index
page's header epoch is never moved - only the clustered tree sets one
(`src/storage/btree/btree.cpp:718`, `:881`) - so no Waystone trail or
Cabin hint names an index slot. Foreign keys do not read an index; the
plan statistics are index-blind (`index.md:1110-1112`).

**D. What a rebuild keeps is already one function, and its entries come
by another road than a writer's.** `IndexStagedRow`
(`src/exec/index_ddl.cpp:304-435`) appends an entry for a row's head
version, then for each older version, and stops below the first version
whose superseding writer is `ResolvedForEveryReader` (`:306-308`, `:356`,
`:395-397`; `index.md:658-664`). A head writer in flight at the copy is
never trusted resolved (`:311-314`, CI-R7). A delete-marked row is walked
like any other, so its head version keeps its entry even when every reader
sees the delete (`index_ddl.cpp:447-449`; `index.md:654-656`). Both the
walk and the write hook encode through `AppendIndexEntry`
(`index_ddl.cpp:242`), but they take a key's values from different
places: the write path from the statement's literals, coerced
(`index_maintain.hpp:53-55`), the walk from `DecodeRow` and
`ResolveSpills` over the stored version (`index_ddl.cpp:202-210`). A
rebuild fills a new tree, so a difference between the two costs it
nothing. A purge compares against entries a writer appended (DD-R1).

**E. The insert's coverage check assumes no removal.** A leaf's upper
bound is its right sibling's first entry, "which equals the separator the
sibling's divide copied up only while entries are never taken out"
(`index_tree.hpp:74-80`; `index_tree.cpp:56-67`; `index.md:1058-1062`;
`page.md:130`). A right sibling with no entries is refused `Corruption`
(`index_tree.cpp:86-92`). The read side asks nothing: a seek lands left of
its key and walks right, and "nothing moves an entry left"
(`index_tree.cpp:564-573`; `index.md:1070-1073`).

**F. A byte-identical entry is not appended.** `IndexInsert` that finds
its entry already in the leaf writes nothing and logs nothing
(`index_tree.cpp:378-386`). So a writer that gives a row a key and covered
bytes it carried before **relies on the old entry**. This is the hazard
DD-Q2 is about: an entry judged dead, then needed again by a write that
appended nothing, then removed.

**G. Holds, and why a writer cannot judge.** A writer holds its clustered
leaf across index maintenance: the `INSERT` from its placement
(`src/server/command_dispatcher.cpp:7376-7377`, `:7441`), the `UPDATE` from
its overwrite (`:10903-10934`). Two user relation pages are unordered, so
nothing descends the clustered tree holding one (`index.md:732-734`;
`index_maintain.hpp:188-192`), and no page is fetched under a live span
(R1, `parser-v2.md:169`; the probe and the unique check collect first for
that reason, `index_maintain.cpp:326-330`). The backfill copies a leaf
out, drops it, then works (`index_ddl.cpp:473-519`). No task parks holding
a pin (`page.md:130`).

**H. The lock family reaches every writer of a row - where there is
one.** `UPDATE` asks `U` on its tuple or window, `DELETE` `X`, every
`INSERT` `X` on its key (`txn.md:903-913`); a named `INSERT` borrows its
key's `X` before it places (`command_dispatcher.cpp:7266-7288`). A
transaction at `max_locks_per_txn` is refused, not left unlocked
(`txn.md:1185`). `TryAcquire` asks without waiting
(`include/kds/txn/lock_table.hpp:634`). A row `PURGE` takes the relation
`IX` and no row unit (`heap-and-tuple.md:274`). A dispatcher can run with
no transaction manager (`index_ddl.cpp:285-287`), and a write with no
transaction, or a dispatcher with no lock table, is granted every borrow
it asks (`BorrowChain`, `command_dispatcher.cpp:11774`).

**I. The horizon.** `ResolvedForEveryReader(w)`: every live and future
reader sees `w` as committed, so a version `w` superseded can go
(`include/kds/txn/manager.hpp:737-742`; `txn.md:602-615`). "Two purges
consume it" (`txn.md:633`). Views exempt by proof read latest state only,
or never outlive one synchronous span on their core's one thread
(`txn.md:615-619`) - an argument about a purge on that same core. A view
`MintReadView` hands out is held: its caller registers it or holds it on a
transaction (`manager.hpp:554-563`). `PURGE` states the obligation every
later view inherits (`heap-and-tuple.md:284`), and a `READ ONLY`
transaction's lease is the horizon's whole record of its view (CS-R2,
`txn.md:167-172`). `SettlementNow()` says when the resolver's answers can
have changed (`manager.hpp:744-755`).

**J. How a rewritten page is logged today.** `FULL_PAGE_IMAGE` (kind 10,
`record.hpp:59`) is written at `kNoTxnId` by a Bound Cabin chain's growth
and a row log's link (`src/exec/assertion_build.cpp:170`,
`src/exec/wal_row_log.cpp:94`), and under a transaction's id by a built
index tree (`index_ddl.cpp:164-173`); `LogFullPageImage` re-enters the
hold its caller has on the page
(`include/kds/storage/device_page_store.hpp:131-133`). A clustered leaf
compacted in place is instead "the one image of a `BTREE_SPLIT`", no
parent written (`heap-and-tuple.md:276`). Analysis ignores a `kNoTxnId`
envelope (`src/wal/analysis.cpp:53`), which is how `PURGE` and CD log with
no loser and no undo, adding no record kind and moving no superblock
(`heap-and-tuple.md:280`). A new kind has cost a bump: `ANCHOR_SLOT_REMOVE`,
superblock 29 (`index.md:930-931`).

**K. Two arguments that lean on append-only.** The online build composes
with writers because every action is an append and a duplicate is reported
(`index.md:776-779`); a building index is flagged
(`include/kds/catalog/rows.hpp:475`), and its drain waits out the
relation's `IX` holders under the lock family's net (`index.md:791-803`).
`cabin.md` §5 already names the two classes a physical removal falls in: a
dangling entry, droppable on sight, and a non-matching one, "droppable
only past the same oldest-active horizon" (`cabin.md:403-416`).

**L. Instruments DD-S0 has.** `sys.indexes` answers `entries` and `height`
(`sys_views.cpp:568-576`); pages per index are not exposed, and BY derived
them from the allocation counters across a `REINDEX` (§1.2 of its results
file). A `REINDEX` of a twin is the oracle: no purge can remove more than
a rebuild leaves out.

## 2. The design `[design]`

- **The judge.** For a tuple key `K`, the entries `K` is owed are the ones
  `IndexStagedRow` would append for it now - one implementation, given a
  sink that collects instead of appending. An entry naming `K` that is not
  among them is dead. A row with no slot is owed nothing.
- **The fence** (DD-Q2). The judge and the removal of `K`'s entries run
  under `K`'s tuple unit in `U`, asked with `TryAcquire` and never waited
  for. A writer of `K` holds `U` or `X` from before its page write, so
  while the purge holds the unit no version of `K` is being written and no
  append of `K`'s is in §1.2 F's window. A refused ask skips `K`.
- **Per leaf, in three phases**, as the backfill works (§1.2 G): copy the
  leaf's entries out under a shared hold and drop it; for each tuple key
  named, take the unit and judge, holding no page across the descent; then
  take the leaf exclusive, re-find each dead entry by its own bytes -
  rightward along the sibling links, since a divide after the copy moved
  it right and never left - remove, log, release the leaf, release the
  units. Units are held in bounded batches.
- **The leaf primitive.** `IndexLeafView` gains a removal that closes the
  array and refuses the entry at position 0 **as the leaf stands when it
  removes**: a divide after the copy can have made a copied entry the
  first of a new right sibling (DD-Q3). No leaf is emptied, freed or
  merged; no internal node is written; `LeafStillCoversKey` is unchanged.
- **The log.** One `FULL_PAGE_IMAGE` of the rewritten leaf at `kNoTxnId`,
  through `LogFullPageImage` under the leaf's hold (AT-S21; DD-Q5). A lost
  record leaves entries in place, which is today's state.
- **The driver** (DD-Q4): `PURGE INDEX <name>`, autocommit, admin. **One
  attempt** is a bounded run of leaves - CLA proposes 64, a walk's slice
  (`sched.md` §3) - under one owned transaction that takes the relation
  `IX`, does the three phases leaf by leaf and ends, giving back the `IX`
  and every unit, as a row `PURGE`'s attempt does
  (`heap-and-tuple.md:272`). The statement is a sequence of attempts with
  nothing held between them, and answers the count removed.

## 3. Rules

Binding once DD opens.

- **DD-R1 - one definition of a live entry, compared in bytes**
  `[quiet-wrong]`. What is kept is DD-Q1's marked option, decided by the
  walk the backfill uses; neither carries a second copy of it. **Its
  premise**: for every version, the entry the judge says it owes is
  byte-equal to the entry the write path appended for it (§1.2 D). Where
  that fails the purge removes a live entry and the row leaves every
  probe. DD-S0 and DD-S1 test it before anything removes.
- **DD-R2 - the horizon obligation is `PURGE`'s, extended to index probes
  and narrowed** `[quiet-wrong]`. Every view under which an index is
  probed is registered with the horizon, reads latest state only, or is
  excluded by a relation `X`. **The one-synchronous-span exemption is left
  out on purpose**: it holds against a purge on the view's own core, and
  `PURGE INDEX` runs on any. DD-S0 lists each view and shows no index
  probe runs under a `MintReadView` view that is not registered; a view
  kind added later owes the same proof.
- **DD-R3 - the fence's premise is stated and read, not assumed**
  `[quiet-wrong]`. Every path that puts a version of a row on a page holds
  that row's tuple unit in `U` or `X`, or a fence over it, from before the
  page write until its last index append and, on a rollback, its last
  compensation. DD-S0 reads every writer for it. With no lock table or no
  transaction manager there is no fence, and the purge is refused
  (DD-R8).
- **DD-R4 - the tree's shape does not move** `[quiet-wrong]`. What stays
  of a leaf is DD-Q3's marked option; no leaf is emptied, freed, merged or
  re-linked; no separator changes. Were a leaf's first entry to go,
  `LeafStillCoversKey` could send an insert into the leaf left of where a
  seek for it lands, and no probe would find it (`index_tree.cpp:61-63`,
  `:564-573`).
- **DD-R5 - the purge holds no clustered page while it holds an index
  leaf**, and waits for nothing while it holds a page or a unit.
- **DD-R6 - logged under the hold that made the change**, by DD-Q5's
  marked option, at `kNoTxnId`, replayed whole or not at all, never
  undone.
- **DD-R7 - the write path is IX2's still.** No statement's write, and no
  rollback, removes an entry; `IndexInsert`'s duplicate arm is unchanged.
- **DD-R8 - what it refuses, each with its byte and by name**: inside
  `BEGIN`; an index flagged building; a system relation; a session below
  admin; a dispatcher with no lock table or no transaction manager, which
  is told so and never answered a count of 0; under `backup.md` B13's
  fail-closed list, which gains it.
- **DD-R9 - it can be run again.** A refusal part-way keeps what was
  removed and says how many.
- **DD-R10 - an attempt's `IX` is bounded** `[loud]`. An online build's
  drain and every DDL asking the relation `X` wait for it, under the 1 s
  fault net; an attempt that outlasts the net refuses them `TxnConflict`.
  DD-S2 measures the longest attempt, and the leaf count is cut until it
  sits far inside the net.

## 4. Items for the operator (DD-Q), CLA's proposals

None is marked. (a) is CLA's proposal in each.

| | item | (a) | other |
|---|---|---|---|
| DD-Q0 | DD-S0's verdict rule. **The numbers are CLA's and the operator may replace them.** | **build** (S1-S3, and S4 offered) if, with a surplus equal to the live set, a statement of cell 1, 2, 4, 5 or 6 costs at least 10 % more server CPU on the churned twin than on the rebuilt one, beyond the control pair's spread; **narrow** (S1-S3, no S4) if none does, the churned tree is at least twice the rebuilt one's pages, and cell 7's `REINDEX` stalls a writer 100 ms or longer or refuses one; **do not build** otherwise, or if DD-R1's or DD-R3's premise is false on the tree with no local fix | (b) pages and a rebuild's stall never suffice: do not build unless a statement reaches the bar; (c) build whatever S0 shows |
| DD-Q1 `[quiet-wrong]` | Which entries are removable. | exactly what a rebuild leaves out: a row with no slot (rolled back, purged), and a version below the oldest reader. A delete-marked row keeps its head entry, as a rebuild keeps it, until a row `PURGE` retires it | (b) also every entry of a delete-marked row whose deleter is resolved for every reader (`PURGE`'s own gate): needed where rows cannot be purged (a heap, a relation under a Transition), no longer equal to a rebuild, and the superset test of §5 no longer holds as written |
| DD-Q2 `[quiet-wrong]` | What keeps a write from relying on an entry the purge is removing (§1.2 F). | the row's tuple unit in `U`, try-only, from the judge to the removal | (b) a per-entry dying mark a writer's duplicate arm clears: a leaf format change and a bump; (c) the relation `X`, which is `REINDEX` |
| DD-Q3 `[quiet-wrong]` | The insert's coverage check, which reads a leaf's bound off its sibling's first entry (§1.2 E). | a leaf's first entry is never removed: at most one dead entry a leaf, the check and the format untouched | (b) the bound stored in the leaf header: a format change and a bump; (c) the check re-derived from the parent's separator |
| DD-Q4 | Who drives it. | the statement alone in S1-S3; a background driver is S4, offered only on a **build** verdict | (b) a background job from the start, on core 0's steward, stepped when `SettlementNow()` moves; (c) the writer, at a full leaf - needs (b) of DD-Q2 so the leaf holds marks judged earlier, since the writer cannot judge (§1.2 G) |
| DD-Q5 | The record. | `FULL_PAGE_IMAGE` at `kNoTxnId`, 8 KiB a rewritten leaf and batch: one more caller of a writer that exists, no kind, no bump | (b) a new `INDEX_REMOVE` naming the entries: smaller, a kind and a bump |
| DD-Q6 | The spelling. | `PURGE INDEX [ns.]<name>` | (b) `REINDEX INDEX <name> ONLINE`; (c) a clause of `PURGE FROM t` |
| DD-Q7 | The close's measurement and bar. | S0's cells again, the purged twin against the rebuilt one; an ordinary indexed `INSERT` and `UPDATE` before against after, no cell slower outside its spread; a writer's wait on a unit the purge holds, and the longest attempt, counted. The standing budget | - |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DD-S0 - census and premise** | No engine code. §1.2 re-read at the opening commit. **Read**: every writer for DD-R3 (the per-row and window paths, `ON CONFLICT`, multi-row and KWP load rows, an issued key, a take-back, rollback's order against its release); every view an index probe runs under (DD-R2); every line that states or leans on "no entry is removed". **Test, with test code only**: on every relation of the index contract suite, a second index of the same definition built by `CREATE INDEX` holds no entry the write-maintained one lacks, byte for byte (DD-R1's premise); one cell replays a `FULL_PAGE_IMAGE` of an index leaf at `kNoTxnId` with no loser - the kind's replay is otherwise the tree's own (§1.2 J). **Measure** (below) | the lists, the numbers, DD-Q0's verdict, and any item back to the operator |
| **DD-S1 - the judge and the leaf primitive** | The collecting sink on `IndexStagedRow`; the leaf removal; the tree-level remove with its re-find. Reached by tests only. **A dry run on every contract-suite relation: every entry the judge owes is present in the tree** | review, the full suite |
| **DD-S2 - the statement** | `PURGE INDEX`: the parse, the role, the refusals, the attempts, the three phases, the units, the record, the count; the longest attempt (DD-R10) | review, the full suite, the cells below |
| **DD-S3 - the close** | DD-Q7's measurement; the documents below; the milestone row | closed, or S4 put to the operator |
| **DD-S4 - a driver** (only on a **build** verdict and the operator's word) | DD-Q4 (b), specified then | its own review and suite |

**DD-S0's measurement.** Three relations of one history a cell: `x_a`
takes a `REINDEX`, `x_b` and `x_c` are never touched. **The control pair
is `x_b` against `x_c`**: the spread DD-Q0 names is the range of their
paired differences. Entries and height from `sys.indexes`, pages from the
allocation counters, server CPU interleaved, `build-release`,
`cores = 1`, `relaxed`.

| cell | shape | what it can show |
|---|---|---|
| 1 | a low-cardinality key that rows cycle through; a probe of one value | **a cost almost by construction**: each stale entry in the run names another row and costs a descent (§1.2 B). What is open is its size against the statement, which is why DD-Q0 carries a magnitude |
| 2 | a key updated to fresh values, BY's missing row; a point probe | whether BY's "no probe moves" holds for `UPDATE` churn |
| 3 | a covered column churned under one fixed key; a probe of that key | **entry visits and pages only**: every further entry of the row is dropped by the dedup with no descent (`step_vm.cpp:1585`, `:1595`). Not a verdict cell |
| 4 | an `UPDATE` of the indexed column, churned twin against rebuilt | a write's cost in a taller, sparser tree |
| 5 | a unique key deleted and placed again | the unique check's walk of a run with stale entries |
| 6 | cell 2's probe with `buffer_pool_frames` between the rebuilt tree's pages and the churned one's | the cell BY's §8.2 named to run next |
| 7 | a stream of single-row writers across one `REINDEX` | the cost only DD removes: a writer's longest stall and any refusal behind the `X` (BY §1.4 cut it) |

**Inside 20 minutes, and what is cut to fit**: cells 1, 2, 6 and 7 at
n = 100,000, the rest at n = 10,000, one surplus level (equal to the live
set), three interleaved repetitions; no `cores > 1`, no `group` or
`strict`, no PostgreSQL twin. If the clock runs short, cell 3 goes first
and cell 5 next, and the results file says so.

**Tests DD owes.** `tests/index_contract_test.cpp` extended: every probe
shape answers the same bytes with `indexes` on and off, before and after a
purge, and with a purge between a reader's `BEGIN` and its probe - a purge
changes no result. A reader whose snapshot predates an `UPDATE` finds its
row through the old key after a purge, and the entry goes once the reader
ends. A purged tree's entries are a superset of a rebuild's at the same
instant; on a quiescent tree - no open reader, no undecided writer, no
skipped unit - the surplus is first entries only. An `UPDATE` back to an
earlier key, racing a purge of that key's entry on another core, loses no
row (the forced interleaving of §1.2 F, and under `tsan`). Two
`PURGE INDEX` runs of one index at once on two cores leave the tree one
run would. An entry that a divide made its leaf's first between the copy
and the removal stays. A rolled-back and a purged row's entries go; an
undecided writer's stay; a unique index refuses the same duplicates after
a purge. An insert routed by a leaf whose sibling was purged lands where a
probe finds it. A dispatcher with no lock table is refused. A crash at
each point of a leaf's rewrite mounts to a tree a rebuild agrees with; a
log cut before the image leaves the entries. The sim's workload draws the
statement, and `scripts/bs/difftest.py`'s reads still match.

**Documents DD-S3 owes**, each read at the close: `index.md` IX1 (`:46-49`),
IX2 (`:78-94`), §1.1 (`:70-72`), IX4b (`:238-246`), §7 (`:359-381`), §10.1
(`:669`), §10.2, §10.3 (`:776-779`), §12 (`:943-946`), §12.2 (`:1058-1062`),
§13 (`:1124-1127`); the comments that state the no-removal premise -
`index_tree.hpp`'s "Duplicates" and "Concurrency", `index_tree.cpp:56-67`
and `:88-91`, `step_vm.cpp:1568-1571`, `index_maintain.hpp:27-33`;
`page.md` (`:130`); `cabin.md` (`:403-416`); `heap-and-tuple.md` (`:282`,
`:284`); `txn.md` §4.1 (`:633`, a third consumer); `backup.md` B13
(`:130-135`); `known-gaps.md` (`:551`, `:1427`, and DD's own section);
`manual/sql/sql.md`; `CLAUDE.md`'s row for secondary indexes.

## 6. What DD does not do

- No change to what an index is: a superset, verified at read time (IX1).
  A purge makes the surplus smaller and never makes it zero.
- No index-only scan. A purged index still has no visibility witness
  (IX7).
- **A leaf left thin stays as it is**: its room is taken by the next
  inserts that sort there. No merge, no freed leaf, no shrinking tree; a
  tree's page count falls only by a `REINDEX`. A merge is a structural
  change with a record and a reclaim gate of its own, and another order's.
- No removal on any statement's write path, and none at rollback.
- No change to `REINDEX`, to the backfill's result, or to what a row
  `PURGE` writes.
- No Cabin pruning: `cabin.md` §5's "nothing prunes" stands.
- No new record kind and no superblock bump under the proposals as made;
  DD-Q2 (b), DD-Q3 (b) and DD-Q5 (b) each cost one.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-18

Written on worktree `db-fpi-cadence-dc-for-update` from `a4d2fb3b`
(`v3.4.12-42-ga4d2fb3b`), documents only. The letter DD is CLA's proposal.
No suite was executed: the commit changes no build input. Nothing was
measured for it; §1.1's numbers are the tree's own, at the versions named.

**Reviewed by `critics-developer` before the commit.** It opened every
cited line and results file and read the design against the tree's own
arguments. Seven things it corrected in place: the rollback cite
(`txn.md:1210-1212`) and the exemption's (`:615-619`); the borrow that
grants with no lock table, at `BorrowChain` (`command_dispatcher.cpp:11774`);
the index page's epoch word, which is present and never moved, where the
order said absent; §1.2 B's dedup, which makes a row's further entries
cost a visit and no descent; the unique check's range
(`index_maintain.cpp:325-392`); and the IX4b row, whose 169 and 20 are
refusals on `0479cb29`, not runs the IX4b binary keeps. Applied from its
findings: DD-R1's byte-equality premise between the judge's entries and
the write path's, with a test in S0 and a dry run in S1; DD-R2 leaving the
one-synchronous-span exemption out and owing the `MintReadView` census;
the premise called weak, with DD-Q0 given a magnitude and S0 its control
pair, the pool cell BY named and the `REINDEX` stall cell, cell 3 marked
as unable to show a descent; DD-R4 marked `[quiet-wrong]` and the
primitive testing position 0 at removal time; a refusal by name where no
lock table exists; the record named, `FULL_PAGE_IMAGE` at `kNoTxnId`, with
DB cited; DD-R5 reworded so the rightward re-find is allowed; one attempt
defined and its `IX` bounded (DD-R10); the rules bound to the marked
options, the thin-leaf item moved to §6, the holder item and the close's
(b) cut and the items renumbered DD-Q0..Q7, BY's result told once, "a
purge changes no result" made a test; the documents and tests the review
found missing. It found to hold: the tuple-unit fence against §1.2 F, the
first-entry rule for the coverage check, that no reader keeps a position
in a leaf, and the latch order. **Rejected**: none.

### A note added 2026-11-20

In the main checkout on `main` at `9c7d3b2e`, documents only, on the
operator's word (`raft-marks-2026-11-20.md` §16): §0a, DN's consumer and
the eight things DD-S0 reads again with it. No DD-Q marked, no rule or
stage changed, nothing run.

