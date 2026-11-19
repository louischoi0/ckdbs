# Work order DC - `SELECT ... FOR UPDATE`: a read that claims the rows it returns

Status: **CLOSED 2026-11-19 on worktree `dc-select-for-update`, its code
at `7e18598d` (`v3.4.13-16-g7e18598d`), and landed on `main` the same day
on the operator's word "push no verify" (`raft-marks-2026-11-19.md` §6) -
a `--no-ff` merge pushed with `--no-verify`, the suite 0 failed of 4794 on
those build inputs: every stage is done (§7) and the close is measured
(`bench/v3.4.13/results-dc-close-v3.4.13-14-gcd80d31d.md`,
`bench/v3.4.13/results-dc-close-v3.4.13-16-g7e18598d.md`). Opened the same
day on the operator's standing go-ahead (go-ahead-achieving-milestone,
`raft-marks-2026-11-19.md` §5), from `8e7c820f` (`v3.4.13-10-g8e7c820f`).
DC-Q0..Q12 are each adopted (a) as CLA proposed, none the operator's own
mark** (§4). Written on worktree
`db-fpi-cadence-dc-for-update` from `a4d2fb3b` (`v3.4.12-42-ga4d2fb3b`) on
the operator's word of 2026-11-18 (`raft-marks-2026-11-18.md` §23): *"Write
the work order for item 8, SELECT FOR UPDATE."* (given in Korean; recorded
in English). Every `[source-read]` claim in §0 to §2 was read at
`a4d2fb3b`, and its line numbers are that commit's; DC-S0 (§7) re-read them
at `8e7c820f` and cites that commit's lines. The letter DC is CLA's
proposal.

**Item 8** is the eighth item of a survey CLA gave the operator in
conversation the same day ("what should this engine add, as an OLTP
engine"): the parser and the manual have no `FOR UPDATE`, it is the
standard tool for a read-modify-write, and the `U` mode exists since
CP-S2b.

**The motive is a result, not a speed.** Under `READ COMMITTED`, the
default, every statement mints its own view (`docs/spec/txn.md:103-128`),
and a write is checked against the view of the statement that writes
(`src/txn/manager.cpp:190-209`). `[design]` So a client that reads a
balance in one statement and writes a value computed from it in the next
overwrites a commit that landed between the two, and nothing refuses it:
the tools today are a `SET` expression over the before-image, `REPEATABLE
READ`'s refusal, or a declared assertion. A locking read is the fourth: the
row is claimed from the read to the decide.

**What CLA told the operator, corrected against the tree.**

- *"`U` exists, so the cost should be small"* (an estimate). The claim
  machinery does exist whole - but on the **write** walk, which reaches
  its rows through no index, yields its core nowhere, cannot be cancelled
  mid-walk and dirties every leaf it visits
  (`docs/inflight/known-gaps.md:2018-2026`, `:904-907`; §0, DC-Q9). A
  locking read built on it inherits all four (§1.4, DC-Q0).
- *"Rows reached by tuple key, index or scan"* (CLA's first sketch of the
  shape). Under DC-Q0 (a) there is no index reach, and only an equality or
  a list on the tuple key descends: everything else is a walk of the whole
  relation, and on a `HEAP` relation every locking read is one (§0).
- *"The standard tool for a read-modify-write"*, with `U` as its mode.
  Under `U` the spelling `FOR UPDATE` gives what PostgreSQL calls `FOR NO
  KEY UPDATE`, not its `FOR UPDATE`: a foreign key child's share passes
  it. That is enough for the read-modify-write of one row and not for a
  client that locks a parent to keep children out (§2, DC-Q1
  `[quiet-wrong]`).
- *"The `readonly` role must refuse it."* It must. The role gate reads
  the routing token and admits `SELECT`; but a `readonly` connection's
  transactions are `READ ONLY`, whose write scope refuses, so the gap is
  autocommit's alone and DC-Q7 (a) closes it (§1.5, DC-R7).
- **Not CLA's, found on the way**: CP's order calls `SELECT ... FOR UPDATE`
  "out of scope (`txn.md` §9)"
  (`workorder-cp-lock-model-exploration.md:592-593`), and `txn.md` §9 names
  `SERIALIZABLE` and savepoints only (`docs/spec/txn.md:1291-1294`). No
  ruling against a locking read is recorded; two orders left it out of
  their own scope (CP, and `workorder-cq-column-reservation.md:388`).

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured.

Related: `docs/spec/txn.md` §1, §5, §9; `docs/spec/foreign-keys.md` §2c;
`docs/spec/parser-v2.md` I10, I11, I19; `docs/spec/protocol.md` §10;
`docs/spec/sched.md` §3; `docs/inflight/known-gaps.md` (Locks, Key sets);
`instructions/v3.2.0/workorder-cp-lock-model-exploration.md` (CP-S2b's
`U`, CP-Q10); `instructions/v3.2.0/workorder-cr-read-committed-rerun.md`
(CR, written and not opened: DC-Q4);
`instructions/v3.2.0/workorder-cs-read-only-transaction.md` (CS-R1, the
gate); `instructions/v3.2.0/workorder-cu-key-set-access.md` (the write's
point and list arms); `manual/sql/sql.md` §4, §5, §7;
`manual/client/client.md` §4, §5.

## 0. What DC is

`[operator]` A work order for `SELECT ... FOR UPDATE`.

`[design]` `SELECT <columns> FROM <relation> [WHERE ...] FOR UPDATE`,
inside an explicit `READ WRITE` transaction, returns the rows an `UPDATE`
with the same `WHERE` would write, each under the claim that `UPDATE`
would take - the row's `U`, or the declared key window's - held until the
transaction decides. It writes nothing and logs nothing. Until the decide,
another transaction's `UPDATE`, `DELETE` or locking read of a returned row
waits, and a foreign key's share on it is admitted.

**In one sentence: it is an `UPDATE`'s row walk with no `SET` list and a
`RETURNING` list** - the claim pass `UPDATE` already runs, then the emit
pass `RETURNING` already runs, over the claimed keys in place of the
written ones. DC-Q0 puts that choice to the operator with its alternative.

**What that choice makes of it, said before anything else** (DC-Q0 (a),
`[source-read]`):

- **Only an equality or a list on the tuple key descends**
  (`src/server/command_dispatcher.cpp:11020-11097`). A range on the tuple
  key and every other predicate walk the whole relation, whatever index
  the column has (`docs/inflight/known-gaps.md:2011-2021`).
- **The walk does not yield and cannot be cancelled mid-walk**: it holds
  its core to its end, and a `C_CANCEL` or a closed connection is seen at
  the session's next frame (`known-gaps.md:2022-2023`, `:904-907`).
- **On a `HEAP` relation the tuple key is hidden**, so no statement names
  it and every locking read there is a whole walk taking a unit a row.
- **The walk fetches leaves for writing, which dirties each one** (§1.3);
  DC-Q9 asks whether a locking read may.
- **A range on the tuple key declares a window**, and the window raises
  the relation's fence counter until the transaction decides, so every
  writer of the relation pays the all-partition probe for that long
  (`docs/spec/txn.md:955-959`; a borrow is the transaction's,
  `include/kds/server/command_dispatcher.hpp:1076-1079`).
- **Under `U` a foreign key child's insert passes the claim** (DC-Q1).

So the first build serves the point shape - `WHERE <tuple key> = ?` or
`IN (...)` on a btree relation - at an `UPDATE`'s cost, and every other
shape at a whole-relation write walk's.

## 1. The tree as read

### 1.1 What is measured `[measured]`

Nothing, for a locking read. The nearest numbers are the hot-row refusal
rates CR's order records - 7.9 % of autocommit `UPDATE`s of one row at
eight sessions, 1.4-12.8 % of an explicit transaction's debits
(`workorder-cr-read-committed-rerun.md:52-56`, citing CP-S0 and CQ-S0;
the author did not open those results files, and the review found both
numbers there: `bench/v3.4.7/results-cp-s0-premise-v3.4.7-1-gcc54a296.md:17`,
`bench/v3.4.7/results-cq-s0-premise-v3.4.7-13-g92449e65.md:99-132`). They price the window
a locking read keeps under DC-Q3 (a) and DC-Q4 (a), and say nothing of how
often a locking read would meet it.

### 1.2 The statement today `[source-read]`

- **`FOR` after a `SELECT` is trailing text.** The native tail is `[ORDER
  BY] [LIMIT] [OFFSET]`, each word an ordinary identifier matched by text
  at clause position (`src/parser/parser.cpp:2953-2960`, `:2970-3067`),
  and anything after it is refused `InvalidArgument` "unexpected token ...
  after end of statement" with its byte (`:3808-3815`). The dispatcher
  then offers the text to the query grammar (BU-Q9,
  `src/server/command_dispatcher.cpp:8809-8816`), which refuses the same
  token the same way (`parser.cpp:3914-3928`), so the native refusal is the
  reply. Read, not run.
- **No word of it is reserved and none needs to be.** Eleven words are
  (`manual/sql/sql.md:2018-2026`); a relation's bare alias is not accepted
  (`parser.cpp:2675-2685`), so no accepted statement has an identifier
  where the clause would stand. A clause inside a subquery has a precedent
  for its refusal, `ORDER BY` at `depth > 0` (`:2980-2985`).
- **No test, simulator draw or tool spells a locking read**
  (`grep -rni 'for update' tests sim scripts tools` finds one comment).

### 1.3 The claim an `UPDATE` takes `[source-read]`

- **Five modes; `U` is a version claim.** `U` admits `IS` and `S`, refuses
  `IX`, `U` and `X`; `X` covers `U`, `S` does not
  (`include/kds/txn/lock_table.hpp:225-231`, `:261-280`;
  `docs/spec/txn.md:903-929`). **`S` admits `U`**: a key share protects a
  row's existence and key and nothing else (`lock_table.hpp:253-255`).
- **The row walk `UPDATE` and `DELETE` share** is `WriteWalk`, kept in one
  copy "so that both judge and claim a row the same way"
  (`command_dispatcher.cpp:10047-10057`). `Qualify` reads a row under the
  statement's view and its `WHERE`, with F4's rule for a row whose writer
  the view cannot see (`:10099-10184`; `txn.md:868-901`). `Claim` asks the
  row's unit - skipped when a declared window covers it - then takes the
  first-updater-wins verdict, and never admits a version reached through
  undo (`:10197-10241`).
- **How it finds rows**: one descent for a tuple key (`:11020-11068`), one
  per value of a list on it (`:11089-11097`), and otherwise a walk of the
  whole relation with leaves held for writing (`:11106-11124`). **A leaf
  fetched for writing is dirtied whether or not a row of it is written**:
  the walk's `kWrite` is `PageStore::Get`, an exclusive pin that is not
  `for_read` (`include/kds/storage/page_store.hpp:266-277`), and leaving a
  frame clean is the read fetch's opt-in (`:641-659`). A `WHERE`
  holding a subquery is collected by a read pass first (`:10254-10271`). A
  bound on the tuple key declares its window as a range unit in `U` before
  the walk, and no `WHERE` declares the whole id space
  (`:10615-10632`, `include/kds/server/command_dispatcher.hpp:1076-1087`,
  `txn.md:931-959`).
- **The unit's ancestor is the relation's `IX`**, held like the unit to
  the decide (`command_dispatcher.cpp:11814-11833`), so a DDL's relation
  `X` waits for the holder as for any open writer.
- **The holder is a transaction.** Units are held under the transaction's
  id (`:11775-11776`); an explicit `READ WRITE` transaction has one, a
  `TXN_BEGIN` record and an in-flight slot from its `Begin`
  (`src/txn/manager.cpp:126-166`). A `READ ONLY` transaction has none, and
  a write scope refuses it (`command_dispatcher.cpp:11612-11621`).
- **A held row is waited for, and a statement that wrote nothing is run
  again whole.** A refused unit names its holder; a holder that wrote no
  version of the row is one even a `REPEATABLE READ` waiter may wait on
  (`:12184-12234`, `:12228-12230`; `txn.md:977-1024`). The walk stops at
  the row, and a statement whose trail is at its mark is answered with the
  conflict and re-run by `DispatchAsync` (`:10318-10336`, `:12259-12263`,
  `:850-868`; the re-run itself in `AwaitWriteBlock`, `:504-505`). The
  re-run passes the statement boundary
  (`:1049-1055`, `:1367-1372`), where `READ COMMITTED` mints a fresh view
  and `REPEATABLE READ` keeps its own (`manager.cpp:170-188`).
- **What that leaves refused.** A row whose newest version committed
  after the statement's view is `TxnConflict`: the level's contract under
  `REPEATABLE READ`, and under `READ COMMITTED` the window between the
  mint and the claim, "a deliberate simplification - there is no re-read
  loop" (`txn.md:862-866`). CR is the order that would re-run it. **Inside
  an explicit transaction that refusal fails the transaction**
  (`session.Poison()`, `command_dispatcher.cpp:12301-12306`), and the
  row's unit stays held until the `ROLLBACK`, because `Claim` borrows
  before it checks (`:10209-10214`). A row inserted by a writer the view
  cannot see has no version for it and is skipped, not waited on
  (`include/kds/txn/visibility.hpp:70-75`, `:10118-10119`). And
  **a wake is not a grant**: `TryAcquire` does not queue, so under
  sustained contention a waiter can end at the 1 s fault net
  (`known-gaps.md:629-632`; the decide slot is designed and not built,
  `:658`).
- **Nothing of a claim is persisted** (AO-R3, `txn.md:789-792`,
  `:1163-1165`). The cap is 65,536 units a transaction, refused
  `ResourceExhausted` and not retryable (`lock_table.hpp:184`, `:417`;
  `txn.md:1184-1189`). An explicit transaction, and so its claims, ends at
  `txn_lifetime_ceiling_ms` at the latest (`txn.md:22`).
- **`RETURNING` is an emit pass after the walk**: the keys the statement
  wrote, read off its trail, each found again by a shared descent and
  rendered through the items; the reply completes under the write's tag
  (`command_dispatcher.cpp:9818-9973`, `:9926-9969`;
  `src/server/kwp_session.cpp:764-768`).

### 1.4 The two executors `[source-read]`

| | the read (`HandleSelect`, the step chain) | the write (`WriteWalk`) |
|---|---|---|
| finds rows by | tuple key, key set, index, Cabin, walk, join | tuple key, key set, walk (`known-gaps.md:2018-2021`) |
| version it judges | the snapshot's, through undo | the page's newest, or refuses (`:10111-10117`) |
| takes | the relation `IS`, nothing finer; never waits (`txn.md:1026-1061`) | relation `IX`, a unit or a window, waits |
| yields its core | every 64 pages of the outermost walk | never (`known-gaps.md:2022-2023`) |
| rows leave | while it runs (CE) | after the walk (`RETURNING`) |
| consults | an opted Waystone trail, a Cabin | neither (a `WHERE` subquery's runner: `src/exec/step_vm.cpp:3054-3059`) |

The left column's access paths, its 64-page slices and its streamed reply
are as `CLAUDE.md`'s milestone rows state them; the author did not open
`sched.md` §3 or `protocol.md` §7, and the review found both columns borne
out there (`docs/spec/sched.md:43`: a write, a tuple key descent and an
index walk do not yield; `docs/spec/protocol.md:104-108`: a tuple key
lookup, an index step and a write's `RETURNING` are sent whole). DC-S0
re-reads them before DC-Q0 (b) is weighed further.

`[design]` A locking read on the right-hand column needs no new rule. On
the left it needs four: a park on a lock inside a sliced walk, the write's
version rule in the step VM, a re-run rule for a statement whose rows have
left, and a gate keeping a trail or a Cabin from answering it.

### 1.5 The gates `[source-read]`

- **The role** is decided from the routing token: `SELECT` is `readonly`'s
  (`command_dispatcher.cpp:1325-1343`, enforced `:1398`;
  `docs/spec/protocol.md:140`).
- **The `READ ONLY` gate** is an allowlist on the same token and admits
  `SELECT` (`:171-179`, `:1456-1464`); its second line is the write
  scope's refusal, which names byte 0 (`:11617-11621`).
- So a clause at the statement's tail is seen by neither. **A `readonly`
  connection's `BEGIN` is `READ ONLY` and `READ WRITE` is refused it**
  (`docs/spec/txn.md:136-138`), so inside a transaction the write scope's
  refusal catches it. `[design]` What is left is autocommit, where a
  write scope begins a transaction of its own: there, and only there, a
  `readonly` user could take a claim.

### 1.6 The fingerprint `[source-read]`

The version moves only when an already storable statement would hash
differently; making an unparseable statement parse does not move it
(`include/kds/parser/fingerprint.hpp:162-187`, `:203`). The clause's words
are hashed as shape, so a locking statement has its own `pattern_id` and
`fetch_id`, apart from the plain statement's.

## 2. The design `[design]`

As DC-Q0 (a), DC-Q1 (a) and the other proposals would have it; the marks
decide.

1. **The grammar.** After the pagination tail of a depth-0 block, `FOR
   UPDATE` sets a lock field on `SelectStmt` with the byte of `FOR`. Every
   other word after `FOR`, and every word after `UPDATE`, is refused by
   name (DC-Q2, DC-Q6); a clause at `depth > 0` is refused at its byte.
   The query grammar refuses the clause by name and is not taught to read
   it (DC-R2).
2. **The route.** `HandleSelect` branches on the parsed field before any
   chain is compiled: a locking statement never reaches `SelectBody`,
   `TryQueryLayer`, a catalog view, a trail or a Cabin. `ANALYZE`, a pinned
   caller (a subquery's run) and `WAYSTONE ON` refuse it.
3. **The gates**, after the parse and before anything is borrowed: the
   role, the `READ ONLY` transaction, autocommit (DC-Q7), the shape
   (DC-Q5).
4. **The claim pass.** The session's write scope; the relation resolved
   and the `WHERE` compiled as an `UPDATE`'s; the window declared in `U`
   where the `WHERE` bounds the tuple key; then `UPDATE`'s arms in
   `UPDATE`'s order - point, list, collection pass, walk - with `Qualify`
   and `Claim(U)` per row and the row's key kept. No `SET`, no stage-2
   decode, no hook, no trail entry.
5. **A held row** stops the walk as it stops an `UPDATE`'s. The trail is
   at its mark by construction, so the statement is always the re-run
   arm: answered with its conflict, waited for, run again whole. Units the
   first run took stay the transaction's.
6. **The emit pass**, once the claim pass ends: `RETURNING`'s, over the
   claimed keys in key order, the fields those of the select list, the
   reply completing as a `SELECT`'s. Each row is the page's version, which
   no other transaction can replace while the claim stands.
7. **Leaf holds** (DC-Q9 (a)). The pass writes no page, so it fetches for
   read: the walk as the collection pass takes it (`:10257-10258`) and a
   shared descent for a key, where `LocateByTupleKey` holds its leaf
   "exclusive and dirty" (`:11029-11031`). `Claim`'s no-re-read argument
   is stated for a write hold (`:10199-10203`); DC-S0 re-argues it for a
   shared one, and if it does not hold the item goes back to the operator.
8. **Afterwards** the transaction holds the relation's `IX` and its units
   or window until it decides, is poisoned, or meets the lifetime ceiling.
   Its own `UPDATE` of a claimed row asks nothing new (`U` covers `U`);
   its `DELETE` asks `X` and waits for any foreign key share, which is
   CP-Q10's accepted class and no new one.
9. **Counted by nothing new**: its own row in `sys.statement_stats`, its
   waits in `sys.lock_waits`, and no row of `sys.relation_stats`' write
   activity, since it writes none.

**What `U` under the spelling `FOR UPDATE` does not give**
`[quiet-wrong]` (DC-Q1). `U` admits a key share
(`include/kds/txn/lock_table.hpp:265-266`), and a foreign key child's
insert holds exactly that on its parent. So a client that locks a parent
row to keep new children out, then sums its children, is wrong here: a
second session's child `INSERT` passes the claim and commits, and the sum
is stale with no refusal anywhere. And a `DELETE` of the locked row later
in the same transaction can be refused RESTRICT by a child that committed
meanwhile. PostgreSQL's `FOR UPDATE` blocks both; its `FOR NO KEY UPDATE`
is what `U` is. `CLAUDE.md`'s rule - never accept a spelling and enforce
something other than what was written - is why the mode is the operator's
to mark and not CLA's to choose.

## 3. Rules

Binding once DC opens.

- **DC-R1 - a row is returned only under a held claim, and only as the
  version the claim admitted** `[quiet-wrong]`: the page's newest version,
  written by a transaction the statement's view sees or by this one.
  Never a version reached through undo. A stale row under a held lock is
  the lost update the clause exists to prevent. **The other direction is
  not promised: rows are claimed that are not returned** - every key of a
  declared window, which is a superset of the rows; a row an earlier run
  claimed that the re-run no longer collects; a row refused after its
  unit was granted; rows a capped portal never delivers. **And what is
  never skipped**: a row whose newest version matches the `WHERE` and is
  newer than the view is waited on or refused, by `txn.md` §5's table
  (`:868-901`). A row inserted after the view is invisible and skipped,
  unless a declared window made its insert wait.
- **DC-R2 - no path that does not claim answers a locking statement**
  `[quiet-wrong]`: not the step chain, the query layer's fallback, a `sys`
  view, `ANALYZE`, a Waystone trail or a Cabin. Each has a test that the
  clause is refused there, not dropped.
- **DC-R3 - under DC-Q0 (a), one copy of how a row is judged and
  claimed.** `Qualify` and `Claim` decide for a locking read what they
  decide for an `UPDATE`; DC adds no mode, no unit, no wait and no version
  rule (but what DC-Q3 marks).
- **DC-R4 - under DC-Q0 (a), no row leaves before the claim pass ends**,
  so a re-run sends no row twice and every row of one reply was judged
  under one view. Under (b) the stage states what replaces both.
- **DC-R5 - a claim is the transaction's**: held to the decide, released
  by nothing else, persisted nowhere - no record, page bit or superblock
  field (AO-R3). The log is the same length after a locking read as
  before it.
- **DC-R6 - a locking read writes nothing**: no row, undo record, index
  entry, Cabin witness, assertion reservation or trail entry, and it runs
  no foreign key, assertion or transition check. **Under DC-Q9 (a) it
  dirties no page either**, pinned by a test as CS's is; under (b) the
  pages it dirties are counted in the close and named in `known-gaps.md`.
- **DC-R7 - it is a write for authorization and access mode.** In a
  `READ ONLY` transaction it is refused `READ_ONLY_TRANSACTION` at the
  byte of `FOR`, unpoisoned and counted as CS-Q7 and CS-Q8 have it. **It
  needs `readwrite`**: a `readonly` connection cannot open a `READ WRITE`
  transaction (`txn.md:136-138`), so under DC-Q7 (a) it reaches no claim
  whatever the role check says, and the check after the parse is the
  second line; under DC-Q7 (b) that check is the only line and
  `[quiet-wrong]` in the authorization sense - a read-only user holding
  writers.
- **DC-R8 - every refusal names its byte, in the right code.** A form
  nobody built is `NotImplemented`; a `sys` view, which is no relation and
  has no row to claim, is `Unsupported`. **`FOR SHARE` is never served by
  the lock table's `S`** `[quiet-wrong]`: `S` admits another writer's `U`,
  so the spelling would be accepted and something else enforced.
- **DC-R9 - nothing is reserved and the fingerprint version does not
  move.** The words are matched by text at clause position; the claim
  that no accepted statement's hash changes is argued in the stage and
  pinned by the golden corpus.
- The standing rules: each stage reviewed, the full suite per stage, the
  overhead measured once at the close in `build-release`, a stage's
  measurement inside 20 minutes under a 60 s watchdog, nothing pushed or
  merged without the operator's word.

## 4. Items for the operator (DC-Q), CLA's proposals

**None is the operator's own mark. Each of DC-Q0..Q12 is adopted (a) on the
operator's standing go-ahead of 2026-11-19 (go-ahead-achieving-milestone,
`raft-marks-2026-11-19.md` §5)**; (a) is CLA's proposal in each. DC-Q11 and
DC-Q12 were raised by DC-S0 (§7) and written here before they were
adopted. Every other option stays open to the operator's own mark.

| | item | (a) | other |
|---|---|---|---|
| DC-Q0 | Which executor carries it. | the write walk (§1.4's right column): no new rule, and no index, no yield and no streamed reply - a predicate other than an equality or a list on the tuple key, a range on it included (`known-gaps.md:2011-2014`), walks the whole relation holding its core, as an `UPDATE`'s does | (b) the step chain with a claim at the driving step: index reach, yields and a streamed reply, at the price of §1.4's four new rules and a second copy of the version rule `[quiet-wrong]` |
| DC-Q1 | The mode the spelling `FOR UPDATE` takes `[quiet-wrong]` (§2's last paragraph). | `U`, the claim an `UPDATE` of the row takes in this engine: other writers of the row wait, a foreign key child's share passes, and the transaction's later `DELETE` asks `X` then and may be refused RESTRICT. The manual says in so many words that it is PostgreSQL's `FOR NO KEY UPDATE` | (b) `X`, the claim a `DELETE` takes, which a child's `S` waits for: what the spelling means elsewhere, and the pair CP-S2b removed for `UPDATE` (`foreign-keys.md:281-290`) returns for this statement; (c) the `U` form is spelled `FOR NO KEY UPDATE` and `FOR UPDATE` is refused `NotImplemented` until `X` is built |
| DC-Q2 | The other strengths. | `FOR SHARE` and `FOR KEY SHARE` each refused `NotImplemented` by name, and `FOR NO KEY UPDATE` too unless DC-Q1 (c) makes it the spelling | (b) `FOR KEY SHARE` built as the tuple `S` a foreign key takes; (c) under DC-Q1 (a), `FOR NO KEY UPDATE` accepted as a second spelling of the `U`. `FOR SHARE` as `S` is not an option (DC-R8) |
| DC-Q3 | Which version is returned when a newer one exists `[quiet-wrong]`. | the write's verdict, unchanged: `REPEATABLE READ` is refused first-updater-wins; `READ COMMITTED` waits for an undecided writer and is re-run under a fresh view, and in the window between its mint and its claim is refused retryably, as an `UPDATE` is today. **Either refusal fails the whole transaction** (§1.3) - every locking read being inside one under DC-Q7 (a) - with the row's `U` kept until the `ROLLBACK`, so the client retries the transaction, not the statement | (b) `READ COMMITTED` returns the newest committed version whatever the statement's view, PostgreSQL's re-check: a second version rule, for this statement only, and no failed transaction in that window |
| DC-Q4 | DC and CR. | independent. DC lands on today's verdict and names the window as a known gap; when CR opens, CR-Q5's list gains the locking read, whose admission test holds by DC-R4 and DC-R6 | (b) CR first, DC after; (c) DC builds the decided-newer re-run for the locking read alone |
| DC-Q5 | The shapes of the first build. | one relation that is not a `sys` view; the `WHERE` an `UPDATE` admits, a subquery in it included; a select list of `*` or plain columns. Refused `NotImplemented` at the byte: joins, aggregates and `GROUP BY`, `ORDER BY`, `LIMIT`, `OFFSET`, every query-layer form, a clause inside a subquery | (b) also `LIMIT`, and `ORDER BY <tuple key>` ascending, which is the walk's order - the job-queue shape; (c) also select-list expressions and aliases, typed as `RETURNING`'s items are |
| DC-Q6 | `NOWAIT`, `SKIP LOCKED`, `OF <relation>`. | each refused `NotImplemented` by name | (b) `NOWAIT` built: a refused unit is answered `TxnConflict` naming the holder with no park; (c) `SKIP LOCKED` as well |
| DC-Q7 | Autocommit. | refused `NotImplemented` at the byte of `FOR`: the claim would end with the statement, and holding it would cost an id, a `TXN_BEGIN` and a commit record (`manager.cpp:138-148`) | (b) admitted as an owned scope, like an autocommit `UPDATE` that matches rows |
| DC-Q8 | What a predicate on the tuple key declares. | what an `UPDATE`'s does: a bound declares its window in `U`, no `WHERE` the whole id space, so an insert into the window waits for the decide and a keyed read never meets the cap; a predicate naming no key takes a unit a row and is refused at 65,536 | (b) a unit a row always: nothing but returned rows is fenced, and a wide read is refused at the cap |
| DC-Q9 | Whether a locking read may dirty a page. Under DC-Q0 (a) as the walk stands it dirties every leaf it visits (§1.3), so a whole-relation locking read hands the checkpoint the relation. | no: it fetches for read (§2 step 7), a test pins that no frame is dirty after it, and DC-S0 reads whether the claim pass can - if not, back to the operator | (b) the dirtying accepted, its writeback priced in the close and entered in `known-gaps.md` |
| DC-Q10 | The measurement. | DC-S0 is a read. The close: a point `SELECT` and a point `UPDATE` before against after, interleaved, the bar "no cell slower outside its own spread" - a statement without the clause pays one peek at its tail and one test of a parsed field; and one descriptive cell with no bar - eight sessions, one row, `BEGIN; SELECT ... FOR UPDATE; UPDATE; COMMIT` at `cores = 4` - reporting how many locking reads DC-Q3 (a)'s window refuses. The standing budget | (b) the close without the descriptive cell |
| DC-Q11 | Which refusals of a locking read fail its transaction (raised by DC-S0: an `UPDATE` fails its transaction on any refusal, a syntax error included, since every one of its refusals ends through `EndWrite`; a plain `SELECT`'s refusal fails nothing). | from the first lock ask on, as the write whose claim it takes: a conflict, the cap, a walk's or the emit's error fail the transaction, and a held row waited for does not. **Before any unit is asked nothing is held and nothing fails**: the parse, the gates (role, `READ ONLY`, autocommit, shape), the relation's resolve and the compile of the `WHERE` and of the select list are refused as a `SELECT`'s are, and the client may go on | (b) every refusal as an `UPDATE`'s: a mistyped column in a locking read forces a `ROLLBACK` |
| DC-Q12 | What bounds the rows a locking read returns (raised by DC-S0: the emit pass inherits `RETURNING`'s, which has no cap - every row is encoded into the session's reply before the statement ends, and a KWP sink's sealed batches are held in memory; a row-unit read is bounded at 65,536 by the lock cap, a declared window is not). | nothing new: bounded as a write's `RETURNING` is, the claimed keys at 8 bytes a row beside it, and entered in `known-gaps.md` with `RETURNING`'s own missing bound | (b) refused `ResourceExhausted` past `sort_max_rows` claimed rows, before any row is emitted, the claims kept to the decide |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DC-S0 - census** | No engine code, no benchmark. §1 re-read at the opening commit. **Read**: design step 7's read fetch and shared holds (DC-Q9); what a cancel and a closed connection do to a walk that does not yield; what bounds a `RETURNING` reply's rows and memory, since the emit pass inherits it; how a bound parameter and a KWP portal (`max_rows`, `C_DESCRIBE`) reach `HandleSelect` and a write handler, and where they differ; what an explicit transaction with an empty trail costs at `COMMIT`; whether access statistics record a write walk; every test that pins the `FOR` refusal's text or a fingerprint of a statement that does not parse; DC-R9's argument written out. | the list, and any item back to the operator |
| **DC-S1 - the grammar and the refusals** | The clause parsed; the route's branch; every gate and every refusal of §2 steps 1-3, each with its byte. A well-formed locking read is itself refused `NotImplemented` at this stage. | review, the full suite |
| **DC-S2 - the claim pass and the emit pass** | §2 steps 4-8 for every shape DC-Q5 admits. | review, the full suite |
| **DC-S3 - the close** | DC-Q10's measurement; the documents below; the simulator's and the history checker's draws; the milestone rows. | closed |

**Tests DC owes.** Each refused spelling and shape at its byte and code,
and `for` and `update` still usable as column names; `INSERT ... SELECT
... FOR UPDATE` and `WITH ... FOR UPDATE`, which the query grammar reads
(`src/parser/parser.cpp:2526-2531`), refused there by name; the golden
corpus unmoved and a locking statement's `fetch_id` apart from the plain
one's.
In a quiet instance the rows equal the plain statement's, byte for byte,
on a btree relation and on a heap. Two sessions, at one core and across
cores under `tsan`: an `UPDATE`, a `DELETE` and a second locking read each
wait for the holder and proceed at its commit and at its rollback; a
foreign key child insert passes the holder; an insert into a declared
window waits; under `REPEATABLE READ` a waiter is refused after the holder
wrote the row and proceeds after one that did not; two sessions claiming
two rows in opposite orders end with one refused naming deadlock. **The
motive's cell**: two `READ COMMITTED` sessions each read a counter, add
one and write it back, each retrying its whole transaction on a refusal -
with the clause no increment is lost, and the same cell without it loses
one. **The refusals' cost**: a `READ COMMITTED` locking read refused in
the window and a `REPEATABLE READ` one refused first-updater-wins each
leave the transaction failed, `COMMIT` refused, and the row's unit held
until `ROLLBACK`. **DC-Q1's cells**: a child `INSERT` passes the holder
and commits, and the holder's later `DELETE` is then refused RESTRICT -
or, under (b), the child waits. The cap's refusal carries `kLockCap`; a
claim survives a failed statement until `ROLLBACK` and ends at the
lifetime ceiling; a DDL's `X` waits for the holder, and an online `CREATE
INDEX`'s drain waits on the holder's `IX`. A `C_CANCEL` sent during a
walking locking read is seen at the next frame, and the claims the walk
took are released at that transaction's end. No frame is dirty after a
locking read (DC-Q9 (a)). The log's length is unchanged
across a locking read (DC-R5); an opted Waystone shape and a Cabin over
the predicate do not answer it (DC-R2); the `readonly` role, a `READ ONLY`
transaction (unpoisoned, counted) and autocommit are refused. The
simulator draws locking reads and reconciles them; the list-append checker
runs read-modify-write cycles through the clause.

**Documents DC-S3 owes**, each read at the close and amended where it
states today's behaviour: `txn.md` §5 - the modes paragraph (`:903-929`),
"KDS aborts retryably rather than re-reading" (`:862-866`), and the read
borrow's "a read is never refused and never waits" and "a reader never
waits, so it is always a sink" (`:1043-1061`), which stay true of a read
that does not claim - §1's `READ ONLY` section (`:130` on) and §9
(`:1291-1294`); `foreign-keys.md` §2c (`:281-296`); `parser-v2.md` I11
(`:133`) and I19 (`:210`); `protocol.md` §10 (`:140`; the dispatcher's comments call it §14, a stale number); `sched.md` §3 (a
locking read's walk does not yield); `known-gaps.md` Locks - the window
DC-Q3 (a) leaves, the fault net under sustained contention (`:629`), and
under DC-Q0 (a) the walk with no index and no yield (`:2018-2026`);
`manual/sql/sql.md` §4 (`:1131`), §5 (`:1637`, and `:1690-1696`, whose
`READ ONLY` list admits `SELECT`), §7 (`:1862`);
`manual/client/client.md` §4, §5; `CLAUDE.md`'s rows for transactions and
for the query language.

## 6. What DC does not do

- No index-driven or yielding write walk: both are the write path's gaps
  and stay where `known-gaps.md` records them.
- No queue behind a held row: a wake stays not a grant, and the decide
  slot stays unbuilt.
- No re-read loop and no change to first-updater-wins (but what DC-Q3 or
  DC-Q4 mark).
- No savepoint, no `SERIALIZABLE`, no predicate lock: a declared key
  window fences its keys as an `UPDATE`'s does and nothing else is a
  phantom guard.
- No unlock statement, no lock timeout setting, no persisted lock.
- No change to a `SELECT` without the clause: its view, its borrow, its
  plan, its trail and its reply.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-18

Written on worktree `db-fpi-cadence-dc-for-update` from `a4d2fb3b`
(`v3.4.12-42-ga4d2fb3b`), documents only. No suite was executed: the
commit changes no build input. Nothing was measured for it; §1.1's numbers
are CR's order's, at the versions it names.

**Reviewed by `critics-developer` before the commit.** It opened the cited
lines and the two results files §1.1 rests on. Corrected in place: the
CP-S0 and CQ-S0 results paths and lines, which the author had cited
through CR's order unopened; `:504-505` for the re-run itself; the
subquery runner at `step_vm.cpp:3054-3059`; `sched.md` §3 and
`protocol.md` §7, read in review and found to bear §1.4 out; DC-Q0's
statement that a range on the tuple key walks the relation; and
`protocol.md` §14, a stale number in the dispatcher's comments, to §10.
Applied from its findings: DC-Q1 is `[quiet-wrong]`, with the stale-sum
case and the RESTRICT refusal stated and a third option, the `U` form
under its own spelling; DC-Q3 states that either refusal fails the
transaction and keeps the row's unit; DC-R1 states both directions - what
is claimed and not returned, what is never skipped; the walk's dirtying of
every leaf it visits became DC-Q9, and DC-R6 follows its mark; §0 states
the scope of DC-Q0 (a) plainly - the point shape descends, everything else
is an unyielding, uncancellable whole walk, always so on a heap, and a
window holds the fence counter to the decide; DC-R3 and DC-R4 are
conditional on DC-Q0 (a); the role item was folded into DC-R7 and §1.5
corrected, a `readonly` connection reaching a claim in autocommit only;
the rule that restated the measurement's bar was cut and the counting item
became §2 step 9, the measurement taking the number DC-Q10; four tests
were added. The header's corrections of what the operator was told were
extended by three of these. **Rejected: none.**

### DC-S0 - the census (2026-11-19)

On worktree `dc-select-for-update` at `8e7c820f` (`v3.4.13-10-g8e7c820f`).
A read: no engine code, no benchmark, no suite executed for it (documents
only). §1's claims were re-read at this commit and hold; its line numbers
are `a4d2fb3b`'s, and the ones below are `8e7c820f`'s.
`src/server/command_dispatcher.cpp` is `CD`.

**The adopted marks.** DC-Q0..Q10 (a), on the standing go-ahead (§4). The
two `[quiet-wrong]` items were re-read before they were adopted:

- **DC-Q1 (a), `U`.** `S` and `U` are compatible and `U` refuses `IX`, `U`
  and `X` (`include/kds/txn/lock_table.hpp:261-268`, the matrix); a foreign
  key child's insert holds `S` on its parent row. So the premise holds: a
  child's share passes a locking read's claim, and the holder's later
  `DELETE` asks `X`. **The spelling `FOR UPDATE` therefore gives PostgreSQL's
  `FOR NO KEY UPDATE`**, and the manual says so in so many words (DC-S3).
- **DC-Q3 (a), the write's verdict.** `CheckWriteConflict` admits a head
  written by this transaction or by one its view sees and refuses every
  other (`src/txn/manager.cpp:190-209`); a committed-after-view writer is
  not in flight, so `NoteBlockingWriter` records no wait (`CD:12565`) and
  `EndWrite` poisons (`CD:13168-13174`); `Claim` borrows before it checks
  (`CD:11072-11078`), so the unit stays held. The premise holds.

**The reads the stage owed.**

1. **Design step 7, the shared hold (DC-Q9 (a)) - it holds.** A read fetch
   is a pin in `PinMode::kShared`, which is the page latch taken shared
   where the latch is armed, and `Get` is `kExclusive`
   (`include/kds/storage/page_store.hpp:142-152`, `:266-285`). Every writer
   of a tuple header fetches its leaf with `Get`, so a shared holder
   excludes it; where the latch is not armed one thread reaches the store.
   `Claim`'s no-re-read argument (`CD:11063-11067`) rests on two facts - the
   page is held against writers, and the walk carries no `co_await` - and
   both are true of a shared hold. Nothing the claim pass does under the
   leaf asks it exclusively: `Qualify` reads spills and the undo chain
   shared, `Claim` asks the lock table. So the pass fetches for read:
   `VisitRelation(kRead)` for the walk, as the collection pass already does
   (`CD:11121-11122`), and `btree::BtreeLookup` in its default read access
   for a key, where `LocateByTupleKey` takes `kWrite` (`CD:8885-8886`). The
   item does not go back to the operator.
2. **A cancel and a closed connection.** A write never reads the cancel
   flag: the takers are `SelectRun`'s slice, `BuildRun` and the session's
   next-frame gate. A wait in `AwaitWriteBlock` has no cancel term
   (`CD:504-515`) and ends at the holder's decide, as the victim, or at the
   fault net. A closed connection requests the cancel and rolls an open
   transaction back when the statement completes
   (`src/server/tcp_server.cpp:1092-1114`). The locking read inherits all of
   it: a cancel is seen at the next frame and fails the transaction.
3. **What bounds the reply.** `RETURNING` has no cap (`CD:10788-10831`): a
   KWP sink seals a batch at 64 KiB and holds every sealed batch until the
   statement ends (`src/server/kwp_session.cpp:263-289`); only a `SELECT`'s
   sliced walk ships while it runs. **Raised as DC-Q12**, adopted (a).
4. **A bound parameter and a portal.** Parameters are substituted into the
   text at `C_BIND`, so the handlers see no difference. A portal's
   `max_rows`, its suspension and `C_DESCRIBE` act on any described sink, a
   write's `RETURNING` included; the completion tag is the write's reply
   when `DispatchOutcome::returning` is set and `SELECT` otherwise
   (`kwp_session.cpp:762-768`). A locking read leaves it unset and so
   completes as a `SELECT`, with no code in the session.
5. **The sites that read the leading word**, each of which sees `SELECT`:
   `RequiredRole` (`CD:1418-1440`) and `ReadOnlyAdmits` (`CD:173-181`), so
   both gates are asked again after the parse (DC-R7); the archive's gate
   (`CD:1513-1517`), which a statement that writes no log record does not
   owe; the failed-transaction allowlist, which refuses it like any
   `SELECT`; `StatementKindOf`, under which it is counted a `SELECT`.
6. **An explicit transaction with an empty trail.** `BEGIN` draws an id
   and appends `TXN_BEGIN` (`src/txn/manager.cpp:138-155`); `COMMIT` appends
   `TXN_COMMIT` and waits at the session's rung whatever the trail holds.
   Both are the transaction's. The statement appends nothing (DC-R5).
7. **Statistics.** `sys.relation_stats` is written by `FinishWrite`
   (`CD:6560-6572`), which the locking read does not call; a write walk
   records no access shape; `sys.statement_stats` is keyed by the parse's
   fingerprint in `HandleSelect` (`CD:9635-9639`) and recorded for every
   statement.
8. **Tests that pin today's refusal.** None spells `FOR` after a `SELECT`:
   the trailing-token text is pinned for `FOO` (`tests/parser_test.cpp:138`)
   and `garbage` (`tests/assertion_ddl_test.cpp:414`). The golden corpus is
   `tests/testdata/parser_corpus.txt`, with no line holding `FOR`.
9. **DC-R9's argument.** The fingerprint is the lexer's, over the token
   stream, and no grammar reads it; DC changes no lexer rule and reserves
   no word, so no statement's hash moves. The production is entered at two
   positions - after a native block's pagination tail, and at the end of a
   query expression of the query grammar - and before DC the only tokens
   admitted there were `)`, `;`, the end of input and, after an `INSERT`'s
   source, `ON` or `RETURNING`. So no statement that parsed has `FOR`
   there, and every statement that parsed parses as it did. The corpus
   gains locking lines and its existing lines are unmoved (DC-S1).

**Corrections to the order's text.**

- §5's "`WITH ... FOR UPDATE`, which the query grammar reads" is wrong:
  `ParseQuery` takes `SELECT` or `(` only, and `WITH` is refused by the
  native grammar as a CTE, by name (`src/parser/parser.cpp:3793-3801`).
  `INSERT ... SELECT ... FOR UPDATE` is read by the query grammar and is
  refused there by name.
- §2 step 6's "`RETURNING`'s" emit is the pass's shape, not its renderer:
  the rows go through the `SELECT`'s own description and encoder
  (`DescribeStar`, `DescribeFields`, `ResultSink::EncodeProjectedRow`), one
  renderer per output shape, which is what makes a quiet locking read's
  rows the plain statement's byte for byte.
- §2 step 2's list of what refuses a locking statement is closed in one
  place: every consumer of a parsed `SELECT` but `HandleSelect`'s own
  branch reaches `exec::Compile` (`ANALYZE`, `WAYSTONE ON`, `CREATE PLAN`,
  `ADVISE PLAN`, a `SET` value's subquery), so the compile refuses a
  statement carrying the clause and no chain exists for one.
- A native refusal that is not about the clause must not answer a
  statement the query grammar reads up to the clause: `SELECT a + 1 FROM t
  FOR UPDATE` would be told that an expression is not supported, which is
  false. The clause's one production serves both grammars, and its refusal
  is the reply wherever it is the grammar's own.

**Raised and adopted**: DC-Q11 (which refusals fail the transaction) and
DC-Q12 (the reply's bound), both (a), in §4.

**No item goes back to the operator.**

### DC-S1 - the grammar and the refusals (2026-11-19)

On worktree `dc-select-for-update` at `eaeb26ea`. The clause is one
production, `Parser::ParseLockClause`, read after the pagination tail of
the outermost native block into `SelectStmt::lock` and refused by name
everywhere else, by both grammars. `exec::Compile` refuses a statement
carrying it; `HandleSelect` hands a parsed one to `HandleLockingRead`
before anything else is asked; the gates are refused at the byte of `FOR`
and fail no transaction. A well-formed locking read was itself refused
`NotImplemented` at this commit. The golden corpus gained 18 locking lines
and none of its existing lines moved, which is DC-R9's pin.

**Reviewed by `critics-developer`**, which found no path dropping the
clause. Applied, all seven: a statement whose clause the native grammar
accepted and which then failed for a trailing token was answered with the
query grammar's refusal - the parser now says a clause was *met*
(`met_lock_clause()`), and such a statement is answered by the grammar
that read it; the gate cells could not tell their gate from the stage's
own last refusal, so each now asserts the words of its own; the role's
refusal carries its byte; three comments that named a file that does not
exist or a neighbour that had moved; `sys.<unknown>` no longer answered as
a catalog view; a redundant `GROUP BY` test and an unused parameter cut; a
plans check that searched for the letter `p`. **Rejected: none.**

**Suite**: 0 failed of 4766 at `eaeb26ea`'s tree (0 failed of 4765 before
the review's fixes). Overhead not measured; measured at the milestone's
close.

### DC-S2 - the claim pass and the emit pass (2026-11-19)

On worktree `dc-select-for-update` at `f6a12f5d`. `RunLockingRead` is an
`UPDATE`'s row walk with no `SET` list, under the session's transaction:
the relation's `IX`, the declared window or each row's unit in `U`,
`UPDATE`'s arms in `UPDATE`'s order, the leaves fetched for read; a held
row ends the pass with the conflict `DispatchAsync` waits on and the
statement is run again whole; then the emit pass, through the `SELECT`'s
own description and encoder. What the stage found and fixed in the design
as written:

- **§2 step 6's "the page's version, which no other transaction can
  replace" is asked, not assumed**: the emit checks the row it re-reads -
  present, not delete-marked, its writer this transaction or one the view
  sees - and answers `Corruption` otherwise, so a breach of DC-R1 is a
  refusal and never a row.
- **DC-Q11 as built**: the resolve and the compile return without ending
  the write scope, so they fail nothing; every later end is
  `EndWriteWith`, `FinishWrite` less its `sys.relation_stats` count.
- **The statement's view is the one `HandleSelect` took**, handed down:
  inside an explicit transaction the boundary is taken once a statement.
- **`sys.<unknown> FOR UPDATE`** is answered by the relation's resolve, as
  any missing relation is.

**Reviewed by `critics-developer`**, which traced every other writer of a
claimed row, the shared hold, the re-run and the early exits and found no
wrong or skipped row. Applied, all thirteen: **DC-R4 had no pin** - the
walked cell ran on the text protocol, whose sink is rebuilt each run, so
rows sent during the claim pass would have passed it; the cell is now on a
session's own sink, which a re-run does not rewind, and asserts nothing
has left while row 1 is claimed and row 2 held; a claimed row not found at
the emit answered `NotFound` and is now the same `Corruption`;
`CompileSelectList` decided `*` from an empty list, which an aggregated
statement's is too, and takes `star()` from its caller; **a deterministic
cell for the `READ COMMITTED` window** through the after-view seam, with
the commit made by the other core's dispatcher
(`LockingReadRigTest.AReadCommittedReadRefusedInItsWindowFailsItsTransactionAndKeepsTheUnit`),
which the order had thought unreachable on one core; the cells §5 owed and
lacked - the transaction's own version returned, a claim outliving a later
failed statement, a cancel during a locking read, each waiter at both
decides; three duplications cut (`EndWriteWith`, `ReadRowByKey` shared
with `RETURNING`, `RenderedThrough` shared with `SelectBody`);
`ProjectColumns` returns its list; the `WHERE` compiled before the select
list, as a chain's is; a redundant scratch clear; a `sys.relation_stats`
assertion that could not fail, cut; and the Cabin cell now shows the plain
statement is a Cabin probe. **Rejected: none.**

**Suite**: 0 failed of 4792 at `f6a12f5d`'s tree. A first run failed one
cell, `StrictParkRigTest.AnExplicitStrictCommitParksAndPublishesAtItsSync`,
the one-off `known-gaps.md` (Testing) records, beside another session's
build; eight repeats of it passed and the run repeated whole passed.
Overhead not measured; measured at the milestone's close.

**Tests §5 owes that DC-S2 did not write**: a `tsan` run of the cross-core
cell (the `bs-nightly` build's; not executed for DC), and a portal's
`max_rows` over a locking read, which is the session's code for any
described reply. Both are in `known-gaps.md` ("Locking reads (DC)").

### DC-S3 - the close (2026-11-19)

On worktree `dc-select-for-update`, the code at `7e18598d`
(`v3.4.13-16-g7e18598d`).

**The simulator and the history checker** (`cd80d31d`). With the shapes on,
40 in 100 of the simulator's reads by tuple key, by range and by `v` inside
a `READ WRITE` transaction carry `FOR UPDATE`, drawn from a stream of their
own so the pinned corpus is unmoved; the oracle reconciles each as the
plain read it replaces. `scripts/sim.sh` (Debug, 4 fresh seeds): 280 runs,
0 failures. A suite cell runs a clean shapes run that must reconcile its
locking reads. The list-append checker (`bs-nightly`) gained a
read-modify-write cycle - a key's list read, then written back whole with
one more element - and
`BsHistory.ReadModifyWriteThroughALockingReadLosesNoAppend`: a control by a
plain read under `READ COMMITTED` must lose committed appends, and the same
seeds through the clause, and one `REPEATABLE READ` seed, must show no
anomaly. `kds_bs_tests BsHistory.*`: 4 of 4 passed, run by hand; no
`bs-nightly` run was made. **Reviewed by `critics-developer`**: no bug.
Applied: the control counted any anomaly and now counts the checker's own
"was lost" and fails on every other kind; the suite cell above, which the
stage lacked; a comment's layout. **Rejected, one nit**: `MaybeLock`'s
`READ ONLY` test cannot be false as the stream is drawn today, and stays
as the rule the workload cell asserts.

**The close measurement** (DC-Q10 (a); `ck-tester`, Release, A `8e7c820f`
against B `cd80d31d`, then against C `7e18598d`; two servers of A's binary
as the floor).

- **The descriptive cell** (B, `cores = 4`, `group`, `READ COMMITTED`,
  eight sessions on one row, three cells of 6 s): `BEGIN; SELECT ... FOR
  UPDATE; UPDATE; COMMIT` committed 90,681 transactions and **lost no
  increment** - each cell's final value equals its committed count - at
  5,574, 5,408 and 4,128 transactions a second. The same loop with a plain
  `SELECT` committed 111,545 and lost 77,905 increments (69.8 %), with no
  error. **DC-Q3 (a)'s window refused 2,512 of 93,193 locking reads
  (2.70 %)**, every one `TXN_CONFLICT ... was written by transaction`; no
  deadlock and no fault net. The clause cost 11 to 33 % of the plain
  loop's rate, in the tail: p99 1.7 to 3.7 times the plain loop's.
- **The barred cells** (a point `SELECT` and a point `UPDATE` without the
  clause, `cores = 1`, server CPU a statement, pooled over clean
  lifetimes). At B the `SELECT` read +0.10, +0.23 and +0.17 us against A
  at 200, 1K and 10K rows - inside the floor, a lean not excluded - and
  the `UPDATE` was not resolved from a floor of 2 to 3 us.
- **A remedy, applied and measured again** (`7e18598d`). Two refactors of
  DC-S2's review cost a statement without the clause what it had not
  paid: a select list built and moved into the chain (one allocation a
  compile) and a write's outcome passed by value through `EndWriteWith`
  (two moves a write). Both are gone; **reviewed by `critics-developer`**,
  correct and equivalent, one nit rejected as it advised (a second
  four-byte prefix test in `FinishWrite`). At C: **the `SELECT` is +0.13,
  +0.10 and +0.10 us against A - the bar met, inside the floor, the lean
  of about 0.5 % of 20.4 us not excluded and not moved by the remedy** (C
  against B within 0.05 us); **the `UPDATE` is not resolved from its
  floor** (+0.19, -0.35, -1.12 us of about 75 us), the remedy reading -1.59
  us at 200 rows and nothing at the other two sizes. No cell is slower
  outside its own spread. What a statement without the clause still pays
  is a peek at its tail, a test of a parsed field in `HandleSelect` and in
  `exec::Compile`, and eight vector moves; the residue of the lean is not
  separated from a binary's layout on a host with no PMU.
- **Cut**: three and four arms in place of the CX bisect's ten; lifetimes
  that ran beside another session's build or suite excluded (three of
  eight, then two of seven); no PostgreSQL twin. **Time**: about 41 minutes
  for the first run and about 23 for the second, building included, about
  9 minutes held by the quiet gate outside both.

**Suite**: 0 failed of 4794 at `7e18598d`'s tree, and at `cd80d31d`'s.

**The documents** (`7c36f610` and the close's commit): `txn.md` §5 ("A
locking read"), §1 and the verdict table's paragraph; `parser-v2.md` I11a
and I19; `protocol.md` §7 and §10; `sched.md` §3; `foreign-keys.md` §2c;
`known-gaps.md` ("Locking reads (DC)"); `manual/sql/sql.md` §4, §5, §7;
`manual/client/client.md` §5; `CLAUDE.md`'s rows for transactions and for
the query language. `txn.md` §9 needed no change: it names `SERIALIZABLE`
and savepoints, and no ruling against a locking read stood there.

**Not done, stated**: no `tsan` and no `bs-nightly` run for DC; the
overhead bar is met by the floor and the 0.1 us lean on a point `SELECT`
is neither shown nor excluded; DC-Q0 (b), DC-Q1 (b)/(c), DC-Q3 (b), DC-Q5
(b)/(c), DC-Q6 (b)/(c), DC-Q7 (b) and DC-Q12 (b) are not built and stay
open to the operator's own mark.

**The sync**: `origin/main` was `8e7c820f` at the close, the commit DC
opened from, so the merge brought nothing.

DC is closed on the work branch. It is not merged, not pushed and not
tagged: each waits for the operator's word.

### Landed (2026-11-19)

On the operator's word "push no verify" (`raft-marks-2026-11-19.md` §6):
the work branch at its close merged `--no-ff` onto `origin/main` at
`8e7c820f`, which had not moved, and pushed with `--no-verify`. The hook
was skipped; the suite ran 0 failed of 4794 at `7e18598d`, on the build
inputs pushed. Not tagged, and no version named.
