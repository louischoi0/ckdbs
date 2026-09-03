# AR2 — Architecture Revision: The Borrow Model

Status: DRAFT, pending operator ratification
Author: CLA, 2026-09-03, on worktree `ar2-borrow-model` against `183b956`
(`v2.7.0-171-g183b956`)
Scope: what replaces write ownership under AR0-4 — the unit, mode, holder
and scope of every tenancy a mutating or fencing operation takes — and
the spec sentences that rested on a permanent owner (`crosscore.md` CC3,
`cabin.md` §6, `assertion.md` AS4, `foreign-keys.md` F3, `namespace.md`
NS10, `physical-optimizer.md` R6)
Claim tags: a `path:line` citation **is** the `[source-read]` tag, at
`183b956`, and AR2-V checks every one; `[measured]` with the
`bench/v3.0.0/` file that holds the number, or the AR0-V row that
attributes it; everything else `[design]`. Nothing here was measured by
this draft.
Relation to AR0: a refinement of AR0-4 inside M2 and M3. Nothing here
precedes M1, and nothing here touches M1 (§9).
Relation to AR1: one crossing, §5.1's last paragraph.

**Read AR2-V (appended) before citing the body.** It is the source read
at `183b956`, and it is what the tree says wherever the body disagrees.

---

## 0. Decisions proposed

One line each. The rule that carries each is in §4; the operator's items
are §7, and nothing here is decided outside them.

- **AR2-1** Write ownership becomes **borrowing**: a scope-bounded tenancy
  over one of five units, and no unit has a permanent writer (§1, R1).
- **AR2-2** Units relation, range, page, slice, tuple; families **latch**
  (the page, outside the hierarchy) and **lock** (the other four, with
  intention modes) (§2, R2, R3).
- **AR2-3** Every operation borrows at the unit §3's table states; an
  engine-issued `INSERT` borrows a range's id block, never a page (R5).
- **AR2-4** A borrow ends with its scope, never with a clock (R1, E11).
- **AR2-5** Intention modes, no escalation, a cap that refuses (R3, R4, E2).
- **AR2-6** A slice is keyed in key space (R6, E4).
- **AR2-7** Affinity is a statistic borrows leave behind, stored where
  AR0-M5 put it (R8, E5).
- **AR2-8** `UPDATE` and `DELETE` execute where the session is; an
  `INSERT` does so only under spreading; DDL and a named-key admission
  still ship to core 0 (R5, R12, E7).

---

## 1. Background — what AR0 decided, and the one word this draft adds

AR0-4 decomposed core ownership into three properties and retired the
first `[source-read: instructions/v3.0.0/ar0-architecture-revision.md:35-40]`:

| property | fate under AR0 | what replaces it |
|---|---|---|
| write-serialization authority | retired | "row locks + page latches under single-LSN MVCC" |
| execution affinity | retained as an optimizer hint | D10, weight 0 until RW-C1 is attributed |
| allocator authority | retained | range-unit id issuance |

The operator's marks of 2026-09-03 fix the substrate the replacement
runs on: D1 **(b)** conditionally, D2 **(a)**, D8 as proposed, D9 **(a)**,
D11 (the R5 mover retired), D15 (`v3.0.0`)
`[source-read: ar0-architecture-revision.md:281-492]` (AR0-M's header at
`:281-288`, the six marks AR0-M1..AR0-M6 at `:290-492`). D3–D7, D10,
D12–D14 and D16 are pending.

**What "row locks + page latches" leaves unsaid.** The retired property
answered three questions with one word: who may write a unit, at what
granularity the answer is given, and for how long it holds. AR0's
replacement names two mechanisms and no unit table. Every operation the
engine runs — an engine-issued `INSERT` against a tail-append heap, an
assertion check that needs a phantom fence, a parent `DELETE` whose
children have no covering structure, a relayout mover that needs the
relation to itself — must say which unit it takes, in which mode, for
which scope, and each of those is a place a quiet wrong answer can hide.
**"Borrow" is the word for the replacement, chosen so that every
operation must state its unit and scope**, and §3 is the table AR0 does
not have.

**The measured premise on both sides.** `[measured]`

- The cost of owner routing on a long transaction: scenario 2 at
  `cores = 8` loses 46% (group) and 48% (strict) against `cores = 1`,
  attributed to the shipping hop compounding across an eight-statement
  transaction
  (`bench/v3.0.0/results-scenario2-freight-v2.7.0-157-gf6ed10c.md:62-92`).
- The cost on short autocommits: scenario 0 gains 7.6–7.7% at `cores = 8`
  (`bench/v3.0.0/results-scenario0-stockmarket-v2.7.0-157-gf6ed10c.md:80-101`).
- A peer writing its own relation locally under one stream: p50 and p99
  within 1–5% of core 0's own
  (`bench/v3.0.0/results-wal-single-stream-v2.7.0-157-gf6ed10c.md:78-128`).
- The old ~3 ms cross-owner commit decomposes into three serialized
  device syncs of the 2PC protocol, not scheduler latency — attributed
  by AR0-V's first table row to files at `1769487`, not measured under
  `bench/v3.0.0/` (`ar0-architecture-revision.md`, AR0-V).

Read together: a local write costs what an owner's write costs, and the
hop costs up to half the throughput on the shape scenario 2 has — a hop
with **no 2PC in it**, since every relation in those cells is
core-0-owned, so AR0 §4.5's retirement of XD shrinks none of the 46%.
What no cell answers is
the hot-row shape — scenario 2's `operations` update — which one core
serializes today at zero lock cost and which a borrow turns into lock
waits plus a hot page migrating across cores. §9's cell C3 is that
number, and it cannot run before M1.

---

## 2. Definitions

**Unit.** One of five, nested in this order:

- **relation** — the catalog object, `sys.tables` row and everything
  reachable from it.
- **range** — CC8's `[lo, hi)` of one relation, page-aligned by CC10
  (`min_key` is the split key) `[source-read: docs/spec/crosscore.md:35,37]`.
  A relation with no `sys.ranges` row is one range.
- **page** — one frame of the pool.
- **slice** — a key interval `[lo, hi)` within one relation's key space,
  bounded, not necessarily page-aligned. The request's "range per page"
  is the slice whose bounds are one page's `[min_key, next.min_key)`; it
  is a special case, not the definition (§4 R6 says why).
- **tuple** — one Keystone id.

**Borrow.** A tenancy over a unit in a **mode**, by a **holder**, for a
**scope**, released by the scope's end event.

| family | unit | holder | scope | released by | waits | deadlock |
|---|---|---|---|---|---|---|
| latch | page | a task | critical section | leaving the section | spin or yield, no queue | impossible by acquisition order |
| lock | relation, range, slice, tuple | a transaction | the transaction (an autocommit's is its statement) | decide — commit or abort | D13: asynchronous, the waiter parks and is re-enqueued on grant | D12: wait-for graph, timeout aborts the waiter |

**Modes.** `S`, `X` on the unit borrowed; `IS`, `IX` on every ancestor
in the lock family. Standard compatibility: `IS` conflicts only with `X`;
`IX` with `S` and `X`; `S` with `IX` and `X`; `X` with everything. No
`SIX`, no update mode in v1 `[design]`.

**Fence.** A borrow taken not to mutate the unit but to keep another
transaction from mutating it: a gap lock is an `S` or `X` fence on a
slice; D9(a)'s shared lock on a parent row is an `S` fence on a tuple.

**Two borrow-like things already in the tree, named so they are not
rebuilt** `[source-read]`: the reader lease (`docs/spec/txn.md:296-309`;
a `ReaderLease` beside every autocommit snapshot, folded into
`ReadHorizon()`) is a shared borrow of a version horizon; the id block
lease (`include/kds/catalog/catalog.hpp:643` `AllocateRowIdRange` carves
it, `include/kds/catalog/row_id_lease.hpp:36` `RowIdLease` holds it, one
run per relation per core) is an exclusive borrow of a range's ids by one
core. The *page*-id lease its design copies is a different structure
(`include/kds/storage/extent_lease.hpp:46` `LeasedIdSource`, non-blocking
refill), and AR2 renames neither.

---

## 3. The unit table — normative for every mutating or fencing operation

| operation | unit borrowed | mode | family | scope | today, or the ruling that builds it |
|---|---|---|---|---|---|
| `INSERT`, pk issued by the engine | the range's **id block**, plus `IX` on the relation | `X` on the block (implicit: one core holds it) | lock (implicit) + latch on the tail page | block: the lease; tail page: critical section | `AllocateRowIdRange`; `heap-and-tuple.md:180` "row-id leases work on every relation"; R5 |
| `INSERT`, pk **named** — at or above the mark, or below it on a btree relation | the row above's units, or the leaf page and the new tuple; **and the relation's `sys.tables` row**, a core 0 catalog page | `X` on the tuple, `IX` on the relation; the catalog write is core 0's | lock + latch | transaction; critical section | ships to core 0 as today (`heap-and-tuple.md:182` "a peer core refuses a named key, per row"); R5's third paragraph |
| `UPDATE`, `DELETE` | the **tuple**, `IX` on the relation; the page for the write | `X` | lock + latch | transaction; critical section | M2's row lock: the Keystone lock byte is its fast path and D2(a)'s table its wait path (R2's second paragraph; `docs/spec/txn.md:401-402`, `include/kds/storage/heap/heap_page.hpp:127-128`, AR0-V4) |
| `SELECT` under SI (D1(b)) | none for visibility; `IS` on the **relation** while the statement holds a position in it | `IS` | lock | the statement | AN's snapshot LSN decides what is visible; the `IS` exists only so a relation `X` (DDL, the mover) waits for a positioned reader — §5.4, **E12** |
| assertion check (D8) | the **slice** of the group key | `S` while checking, `X` when the writer changes the group's state | lock + latch on the Bound Cabin page | transaction | D8; §5.2 |
| FK forward check, child insert (D9(a)) | the parent **tuple** | `S` fence | lock | the child transaction | D9(a); §5.3 |
| FK reverse check, parent delete | the child **slice** of the fk value when a covering Bound structure exists; else the child **relation** | `S` fence | lock | transaction | AR0-M3 item 3 left this open; **E3** `[quiet-wrong]` |
| Observational Cabin bank | none | — | — | — | §6a's rule survives in content; its test is re-expressed against AN-S2's view — **E6** `[OPEN]` |
| Bound Cabin append | covered by the writer's slice or tuple borrow; the Cabin page | `X` inherited; latch | latch | critical section | AR0 §4.2 |
| DDL: `CREATE`/`DROP`/`ALTER`, `CREATE INDEX`, range split | the **relation** | `X` | lock | the DDL transaction | executes on core 0, unchanged (`crosscore.md:38,40` CC11, CC13) |
| relayout mover (future) | the **relation** | `X` | lock | the maintenance task's run | R6's precondition becomes checkable once readers hold `IS`; §5.4 |
| checkpoint, eviction writeback, sweep | the **page** | `S` for a read-out, `X` for a write-back | latch | critical section | AM-R3 |

Three rows carry the weight. The first says an `INSERT` takes no page
*lock* — its page tenancy is a latch on the tail, and its unit of
parallelism is the block (R5). The `SELECT` row is the one cost AR2 puts
on a read (E12). The FK reverse row says a check may have to borrow a
whole relation (R7), a refusal-class fact rather than a performance one.

---

## 4. Rules AR2-R1..R12 (CLA's proposals)

**AR2-R1 — Scope-bounded, never time-bounded.** `[quiet-wrong]`
A distributed lease has an expiry so a dead holder cannot block forever.
In one process under cooperative scheduling an expiry does the opposite:
a holder that keeps writing after its lease lapses turns a wait into a
silent lost update, with no gate anywhere. So: a latch ends when its
critical section ends; a lock ends at decide; D12's safety timeout
**aborts the waiter's transaction** and never touches the holder. There
is no lease renewal, no expiry, no revocation path.

**AR2-R2 — Two families, and the latch is outside the hierarchy.**
`[design]` A page latch is held for one critical section by a task, is
never held across a park (`docs/spec/sched.md:42`'s suspension-safety
rule already forbids it for a page span), has no wait queue, and is
proved deadlock-free by acquisition order rather than by detection. The
lock family is D2(a)'s partitioned table with D12 and D13. **Acquisition
order:** a lock is acquired before the page it protects is latched, and
never under a page latch — so a lock wait, which may park, can never
park a latched page. The page latch's order against the WAL latch is
AM-S1's; the window latch is taken with the WAL latch released
(`instructions/v3.0.0/workorder-an-read-view.md:458` AN-R9). The order
is stated in the lock manager's subsystem header per `rules.md` §3.

**The tuple lock has one home and two paths, and the order above is
about the wait.** The Keystone lock byte (`docs/spec/txn.md:401-402`) is
the **fast path**: a writer takes the tuple by CAS on the byte under the
page latch, which is where the byte is. On conflict — the byte already
carries a live transaction — the writer **releases the latch**, registers
the tuple in D2(a)'s table and parks there (D13); the holder's decide
clears the byte and wakes the table's queue. So no wait ever happens
under a latch, the table carries only contended tuples, and an
uncontended write touches no table entry for its tuple. The relation's
`IX` and `IS` are the table's, always (R3).

**AR2-R3 — Intention modes, and why.** `[design]` A relation-level `X`
(DDL, the mover) must conflict with a tuple-level `X` somewhere below it.
Without `IS`/`IX` on every ancestor the conflict test is a scan of the
descendants' locks; with them it is one compatibility check at the
relation. D2(a)'s hash-partitioned table takes a relation's lock as one
well-known key. **That key is the one cost AR2 adds to every workload**:
every writing transaction touches the relation's `IX` entry, and every
positioned reader its `IS` (E12), in a shared table with atomics, paid
whether or not two transactions ever meet on a tuple. §9's C3 prices it
beside the hot row. The ancestor chain in the lock family is
relation → range → {slice, tuple}; the page is not in it.

**AR2-R4 — No escalation; a cap refuses.** `[constant]` Escalation
converts a transaction's fine borrows into a coarse one mid-flight, which
is a new wait the transaction did not ask for and a new deadlock edge.
V1 has none. A transaction that reaches the per-transaction borrow cap
is refused and aborts — the "a cap refuses, never truncates" rule. The
cap is one new key, `max_locks_per_txn`, a genuinely new quantity no
existing setting expresses. **Both its value and its code are E2**: the
code is wire-visible (`docs/spec/protocol.md` §11 pins the registry and
the retryable bit), and CLA proposes `ResourceExhausted`
(`include/kds/base/status.hpp:69`), non-retryable since a retry meets
the same cap — noting that the code's own comment today reads "a
statement spent its per-statement work budget" and would widen.

**AR2-R5 — `INSERT` borrows a range's id block, not a page.** `[design]`
A heap relation grows by tail append and its ids ascend, so the tail is
the only legal insert page (`docs/spec/heap-and-tuple.md:36-47`). Every
`INSERT` into one range therefore serializes on one page's latch whatever
the lock granularity, and insert parallelism comes only from **ranges**:
per-core id blocks aligned to per-range chains
(`docs/spec/heap-and-tuple.md:202-248` §4.1a). On `ar2-borrow-model` at
`183b956` that mechanism is built and disarmed: `range_size_ids` ships as
`kRangeSizeOff` (`include/kds/server/range_alloc.hpp:90`), and arming it
is D5/D6.
**Consequently:**

- with spreading **off**, a relation is one range whose block is held by
  one core at a time; an engine-issued `INSERT` arriving on another core
  is routed to the block's holder — today's shipping, kept as R12's
  affinity route — and the pk stays a sequence;
- with spreading **on**, an `INSERT` executes locally into its core's own
  range, and the pk is an identity and nothing more (§4.1a). R12's "local
  by default" reaches `INSERT` only under this arm.

**A named pk is a catalog write, and neither arm reaches it.** Admitting
a named key writes the relation's `sys.tables` row — the high-water mark,
or the `key_order` flip — and that page is core 0's under CC11, which is
why a peer refuses a named key per row today
(`docs/spec/heap-and-tuple.md:182`). AR2 does not make a catalog page
borrowable, so a named-key `INSERT` from any core but 0 **ships to core
0** as DDL does (R12), whatever spreading's state. Below the mark it is
btree-only (§4.1) and lands in a leaf by key — leaf latch, tuple `X`, no
block — on core 0.

**AR2-R6 — A slice is keyed in key space.** `[quiet-wrong]` A gap is a
key interval, and a fence over it must survive every physical event that
moves keys between pages. Two such events exist or are planned: the
btree leaf division — today the one operation that bumps a page epoch
(`docs/spec/heap-and-tuple.md:28-34`) — and a future relayout
(`physical-optimizer.md:102-148`). A slice keyed by page is silently lost
by either; a slice keyed `(rel_oid, [lo, hi))` is not. The page is a
locality hint for the lock table's partitioning and nothing more. A
page's own slice is `[min_key(A), min_key(B))` by §2's definition — both
bounds immutable, so the partition a key falls in is fixed for the
pages' lives — a convenience for partitioning, not a correctness
dependency.

**AR2-R7 — The FK child side borrows what can be named.** `[quiet-wrong]`
AR0-M3's third item recorded that a gap lock needs a structure to name
the gap in and that the FK child side has none — `child.fk_col` carries
no required index, and an Observational Cabin holds nothing for a value
nobody probed (`ar0-architecture-revision.md:399-408`). The honest rule:
a parent `DELETE`'s reverse check takes an `S` fence on the child
**slice** when a Bound structure covers the fk column, and on the child
**relation** when none does. Coarse is a wait, never a wrong answer; the
alternative — checking under no fence — is the phantom D8 exists to
close. **E3** is the operator's mark on this, because it converts a
refusal-class path into a lock-protected one.

**AR2-R8 — Affinity is derived from borrows.** `[design, E5]` D11 makes
affinity a catalog fact updated from statistics with no data movement,
and AR0-M5 fixes **where** it lives: `sys.ranges.owner_core` re-scoped
from an authority to a hint, not a new `sys.range_affinity` relation,
which would be a second name for the quantity
(`ar0-architecture-revision.md:455-465`). AR2 proposes only the **feed**:
the lock manager's grant count per `(range, core)`,
decayed by `physical-optimizer.md` R1's lazy-decay score — one decay
implementation (`include/kds/stats/decay.hpp`) — so the core that borrows
a range most is its affinity. This is a **new collector**, which `physical-optimizer.md`
R2 says must be specified in the layer that owns collection; it is not
an extension of `sys.access_stats`, whose key is `(kind, rel_id,
column_mask)` with no core and a 4,096-shape cap
(`include/kds/stats/access_stats.hpp:30`, `include/kds/catalog/rows.hpp:714`).
D10's weight stays 0 until RW-C1 is attributed; the collector exists so
that the attribution has a number to read.

**AR2-R9 — Compiled out at `cores = 1`.** `[measurement-gated]` Every
borrow primitive is a no-op at one core: the latch per AM-R3, the lock
family likewise. AM-R3's sub-decision — a run-time branch or a
compile-time one — is inherited unchanged, and its measurement decides
for both families at once.

**AR2-R10 — Deadlock.** `[design]` D12's wait-for graph on the log core
with a safety timeout; the timeout aborts the waiter (R1). Multi-unit
borrowing widens the graph's edge set — a slice fence waiting on a tuple
`X` is a new edge shape — and the detector is over transactions, not
units, so the shape does not change it.

**AR2-R11 — Auxiliary structures.** `[design, E6]`

- A **Bound** Cabin is updated in the writer's transaction under the
  writer's slice or tuple borrow, with a latch on the Cabin page (AR0
  §4.2). Its full coverage (`cabin.md` §12.1) is what makes every group
  key a lockable slice, including one with no entry yet.
- An **Observational** Cabin is read at the snapshot LSN. `cabin.md`
  §6a's rule — bank a set only from a view nothing can contradict — keeps
  its content: a transaction unresolved at the walk can commit rows the
  walk could not see. Its **test** (`view.in_flight_count == 0 &&
  view.own_trx_id == kNoTrxId`, spelled as the decline at
  `src/exec/step_vm.cpp:851-852` over the fields at
  `include/kds/txn/read_view.hpp:68-74`) names **one** field AN-S2
  retires: `ReadView` loses `up_to_trx_id`, `in_flight` and
  `in_flight_count`, while `own_trx_id` survives as the second branch of
  AN-R3's `Visible` (`workorder-an-read-view.md:326,334`). So the
  re-expression is the in-flight half alone; it is AN's to write, and AR2
  fixes only that the rule is kept, not relaxed.
- `cabin.md` §6's soundness argument rests on "statements for a relation
  run on its owning core (D3) to completion" (`docs/spec/cabin.md:361-374`)
  and states that if the execution model changes, §4–§5 "must be
  redesigned, not relaxed". AR2 takes that sentence at its word: §5.1
  is the redesign, and it is a trade written as AM-R7 writes one.

**AR2-R12 — Execution locality.** `[measurement-gated, E7]` A write
executes on the core the session landed on, under §3's borrows. Shipping
survives in two forms: **DDL to core 0** (CC13, unchanged, because
catalog pages are core 0's under CC11 and AM-R5), and an **affinity
route** — a session may be routed to a range's affinity core for the
warm-cache benefit D10 prices, which for a spreading-off `INSERT` is not
optional (R5). `UNKNOWN_OUTCOME`, the lost-answer class of a shipped
write, shrinks to the two forms that still ship. The default — local
unless routed, or routed unless local — is E7, decided by §9's cells.

---

## 5. Consequences per feature

### 5.1 Cabin

`cabin.md` §6 deletes the write-racing-scan hazard by core ownership.
Under AR2 the hazard returns and is closed twice: for a Bound set by the
writer's borrow (the append is inside the transaction that took the
slice), for an Observational set by §6a's bank rule. §6 is rewritten as
a trade — what the ownership argument gave, what replaces it — rather
than deleted, so the spec can still be audited against the engine that
made the argument. The per-core store ("a store per core: the relation's
owner observes, appends and serves", `CLAUDE.md`'s Cabin row) becomes a
store per relation under M3's topology.

**The one crossing with AR1.** AR1 generalises the Cabin's *shape*
(`F`, `G`, five shapes) and states its own single crossing with AR0 in
its §11. AR2 touches the Cabin's *lifecycle* only — who appends a Bound
entry and under what borrow, when an Observational set may be banked —
and no shape. The two drafts do not share a decision.

### 5.2 Assertion

§6's reservation protocol (`docs/spec/assertion.md:270`) becomes the
slice fence of §3, and §6.1's two ownership bullets — Bound state lives
on the relation's owner core; "the enforcing core is the owning core, at
every mount too" (`assertion.md:272-285`) — become statements about the
Bound Cabin's *home* rather than its only writer (§8). **What survives
unchanged** is §6.1's refusal
rule: a core that knows of an assertion it cannot enforce refuses the
relation's writes. Under AR2 "cannot enforce" means "cannot take the
fence" — no Bound Cabin covers the group key — and the answer is still
a refusal, never a check skipped.

### 5.3 Foreign keys

Under D9(a) the forward check is an `S` fence on
the parent tuple for the child transaction's duration; the reverse check
is R7. The reference intent, the dispatch-fork park and the enrolment
counters AR0-M4 lists go with D9.

### 5.4 Physical optimizer

R6 requires a mover to run with "no in-flight statement holding a
position on the relation and no open transaction whose undo trail names
addresses in it" (`docs/spec/physical-optimizer.md:66`). Today nothing
can check that, and a relation `X` alone checks **half** of it: an open
transaction with writes in the relation holds `IX` until decide, so the
undo-trail clause is implied — but a `SELECT` under SI takes no borrow
for visibility, and a chain walk holds no page span across a park
(`docs/spec/sched.md:42`), so a mover would move tuples under a parked
reader's position and the walk would miss or repeat rows. The first
clause therefore needs a **read borrow**: an `IS` on the relation for the
statement's duration, §3's `SELECT` row, the one cost AR2 puts on reads
(R3, E12). With it the precondition is one compatibility check — the
first mechanism the mover has had; without it the mover's gate is
writers-only and R6's first clause stays unenforced. Nothing here moves
§6's gates. Noted in AR2-V: §6's first gate cites a premise `txn.md`
§4.1 no longer holds.

### 5.5 Statement shipping and cross-owner transactions

CC3 is retired (§8). `cross-owner-txn.md`'s
coordinator/participant model is already retired by AR0 §4.5 (XD) and
AR2 adds nothing to that. What stays: CC13's DDL route, the typed answer
edge for shipped reads (`crosscore.md` §4a), and the executor as the
affinity route's carrier.

### 5.6 Namespaces

NS10 — "a namespace selects the core that owns the relations created in
it" (`docs/spec/namespace.md:3-4`) — becomes "a namespace declares the
**affinity** of the relations created in it". The verb changes a
user-visible contract: today a misplaced namespace is a refusal or a
hop; under AR2 it is a slower answer. `PlacementPolicy::kNamespace` is
re-read as an affinity policy; `sys.tables.owner_core` and
`sys.ranges.owner_core` (`include/kds/catalog/rows.hpp:104,955`) keep
their bytes and change their meaning to affinity. **E8.** `core_count`
is pinned on two grounds, and only one of them is these fields: AL-S2's
(`workorder-al-m0-single-wal.md:492`) is that a mount at fewer cores
leaves relations owned by a core that does not exist — under AR2 a core
to *prefer* rather than one that must write — and `docs/spec/wal.md:58`'s
is the anchor's warm-up, defined over "every core has published", which
nothing in this draft touches. E9 carries both.

### 5.7 The range refusals

A multi-owner relation today refuses a write naming no pk, a join,
`LIMIT`/`OFFSET`, any sort but `<pk> ASC`, any read inside an explicit
transaction, and `CREATE INDEX`/`CABIN`/`ASSERTION`/FK once it has two or
more ranges (`CLAUDE.md:74`); on `ar2-borrow-model` at `183b956`,
`RangeEligible` names six gates
(`include/kds/exec/range_eligible.hpp:83-91`) of which **four are ever
returned** — `kIndex` and `kCabin` have not been since SB3 of 2026-09-01
(the SB work order left with `instructions/` at `1769487`, and
`include/kds/exec/range_eligible.hpp:76` is the one place in the tree that names it), and
their values stay only as a counter's detail key
(`include/kds/exec/range_eligible.hpp:76-82`, `src/exec/range_eligible.cpp`). Every one of
those exists because two ranges had two write authorities and a reader
had to fan in. Under AR2 with the shared pool and the instance view, a
multi-range relation is one relation with several id blocks; a reader
walks all of it locally, and a durable auxiliary is maintained under the
writer's borrow whichever range the row is in. **CLA's reading is that
every refusal but `kSpill` dissolves** (`kSpill` is a var-heap partition
question, `crosscore.md` §6a, independent of authority). This is a
`[design]` claim to be verified gate by gate in M3's work order, not
something AR2 decides — and ratification AE's principle of 2026-09-01
that a relation with a durable auxiliary does not split (not in the tree
at `183b956`; the main checkout holds it untracked under
`instructions/v2.8.0/`) was ratified against the old substrate and needs
the operator's word again (**E10**).

---

## 6. What does not change

Beyond what §3 and §9 already hold fixed — one WAL appender, core 0's
three system structures, the id block as the range borrow, `cores = 1`
byte-identical — three invariants bound every rule above:

- **The fixed-length tuple rule** (invariant 13). It is what makes a
  tuple borrow's key stable: an `UPDATE` never moves the row, so the
  Keystone id names the same slot for the borrow's whole scope.
- **Invariants 2 and 3, per range.** A slice fence is over keys; it
  never moves a tuple and never edits a `min_key`.
- **Every advisory contract** (invariants 8 and 9). A borrow is
  authoritative machinery; Waystone stays advisory whatever it costs.

---

## 7. Items for operator judgement

Per AR0's standing rule, a CLA proposal is accepted by default except
where it fixes a constant or converts a refusal into a possible silent
wrong result. Every item below is in one of those classes, or is a
user-visible contract — except E1, listed so that the omission of `SIX`
is a stated default rather than an oversight.

| # | item | class | CLA proposal |
|---|---|---|---|
| E1 | Lock mode set | default, listed | `IS`, `IX`, `S`, `X` only; no `SIX`, no update mode in v1 |
| E2 | Per-transaction borrow cap, `max_locks_per_txn`: its value and its refusal code | constant; user-visible | 65,536; `ResourceExhausted`, non-retryable, never escalate (R4) |
| E3 | FK reverse check with no covering Bound structure | quiet-wrong | child **relation** `S` fence; slice fence when covered (R7) |
| E4 | Slice key | quiet-wrong | `(rel_oid, [lo, hi))`, page as hint only (R6) |
| E5 | Affinity collector | spec (R2 of `physical-optimizer.md`) | lock-manager grant counts per `(range, core)`, decayed by that spec's R1, feeding `sys.ranges.owner_core` per AR0-M5; not a `sys.access_stats` extension (R8) |
| E6 | Observational bank rule under the LSN view | OPEN, AN's | keep the rule's content; AN-S2 re-expresses the test (R11) |
| E7 | Execution default: local unless routed, or routed unless local | measurement-gated | decided by §9's C1–C3; CLA proposes **local** for `UPDATE`/`DELETE` now and `INSERT` per R5's arms |
| E8 | NS10's verb: "selects the core that owns" → "declares the affinity of" | user-visible | take it; `owner_core` fields keep their bytes (§5.6) |
| E9 | `core_count` pinning once `owner_core` means affinity | format / mount rule | stays pinned: E7 answers the ownership ground and `wal.md:58`'s warm-up ground is untouched by anything here (§5.6) |
| E10 | "A relation with a durable auxiliary does not split" (ratification AE, 2026-09-01) under AR2 | spec | re-ratify or retire in M3's work order after §5.7's gate-by-gate check |
| E11 | Borrow scope: by scope, never by clock | quiet-wrong | take R1 as written; no expiry, no renewal, no revocation |
| E12 | A read borrow: `IS` on the relation for a positioned statement | cost on every read | take it, priced in C3 (R3, §5.4); without it the mover's gate is writers-only |

---

## 8. Retired and amended

- `docs/spec/crosscore.md:30` CC3 "a transaction's writes bind to one
  owner core" — retired (§5.5).
- `docs/spec/crosscore.md:34` CC7 "page ownership is a function of the
  catalog" — amended: the catalog records **affinity**; no page has a
  writer of record.
- `docs/spec/crosscore.md:35` CC8 "Ownership unit" — amended: the range
  stays the id-allocation, export and affinity unit and stops being a
  write-authority unit; CC10's core-0-only Cabin discard at a split
  (`crosscore.md:37`) is re-homed with §5.1.
- `docs/spec/cabin.md:361-374` §6 — rewritten as a trade (§5.1).
- `docs/spec/assertion.md:49` AS4 — struck (by D8; inherited), and
  `assertion.md:272-285` §6.1's two ownership bullets — amended: the
  Bound Cabin has a *home*, and any writer holding the slice appends
  (§5.2).
- `docs/spec/foreign-keys.md:25` F3 — struck (by D9; inherited).
- `docs/spec/namespace.md:3-4` NS10 — verb amended (§5.6, E8).
- `docs/spec/physical-optimizer.md:66` R6 — amended: the precondition is
  a relation `X` borrow behind readers' `IS` (§5.4, E12).
- `docs/spec/txn.md:401-402` "No lock manager, no waiting, no deadlock
  detection, and the Keystone lock byte stays unused" — the whole
  sentence is amended at M2, not only its last clause: the byte becomes
  the tuple lock's fast path (R2).
- `src/storage/device_page_store.cpp:804-817` `MayWrite`'s lease/grant arm
  — refuses exactly the local write §3's `INSERT` row takes; kept through
  M1 by AM-R2 "as long as AM-R1 holds", and retired with AM-R1 at M2.
- `docs/rules/rules.md` §3's declared-shared table — gains the lock
  table's row, the obligation AR0-M2 already assigned
  (`ar0-architecture-revision.md:368`): a spec change first, code second.
- `src/txn/manager.cpp:59-61` — the comment "no lock manager and no
  reader registration" is already wrong on the second half (AR0-M1
  records it) and becomes wrong on the first at M2.

---

## 9. Sequencing

AR2 lands inside AR0 §8's chain and adds no milestone.

1. **Now, no code — two cells** under `bench/README.md`'s rules,
   `build-release`, interleaved, `git describe --tags`, filed under
   `bench/v3.0.0/`:
   - **C1, co-location ceiling.** Scenario 2 at `cores = 8` with the
     relations a booking touches declared in one namespace (NS10). It
     measures how much of the 46% owner routing alone recovers.
   - **C2, local parallel inserts.** Scenario 0 at `cores = 8` with
     `range_size_ids` armed — a range-granularity borrow, already built.
     If it does not beat 754.7 TPS, group commit bounds ingest and finer
     borrowing cannot help; if it scales, R5's second arm has a number.
     Arming it in a cell is a measurement, not a default change (D6).
2. **M1 (AM) — untouched.** AM-R1 keeps write authority with the owner
   through M1 and AR2 respects it: nothing in §3 executes before the
   page latch and the shared pool exist.
3. **M2 — the lock family.** R1–R4, R6, R9, R10 with D2, D12, D13; the
   tuple lock in the Keystone lock byte; the slice fence; the relation
   lock. E1–E4, E11 and E12 move into M2's ruling table when it opens.
4. **M3 — the consequences.** R7, R8, R11, R12; §5.1–5.7; E5–E10 move
   into M3's ruling table.
5. **C3, contention** — many sessions across cores updating one row
   locally under the tuple lock, and the same sessions on disjoint rows
   so the relation-level `IX`/`IS` key is priced alone (R3) — after M2's
   row lock exists. It decides E7's default, E12's price, and whether
   tuple-granularity borrowing wins or loses on the shape scenario 2 has.

**Not proposed:** a lock-manager prototype before M1; any time-bounded
lease variant; escalation.

---

# AR2-V — Source-read verification at `183b956`

Every `path:line` citation in the body — the tag, per the header —
checked against the tree on worktree `ar2-borrow-model` at `183b956`. Between `30e0377` (the main
checkout when this draft was surveyed) and `183b956` only `CLAUDE.md`
and `docs/spec/null.md` changed, in lines this draft does not cite.

| claim | where | what the tree says |
|---|---|---|
| AR0-4's three-property table | `instructions/v3.0.0/ar0-architecture-revision.md:35-40` | as quoted in §1 |
| The operator's marks | `ar0-architecture-revision.md:281-492` (AR0-M; the six marks are AR0-M1..M6 at `:290`, `:344`, `:377`, `:421`, `:455`, `:466`) | D1(b) conditionally, D2(a), D8, D9(a), D11, D15; the rest pending, listed at `:287-288` |
| Ranges are page-aligned pk intervals | `docs/spec/crosscore.md:35,37` (CC8, CC10) | "split at a page boundary only — `min_key` is the split key" |
| Core 0 alone writes the three system structures; DDL ships there | `docs/spec/crosscore.md:38,40` (CC11, CC13) | as stated |
| Readers are registered; two purges consume the horizon | `docs/spec/txn.md:296-320` | "Readers are registered" … `ReaderLease` … "Two purges consume the horizon" |
| The Keystone lock byte is unused today and is the row lock's home | `docs/spec/txn.md:401-402`; `include/kds/storage/heap/heap_page.hpp:127-128`; `include/kds/storage/keystone.hpp:11,34,49` | "the Keystone lock byte stays unused"; "the lock-slot role xmax plays in Postgres belongs to the Keystone lock byte here"; `flags:8` |
| Tail append; the tail is the only insert page; no free-space reuse | `docs/spec/heap-and-tuple.md:36-47` | "Growth is tail append, never a split" … "No free-space reuse" |
| Per-range monotonicity; spreading off by default | `docs/spec/heap-and-tuple.md:202-248`; `include/kds/server/range_alloc.hpp:90,150` | `kRangeSizeOff = 0`; `kRangeSizeIdsDefault = 65536` |
| Row-id leases on every relation; the block issuer and its holder | `docs/spec/heap-and-tuple.md:180`; `include/kds/catalog/catalog.hpp:643`; `include/kds/catalog/row_id_lease.hpp:36`; `include/kds/storage/extent_lease.hpp:46` | `AllocateRowIdRange` carves; `RowIdLease` is "one relation's leased run of row ids on one core"; `LeasedIdSource` is the **page**-id lease whose design it copies, refill non-blocking |
| The leaf division is the one epoch-bumping operation | `docs/spec/heap-and-tuple.md:28-34` | "the one operation that does is the btree leaf division" |
| A page span is never held across a park | `docs/spec/sched.md:42` | "A coroutine must not be parked while holding a resource that only makes sense within a call — above all a page span" |
| The window latch is taken with the WAL latch released | `instructions/v3.0.0/workorder-an-read-view.md:458-486` (AN-R9) | as stated |
| `ReadView`'s in-flight fields, and which AN-S2 retires | `include/kds/txn/read_view.hpp:68-74`; the bank test at `src/exec/step_vm.cpp:851-852`; `workorder-an-read-view.md:326,334` | `own_trx_id`, `in_flight[]`, `in_flight_count`; AN-R3 drops `up_to_trx_id`/`in_flight`/`in_flight_count` and **keeps** `own_trx_id` as branch 2 of `Visible` |
| Cabin §6 rests on owner-core execution and says "redesigned, not relaxed" | `docs/spec/cabin.md:361-374`; §6a at `:375` | as quoted |
| AS4 and F3 texts | `docs/spec/assertion.md:49`; `docs/spec/foreign-keys.md:25` | as quoted |
| R6's mover precondition; R3's seam; §4's legal moves; §6's gates | `docs/spec/physical-optimizer.md:66,63,102-148,180-201` | as quoted |
| NS10's sentence | `docs/spec/namespace.md:3-4` | "A namespace selects the core that owns the relations created in it" |
| `owner_core` on `sys.tables` and `sys.ranges` | `include/kds/catalog/rows.hpp:104,951-955` | the second comment says it is "Not a duplicate of `sys.tables.owner_core`" |
| `sys.access_stats`'s key and cap | `include/kds/stats/access_stats.hpp:30`; `include/kds/catalog/rows.hpp:714` | `(kind, rel_id, column_mask)`; `kMaxAccessShapes = 4096` |
| One decay implementation | `include/kds/stats/decay.hpp` | exists; R1 of `physical-optimizer.md` |
| The range-split gates | `include/kds/exec/range_eligible.hpp:83-91`; the removal note at `:76-82`; `src/exec/range_eligible.cpp:27,59,65,73` | six names — `kBtree`, `kIndex`, `kCabin`, `kSpill`, `kForeignKey`, `kAssertion` — and four `return` sites: `kBtree`, `kSpill`, `kForeignKey`, `kAssertion`. "**Two of the seven no longer occur** (SB3, 2026-09-01): `kIndex` and `kCabin`" |
| `MayWrite` refuses a leased store's write to the system range | `src/storage/device_page_store.cpp:804` (definition), `:811` (the system-range test), `:479` (the refusal, in every build) | `if (page_id < system_page_limit_) return false;`, and below it the lease/grant arm — `lease_->Owns(page_id)`, `HasWriteRight(page_id)` — which is what refuses a peer's write to *any* page it was not granted; AM-R2 keeps both through M1 |
| `base/latch.hpp`'s includers | `include/kds/txn/instance_visibility.hpp`, `include/kds/wal/stream.hpp` | two at `183b956` (AM's survey counted one at `f6ed10c`; AN-S1 added the second) |
| The refusal code R4 proposes | `include/kds/base/status.hpp:69` | `kResourceExhausted`, whose comment reads "a statement spent its per-statement work budget" — E2 notes the widening |
| A peer refuses a named key because admitting it writes `sys.tables` | `docs/spec/heap-and-tuple.md:182` | "A peer core refuses a named key, per row. Admitting one writes the relation's `sys.tables` row … and that page is the system core's" |
| `core_count`'s second pinning ground | `docs/spec/wal.md:58` | "the anchor's warm-up is defined over 'every core has published'" |
| Assertion §6.1's ownership bullets | `docs/spec/assertion.md:272-285` | "The enforcing core is the owning core, at every mount too" |
| The lock table's declared-shared obligation | `instructions/v3.0.0/ar0-architecture-revision.md:368`; `docs/rules/rules.md:21` §3 | "a **new row in §3's declared-shared table**" |
| AM-R1, AM-R3, AM-R5; AM's row status | `instructions/v3.0.0/workorder-am-m1-shared-pool.md:141-153,162-173,187-193,229-235` | as quoted. At `183b956` the row table read "not started" although `git log` carried two AM-S0 commits (`a2cd96a`, `30e0377`); `f710b3d`, merged into this branch at `ffccb61`, records AM-S0 (a) and (b) **done** at `:233-234` and AM-S1..S6 not started at `:235`. AM-R1's "through M1" and this draft's §9 step 2 are unaffected |
| AR0-M3's third item (the FK child side has no structure to name the gap) | `ar0-architecture-revision.md:399-408` | as quoted |
| `sys.range_affinity` | `include/`, `src/`, `docs/` at `183b956` | **absent**, and AR0-M5 (`ar0-architecture-revision.md:455-465`) says it stays absent: affinity is `sys.ranges.owner_core` re-scoped, not a new relation |
| The three measured numbers | `bench/v3.0.0/results-scenario2-freight-…:62-92`, `results-scenario0-stockmarket-…:80-101`, `results-wal-single-stream-…:78-128` | 578.4 → 312.7 (−46%) and 543.8 → 284.7 (−48%); 700.9 → 754.7 (+7.7%) and 192.6 → 207.2 (+7.6%); peer p50/p99 within 1–5% of core 0's |

**A discrepancy found on the way, not AR2's to fix.**
`docs/spec/physical-optimizer.md:180-201` §6's first gate and
`include/kds/stats/relayout_planner.hpp:69` both say readers are
unregistered and cite `txn.md` §9; `docs/spec/txn.md:296-309` §4.1 says
readers are registered and `ReadHorizon()` already feeds two purges. The
compaction gate's stated premise is stale; its true blocker is that no
heap consumer of the horizon and no mover exist. Belongs in
`docs/inflight/known-gaps.md` with the commit it was verified at.

**What AR2-V does not verify.** No `[design]` claim, and no number: every
number above is AL-S8's, measured at `v2.7.0-157-gf6ed10c` on the host
those files stamp, and this draft ran nothing.
