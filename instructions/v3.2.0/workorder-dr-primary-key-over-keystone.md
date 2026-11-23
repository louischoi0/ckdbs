# Work order DR - a primary key of any type and column count over an unchanged Keystone: the declared key as a name the engine resolves to the row's identity, priced on every relation before anything is built

Status: **DRAFT, WRITTEN 2026-11-23, not opened.** Written in the main
checkout on `main` at `5fdca9e0` (`v3.4.17-127-g5fdca9e0` by `origin`'s
tags; `v3.5.0` is a local tag and is not on `origin`), documents only, on
the operator's words of 2026-11-23 (`raft-marks-2026-11-23.md` §10).
**Exploratory and "the Keystone left as it is" are the operator's words**
(DR-Q0, DR-R1). The letter DR, every candidate, every rule and every stage
are CLA's proposals. **DR-Q1..Q10 are unmarked**: CLA put Q1, Q2 and Q3 to
the operator before writing and the operator answered with the word to
write the draft, which marks none of them. Every `[source-read]` claim in
§1 was read at `5a7dfb92`; the two commits from there to `5fdca9e0`
change records and `known-gaps.md` only; §1.5's `known-gaps.md` line is
cited as it stands at `5fdca9e0`.

**Exploratory, and the census and the measurement come first.** DR-S0 can
close the order with nothing built, and that is a result, not a failure.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` where a wrong choice turns a
correct answer into a wrong one with no error.

Related: `instructions/v3.2.0/workorder-dn-heap-rowid.md` (the heap whose
declared primary key is already a unique index beside a hidden Keystone:
DN-D3, DN-D5, DN-Q5, DN-Q6, DN-Q13);
`instructions/v3.2.0/workorder-bx-unique-index.md` (the unique check a
primary key is made of); `instructions/v3.2.0/workorder-ch-tuple-key-update.md`
(a tuple key change in one transaction, written, not opened);
`instructions/v3.2.0/workorder-cg-on-conflict-do-update.md` (the arbiter);
`docs/rules/keystoneid-invariant.md` (K1 to K5);
`docs/spec/heap-and-tuple.md` §3.1b; `docs/spec/index.md` §5, §6, §10.2;
`docs/spec/foreign-keys.md` F1.

## 0. What DR is

`[operator]` *"An exploratory milestone approach that leaves the Keystone
as it is and can express a primary key with several types and with
multiple columns."* and, after CLA's reading of the tree and its three
questions, *"Write the draft work order and push."* (given in Korean;
recorded in English.)

`[design]` What CLA told the operator, and what DR takes from it:

1. **The frame already exists on one relation kind.** Since DN a heap's
   declared primary key that is not a single `int64` is a unique index
   beside an engine-issued hidden Keystone (§1.2). The row's identity is
   the Keystone; the primary key is a name the engine resolves to it. DR
   does not invent a second frame. It asks how far that one reaches.
2. **Three directions were named, and one is the trunk.**
   - **A, a logical primary key**: a unique index maps the key to the
     Keystone, and every structure that finds a row by its Keystone is
     untouched. This is the trunk.
   - **B, an embedded primary key**: a key whose whole domain fits the
     Keystone is the Keystone. A candidate on paper only (P4).
   - **C, a relation clustered by its primary key**: the tuple key and
     `min_key` become a byte string. It changes the Keystone, so the
     operator's premise excludes it (§6).
3. **What A costs is known in kind and not in size.** On a heap a key
   read is the index descent and one page fetch. On a btree it would be
   the index descent and the clustered descent. A range by primary key
   walks an index and not the rows. Nothing here is measured.
4. **A declared primary key is not yet a key at every site.** It cannot be
   a foreign key's target, a multi-column one cannot arbitrate an
   `ON CONFLICT`, and a btree refuses the keyword (§1.3, §1.5).
5. **One consequence may answer an older question.** With the identity
   held by the Keystone, changing a primary key's value moves a unique
   key and changes no identity. That is the demand CH was written for,
   reached with no exception to K1 or K2 (P3, DR-Q1, DR-Q8).

What DR is not: a change to the Keystone, to the tuple header, to
`min_key` or to the clustered tree's key (§6).

## 1. The tree as read `[source-read]` at `5a7dfb92`

### 1.1 The Keystone and what rests on it

- K1: a Keystone id "is bound to at most one committed tuple in the
  lifetime of a relation", `PURGE` the one named exception
  (`docs/rules/keystoneid-invariant.md:20-22`). K2: it "never changes after
  insert", and an `UPDATE` that targets it is `Unsupported`
  (`keystoneid-invariant.md:39-41`).
- A foreign key references the parent's Keystone id: the child's column
  holds it (`docs/spec/foreign-keys.md:15`, F1).
- A btree relation's tuple key is its first column, written with no
  keyword (`src/server/command_dispatcher.cpp:6663-6667`, the refusal's own
  text).

### 1.2 What a heap already has (DN, superblock 39)

- "A single `int64` `PRIMARY KEY` declared first is the Keystone"; any
  other primary key - several columns, another type, or none - "leaves the
  Keystone the engine's hidden column 0", and "a declared primary key that
  is not the Keystone is a unique index beside it"
  (`docs/spec/heap-and-tuple.md:46`).
- `CREATE TABLE` builds the Keystone index `$keystone_<oid>`
  (`kIndexFlagUnique | kIndexFlagKeystone`) and then, for a primary key
  that is not the Keystone, `$pkey_<oid>` with `kIndexFlagUnique` alone,
  both in the relation's own transaction
  (`src/server/command_dispatcher.cpp:6859-6868`). So in the catalog a
  primary key's index differs from any unique index by its name only.
- A primary key column declared `NULL` is refused at its byte
  (`command_dispatcher.cpp:6682-6688`); a second primary key is refused
  (`:6636-6640`); a single `int64` primary key not declared first is
  refused (`:6675-6680`).
- Every index entry of a heap carries the row's rowid after its tuple
  key, and a probe "fetches that page instead of descending"; the rowid
  "is resolved, never trusted" (`heap-and-tuple.md:48`, `docs/spec/index.md:46-47`).
- `keyidentity` reads the Keystone by name on every relation, and an
  `INSERT` naming it names the key (`heap-and-tuple.md:52`).

### 1.3 What a btree refuses

- `PRIMARY KEY` on a btree is `NotImplemented`: "PRIMARY KEY is a heap's
  ... a btree relation's tuple key is its first column, with no keyword"
  (`command_dispatcher.cpp:6663-6667`).
- A secondary index entry of a btree relation names the tuple key, and
  resolving it descends the clustered tree (`index.md:186-187`).
- BZ's heap, which DN replaced, was "a btree relation whose tuple key is
  hidden" (`workorder-dn-heap-rowid.md:99`, quoting the spec DN replaced). So a btree with a hidden
  Keystone is a form the tree has carried before.

### 1.4 What a primary key is made of: the unique index (BX)

- A key is one order-preserving byte string compared with `memcmp`:
  `int32`/`date` 4 bytes, `int64`/`timestamp`/`decimal` 8, `uint64` 8,
  `decimal128` 16, `char`/`varchar` a fixed prefix, each column preceded by
  one discriminator byte (`index.md:332-351`, IX5).
- At most four key columns (`include/kds/catalog/rows.hpp:463`,
  `kMaxIndexKeyColumns = 4`).
- A string contributes its first 32 bytes, zero-padded
  (`include/kds/exec/index_key.hpp:85`, `kIndexStringKeyBytes = 32`;
  `index.md:369`, IX6). The unique check therefore walks the key's whole
  run and "compares **full values** - never the encoded key alone"
  (`index.md:867-870`, BX-R2).
- A nullable key column is refused (`index.md:349`, `:901-902`, BX-Q5 (a)).
- The check runs per row, immediately, so `UPDATE t SET k = k + 1` over
  consecutive keys is refused where the standard admits it
  (`index.md:899-902`, BX-Q4 (a)).
- An `UPDATE` that moves a unique key is checked at the row and only a
  write that moves a key column is checked (`index.md:879-882`, BX-R4,
  BX-R5).

### 1.5 Where a declared primary key is not yet a key

- **A foreign key's parent.** "A heap is a parent only where its Keystone
  is a declared single `int64` primary key; a heap with another key, or
  none, is refused as a parent by name" (`foreign-keys.md:25-27`, DN-Q5
  (a)). The refusal's stated reason is that the tuple key is hidden, "so no
  child value could name a parent row" (`src/catalog/foreign_key.cpp:21-27`),
  a comment that predates `keyidentity`.
- **`ON CONFLICT`.** The arbiter is the tuple key or "the one column of a
  unique index": the search admits `ix.unique && ix.nkeys == 1` only
  (`command_dispatcher.cpp:8300-8303`, `:8305-8311`). A multi-column
  primary key cannot arbitrate.
- **`PURGE`.** A heap whose Keystone is its declared primary key is purged
  by Keystone; the hidden Keystone's heap is refused
  (`command_dispatcher.cpp:16274-16282`, DN-Q6 (a)). A hidden-Keystone
  heap's tombstones are retired by its own slot release
  (`heap-and-tuple.md` §3.1b), so a purge by primary key has no stated
  consumer.
- **The simulation.** "The sim harness generates no hidden-Keystone heap
  ... because the oracle reconciles rows by tuple key"
  (`docs/inflight/known-gaps.md:1763-1769`, at `5fdca9e0`). A relation with a declared
  non-Keystone primary key has never met a seeded crash workload.

### 1.6 What CLA did not read

Named so DR-S0 reads them and no one takes them from this section:

- which access a statement that names every primary key column by
  equality is given, and whether it is the same on every path that
  chooses an index (IX9);
- whether an `UPDATE` of a `$pkey_<oid>` column on a heap takes BX's key
  move and nothing else. A search of `src/` and `include/` for the
  primary key's spellings found no site that refuses it, which is not the
  same as having read the path;
- `RETURNING`, the KWP load stream's field 0, CU's value list and CV's
  ordered walk over a `$pkey_<oid>` index;
- the range of an `int64` named key against the 48-bit Keystone (K4),
  which P4 depends on.

## 2. The candidates `[design]`

None is built, none is measured, and DR-S0 can strike any of them.

- **P0 - nothing: the heap's form is the answer.** A relation whose key
  is not a single `int64` is declared `HEAP`, and DR closes by saying so
  in the specs and the manual. P0 is the baseline every other candidate
  is measured against, and it is a legitimate close.
- **P1 - a btree admits `PRIMARY KEY` by the heap's rule.** A btree with a
  non-Keystone primary key carries a hidden Keystone at column 0 and a
  `$pkey_<oid>` unique index, the form BZ's heap had (§1.3). A key read
  pays two descents where the heap pays one and a fetch. What the btree
  would give in return is unstated today; DR-S0 must name a consumer or
  P1 is struck (DR-Q2).
- **P2 - the primary key as a key at the sites of §1.5.**
  - **P2a - a foreign key to a hidden-Keystone heap, the child holding
    the parent's `keyidentity`.** F1 is unchanged: the child's column
    holds a Keystone. What changes is that the refusal's reason no longer
    holds, since a client reads the value by name. No new check, no new
    structure.
  - **P2b - a foreign key by primary key value.** The child's columns
    hold the parent's key values and the check is a probe of
    `$pkey_<oid>` under F1's latest-state rule. This is a new foreign key
    form, and it is sound only while a parent's key value cannot move
    under a child (DR-R3, DR-Q1). Its own order if it is taken (DR-Q4).
  - **P2c - a multi-column arbiter.** `ON CONFLICT (a, b)` names a unique
    index by its whole column list.
  - **P2d - `PURGE` by primary key.** No consumer is known (§1.5).
    Proposed not built (DR-Q6).
- **P3 - a primary key's value may change.** `UPDATE t SET <pk column> =
  ...` is BX's key move: the entry under the old value is superseded, the
  new value is checked, and the row keeps its Keystone, its rowid, its
  undo chain and every reference held by Keystone. K1 and K2 are not
  touched. If DR-S0 finds the path already does this on a heap, P3 is a
  statement in the spec and a test, not a build. **P3 and P2b cannot both
  be taken as written** (DR-R3).
- **P4 - an embedded primary key (direction B), on paper only.** A
  single-column key of a type whose every value maps one-to-one and
  order-preserving into the Keystone's named-key range is the Keystone,
  as a single `int64` is since DN-D5: no second index and one descent.
  It makes such a key immutable (K2) where P3 makes every other key
  movable, which is two behaviours under one keyword. DR writes the
  trade and builds nothing (DR-Q7).
- **P5 - a place hint in a btree's `$pkey_<oid>` entry.** Only if P1 is
  taken: page, epoch and slot beside the tuple key, verified as a Cabin's
  hint is (`heap-and-tuple.md` §3.1a's pairing rule), the clustered
  descent the fall-through. An entry format change, so DR-R5 governs it.
- **P6 - the limits.** Four columns, a 32-byte string prefix, no `NULL`,
  a per-row immediate check. Either they are a primary key's documented
  constraints, or one is lifted. A longer prefix or a fifth column is a
  catalog row and entry format event. Proposed documented (DR-Q3).

**Rejected on paper.**

- **A hash of the key as the Keystone.** Two keys can share a hash, which
  K1 cannot admit, and a hash keeps no order.
- **Clustering by the primary key** (direction C): §6.

## 3. Rules

Binding from DR-S2 on, if the order reaches it. DR-R1 binds from the
first line of DR-S0.

- **DR-R1 `[operator]` `[quiet-wrong]` - the Keystone is left as it is.**
  K1 to K5 stand with no new exception. No candidate changes the tuple
  header, the Keystone's width, `min_key`, the clustered tree's key or
  what an index entry names a row by.
- **DR-R2 `[quiet-wrong]` - a row's identity to the engine is its
  Keystone, on every relation.** Lock units, undo, index entries, Cabin
  entries, Waystone trails and foreign keys name a row by Keystone. A
  primary key is resolved to a Keystone once, at the statement's edge,
  and never stored in the Keystone's place by an engine structure.
- **DR-R3 `[quiet-wrong]` - no stored reference by a value that can
  move.** A structure or a child row may hold a primary key value only
  where that value cannot change under it. So either a primary key is
  immutable and P2b is admissible, or it is mutable (P3) and every stored
  reference is a Keystone (P2a). DR-Q1 chooses; nothing is built on both.
- **DR-R4 `[quiet-wrong]` - a primary key's uniqueness is decided on full
  values under BX's check, never on the encoded key.** A string key
  longer than its prefix is the case (IX6, BX-R2). No candidate adds a
  second uniqueness path beside `kIndexKey` and the latest-state check.
- **DR-R5 - no format event without the operator's word.** A superblock
  move, a `sys.indexes` row change or an entry layout change is put to
  the operator as its own item before it is built.
- **DR-R6 - measurement first, and no constant without a measured or
  explicitly provisional value.** A bar in §4 that names no number is set
  at DR-S1 from DR-S0's results file.
- **DR-R7 - a refusal says which kind it is.** What DR leaves refused is
  `Unsupported` where the architecture cannot admit it and
  `NotImplemented` where nobody built it, each at its byte.
- **DR-R8 - the simulation meets the form before a build rests on it.**
  No candidate that changes a write path lands before the sim generates a
  relation with a non-Keystone primary key and reconciles it (DR-S2).

## 4. Items for the operator (DR-Q)

(a) is CLA's proposal in each. **Only DR-Q0 is the operator's mark. Q1 to
Q10 are unmarked.**

| | item | (a) | other |
|---|---|---|---|
| DR-Q0 | Is DR exploratory - may DR-S0 close it with nothing built? | **yes - marked, the operator's word** (`raft-marks-2026-11-23.md` §10) | |
| DR-Q1 `[quiet-wrong]` | **May a declared primary key's value change?** | **yes (P3)**: the identity is the Keystone, the change is a unique key move, and every stored reference is a Keystone. P2b is then not built | (b) immutable, as the Keystone is: P2b is admissible and CH's demand stays unanswered; (c) mutable only while no foreign key names the relation by value |
| DR-Q2 | A btree with a primary key (P1). | **decided at DR-S1 from DR-S0**: struck unless the census names a consumer the heap cannot serve and M1 to M3 show a cell where it leads. Until then the refusal stays | (b) built for symmetry, whatever the cells say; (c) struck now |
| DR-Q3 | The limits (P6). | **documented as a primary key's constraints**: four columns, a 32-byte prefix decided on full values, no `NULL`, a per-row check | (b) a longer prefix; (c) more columns; each a format event (DR-R5) |
| DR-Q4 | A foreign key to a relation whose Keystone is hidden. | **P2a**: the child holds the parent's `keyidentity`, F1 unchanged, DN-Q5's refusal lifted for it | (b) P2b, by value, as its own order and only under DR-Q1 (b) or (c); (c) neither, DN-Q5 stands |
| DR-Q5 | A multi-column `ON CONFLICT` arbiter (P2c). | **in DR if DR-S0 finds the arbiter's path needs only the column list matched**; else its own order | (b) not in DR |
| DR-Q6 | `PURGE` by primary key (P2d). | **not built**: a hidden-Keystone heap releases its own slots, and no consumer is known | (b) built |
| DR-Q7 | The embedded key (P4). | **on paper only**: the trade written into §7, nothing built | (b) built for `int32` and `date`; (c) struck |
| DR-Q8 | DR and CH. | **CH stays written and unopened**; DR-S1 records whether P3 answers CH's demand, and CH's standing is then the operator's to mark | (b) CH abandoned now; (c) CH folded into DR |
| DR-Q9 | The measurement budget. | the standing one: 20 minutes a stage, about an hour at the close, cuts named in the results file | (b) lifted for DR-S0 |
| DR-Q10 | Which relation kind the manual recommends for a non-`int64` key. | **`HEAP`, stated in the manual at the close**, unless DR-S1 takes P1 | (b) no recommendation |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DR-S0 - census and premise** | No engine code on `main`; drivers and counters live on a branch that is not landed. **Census**: §1.6's four unread paths; every site that takes "the tuple key" from a statement where a user would mean the primary key (the foreign key's two sides, the arbiter, `PURGE`, `RETURNING`, the load stream, CU, CV, the `ORDER BY` elision, `DESCRIBE` and the `sys` views); every site that reads `columns.front()` or column 0 as the user's key; what a btree with a hidden Keystone would meet that BZ's heap did not. **Measurement**: M1 to M5 below, `build-release`, inside the budget. | a results file; each candidate go, struck or on paper |
| **DR-S1 - the verdict, on paper** | Which candidates go, against P0. DR-Q1 to Q10 put to the operator with the numbers beside them; any format event named apart (DR-R5). The amendments owed are listed by file: `heap-and-tuple.md` §3.1b, `index.md` §10.2, `foreign-keys.md` F1, `parser-v2.md`, `keystoneid-invariant.md`, the manual. | the operator's marks, or the close with nothing built |
| **DR-S2 - the sim learns the form** | The workload generates a heap with a non-Keystone primary key and the oracle reconciles by it (DR-R8); `known-gaps.md`'s entry is closed or narrowed. Runs before any build stage, and stands on its own if DR closes after it. | a branch at the push gate |
| **DR-S3 - what was taken** | One stage a candidate, in this order where taken: P3, P2a, P2c, P1 (with P5 only on its own word). Review and the full suite each; the contract tests of every structure that resolves a row (DR-R2). | a branch at the push gate, a stage each |
| **DR-S4 - the close** | Interleaved A/B over what was built against DR-S0's cells; the specs and the manual amended; the row flipped. | closed, or closed narrow |

**DR-S0's cells.** Each on a heap with a declared `int64` Keystone, as the
reference, beside a heap with a hidden Keystone and the key under test:
`int32`; `(int32, int32)`; `varchar(64)` whose values differ inside the
first 32 bytes; four columns of mixed type. A btree relation read by its
tuple key stands beside them as the second reference.

- **M1** - a point read naming the whole key: latency and throughput,
  with fetches and descents counted, not inferred.
- **M2** - an insert: the primary key's check and entry against a
  relation with none, at `cores` 1 and the largest clean cell, on keys
  that never collide and on keys two sessions race for.
- **M3** - a range and an `ORDER BY <key> LIMIT n` over the key (CV's
  walk over `$pkey_<oid>`), against the same over a btree's tuple key.
- **M4** - an `UPDATE` that moves the key against one that moves a column
  no index covers (P3's price, if the path exists).
- **M5** - the prefix: `varchar` keys sharing their first 32 bytes, run
  lengths 1, 16, 256 and 4,096, the check's and the read's cost by run
  length. The lengths are provisional and are DR-S0's to cut.

## 6. What DR does not do

- No change to the Keystone: its width, its issue, K1 to K5, the tuple
  header, `min_key`, the clustered tree's key or its page format.
- No relation clustered by its primary key, and no variable-width key.
- No index-only scan, no nullable key column, no deferred uniqueness
  check.
- No `ON UPDATE` or `ON DELETE` action beyond F2's RESTRICT.
- No primary key added to, dropped from or changed on an existing
  relation: the key is `CREATE TABLE`'s.
- No change to which relation kind `CREATE TABLE` defaults to.
- No tag and no version.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-23

Written in the main checkout on `main` at `5fdca9e0`
(`v3.4.17-127-g5fdca9e0` by `origin`'s tags), documents only, as a draft.
The exploratory standing and the premise that the Keystone is left as it
is are the operator's words (DR-Q0, DR-R1); the letter and everything
else are CLA's proposals, DR-Q1..Q10 unmarked. §1 is a reading of the
specs and of the sites they name, not a census: §1.6 lists what was not
read. Nothing was built or measured. No review was run and no suite was
executed: the commit changes no build input.
