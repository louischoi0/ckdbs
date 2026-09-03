# Ratification AN — a global commit sequence is admissible; §1 rejected one and never justified it

Drafted 2026-09-03 by CLA on `worktree-commit-order-ratification` at
`f027a3c` (`v2.7.0-158-gf027a3c`), and **rewritten the same day against a
`critics-developer` review** that refuted four of the first draft's claims
against the tree. AN-7 records what changed and why, because three of the
four were the document reading a spec at `d15b5ac` while claiming `f027a3c`.

**Marked 2026-09-03; see AN-D10.** AN-Q1, AN-Q2 and AN-Q4 are settled by
the operator's mark of `workorder-an-read-view.md` AN-R7(B) and are built
there. AN-Q3, AN-Q5 and AN-Q6 remain open and have **moved** into that work
order as AN-R9, AN-R10 and AN-R11. AR0 §5's standing rule did not carry
them: AN-Q3 is a `[quiet-wrong]` item in that rule's own sense — an
implementation window whose failure is a missing row rather than an error —
and AN-Q5 reopens a policy decided against a smaller cost. Both were named
exceptions to CLA's proposals standing by default, and AN-Q5 is the one the
mark was taken without.

**No code changes.** AN rules on five sites across four specs (AN-D6) and
names what a work order would then owe.

Source of record: `docs/spec/cross-owner-txn.md:35-37`, `:308-310` and
`:332-336`; `docs/spec/txn.md` §4.1 and §4.2; `docs/spec/wal.md` §3;
`docs/spec/crosscore.md` §5; `docs/rules/rules.md` §3;
`instructions/v3.0.0/ar0-architecture-revision.md` AR0-3 and AR0-V4;
`instructions/v3.0.0/workorder-al-m0-single-wal.md` AL-R1 and AL-S1c;
`src/wal/analysis.cpp:248`; `src/txn/manager.cpp:101` and `:125`.

**Why this form, and the boundary it stands beside.** `index.md` rules that
a decision awaiting the operator lives in its work order's ruling table.
AN has no work order to live in — AM-1 states M1 is not the lock manager
and AR0 §8 puts that in M2, which is unwritten — and its subject is a spec
sentence rather than a build. `index.md` is amended by this change to name
the form; when M2's work order exists, AN-D5's Q-items move into its ruling
table and this document keeps D1-D4. *(The comparable v2.8.0 document,
`ratification-ae.md`, is untracked working-tree state in the primary
checkout and is named here as a shape, not as a resolvable citation.)*

---

## AN-D1 — §1 states two rejections and justifies one; §3 cites the unjustified half

The two sentences, verbatim at `f027a3c`.

`cross-owner-txn.md:35-37`, on a participant's transaction id:

> There is no shared transaction id and no global counter — a shared id
> would put foreign ids in every participant's stream, which
> `CoreRuntime::Open`'s mount check refuses.

`cross-owner-txn.md:308-310`, on why two participants' watermarks cannot be
compared:

> The single global instant that would let them be compared needs a global
> commit sequence, which is the shared counter §1 rejects.

**§3's citation is accurate and its justification is empty.** §1 does reject
a global counter — the words are there — but the reason it gives reaches
only the *other* object in the same sentence:

- A **shared transaction id** is one id used by every participant of one
  transaction. It lands in tuple headers, undo chains and each
  participant's log records, which is exactly what "foreign ids in every
  participant's stream" is about.
- A **global commit sequence** is a number assigned *at commit*, ordering
  commits. Every participant keeps its own leased trx id in every header it
  writes; the sequence orders the commit records, not the writers. No id
  becomes foreign to anything.

So the sentence states two rejections and justifies one, and §3 inherits
the unjustified half. A full read of §1 and §3 finds no second reason
anywhere that reaches a commit-ordering number. **AN-D1 rules that the
rejection of a global counter was never argued**, which is a lighter thing
to overturn than a reasoned decision and a different thing from AN-D2.

## AN-D2 — §1's stated reason is scoped, not void

The first draft said "every participant's stream" names nothing that
exists. That is wrong. `wal.md` §3 keeps **per-core streams as a live
branch** — "a pre-M0 volume, still mountable" — with "no way to convert a
volume between the two", AL-S1c's row says such a volume "keeps every
per-core rule", and `client-manual.md` promises a client
`wal_topology=per-core` for that volume's life.

**The correct narrowing:** on a `kSingleStream` volume there is one stream
and no id can be foreign to it, so §1's reason does not bite; on a
pre-M0 per-core volume a participant still has its own stream and the
reason bites exactly as written. §1's sentence therefore needs a **scope**,
not a deletion, and AN-D6's first row is written that way.

This does not weaken AN-D1, which never needed M0: the reason was already
about the wrong object on either topology.

**Stated so it is not read as more:** AN rules nothing about a shared
transaction id. That object has costs AN does not survey — recovery
scoping, `CoreRuntime::Open`'s check, a participant's undo chain — and
AN-D4's constraint 2 is the only fence this document puts around it.

## AN-D3 — The gap, by pointer

AR0-V4's first bullet already states it: not id uniqueness — one
instance-wide trx-id sequence leased per core (`txn/trx_id.hpp:74`,
`kTrxIdBlockSize = 4096`) — but a total **commit order**, ids being issued
in lease order. `cross-owner-txn.md:332-336` states it a second time, added
by AL-S9: "a shared WAL gives every commit a comparable LSN, but nothing
mints a snapshot across cores". AN does not restate it; AN rules on it.

## AN-D4 — The ruling

**A global commit sequence is admissible, and it is the engine's ordering
authority for visibility.** Four constraints, each a correctness statement.

1. **Invariant 12 is untouched.** The MVCC header stays 20 bytes,
   `trx_id:48 | undo_ptr | data_len | flags`, and gains no field. Commit
   order is carried outside the tuple. A proposal that widens the header is
   not this ruling.
2. **The sequence is not the transaction id.** A participant keeps its own
   leased id in every header it writes. Anything that makes a tuple carry an
   id its writer did not lease is outside AN (AN-D2's last paragraph).
3. **Under `kSingleStream`, the sequence is the commit record's LSN.** M0
   serializes the append under one latch with the LSN fixed under it
   (AL-R1), so commit order *is* the order of commit records in the one log.
   No second counter is introduced — `CLAUDE.md`'s rule against a second
   name for a quantity an existing one expresses applies directly. **With M0
   landed, the sequence the engine needs already exists as a byte offset**;
   what has to be built is the mapping from a transaction id to it.
4. **A per-core volume is out of scope and keeps what it has.** `wal.md` §3:
   under per-core streams "LSNs are stream-local and never compared across
   streams", so constraint 3's authority does not exist there. Such a volume
   keeps the trx-id `ReadView` of `txn.md` §4.1 and keeps `crosscore.md`
   §5's per-core weakening. Whether it should instead be refused the mount
   is **D14's**, which is unmarked; AN does not decide it and does not
   depend on it.

**If AN-D4 is declined**, the consequence is a gap rather than a decision:
`docs/inflight/known-gaps.md` gains one entry — that `REPEATABLE READ`
cannot span cores as a single instant and `crosscore.md` §5's sentence
survives AR0-2 — pointing here for why.

## AN-D5 — What must be decided with AN-D4, not after it

Each item is `[OPEN]`. CLA proposes.

**AN-Q1 — Where the mapping lives, and what answers for what it does not
hold.** *Rewritten after review; the first draft's "the mount's analysis
pass already reads every commit record, so the table is rebuilt there" is
false — `src/wal/analysis.cpp:248` scans from the checkpoint's
`redo_start_lsn`, not from 0.*

Proposal: a **window above a floor**, not a table over all history.

- The **window** is an in-memory `trx_id → commit_lsn` map owned by the log
  core, written at the commit append (already the serialization point) and
  read by `Visible` on every core.
- The **floor** is a single instance-wide trx id below which every
  transaction is resolved. A version still on a page whose writer is below
  the floor was **committed**, because recovery's undo phase physically
  rolls back every loser before the database is served — `txn.md` §4.1's
  existing load-bearing assumption, reused rather than replaced.
- **At mount the floor is the post-recovery high-water and the window is
  empty**, so nothing is persisted and no format event is needed. This is
  what the first draft was reaching for and got wrong: the derivation is of
  the *floor*, not of the history.
- **Reclamation advances the floor.** An entry may be dropped, and the floor
  raised past its transaction, only when its `commit_lsn` is below every
  live snapshot — which is what `ReadHorizon()` already bounds (AN-Q4).

**This is `rules.md` §3's fourth declared-shared structure, and §3 says the
fourth "should be argued for rather than noticed later".** The argument:
written by the log core alone at the commit append, which is already
serialized by the stream latch; read without a lock by every core, the
window being append-mostly and the floor a single atomic that only rises;
declared in `txn.md` §4.1 as its owning spec. A reader that misses a
just-published entry falls to the floor test and answers "uncommitted",
which is AN-Q3's window and is why AN-Q3 is not optional.

**AN-Q2 — The snapshot's shape, and the visibility rule in full.** Proposal:
a scalar LSN replaces `up_to_trx_id` + `in_flight[64]`, and `Visible`
becomes four branches in this order:

    t == kAlwaysVisibleTrxId  -> true      (txn.md §4.2, unconditional and permanent)
    t == own_trx_id           -> true
    t <  committed_floor      -> true      (AN-Q1: resolved, and still on the page)
    otherwise                 -> window lookup: entry present && commit_lsn <= snapshot_lsn

The first and third branches are what the first draft omitted, and without
them "no entry ⇒ invisible" erases every bootstrap catalog row and every
row whose writer's entry has been reclaimed. The in-flight set stops
existing, which retires `kMaxTrackedLiveTxns = 64`
(`include/kds/txn/read_view.hpp:48`) and its `Begin` `OutOfSpace`
(`src/txn/manager.cpp:86-88`) as a **side effect rather than as work** —
one of AR0-V4's three per-core facts closed for free.

**AN-Q3 — The publication window.** Between a commit LSN being fixed under
AL-R1's latch and its window entry becoming readable, a snapshot at that
LSN sees a committed transaction as uncommitted. Proposal: the snapshot
ceiling advances only to the highest LSN below which every reserved commit
has published. **Named because it is the one place this ruling can be
implemented wrongly and pass every test** — the window is narrow,
load-dependent, and its symptom is a missing row rather than an error.

**AN-Q4 — `ReadHorizon()` goes instance-global.** Forced, not chosen:
`txn.md` §4.1 states its soundness condition as "sound while every reader
reads its own core's versions", and a global snapshot reads any core's. It
also becomes AN-Q1's reclamation bound. AR0-V4 counts it among the three.

**AN-Q5 — The retention policy is reopened, and this is AN's real cost.**
`txn.md` §4.1 declines a byte-cap retention and never raises
`SnapshotTooOld`, priced explicitly as "one long-running transaction holds
reclamation for its lifetime" — **per core**. Made global, one idle
session's open `BEGIN` holds the whole instance's undo. CLA proposes no
resolution and proposes instead that **AN-D4 not be marked without one**:
the policy was decided against a cost an order of magnitude smaller than
the one it will bear.

**AN-Q6 — Snapshot acquisition moves from `BEGIN` to the first read.** Two
sites, and a work order needs both: `src/txn/manager.cpp:101` **mints** the
view inside `Begin`, and `:125` is the branch in `StartStatement` that
**declines to re-mint** it for REPEATABLE READ. Under AN-Q4's global horizon
minting at `BEGIN` turns `BEGIN`-then-idle into an instance-wide cost, and
under AN-Q2 it also hands the session a world older than its first
statement. Proposal: mint at first read (PostgreSQL's rule). **A
user-visible semantic change, and so the operator's rather than CLA's.**

## AN-D6 — Spec placement, and the order it lands in

Nothing below changes on a mark alone; a work order lands each. The order
is a correctness statement in the last row only.

| site | what changes |
|---|---|
| `cross-owner-txn.md:35-37` | the reason gains an explicit **scope** — "under per-core streams" — per AN-D2, and the clause "and no global counter" is struck as never argued (AN-D1). The shared-transaction-id rejection itself stands |
| `cross-owner-txn.md:308-310` | "which is the shared counter §1 rejects" struck, replaced by a pointer here. §3's per-core weakening stays *true* until AN-Q2 lands |
| `cross-owner-txn.md:332-336` | "nothing mints a snapshot across cores, and AR0-3 declined the cut vector that would" — accurate at `f027a3c` and **made false by AN-Q2**. Rewritten by the work order that lands it, not before |
| `txn.md` §4.1, §4.2 | the `ReadView` block, the `Visible` rule, the "why no commit table is needed" paragraph and the horizon's per-core scope are AN-Q1/Q2/Q4's, and §4.2's always-visible id becomes `Visible`'s first branch explicitly. AN records the scope; it changes nothing |
| `crosscore.md` §5 | now reads "sharing the log did not change that" (`:333-336`, AL-S9). True at `f027a3c` and **the sentence AN-D4 is aimed at**; it is rewritten with the mechanism, not pointed at |
| `client-manual.md` | the client-facing statement of the per-core weakening **changes last**, after the mechanism, never before. A manual promising a global instant the engine does not yet give is worse than one promising less |

## AN-D7 — Scope, in two clauses

AN does not decide D1's isolation level; it makes D1's condition
*evaluable* (AN-D9). And it does not touch constraint-check visibility:
`foreign-keys.md` §4 reads latest-state and `assertion.md` §4.3 reads
committed-plus-reservations, both **by design**, and aligning them to a
transaction's snapshot is D8/D9's lock work rather than a visibility change
— the world is defended by locks, not widened by snapshots.

## AN-D9 — D1 was marked ahead of AN, and what that puts on AN's critical path

Recorded 2026-09-03. AN-D7 says this document makes D1 evaluable; the
operator marked D1(b) with AN-D4 still unmarked, so the order is the
reverse of the one AN assumed (`ar0-architecture-revision.md` AR0-M1).

D1's mark is conditional on RU, RR and RC all being deliverable — SR left
the condition on the operator's amendment of the same day — and the three
do not depend on AN alike:

- **RU is unaffected.** It takes no read view, consults no window and walks
  no undo chain, so no commit order is involved. Deliverable at M2 whatever
  AN-D4 is marked.
- **RR and RC as levels meaning one instant across the instance are exactly
  what AN-D4 supplies.** Without a total commit order a snapshot is a
  per-core high-water mark.
- **So D1's condition cannot be evaluated until AN-D4 is marked.** A decline
  is not a dead end: RR and RC survive as **per-core** levels, which is what
  the engine ships at `f027a3c`. Whether that counts as "지원 가능" under
  D1's condition is the **one remaining unresolved half** of that condition,
  and AR0-M1 records it.

AN-Q5 is now load-bearing twice — for AN-D4 and for the condition D1's mark
hangs on.

## AN-D10 — The mark, and one correction it inherits

Recorded 2026-09-03. The operator marked `workorder-an-read-view.md`
AN-R7 as **(B)**, which is this document's mechanism: a scalar snapshot
LSN over a `trx_id → commit_lsn` window above a floor. What that settles
and where each open item went:

| item | after the mark |
|---|---|
| AN-D1..D4, D6, D7, D9 | stand, here. AN-D4's four constraints bound the build and none moved |
| AN-Q1 (the window and the floor) | **settled**, built at `workorder-an-read-view.md` AN-S1. **Corrected there by AN-R8** — see below |
| AN-Q2 (the snapshot's shape) | **settled**, built at AN-S2. `kMaxTrackedLiveTxns` and `Begin`'s `OutOfSpace` retire with it, as this document said they would |
| AN-Q4 (`ReadHorizon()` global) | **settled**, and larger than stated here: it has four trx-id-valued consumers, listed at that work order's AN-3 D, and all four change unit |
| AN-Q3 (the publication window) | moved, as **AN-R9**. Ruled there, not closed the obvious way: publishing inside the append latch would put visibility *ahead* of durability, which `src/txn/manager.cpp:230` and `:243` show it is not today |
| AN-Q5 (retention, global) | moved, as **AN-R10**, still open. This document proposed AN-D4 not be marked without it; the mark came first, and AN-R10 records that plainly and gates AN-S2 |
| AN-Q6 (mint at first read) | moved, as **AN-R11**, still open and separable |

**The correction.** AN-Q1 defines the floor as *"a single instance-wide trx
id below which every transaction is resolved"*. That is not maintainable
under the block lease, and it fails the way the trx-id high-water mark
fails: a core holding an unspent range **below** an advanced floor can
issue into it at any time, and the floor's branch then answers "committed"
for a writer that is live. The floor takes a second bound — the minimum
issue cursor across cores — and `workorder-an-read-view.md` AN-R8 carries
it with the derivation. The *mount* case this document states is
unaffected and was right: every core's first block is leased at or above
the post-recovery high-water.

## AN-7 — What the review changed

`critics-developer`, 2026-09-03, ~44 tool calls against this tree. Four
findings were refutations rather than criticisms and each is applied above.

| finding | what it refuted | where it landed |
|---|---|---|
| Analysis scans from `redo_start_lsn`, not 0 (`analysis.cpp:248`, `wal.md`) | AN-Q1's "derived state, rebuilt at mount"; with AN-Q2's "no entry ⇒ invisible" it made every pre-checkpoint row and every `kBootstrapXid` row invisible — a silent wrong answer if marked as written | AN-Q1 rewritten to a window above a floor; AN-Q2 given its four branches |
| A pre-M0 per-core volume is still mountable and keeps every per-core rule (`wal.md` §3) | AN-D2's "names nothing that exists"; AN-D4's unconditional constraint 3 | AN-D2 narrowed to scoping; AN-D4 gained constraint 4 |
| `crosscore.md` §5's "never will be given from a shared-nothing engine" was replaced at `f6ed10c` | a quotation of text deleted one commit before this document was drafted — the document read that spec at `d15b5ac` | quote removed; AN-D6's row now names the current sentence and says it is rewritten, not pointed at |
| `cross-owner-txn.md:332-336` states the gap already | AN-D3's "unstated blocker" | AN-D3 cut to a pointer, 12 lines to 6 |

Also applied: the title and AN-D1's framing (§1 *did* reject a global
counter and never justified it — the sharper claim and the true one); the
header's AR0 §5 exemption argued from AN-Q3's class rather than from a
class that rule does not have; "four sentences in two specs" corrected to
five sites across four; AN-Q6 citing both `:101` and `:125`; the AE
citation marked as untracked; AN-Q1 arguing itself as `rules.md` §3's
fourth declared-shared structure; and the shared-transaction-id fence,
stated three times, cut to one. AN-D8 folded into AN-D4's last paragraph.

Not applied: the review's suggestion to open a stub M2 work order and put
AN-D5 in its ruling table. `index.md`'s boundary paragraph is amended
instead, because a work order opened to hold a table is a document with no
survey and no stages, and AM-1's own gate ("M1 must not start before
AL-S8's numbers are read") says what an unearned work order costs.
