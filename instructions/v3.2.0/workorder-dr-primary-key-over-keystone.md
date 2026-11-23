# Work order DR - a primary key of any type and column count over an unchanged Keystone: the declared key as a name the engine resolves to the row's identity, priced on every relation before anything is built

Status: **OPENED 2026-11-23 on the operator's standing go-ahead**
(`/go-ahead-achieving-milestone DR`, `raft-marks-2026-11-23.md` §14), at
`e9df68d0` (`v3.5.0-114-ge9df68d02`), on worktree `dq-cabin-miss-build`,
branch `worktree-dr-primary-key`. **DR-S0, DR-S0b and DR-S0c are run**
(§1.8, `bench/v3.5.0/results-dr-s0-premise-v3.5.0-114-ge9df68d02.md`) and
**DR-S1's verdict is written** (§5a): P3, P7 (new: a write by a unique key
descends), P2a, F0's keyword, F1 and P1 go; F2 is not built (DR-Q11 (a):
its point read is 0.9 us to 1.8 us ahead of P1's and its insert 23 % to
27 % dearer, past the 10 % bar DR-S1 set). **Every DR-Q item but DR-Q0 and
DR-Q2 (the operator's marks) and DR-Q12 and DR-Q13 (not reached) is
adopted on that standing go-ahead and is CLA's proposal, not the
operator's per-item mark** (§4a). The stage log is §7. What follows is the
header as the draft carried it.

Draft status: **DRAFT, WRITTEN 2026-11-23, not opened.** Written in the main
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
cited as it stands at `5fdca9e0`. **Amended 2026-11-23 on the operator's
word** (`raft-marks-2026-11-23.md` §11), from a review of the draft and a
probe of the engine: §1.7 is new, and §1.5, §1.6, P2a, P2c, P3, DR-Q1,
DR-Q4, DR-Q5, DR-Q9 and DR-S0's measurement are amended to it. Still a
draft, still unopened, DR-Q1..Q10 still unmarked. **Amended a second time
the same day on the operator's word** (`raft-marks-2026-11-23.md` §12): a
btree carries the key too, a range over it is served, and a key of the
recommended type works as today. §2a is new, with DR-R9 to DR-R12, DR-Q11
to DR-Q15 and DR-S0c; DR-Q2 is the operator's mark, and DR-Q7 and DR-Q10
are reworded. Every design detail of §2a is CLA's proposal.

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

`[operator]` **Later words, the same day** (given in Korean; recorded in
English), after CLA's reading of the draft and its account of encoding a
key into the Keystone id:

1. *"A btree must support a primary key of several types and several
   columns too. How can the primary key be encoded into the Keystone id so
   that a btree carries a multi-type composite primary key with no added
   step?"*
2. On CLA's account of colliding keys: *"What about identifying the
   colliding records with a hash list?"*
3. *"Without range search it means nothing. Amend the order to a scheme
   that identifies a row by (primary key values, Keystone id, collision
   flag) and supports range queries as well, the collision hash list work
   included."*
4. *"And when a primary key is created with the type KDS recommends, it
   must work as it does today, with no added overhead."*

`[design]` §2a is what CLA makes of them. The four requirements are the
operator's; the forms, the id split, the flag's place and the list's home
are CLA's.

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
  primary key cannot arbitrate. **The refusal a client meets is earlier,
  in the parser**: a conflict target of more than one column is
  `NotImplemented` at its byte (`src/parser/parser.cpp:2724`), and the
  target the parser hands on is one column (`IndexColumnRef`), so the
  dispatcher's search is never reached with two.
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
  move and nothing else. §1.7 ran it: the statement succeeds and the
  unique check holds. What is still unread is the path itself - which
  records it logs, what it leaves in the index, and whether a foreign key
  child or an assertion on the relation changes it;
- the KWP load stream's field 0, CU's value list and CV's ordered walk
  over a `$pkey_<oid>` index. `RETURNING keyidentity` is run in §1.7;
  `RETURNING` over the query layer and under `ON CONFLICT` is not;
- the range of an `int64` named key against the 48-bit Keystone (K4),
  which P4 depends on.

### 1.7 What the engine does today `[measured]`

Run 2026-11-23 against a release `kds_server` built at `7e31d272`
(`v3.5.0-101-g7e31d272b`; no file under `src/` or `include/` changed from
there to `c176c5d1`), `cores = 1`, one session, a throwaway data file. It
is a probe of behaviour, eight statements, not a test and not a
measurement of cost; the script is not in the tree. DR-S0 turns each row
into a cell.

| statement | what happened |
|---|---|
| `CREATE TABLE h (a int32, b int32, v int64, PRIMARY KEY (a, b)) HEAP`, then a duplicate `(1, 2)` | created; the duplicate refused: `unique index '$pkey_4000' on (a, b): (1, 2) is already held by tuple key 2` |
| `UPDATE h SET b = 7 WHERE a = 1 AND b = 1` | **`UPDATED 1`: a primary key's value moves today** |
| the same row moved onto the live key `(1, 2)` | refused by the same unique check |
| `SELECT v FROM h WHERE a = 1 AND b = 2` under `ANALYZE` | `IndexProbe`, `pages=2 index_scanned=1 index_resolved=1` |
| `INSERT ... RETURNING keyidentity`; `SELECT keyidentity ... WHERE a = 1 AND b = 2` | the issued id returned (1), and read back by key (2); `RETURNING *` returns the three declared columns |
| `INSERT ... ON CONFLICT (a, b) DO NOTHING` | refused by the parser: `a conflict target of more than one column is not supported` |
| `CREATE TABLE ch (id int64 PRIMARY KEY, p int64 REFERENCES h) HEAP` | refused: the parent's tuple key is hidden (§1.5's text) |
| `PRIMARY KEY (a, b)` on a `BTREE` | refused (§1.3's text) |
| two `varchar(64)` keys sharing their first 32 bytes; then one of them again | both inserted; the true duplicate refused on the full value |

What follows for the candidates:

- **P3 is not a build on a heap.** The move works and is checked. What
  DR can add is the contract and its cells, or a refusal (DR-Q1).
- **P2a's premise holds.** A client can learn a hidden Keystone at the
  insert (`RETURNING keyidentity`) and by key afterwards, which the
  refusal's comment says no client can.
- **P2c starts in the parser**, not at the arbiter's index search.

### 1.8 What DR-S0 read and measured

At `e9df68d0` on worktree `dq-cabin-miss-build`. `[source-read]` items
were read by three census readers this session ran and, where a line is
cited for a decision below, by CLA itself (`WriteKeyOf`, `WriteWalk`,
`DeclaredWriteBorrow`, `WalkHeapPages`, `CheckUniqueKey`,
`IndexChoiceOver`, `CREATE TABLE`'s key block, `foreign_key.cpp`, the
index flags). `cd` is `src/server/command_dispatcher.cpp`, `sc` is
`src/exec/step_compiler.cpp`. `[measured]` items are the results file's.

**§1.6's four paths.**

1. **The access a whole-key equality is given.** A read takes
   `IndexProbe` on every path that chooses an index: the ladder
   (`sc:3407-3640`) asks `LiteralIndexChoiceOf` and scores
   `eq * 4 + nolist * 2 + ranged` (`sc:1294`); the query layer pushes a
   relation's conjuncts into the native walk and gets the same ladder
   (`src/exec/query_layer.cpp:941-960`). **Uniqueness is read by nothing in
   compile or execution** but one cost estimate
   (`src/exec/join_order.cpp:211-225`). **A join key enters an index by
   its leading column only** (`CorrelatedIndexProbeOf`, `sc:1633`,
   `eq_prefix = 1` at `sc:1659`): a join on a composite key probes the
   first column and filters the rest. **A write has no index arm at
   all**: `UPDATE`, `DELETE` and `SELECT ... FOR UPDATE` take a tuple key
   (`WriteKeyOf`, `sc:2587`), a list on it (`sc:2597`), or a walk of the
   relation (`cd:13557-13630`, `cd:13782-13812`, `cd:16921-16965`).
   `[measured]` M4: 6,488 us to 9,554 us a statement at 100,000 rows
   against 31.5 us by the Keystone.
2. **An `UPDATE` that moves a `$pkey_<oid>` column.** The row is claimed
   `U`; `UniqueKeysOf` collects the keys the SET moved; the walk stops and
   releases its leaf; the check takes the `kIndexKey` unit and compares
   full values at latest state (`cd:14928`, `src/exec/index_maintain.cpp`
   `CheckUniqueKey`); the row is overwritten in place under `kUndoWrite`;
   `MaintainIndexes` appends a new entry, logged `kIndexInsert` (or one
   `BTREE_SPLIT`); the row is logged `kHeapOverwrite` last. **The old
   entry is neither superseded nor marked and no record covers it**: a
   reader's residual drops it and the unique check passes it over. It is
   removed only by the heap's slot release, whose index purge runs only
   when tombstones were collected (`src/exec/heap_release.cpp:171-207`),
   and `PURGE INDEX` cannot name `$pkey_<oid>` (`src/parser/parser.cpp:158`, `ParseIdent`).
   So a relation whose keys move and whose rows are never deleted keeps
   every old entry. No foreign key child can exist under it today
   (§1.5), and `UPDATE` reads no `fkeys_in`.
3. **The load stream, CU, CV and `RETURNING`.** The KWP load's field 0 is
   the hidden Keystone, NULL asking for an issued id, and each loaded row
   goes through `InsertOneRow`, so the `$pkey_<oid>` check runs per row
   (`src/server/kwp_load_server.cpp:504-545`, `cd:8760-8766`). A value
   list on a key column is an index key set for a read and never for a
   write (`sc:1254-1265`). CV's ordered walk applies to `$pkey_<oid>`
   under its own rules: a `LIMIT`, one direction, no string key, the
   driving step, and with no equality prefix only a step with no conjunct
   (`sc:1428-1503`). `RETURNING` is one path for every write
   (`cd:11860`), `keyidentity` resolving to column 0. `ON CONFLICT
   (keyidentity)` on a hidden-Keystone heap reaches the tuple key arbiter,
   which a row that names no key never fires: unverified, and a cell's.
4. **A named key against the 48-bit Keystone.** `KeystoneIdOf` refuses a
   value below 1 or above `2^48 - 1` (`cd:6007-6014`), `OutOfRange` at
   `cd:8709-8716`, for every relation kind; a hidden-Keystone heap also
   refuses one below its mark (`src/catalog/catalog.cpp:3011-3014`).

**Where "the tuple key" is taken and a user would mean the primary key.**
B is a btree, D a declared-Keystone heap, H a hidden-Keystone heap with
`$pkey_<oid>`.

| site | B | D | H |
|---|---|---|---|
| a foreign key's parent (`src/catalog/foreign_key.cpp:21-28`) | parent | parent | refused `Unsupported` |
| `ON CONFLICT` target (`src/parser/parser.cpp:2716-2727`, `cd:8283-8312`) | the tuple key, or one column of a unique index | the same | one column of a unique index; no target with `DO UPDATE` is refused |
| `PURGE` (`cd:16241-16295`) | by first column | by `id` | refused `Unsupported` |
| a tuple key range, the `ORDER BY` elision, the reverse walk (`sc:3505`, `sc:3783-3798`) | served | never (a heap is in no key order) | never |
| `DESCRIBE`, `sys.columns` (`src/server/sys_views.cpp:490-610`) | the first row `tuple_key=1` | the same | no row is the tuple key, and **nothing shows which columns are the primary key** |
| `sys.indexes` | - | `$keystone_<oid>` | `$keystone_<oid>` and `$pkey_<oid>`, an ordinary `unique` row |
| the KWP load's field 0 | the first column | `id` | the hidden Keystone |

**A primary key is stored nowhere but as its index.** `$pkey_<oid>`
carries `kIndexFlagUnique` alone and no code reads the name
(`cd:6866-6867`); a `$name` lexes as a parameter, so no statement names
it.

**What a btree with a hidden Keystone would meet.** The hidden column is
a column flag, `Schema::hidden_keystone()`
(`include/kds/catalog/schema.hpp:43-45`), and storage is a separate fact,
`TableAccess::heap()` (`schema.hpp:364`). `*`, `DESCRIBE`, `RETURNING *`,
the positional `INSERT`, views and `keyidentity` are keyed on the flag
(`sc:2374`, `cd:7431`, `cd:11951`, `cd:5758`, `cd:5924`,
`src/catalog/schema.cpp:33-51`). `CREATE TABLE` adds the column and
builds the indexes under `stmt.heap` (`cd:6696-6698`, `cd:6849-6874`); a
btree needs no Keystone index, since `LocateRow` descends the tree
(`src/exec/heap_store.cpp:170`). Keyed on both facts: the refusal of a
named key below the mark (`cd:8933`) and the tombstone rule
(`src/exec/index_ddl.cpp:1137`). CU, CX's access pins, DC's locking read,
CV, DN and DP landed after BZ's heap was a btree and have not met one.

**The issue cursor.** `sys.tables.next_id` is one mark:
`AdmitExplicitRowId` moves it to `id + 1` for a named id at or above it
(`src/catalog/catalog.cpp:3026`), and an issued id is assumed above every
placed one (`include/kds/catalog/catalog.hpp:806-810`). `[measured]`: a
btree row named `2^46 - 1` made the next issued id `2^46`. A hashed home
would drag the mark unless the mark started at `2^47`.

**The reserved bytes.** `SysTableRow` keeps a reserved word at offset 97
(4 bytes, written 0, no member) and `retired_key_order` at 101 (1 byte),
read by nothing but `Encode`/`Decode` and three test lines
(`include/kds/catalog/rows.hpp:85-181`, `src/catalog/rows.cpp:48-77`).
Every index flag carries "written since superblock N, so no binary that
ignores the bit can mount a volume carrying it" (`rows.hpp:470-484`).

**No hash over column values is persisted** anywhere, and
`index_key_test.cpp` pins order, not bytes.

**The sim.** The generator writes btrees and declared-Keystone heaps
only (`sim/workload.cpp:87-95`, `:424-427`); the oracle is a map by tuple
key read back as `id, v, name` (`sim/oracle.hpp:64`, `sim/loop.cpp:1058-1100`).

**Found beside the census, not DR's.** A list write (CU) on a heap whose
descent declines mid-list leaves a cursor with a key and no place, and
`WalkHeapPages` then starts at no page and writes none of the remaining
rows (`cd` `WriteKeySet`, `WalkHeapPages`). Reached only where a descent
fails for a reason other than the key's absence - a damaged Keystone
index, an unreadable page - and then the statement answers a count short
of its rows. Fixed with P7 at DR-S3a, whose loop is the same loop.

**What was measured** (the results file; 100,000 rows unless said):

- M1: a read by primary key 25.9 us to 30.5 us against 25.3 us by the
  Keystone.
- M4: a write by primary key 6,488 us to 9,554 us against 31.5 us; a
  move costs the same, the walk hiding it.
- M2: the key's index costs an insert 4.5 us to 5.8 us a row.
- M3: an inclusive range is served in every form (90.0 us to 99.1 us);
  a page, `key >= x ORDER BY key LIMIT n`, is a walk on every structure,
  the btree's tuple key included; a string key's order keeps its sort.
- M5: keys sharing 32 bytes cost a read 0.25 us and an insert 0.66 us a
  row of the run.
- M6 to M9: F2's point read is 0.9 us ahead of P1's at 100,000 rows and
  1.8 us at 1,000,000, by the client's p50 (3 % and 6 % of the
  statement); its insert is 23 % and 27 % dearer (+1.83 us and +2.48 us a
  row), its data file 26 % and 28 % larger, its scans 1.35x to 1.46x at
  1,000,000 rows. The break-even is 2.0 and 1.4 keyed reads of a row an
  insert.

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
  P1 is struck (DR-Q2). **The operator has since said a btree carries
  such a key** (§0), so P1 is no longer struck for want of a consumer. It
  is the form §2a's F2 is measured against.
- **P2 - the primary key as a key at the sites of §1.5.**
  - **P2a - a foreign key to a hidden-Keystone heap, the child holding
    the parent's `keyidentity`.** F1 is unchanged: the child's column
    holds a Keystone. What changes is that the refusal's reason no longer
    holds, since a client reads the value by name: at the parent's insert
    by `RETURNING keyidentity`, and afterwards by its key (§1.7). No new
    check, no new structure. The child's value is an engine-issued id, so
    a load that names its own parent keys cannot use this form.
  - **P2b - a foreign key by primary key value.** The child's columns
    hold the parent's key values and the check is a probe of
    `$pkey_<oid>` under F1's latest-state rule. This is a new foreign key
    form, and it is sound only while a parent's key value cannot move
    under a child (DR-R3, DR-Q1). Its own order if it is taken (DR-Q4).
  - **P2c - a multi-column arbiter.** `ON CONFLICT (a, b)` names a unique
    index by its whole column list. Three sites change, not one: the
    parser's one-column target (`parser.cpp:2724`), the dispatcher's
    search (`nkeys == 1`), and `DO UPDATE`'s path over the row the
    arbiter found (CG).
  - **P2d - `PURGE` by primary key.** No consumer is known (§1.5).
    Proposed not built (DR-Q6).
- **P3 - a primary key's value may change.** `UPDATE t SET <pk column> =
  ...` is BX's key move: the entry under the old value is superseded, the
  new value is checked, and the row keeps its Keystone, its rowid, its
  undo chain and every reference held by Keystone. K1 and K2 are not
  touched. **It already does this on a heap** (§1.7), so P3 is a
  statement in the spec and its cells, not a build - and declining it
  (DR-Q1 (b)) is the build: a new refusal of a statement that succeeds
  today. **P3 and P2b cannot both be taken as written** (DR-R3).
- **P4 - an embedded primary key (direction B), on paper only.** A
  single-column key of a type whose every value maps one-to-one and
  order-preserving into the Keystone's named-key range is the Keystone,
  as a single `int64` is since DN-D5: no second index and one descent.
  It makes such a key immutable (K2) where P3 makes every other key
  movable, which is two behaviours under one keyword. **Taken up as
  §2a's F1**, for the keys it fits (DR-Q7).
- **P5 - a place hint in a btree's `$pkey_<oid>` entry.** Only if P1 or
  §2a's F2 is taken: page, epoch and slot beside the tuple key, verified as a Cabin's
  hint is (`heap-and-tuple.md` §3.1a's pairing rule), the clustered
  descent the fall-through. An entry format change, so DR-R5 governs it.
- **P6 - the limits.** Four columns, a 32-byte string prefix, no `NULL`,
  a per-row immediate check. Either they are a primary key's documented
  constraints, or one is lifted. A longer prefix or a fifth column is a
  catalog row and entry format event. Proposed documented (DR-Q3).

- **P7 - a write by a unique key descends** (new at DR-S0, §1.8 item 1).
  Where an `UPDATE`'s, a `DELETE`'s or a locking read's WHERE pins every
  key column of a unique index by an equality with a literal, the rows it
  can write are the ones that key's run names. The run is collected as
  the unique check collects it, and the write's point arm is offered each
  tuple key in ascending order, every row tested against the whole WHERE
  where it is found (CU-R1). It takes the units the walk of the same
  WHERE takes: a WHERE with no tuple key bound declares no window
  (`DeclaredWriteBorrow`), so each row written takes its row unit either
  way. Without it a primary key that is not the Keystone is a key a row
  is read by and not written by. It serves every unique index, a primary
  key's included.
- **P8 - a page by key** (new at DR-S0, M3): `key >= x ORDER BY key LIMIT
  n` served by a seek and an ordered walk. It is unserved on every
  structure, the tuple key included, so it is the access ladder's and not
  a primary key's. Its own order (DR-Q17).

**Rejected on paper.**

- **A hash of the key as the Keystone.** Two keys can share a hash, which
  K1 cannot admit, and a hash keeps no order.
- **Clustering by the primary key** (direction C): §6.

## 2a. The btree's primary key `[operator]` direction, `[design]` detail

The operator's words after the first amendment (§0) ask for four things:
a btree carries a primary key of several types and columns; reading a row
by it adds no step; a range over it is served; and a key of the
recommended type works exactly as today. This section is CLA's design to
those words. Nothing in it is built or measured.

### The idea in one paragraph

The Keystone stays what it is, and the engine chooses its **value** from
the declared key. A row is named by the triple **(primary key values,
Keystone id, collision flag)**. The primary key values, in order, are the
key of one ordered unique index, and that index is what a range walks.
The Keystone id is where the row sits in the clustered tree, chosen so
that a point read can go straight to it. The collision flag says the row
could not take the id its key maps to. The index is the authority for
which rows exist; the mapped id is a fast path that is always checked.

### Three forms, fixed at `CREATE TABLE`

The form is a fact of the catalog, decided once from the declaration and
read at compile time. No statement asks at run time which form a relation
has.

| form | declared key | Keystone id | extra structure | point read | range and `ORDER BY` |
|---|---|---|---|---|---|
| **F0 - the recommended key** | one `int64`, the first column (today's btree key, with or without the keyword) | the value itself | none | one descent, as today | tuple key range, as today |
| **F1 - embedded** | columns whose order-preserving codes total 48 bits or less | the code plus 1 | none | one descent | tuple key range: leaf order is key order |
| **F2 - hashed home** | anything else within BX's limits (§1.4) | `hash47` of the key, or an issued id where that is taken | `$pkey_<oid>`, the ordered unique index | one descent on a hit | an ordered walk of `$pkey_<oid>` |

**F0 is today's relation, byte for byte.** It has no index it did not
have, no hash, and no flag, and its statements compile to the chains they
compile to today (DR-R9). `PRIMARY KEY` naming that one `int64` first
column on a btree is admitted as a spelling of what the relation already
is.

**F1** is P4 taken up. The code is IX5's per-column encoding without the
discriminator byte, columns concatenated in declared order. It fits, for
example, `int32`, `date`, `(int32, int16)`, `(date, int16)` and `char(5)`.
It adds 1 because id 0 is not an id. A form whose codes fill all 48 bits
has one combination too many, and that one value is refused `OutOfRange`
by name.

### F2 in detail

- **The id space is split.** Home ids are `[1, 2^47)` and issued ids are
  `[2^47, 2^48)`. `hash47` maps a key's full values (never a prefix) to a
  home. An issued id can then never meet a later key's home.
- **The collision flag is the id's top bit.** A row whose home was taken
  is given an issued id, so `id >= 2^47` is the flag. Nothing is stored
  for it: no bit of the Keystone word's `flags`, and no byte in an index
  entry. `$pkey_<oid>`'s entry is today's entry, the key and the tuple key
  it names.
- **The collision list is the flagged part of `$pkey_<oid>`.** A separate
  list would be a second structure answering the question the index
  already answers. The index holds every row, as it must for a range, and
  its entries whose tuple key is at or above `2^47` are the colliding
  rows. The expected number is about N^2 / 2^48: 36 pairs at 10^8 rows,
  3,600 at 10^9.
- **An insert**: compute the home; run BX's unique check on the full key
  under its `kIndexKey` unit; place the row at the home, or, where the
  clustered tree answers that the id is bound, at an issued id; add the
  entry. That is one index descent and one clustered descent, the same as
  any relation with one unique index.
- **A point read naming the whole key**: descend to the home.
  - The row there is visible and its key columns equal the statement's:
    it is the answer, in one descent.
  - Anything else - no row, a row with another key, a row this snapshot
    does not see - is decided by a probe of `$pkey_<oid>`. A colliding key,
    a key that moved, and an absent key take that second descent.
- **A range, a partial key, `ORDER BY`**: an ordered walk of `$pkey_<oid>`
  (CV's walk), each entry resolved through the clustered tree as a btree
  index entry is today (`index.md:184-186`). So each row a range returns
  costs a descent. P5's place hint is what would lower that, on its own
  word.
- **A key's value may move** (P3 holds here too). The entry moves under
  BX's check and the row keeps its Keystone. The row is then off its home
  without the flag; the read's key comparison at the home is what keeps
  that correct, and the row costs two descents from then on.
- **A delete, then the same key again**: the home stays bound by the
  deleted row until a `PURGE` (K1), so the new row takes an issued id.

### What each form costs, as known today

- **F1**: a key's value cannot move (K2: the Keystone is the value), and a
  deleted key cannot be inserted again before a `PURGE` frees it, which is
  the rule a btree's named key has today.
- **F2**:
  - Inserts land across the whole tree, not at its right edge, because
    homes are hashes. That is the cost most likely to decide the form.
  - A range pays a descent a row.
  - A read of an absent key pays two descents.
  - `hash47` becomes a persisted format: a relation's ids are computed
    from it for the relation's life.
- **Both**: the relation's key form is one more catalog fact (DR-R5).

### Why not the other shapes

- **A flag stored on the home row** ("this home has overflow") would make
  an empty home a conclusive absence and save the absent key's second
  descent. It would also make `PURGE` of such a row a `[quiet-wrong]`
  hazard and use the Keystone word's `flags` for the first time. Left as
  DR-Q13's other option.
- **A window of ids beside the home** (the first shape CLA described to
  the operator) needs no index for a point read, but it has no order, a
  hard limit, and a case at every leaf boundary.
- **P1** (a hidden issued Keystone and the index, the heap's form on a
  btree) serves the same statements with inserts at the right edge and two
  descents on every point read. It is the form F2 is measured against.

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
- **DR-R9 `[operator]` - the recommended key is today's relation.** A
  relation whose key is one `int64` first column (F0) gains no structure,
  no hash, no flag and no instruction on any statement's path. Its
  statements compile to the chains they compile to today, pinned by a
  chain-identity cell, and the close measures it against the opening
  commit with a same-binary control over several server lifetimes. DQ's
  close found placement alone moves a statement by 1 % to 2 % on this
  host, so the bar is no instruction added, shown by the chains and the
  review, and a delta inside the control's range.
- **DR-R10 `[operator]` - a range over the primary key is served on every
  form**, in key order, by a structure that holds that order: the
  clustered tree on F0 and F1, `$pkey_<oid>` on F2 and on a heap.
- **DR-R11 `[quiet-wrong]` - the index is the authority, the home is a
  fast path.** On F2 a row found at its home answers a read only when it
  is visible to the snapshot and its key columns equal the statement's on
  full values. Every other outcome is decided by `$pkey_<oid>`. No read
  concludes that a key is absent from an empty home, and no write decides
  uniqueness from the home.
- **DR-R12 `[quiet-wrong]` - the key's form and its mapping never change
  for a relation.** The form is fixed at `CREATE TABLE`. F1's code and
  F2's `hash47` are persisted formats: each is pinned by vectors, hashes
  full values, and is not changed without a format event (DR-R5).

## 4. Items for the operator (DR-Q)

(a) is CLA's proposal in each. **DR-Q0 and DR-Q2 are the operator's
marks. Every other item is unmarked.**

| | item | (a) | other |
|---|---|---|---|
| DR-Q0 | Is DR exploratory - may DR-S0 close it with nothing built? | **yes - marked, the operator's word** (`raft-marks-2026-11-23.md` §10) | |
| DR-Q1 `[quiet-wrong]` | **May a declared primary key's value change?** It does today on a heap (§1.7), so the question is whether that stands as the contract | **yes (P3)**: the identity is the Keystone, the change is a unique key move, and every stored reference is a Keystone. What exists is written into the spec and given its cells. P2b is then not built | (b) immutable, as the Keystone is: a statement that succeeds today is refused from then on, P2b is admissible and CH's demand stays unanswered; (c) mutable only while no foreign key names the relation by value |
| DR-Q2 | A btree with a primary key. | **yes - marked, the operator's word** (`raft-marks-2026-11-23.md` §12): a btree carries a primary key of several types and columns, a range over it is served, and the recommended key works as today. Which form carries it is DR-Q11 and DR-Q12, unmarked | |
| DR-Q3 | The limits (P6). | **documented as a primary key's constraints**: four columns, a 32-byte prefix decided on full values, no `NULL`, a per-row check | (b) a longer prefix; (c) more columns; each a format event (DR-R5) |
| DR-Q4 | A foreign key to a relation whose Keystone is hidden. | **P2a**: the child holds the parent's `keyidentity`, F1 unchanged, DN-Q5's refusal lifted for it. Its premise, that a client can read that id, holds (§1.7); DR-S0 reads `RETURNING` on the paths §1.6 still names | (b) P2b, by value, as its own order and only under DR-Q1 (b) or (c); (c) neither, DN-Q5 stands |
| DR-Q5 | A multi-column `ON CONFLICT` arbiter (P2c). | **its own order**: the refusal is the parser's, and the change runs through the parsed target, the arbiter's search and `DO UPDATE`'s path (§1.5, §1.7), which is CG's surface and not a column list matched. DR-S0's census still names every site | (b) a stage of DR, after P3 and P2a |
| DR-Q6 | `PURGE` by primary key (P2d). | **not built**: a hidden-Keystone heap releases its own slots, and no consumer is known | (b) built |
| DR-Q7 | The embedded key (P4, §2a's F1). | **built on a btree for every key whose codes total 48 bits or less**, with its two limits stated: the value cannot move, and a deleted key waits for a `PURGE` | (b) not built: such keys take F2 like any other; (c) built for single columns only |
| DR-Q8 | DR and CH. | **CH stays written and unopened**; DR-S1 records whether P3 answers CH's demand, and CH's standing is then the operator's to mark | (b) CH abandoned now; (c) CH folded into DR |
| DR-Q9 | The measurement budget. | the standing one: 20 minutes a stage, about an hour at the close, cuts named in the results file. M1 to M5 as first written do not fit 20 minutes (five measurements over four key forms and two references; DQ-S0 spent 9 minutes on one cell design), so DR-S0 measures M1 and M4 and DR-S0b the rest, each inside its own 20 minutes | (b) lifted for DR-S0, M1 to M5 in one stage |
| DR-Q10 | What the manual recommends. | **one `int64` first column as the key, on either kind** (F0, DR-R9): it is the only form with no second structure and no limit of §2a. For another key, the kind is recommended at the close from DR-S0c's cells | (b) no recommendation |
| DR-Q11 | The form a wide key takes on a btree. | **F2, if DR-S0c's cells show its point read ahead of P1's and its insert within a bar DR-S1 sets from them**; else P1. Both serve the range by the same index | (b) F2 whatever the cells say; (c) P1 whatever the cells say |
| DR-Q12 `[quiet-wrong]` | F2's colliding rows. | **an issued id in `[2^47, 2^48)`, the flag the id's top bit, the list the flagged entries of `$pkey_<oid>`** (§2a): nothing stored for the flag and no second structure | (b) a list of its own beside the index; (c) a window of ids beside the home |
| DR-Q13 `[quiet-wrong]` | A read of an absent key on F2. | **two descents: the home, then the index** (DR-R11) | (b) a flag on the home row makes an empty home conclusive: one descent, a `PURGE` rule, and the Keystone word's `flags` used for the first time (DR-R5) |
| DR-Q14 | May a key's value change on a btree? | **F2 yes, F1 no** (K2: F1's Keystone is the value), so DR-Q1's answer holds for every form but F1, and F1's refusal is `Unsupported` | (b) no on any btree form |
| DR-Q15 | The format events §2a needs (DR-R5). | **put to the operator at DR-S1 with DR-S0's census**: the key form as a catalog fact (the `sys.tables` byte that is written 0 and read by nothing is a candidate), `hash47` and F1's code as persisted formats, and whether any of them moves the superblock | |

### 4a. Items DR-S0 raised, and every adoption `[design]`

**Adopted on the operator's standing go-ahead of 2026-11-23
(go-ahead-achieving-milestone), each CLA's proposal and none the
operator's own per-item mark** (`raft-marks-2026-11-23.md` §14, §15).
DR-Q0 and DR-Q2 stay the operator's marks.

| | adopted | note |
|---|---|---|
| DR-Q1 `[quiet-wrong]` | **(a)**: a primary key's value may change | Premise re-read at `e9df68d0` (§1.8 item 2): the move is BX's check and a new entry, the Keystone, the rowid and the undo chain are untouched, and no stored reference holds the value. It holds. Narrowed by DR-Q14: not on F1 |
| DR-Q3 | **(a)**: the limits documented | M5 prices the prefix: 0.25 us a row of a run on a read, 0.66 us on an insert |
| DR-Q4 | **(a)**: P2a | A hidden-Keystone relation is a parent, the child holding its `keyidentity` |
| DR-Q5 | **(a)**: P2c its own order | |
| DR-Q6 | **(a)**: `PURGE` by primary key not built on a heap | A btree has no slot release, so DR-Q19 and DR-Q20 answer for it |
| DR-Q7 | **(a) as revised**: F1 built for a key of `int8`, `int16`, `int32`, `date` and `bool` columns whose codes total 48 bits or less | Revised against the tree: a `char(N)` key is left to P1, its comparison against its code not having been read. Recorded as a revision |
| DR-Q8 | **(a)**: CH stays written and unopened | P3 answers CH's demand for a key that is not the Keystone and for no other; CH's standing is the operator's |
| DR-Q9 | **(a)**: the standing budget | Every run inside 20 minutes; the cuts are in the results file |
| DR-Q10 | **(a)**: the manual recommends one `int64` first column | For another key: `HEAP` for a wide key, a btree for a narrow one that is ranged or ordered by (the close) |
| DR-Q11 | **(a)**, and by the bar DR-S1 set the form is **P1** | (a)'s first condition is met: F2's point read is 0.9 us and 1.8 us ahead of P1's (3 % and 6 % of the statement). Its second is a bar DR-S1 sets, and DR-R6 wants a number: **F2's insert within 10 % of P1's server CPU a row at 1,000,000 rows**, provisional, CLA's. The ground: a form is not taken for a single-digit gain on one statement shape at a double-digit cost on another, with a persisted hash and two `[quiet-wrong]` rules (DR-R11, DR-R12) beside it. Measured: +27 % (+23 % at 100,000). **F2 is not built.** The bar was set after the cells were read, and at a break-even bar of two keyed reads an insert F2 would pass at 1,000,000 rows (1.4) and sit on the line at 100,000 (2.0): the verdict turns on the bar, and (b) stays the operator's. F2 is P1 with a home, so it can be ordered over P1 later; a relation made as P1 stays P1 (DR-R12) |
| DR-Q12, DR-Q13 `[quiet-wrong]` | **not reached** | They decide F2's details, and F2 is not built. Neither is adopted |
| DR-Q14 | **(a) as revised**: P1 yes, F1 no | F2's place is P1's. F1's refusal is `Unsupported` |
| DR-Q15 | **written out as DR-Q22 and adopted there, on the standing word alone** | (a) was to put the format events to the operator at DR-S1, and DR-R5 asks for the operator's word on a format event as its own item. The standing go-ahead is the only word DR-Q22 has: it is an adoption, never the operator's mark, it is listed first at the push gate, and nothing of it reaches `main` before the operator's word there. DR-S1's review asked that DR-S3d wait for that word; CLA did not stop, the standing word being to follow CLA's proposal where a decision is needed |

The items DR-S0 raised, (a) CLA's proposal and adopted:

| | item | (a) | other |
|---|---|---|---|
| DR-Q16 `[quiet-wrong]` | P7: does a write that names a unique key whole descend by it? | **yes, for every unique index**: the run's tuple keys through the write's point arm, the whole WHERE tested at each row, the walk's units. A row the run does not name is one a read through the same index would not find either. A descent that declines after a row was offered refuses the statement `TxnConflict` | (b) a primary key's index only; (c) not built: a write by primary key walks |
| DR-Q17 | P8, the page by key. | **its own order**: the ladder's gap on every structure. `known-gaps.md` states it | (b) a stage of DR |
| DR-Q18 | A primary key's old entries after a move (§1.8 item 2). | **`known-gaps.md`**, with the remedy named: the slot release's index purge run without a tombstone | (b) built in DR |
| DR-Q19 | `PURGE` on a btree whose Keystone is hidden (P1). | **admitted by `keyidentity` comparisons**: such a btree keeps its delete-marked rows as any btree does and has no other release; a hidden-Keystone heap stays refused | (b) refused: the rows stay |
| DR-Q20 | `PURGE` on F1. | **by the key's own comparisons**, folded to a tuple key window by the embedding, and by `keyidentity`: a deleted key is bound until then (K1) | (b) by `keyidentity` only |
| DR-Q21 | An engine-named index in `PURGE INDEX`. | **`PURGE INDEX $pkey_<oid>` is admitted**: the name `sys.indexes` shows, read where an index name is read and nowhere else. A btree's indexes are purged by nothing unasked (DD) | (b) `known-gaps.md`: a P1 btree's key entries are never purged |
| DR-Q22 | The format events (DR-R5, DR-Q15). | **One, superblock 41**: `sys.tables`' retired key-order byte becomes the key form (0 none, 1 embedded), its reserved word at offset 97 the embedded key's column positions (up to four, one byte each), and a btree may carry a hidden Keystone. F1's code is a persisted format pinned by byte vectors. No index flag, no entry layout and no `sys.indexes` row change. **No `hash47`**: F2 is not built | (b) no superblock move, an older binary misreading an embedded relation; (c) F1 not built, and no event |
| DR-Q23 | A write's use of an index in `sys.indexes`' counts. | **counted**: a run P7 collects is one probe, its entries scanned and its tuple keys resolved. `index_stats.hpp`'s "a write's walk is not counted" is amended to it | (b) not counted |
| DR-Q24 | What shows a primary key. | **`sys.columns` and `DESCRIBE` gain `primary_key`**, the column's 1-based place in the key or 0 | (b) nothing: the key is read off `sys.indexes` |

## 5a. DR-S1 - the verdict `[design]`, adopted on the standing go-ahead

Against P0, from §1.8 and the results file.

| candidate | verdict | why |
|---|---|---|
| P0, the heap's form is the answer | **not the close** | It reads well (M1) and cannot be written by its key (M4), and the operator has said a btree carries the key (DR-Q2) |
| P3, a key's value may change | **go**: the contract and its cells | It is what the engine does (§1.7); nothing is built |
| **P7, a write by a unique key descends** | **go, first** | M4: 206x to 304x. Every other candidate rests on a key a row can be written by |
| P2a, a foreign key to a hidden Keystone | **go** | The refusal's reason no longer holds (§1.7) |
| P2b, a foreign key by value | **not built** | DR-R3 under DR-Q1 (a) |
| P2c, a multi-column arbiter | **its own order** | DR-Q5 (a) |
| P2d, `PURGE` by primary key on a heap | **not built** | DR-Q6 (a) |
| F0, the keyword on a btree's `int64` first column | **go** | A spelling of what the relation is (DR-R9) |
| F1, the embedded key | **go**, integer, date and bool columns | One descent, no index, the range and the order by the tuple key |
| F2, the hashed home | **not built** | DR-Q11 (a): its point read is ahead by 3 % to 6 % of a statement and its insert is 23 % to 27 % dearer, past the 10 % bar (§4a). Not foreclosed: it is P1 with a home |
| P1, a hidden Keystone and the index on a btree | **go**, as the wide key's form | DR-Q11 (a)'s other branch |
| P5, a place hint | **not built** | On its own word only |
| P6, the limits | **documented** | DR-Q3 (a) |
| P8, a page by key | **its own order** | DR-Q17 (a) |

**DR-S3's stages, in order**: S3a P7; S3b P3's contract and cells with
DR-Q23 and DR-Q24; S3c P2a; S3d superblock 41, F0's keyword and P1 with
DR-Q19 and DR-Q21; S3e F1 with DR-Q20. DR-S2 runs before S3a (DR-R8).

**The amendments owed, by file**: `docs/spec/heap-and-tuple.md` §3.1b and
§4.1c; `docs/spec/index.md` §8, §10.2 and §2; `docs/spec/foreign-keys.md`
F1; `docs/spec/parser-v2.md` (the keyed write); `docs/spec/txn.md` §5
(the locking read's descent); `docs/spec/sys-views.md` (`primary_key`,
the counts); `docs/spec/catalog.md` and `docs/spec/page.md` (superblock
41); `docs/rules/keystoneid-invariant.md` (F1's id is a named key);
`docs/inflight/known-gaps.md`; `manual/sql/sql.md`.

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DR-S0 - census and premise** | No engine code on `main`; drivers and counters live on a branch that is not landed. **Census**: §1.6's four unread paths; every site that takes "the tuple key" from a statement where a user would mean the primary key (the foreign key's two sides, the arbiter, `PURGE`, `RETURNING`, the load stream, CU, CV, the `ORDER BY` elision, `DESCRIBE` and the `sys` views); every site that reads `columns.front()` or column 0 as the user's key; what a btree with a hidden Keystone would meet that BZ's heap did not. **Measurement**: M1 and M4 below, `build-release`, inside the budget. | a results file; each candidate go, struck or on paper, or held for DR-S0b |
| **DR-S0b - the rest of the cells, if the verdict needs them** | M2, M3 and M5, each run only where DR-S0 left a candidate undecided on it: M2 and M3 decide P1 (DR-Q2), M5 decides the prefix (DR-Q3). Inside its own 20 minutes, cut to the cells the open candidates need. Skipped, and said so, where DR-S0 struck them. | a results file, or a line in DR-S1 saying why it did not run |
| **DR-S0c - the btree form's premise, with no engine change** | §2a's F2 is priced on today's engine before any of it is built, because a client can already name a btree's tuple key: the driver computes a 47-bit hash of each key and inserts it as the named key, beside a secondary unique index on the key columns. **Census**: every site §2a touches (the compile's key rewrite, the placement's bound-id answer, the issue cursor's second range, CV's walk, the KWP load stream's field 0, `DESCRIBE` and the `sys` views, the sim). **Measurement**: M6 to M9 below, inside its own 20 minutes. | a results file; F2 against P1 for DR-Q11 |
| **DR-S1 - the verdict, on paper** | Which candidates go, against P0. DR-Q1 to Q10 put to the operator with the numbers beside them; any format event named apart (DR-R5). The amendments owed are listed by file: `heap-and-tuple.md` §3.1b, `index.md` §10.2, `foreign-keys.md` F1, `parser-v2.md`, `keystoneid-invariant.md`, the manual. | the operator's marks, or the close with nothing built |
| **DR-S2 - the sim learns the form** | The workload generates a heap with a non-Keystone primary key and the oracle reconciles by it (DR-R8); `known-gaps.md`'s entry is closed or narrowed. Runs before any build stage, and stands on its own if DR closes after it. | a branch at the push gate |
| **DR-S3 - what was taken** | One stage a candidate, in this order where taken: P3, P2a, F0's keyword on a btree, F1, then F2 or P1 (DR-Q11), with P5 only on its own word. P2c is its own order (DR-Q5). Review and the full suite each; the contract tests of every structure that resolves a row (DR-R2). | a branch at the push gate, a stage each |
| **DR-S4 - the close** | Interleaved A/B over what was built against DR-S0's cells; the specs and the manual amended; the row flipped. | closed, or closed narrow |

**DR-S0's cells.** Each on a heap with a declared `int64` Keystone, as the
reference, beside a heap with a hidden Keystone and the key under test:
`int32`; `(int32, int32)`; `varchar(64)` whose values differ inside the
first 32 bytes; four columns of mixed type. A btree relation read by its
tuple key stands beside them as the second reference.

**DR-S0 runs M1 and M4** at `cores = 1`: they price the frame itself (a
read by key, a key that moves) on every key form, and P0, P2a and P3 are
decided on them. **DR-S0b runs M2, M3 and M5** where a candidate still
needs them.

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

**DR-S0c's cells**, each on a btree at `cores = 1`, three arms: **F0** (an
`int64` key in arrival order, the reference), **F2 emulated** (the named
key is the driver's hash of a `varchar(64)` key, with a unique index on
that column), and **P1 emulated** (an issued key and the same unique
index, read through it).

- **M6** - insert throughput and server CPU at 100,000 and 1,000,000 rows:
  what hashed homes cost against the right edge, with leaf divisions
  counted.
- **M7** - a point read of a present key: F2's one descent against P1's
  two, and F0's.
- **M8** - a point read of an absent key: F2's two descents (DR-Q13).
- **M9** - a range of 100 rows and an `ORDER BY <key> LIMIT 100` over the
  index, on F2 and P1, against F0's tuple key range.

## 6. What DR does not do

- No change to the Keystone: its width, its issue, K1 to K5, the tuple
  header, `min_key`, the clustered tree's key or its page format.
- No relation clustered by its primary key, and no variable-width key.
  F1 is not one: its tree is keyed by the Keystone, whose value the key's
  code is.
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

### Amended 2026-11-23

On worktree `dq-cabin-miss-build`, branch `worktree-dr-draft-amend`, over
`c176c5d1`, on the operator's word (`raft-marks-2026-11-23.md` §11), from
a session other than the one that wrote the draft.

- **§1's citations were checked** against the tree at `5fdca9e0`: the
  nine sites read (the btree refusal, the two indexes' creation, the
  arbiter's search, `PURGE`'s refusal, `kMaxIndexKeyColumns`,
  `kIndexStringKeyBytes`, the foreign key refusal, the sim's known gap,
  the marks' §10) say what the draft says they say.
- **A probe of the engine** (§1.7) answered two of §1.6's unread paths
  and corrected one citation:
  - a primary key's value already moves on a heap, so P3 is a contract
    and its cells, and DR-Q1 (b) is the build;
  - `RETURNING keyidentity` returns a hidden Keystone, so P2a's premise
    holds;
  - a multi-column conflict target is refused by the parser, before the
    search §1.5 cited, so DR-Q5's proposal is now its own order.
- **DR-S0's measurement is cut to the budget**: M1 and M4 in DR-S0, the
  rest in DR-S0b where the verdict needs them (DR-Q9).

No DR-Q item is marked by this amendment. Nothing was built; no suite
was executed, since no build input changed; no `critics-developer`
review was run.

### Amended a second time 2026-11-23

On worktree `dq-cabin-miss-build`, branch `worktree-dr-draft-amend`, over
`088d7499`, on the operator's words (§0's later words;
`raft-marks-2026-11-23.md` §12).

- **§2a is new**: a btree's primary key in three forms fixed at `CREATE
  TABLE`. F0 is today's relation. F1 embeds a key of 48 bits or less in
  the Keystone id. F2 gives a wide key a hashed home, an issued id where
  the home is taken, and one ordered unique index that serves the range
  and holds the colliding rows.
- **What changed from CLA's earlier accounts to the operator**:
  - the collision flag is the issued id's top bit, so nothing is stored
    for it;
  - the collision list is the flagged part of the one index, since a range
    needs every row in that index anyway;
  - an empty home concludes nothing (DR-R11), which removes the `PURGE`
    hazard a home-row flag carried and costs an absent key's read a second
    descent (DR-Q13).
- **Rules** DR-R9 to DR-R12, **items** DR-Q11 to DR-Q15, **stage**
  DR-S0c with cells M6 to M9, which price F2 on today's engine through a
  client-named hashed key.
- **Marks**: DR-Q2 is the operator's (a btree carries the key). DR-R9 and
  DR-R10 are the operator's requirements. Every other part is CLA's
  proposal and unmarked.

Read for this amendment at `5fdca9e0`: `docs/spec/index.md:184-187` (a
btree index entry resolves by a clustered descent) and the Keystone
word's `flags` being reserved and written 0 (`docs/spec/heap-and-tuple.md:112`).
**Not read**: every site DR-S0c's census names; whether the `sys.tables`
byte can carry the form without a superblock move. Nothing was built or
measured; no suite was executed, since no build input changed; no
`critics-developer` review was run.

### DR-S0, DR-S0b, DR-S0c and DR-S1, 2026-11-23

On worktree `dq-cabin-miss-build`, branch `worktree-dr-primary-key`, at
`e9df68d0` (`v3.5.0-114-ge9df68d02`), DR opened on the operator's
standing go-ahead (`raft-marks-2026-11-23.md` §14).

- **Census**: §1.8. Three readers, each claim cited; the lines a decision
  rests on read again by CLA. Two findings no section had: **a write has
  no index arm**, so a write by primary key walks the relation; and **a
  moved key's old entry is purged only beside a tombstone**.
- **Measurement**: one results file for the three stages,
  `bench/v3.5.0/results-dr-s0-premise-v3.5.0-114-ge9df68d02.md`, release
  binary rebuilt at the commit. **Run twice.** The first run (04:46 to
  05:00 UTC) was discarded on its review; the second (05:19 to 05:32 UTC,
  the final driver) is the file's: 251.3 s (M1 to M4), 108.1 s (M5),
  120.4 s and 257.1 s (M6 to M9). Both runs together kept each stage
  inside its 20 minutes. No quiet-gate hold. Cut: M2's `cores` cell and
  its racing sessions, every cell with the relation larger than the pool,
  and a per-batch CPU reading for the 1,000,000-row read phases.
- **What changed from the order as written**: M1 to M4 share one
  relation a form, so M2 and M3 ran with DR-S0 and DR-S0b had only M5
  left; M3 and M9 are three shapes (an inclusive range, a page, a bare
  ordered limit), because IX8's range is inclusive and the first probe's
  strict bound measured a scan; M8's absent key on F2 is two statements,
  which bounds its cost and does not price it.
- **DR-S1's verdict**: §5a, with the adoptions of §4a. F2 is not built,
  on the bar DR-S1 set.
- **Review**: `critics-developer` on the driver, the first run's results
  file and these sections. It recomputed every table and the first
  section's ratios from the raw output and read 22 of §1.8's citations.
  **Applied**: (1) the verdict's ground. The first file said F2's point
  read was not ahead of P1's at 1,000,000 rows; the review showed that the
  server CPU there carried work that was not the statement's, with the
  client's p50 ahead. The driver now waits for a quiet server after each
  load, every cell was run again, and the verdict rests on the insert's
  bar, written as a number (DR-R6). (2) M8's absent keys, which all sorted
  past the loaded range and so missed on one hot leaf: drawn through the
  range now. (3) A range of the `(int32, int32)` form that could hold 99
  rows. (4) The driver's own copy of `dk_close.Server`, an unused
  parameter and two reference forms written twice: removed. (5) Five
  slips of text and a misplaced paragraph of this log, fixed by the
  reviewer. **Rejected**: that DR-S3d wait for the operator's word on
  superblock 41 (DR-Q22). The standing word is to follow CLA's proposal
  where a decision is needed; the record now says in §4a that the
  adoption has no other word behind it and is listed first at the push
  gate.
- **Suite**: not executed. The commit changes no build input: the order,
  the marks, the results file, its archive and two tools.
- **Overhead**: not measured; measured at the milestone's close.

### DR-S2, the sim learns the form, 2026-11-23

On worktree `dq-cabin-miss-build`, branch `worktree-dr-primary-key`, over
`a8283443`.

- **Built**: with the shapes, the stream creates `k0`, a heap with the
  primary key `(g, id)` over a hidden Keystone (`sim/workload.cpp`
  `KeyedOp`, its own stream, in autocommit): inserts that name the key,
  updates and deletes by the whole key and by value, key moves, and reads
  by the key and by a range of it. The oracle's key for it is the `id`
  column, and `sim/loop.cpp` `CheckKeyedWrite` owes each insert and move
  exactly one answer: placed, refused because a live row holds the key,
  one row moved, or none. A deleted or moved-off value is free at once,
  which is where the oracle's rule for it differs from a tuple key's.
  Reconciliation after a crash reads it by `id` as every relation.
- **What it reaches**: before DR-S3a every keyed write of `k0` walked; it
  is the relation the keyed write's descents then run under seeded
  crashes (DR-R8).
- **Not generated**: `known-gaps.md`'s entry says what (a keyed write in
  an explicit transaction, a hidden-Keystone heap among the main tables,
  another key type, a foreign key naming `k0`).
- **Cells**: `SimWorkload.TheShapesWriteARelationByItsPrimaryKey` and
  `SimLoop.ARunWithTheShapesReconcilesTheKeyedRelation` (a clean run and
  twelve crash iterations; keys placed, held and moved each counted). The
  shapes-off corpus draws what it drew: the stream is label-forked.
- **Suite**: 0 failed of 5,184, run on the tree holding this stage and
  DR-S3a to S3c together.
- **Review**: `critics-developer`, with DR-S3a to S3c; its findings are in
  the entry after that one.
- **Overhead**: not measured; measured at the milestone's close.

### DR-S3a, S3b and S3c, 2026-11-23

On worktree `dq-cabin-miss-build`, branch `worktree-dr-primary-key`, one
commit over DR-S2's: the three steps share `command_dispatcher.cpp` and
were built, suite-run and reviewed together.

- **DR-S3a, P7 (DR-Q16)**: an `UPDATE`, a `DELETE` and a locking read
  whose WHERE pins every key column of a unique index by an equality with
  a literal descend by that key's run. `exec::WriteIndexKeyOf` names the
  index and encodes the key; `exec::IndexRunTupleKeys`, factored out of
  the unique check's first phase, names the rows; `WriteWalk::WriteKeys`
  offers each tuple key, ascending, to the statement's own point arm, so
  the whole WHERE is asked of every row found (CU-R1, DR-R4). Each run is
  one probe in `sys.indexes` (DR-Q23).
- **The decline rule, and the defect fixed beside it**:
  `WriteWalk::DescentDeclined` is what both key loops do when a descent
  fails for a reason other than absence. A btree's walk resumes by key. A
  heap's resumes by place, so a list write (CU) that declined mid-list
  used to hand the walk a cursor with a key and no page, and the walk
  wrote none of the rows left: now the walk runs from the head where
  nothing was offered, and the statement is refused `TxnConflict` after a
  row was.
- **DR-S3b, P3's contract (DR-Q1 (a)) and DR-Q24**: nothing built for the
  move - it is what the engine did (§1.7) - and now stated in
  `heap-and-tuple.md` §3.1b and pinned by cells: a key's value moves and
  the row keeps its Keystone; a deleted, moved-off or rolled-back value is
  free at once; the key is unique on full values. `sys.columns` and
  `DESCRIBE` gain `primary_key` (`TableAccess::PrimaryKeyColumns`,
  `catalog::PrimaryKeyIndexName`). **A refusal corrected**: a primary key
  column declared `NULL` in the first position was refused as a tuple
  key's, which on such a heap it is not; the key's own refusal now comes
  first.
- **DR-S3c, P2a (DR-Q4 (a))**: `catalog::CheckForeignKeyDeclaration` no
  longer refuses a parent whose Keystone is hidden. F1 is unchanged.
  Cells: a child names its parent by `keyidentity`; a referenced parent is
  not deleted; a parent's key moves under its children; a value taken
  again is another parent.
- **Cells**: `tests/primary_key_test.cpp` (23 cells), among them the
  equivalence cell - one instance writes by the key, the other the same
  rows by a WHERE that names no key whole, and every statement must leave
  the same relation - and the string-prefix cell (`[quiet-wrong]` if a run
  were trusted). Three cells that pinned the old parent refusal now pin
  the admission.
- **Specs**: `parser-v2.md` I10, `txn.md` §5, `index.md` §10,
  `foreign-keys.md` §3, `heap-and-tuple.md` §3.1b, `sys-views.md` §4,
  `stats/index_stats.hpp`; `known-gaps.md` gains DR's section and narrows
  the sim's entry.
- **Suite**: 0 failed of 5,184.
- **Review**: `critics-developer` on a snapshot of the two diffs; its
  findings and what was done with them are the next entry.
- **Overhead**: not measured; measured at the milestone's close.

### The review of DR-S2 and DR-S3a to S3c, 2026-11-23

`critics-developer`, on a snapshot of `adc0ff3b`'s and `ab00f95d`'s diffs,
report only. Its findings were applied on worktree `dq-cabin-miss-build`,
branch `worktree-dr-primary-key`, in the commit that carries DR-S3d and
S3e.

- **What it traced and found sound**: no way the unique-key arm writes a
  row its walk does not, or misses one, while descents succeed - every
  version a reader can reach has its entry (`OwedIndexEntries`), the tuple
  key is read from the right bytes of a heap's and a covering index's
  entry, a string prefix is decided in `Qualify`, a value of the wrong
  form is refused by the encoder and the walk answers, and the units are
  the walk's. DR-S3c: a hidden Keystone id is never issued again and a
  reused rowid is checked against it, so no child can come to name another
  row. DR-S2: the oracle owes each keyed write one answer.
- **Applied, `[quiet-wrong]`**: the heap rule DR-S3a added for a declined
  descent traded one defect for another. With nothing offered, the walk
  took the statement from the head; if that walk then wrote a row and
  parked, the resume came back through the key loop, which offered every
  key again and wrote that row twice. `WriteWalk::WriteByKeys` now hands a
  resume that carries a place to the walk.
- **Applied**: the two key arms were one block copied at three sites, six
  blocks: now `WriteByKeys`, one copy. A statement's use of the index is
  counted at its first collection, not at each resume. Four sentences of
  spec and comment that said what the code does not: the list's decline
  "from that key" on a heap, "a row unit per row" where a tuple key bound
  beside the key declares a window, "no bound is declared for either",
  and what `Qualify` does with a row moved onto the key since a park.
  The equivalence cell now checks that its walking instance walked.
- **Rejected**: that no key arm be asked after the tuple key's own
  descent declined. CU-S3b's contract is the other way and is pinned
  (`KeySetWriteHoldTest.ASingleKeysDescentThatDeclinesIsAnsweredByTheListOrTheWalk`):
  a single key beside a list falls to the list's descents. What the
  finding named is a refusal on a heap where the walk used to answer -
  `TxnConflict`, retryable - never a wrong row.
- **Rejected**: merging `WriteKeys` and `WriteKeySet` into one loop. They
  differ in the leaf a list offers first; a hook for it is more than the
  dozen lines it would save.
- **Cells added**: a keyed write that waits on its held row and is run
  again (`MidWalkWaitTest`); on a heap, a list whose first descent
  declines is the walk from the head and one whose later descent declines
  is refused (`KeySetWriteHoldTest`). **Not written**: the park after a
  declined first descent (`known-gaps.md`).

### DR-S3d and S3e, 2026-11-23

On worktree `dq-cabin-miss-build`, branch `worktree-dr-primary-key`, one
commit over `adc0ff3b`: the two stages share the format event and
`CREATE TABLE`, and were built and suite-run together.

- **The format event (DR-Q22), superblock 41**: `sys.tables`' retired
  key-order byte is `key_form` (0 issued or named, 1 embedded) and its
  retired `owner_core` word the embedded key's column positions; a btree
  may carry a hidden Keystone. `SysTableRow::Decode` refuses a form it
  does not know. A version-40 volume is refused by the version.
- **DR-S3d**: `PRIMARY KEY` is admitted on a btree. On its `int64` first
  column it is the relation as it was (F0, DR-R9): no column, no index, no
  step, and the cell compares its catalog rows, its answers and its plan
  with a relation declared with no keyword. Any other key that does not
  embed is P1: a hidden Keystone and `$pkey_<oid>`, the heap's form on a
  btree, found by `IndexProbe` and written by DR-S3a's descent. `PURGE` is
  admitted on such a btree by `keyidentity` (DR-Q19), and `PURGE INDEX
  $pkey_<oid>` names the key's index (DR-Q21).
- **DR-S3e, F1**: a btree's key of `int8`, `int16`, `int32`, `date` and
  `bool` columns totalling 48 bits or less is its Keystone id
  (`exec/embedded_key.hpp`): `id = code + 1`, pinned by vectors. An
  `INSERT` is placed at the id its key encodes to, through the named
  key's path; the whole key is a `Lookup` and a prefix or a range of it a
  `Range`, for a read and a write alike; a key column is never assigned
  (K2) nor dropped; a deleted key stays bound until `PURGE`, which takes
  the key's own window (DR-Q20). `ON CONFLICT` is refused by name.
- **What is not built**, each in `known-gaps.md`: a join keyed by an
  embedded key, the `ORDER BY` elision over one, and F2.
- **Cells**: `tests/btree_primary_key_test.cpp` (17), among them every
  read of an embedded relation compared with a keyless heap holding the
  same rows over 21 predicates (`[quiet-wrong]` if a window were off by a
  column or a sign) and a crash cell over both forms;
  `tests/embedded_key_test.cpp` (7), the format's vectors; the
  `sys.tables` row's and the superblock's cells.
- **Specs**: `heap-and-tuple.md` §3.1c (new) and §4.1c, `parser-v2.md`
  I10, `index.md` §10, `keystoneid-invariant.md` K2, and the four
  sentences that called the two fields reserved (`crosscore.md` CC7,
  `namespace.md`, `page.md`, `heap-and-tuple.md` §4.1).
- **Suite**: 0 failed of 5,211.
- **Review**: `critics-developer` on a snapshot; its findings are the next
  entry.
- **Overhead**: not measured; measured at the milestone's close.
