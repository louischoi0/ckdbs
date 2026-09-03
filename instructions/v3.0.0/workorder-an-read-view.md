# Work order AN — The instance read view

Written 2026-09-03 by CLA on `worktree-v3.0.0-read-view`, **source-read at
`004f949`**, and rewritten the same day twice: once on the AN-S0 source
read, once on the operator's mark of AN-R7(B), which puts the view on the
commit-LSN window rather than on a shared in-flight set. Every
`path:line` here is read at `004f949`; nothing under `src/` or `include/`
moved between `6ead2a0` and it, so the citations are `6ead2a0`'s too.
Passages tagged `[design]` are proposals. Nothing is `[measured]`.
AN-6 names the commit each stage lands in; a stage with no commit named
has not landed, whatever this header says about when it was written.

**Two documents in this directory are called AN and they are not the same
AN.** This one is the work order for the *milestone*: the instance read
view, AN-R\* rulings and AN-S\* stages. `ratification-an-commit-order.md`
is the ruling on a *spec sentence*: AN-D\* and AN-Q\*, a global commit
sequence. `CLAUDE.md`'s rule applies with force here — **cite the file,
never the bare number**. AN-R7's mark joins them: AN-Q1, AN-Q2 and AN-Q4
are settled by it and are built here; AN-Q3, AN-Q5 and AN-Q6 move into
this table as AN-R9, AN-R10 and AN-R11.

**Position.** AN sits between AM-S0 and AM-S1. It depends on nothing in
the shared pool and the shared pool's cutover (AM-S2) is unsound without
it the moment a peer reads a data page locally; AN-1 says why. AM-S0(a)'s
`Expeditor` two-core assembly cell is AN's test bench, which is why AN
follows S0 rather than preceding it.

## AN-0 — What the operator decided

**2026-09-03, verbal.** The instance read view is built **now**, ahead of
the shared pool, and every reader mints through it. No fast-path
optimisation in AN: the per-statement cost is paid and measured, not
avoided.

**2026-09-03, on AN-R7.** Mechanism **(B)** — the commit-LSN window.
AN-R7 records what the mark carries and what it leaves open; AN-R10 is
the one item the mark was taken *without*, and it is named there rather
than buried.

## AN-1 — The direction, and why it is not an M2 item

`txn.md` §4.1 states a soundness condition in one clause, and **it is
stated of the horizon, not of the predicate**: *"The horizon is
**per-core**, sound while every reader reads its own core's versions"*,
in the "Readers are registered" paragraph. `ReadView::Visible` has no
stated condition anywhere — which is worse rather than better, and is
half of what AN-S4 owes: the prose task is to *add* the predicate's
condition, not only to strike the horizon's. The unstated condition holds
at `004f949` because a peer reaches another core's rows only by shipping
the statement (`crosscore.md` §2), so the reader's core is always the
version's core, and `crosscore.md:288-290` states the premise outright —
*"the trx-id domain is global, so ids compare cleanly"* — which AN-3 E
refutes. AM-S2 puts every core's frames in one pool and AR0 §2 retires
ownership so that a peer can read a relation's page without shipping;
between them the condition is false. (AL-S8's −46–48% on scenario 2 is
the *motive* — what shipping costs — not a third mechanism.) What
happens when the condition is false is not an error: a transaction live in
core 0's `live_` is below a view minted on core 3 (`up_to_trx_id`,
`read_view.hpp:64`) and absent from core 3's `in_flight`
(`read_view.hpp:73`), so `ReadView::Visible` (`read_view.hpp:81-90`)
answers **true** for an uncommitted version. A dirty read returned as a
row.

The same clause guards the purge: `ReadHorizon()` (`manager.cpp:559`)
walks this core's `live_` and reader slots only, so core 0's undo purge
cannot see a reader on core 3 holding a superseded version.

Neither is a lock-manager problem. A lock decides who may *write*; the
view decides what a reader may *see*, and moving write authority (M2)
with per-core visibility opens the same dirty read on the write path.
Whatever milestone first lets a core read another core's data page
locally must carry the instance view in the same commit or earlier. AN
puts it earlier.

## AN-2 — Against AR0, AM, and the ratification

| Item | AN's reading |
|---|---|
| AR0-3 (cut vector dropped) | AN is the replacement AR0-3 implied and never named. **The first draft's reason for saying so was wrong** and AN-3 E replaces it: with one WAL the snapshot is *not* a scalar HWM over trx ids, because ids are leased in per-core blocks and a cursor over them orders nothing across cores. AR0-3's cut vector stays declined, now on the mark's ground rather than on an unstated one — `cross-owner-txn.md:332-336` names the alternative it declined to take: *a shared WAL gives every commit a comparable LSN, but nothing mints a snapshot across cores* |
| `ratification-an-commit-order.md` AN-D4 | **The ruling AN is the build of.** A global commit sequence is admissible and is the ordering authority for visibility; under `kSingleStream` it is the commit record's LSN and no second counter is introduced. AN-R7's mark takes it |
| same, AN-Q1 / AN-Q2 / AN-Q4 | **Settled by AN-R7's mark** and built at AN-S1/S2: the window above a floor, the four-branch predicate, the instance-global horizon. AN-R8 corrects AN-Q1's floor, which has the same defect AN-3 E found in the high-water mark and for the same reason |
| same, AN-Q3 / AN-Q5 / AN-Q6 | Moved here as **AN-R9, AN-R10, AN-R11**. `index.md`'s holding-shape rule says the `[OPEN]` items move when the milestone's work order opens; the mark is what made this that work order |
| AM-R1 (cache shared, authority not) | Unchanged. AN shares a third thing — visibility — and the three are independent |
| `rules.md` §3 (declared shared state, three rows) | AN adds **a** row, and the ordinal is already contested four ways: `rules.md:32` says "the fourth" verbatim, `workorder-am-m1-shared-pool.md` makes AM-S5's deliverable *"`rules.md` §3's fourth row"* for the shared pool, `ratification-an-commit-order.md` calls AN-Q1's window "the fourth declared-shared structure", and AR0-M2 queues D2's lock table. **Whichever lands first takes the number**; §3's rule is that each is argued for, not that one of them is fourth. AN-1 is this row's argument and AN-R1 is its subject |
| AR0 D1 (isolation target) | Not decided here. AN makes the *snapshot* instance-wide; it does not touch write conflicts or predicate locking. `ratification-an-commit-order.md` AN-D9 puts AN on D1's critical path, and AN-R7's mark is what clears it |
| `cross-owner-txn.md` §3 (per-participant watermark) | Retired by AN-R5 if AN-S3 is taken; left standing otherwise |

## AN-3 — The survey

**A. One mint site.** `TransactionManager::MintReadView(own_trx_id)`
(`manager.hpp:435`, defined `manager.cpp:71-83`). `AutocommitSnapshot`
wraps it and registers the reader (`manager.hpp:603-607`). `Begin` mints
at `manager.cpp:101`; `StartStatement` re-mints at `:127`, its `:125`
branch being the one that declines to for REPEATABLE READ — the two
sites AN-R11 needs. Callers of `AutocommitSnapshot`:
`command_dispatcher.cpp:10575`, `remote_step_service.cpp:450/683/1063`.
"Every reader references this view" is therefore already true of the
tree; AN changes one function's body, not the callers.

**B. The ten `ReadView::Everything()` sites** —
`assertion_build_service.cpp:146`, `command_dispatcher.cpp:3572/4131/9786/10934`,
`fk_probe_service.cpp:58/209`, `cabin_optimizer_exec.cpp:29`,
`visibility.hpp:161` and the `assertion_catalog.hpp:315` rule (a comment,
not a call) — are **latest-state check views** that bypass visibility on
purpose; the eleventh hit of a grep is the definition at
`read_view.hpp:131`. Their cross-core correctness is the lock manager's
(M2), not the view's, and `ratification-an-commit-order.md` AN-D7 rules
the same thing from the other side. **AN does not change what they mean
and does change how they say it** — `Everything()` sets
`up_to_trx_id = UINT64_MAX` today (`read_view.hpp:133`), and under the
four-branch predicate that field is gone. AN-R3's last paragraph rules
the replacement, and it is not a free rename.

**C. The view's shape today.** POD, `in_flight[kMaxTrackedLiveTxns = 64]`
sorted, `in_flight_count`, `up_to_trx_id`. Two guards, of which **only
one can fire**: `Begin` caps `ActiveCount()` at 64 (`manager.cpp:86-88`)
and `MintReadView` skips `own_trx_id` (`:79`), so the array never reaches
a 65th entry and `ReadView::AddInFlight`'s refusal (`read_view.hpp:97`)
is unreachable today. Structural, not redundant — but the second is a
guard against a future caller, not a second live bound.
The 64 is stated as "as `kMaxWalCores`" (`read_view.hpp:46`), and
`kMaxWalCores = 64` (`superblock.hpp:365`); the two constants are equal
by allusion, not by dependence. **All of it retires under the mark**, as
`ratification-an-commit-order.md` AN-Q2 records — a side effect rather
than work, and one of AR0-V4's three per-core facts closed for free.

**D. The horizon, and its two consumers.** `ReadHorizon()` = min over
active `live_` views' `MinVisibleBound()` and over `reader_slots_`
(`manager.cpp:559-579`). It answers a **trx id**, and exactly two callers
read it:

- the undo purge, armed through `SetHorizonSource` (`manager.hpp:273`,
  `undo_log.hpp:207`);
- the catalog delete-mark purge, `RetireDeleteMarksBelow(ReadHorizon())`
  (`catalog.cpp:936`), gated **system core only** at
  `command_dispatcher.cpp:8249` with the comment that this core's horizon
  *is blind to every other core's readers* and that a peer's answers
  `UINT64_MAX` (`:8239-8246`).

**Two, not four**, and the correction is worth keeping because the draft
counted comments as callers: `catalog.hpp:1283` describes the
`catalog.cpp:936` call above and, in the same paragraph, the *mount*
sweep, which passes `UINT64_MAX` (`catalog.cpp:907`) and not the horizon;
`varheap.hpp:46` states a lifetime rule its own sentence says that file
does not implement. Both need rewording at AN-S4; neither changes unit,
because neither reads the value.

That gate at `:8249` is the existing acknowledgement of the gap AN
closes; AN leaves it in place (defence in depth, as its comment says) and
makes its premise false. Both real consumers change unit under the mark —
AN-R3 says to what. **AN-S2 is sized L on the predicate cutover and on
AN-R3's retention problem, not on this count.**

**E. Trx ids, and the fact that decided AN-R7.** One instance-wide
sequence leased per core in blocks, `kTrxIdBlockSize = 4096`
(`trx_id.hpp:74`), ids **unique and monotonic per core, never gapless**
(`trx_id_lease.hpp:22`). `MintReadView` sets
`view.up_to_trx_id = ids_.peek()` (`manager.cpp:75`), which is *this
core's* cursor into *this core's* window; `trx_id.hpp:151` says so and
`:153-155` states that everything in `[peek(), ceiling())` is reserved
and unspent.

The first draft argued from this that the predicate needs no change
"provided the set is the union". **That is false in both directions**:

- **H1 — a committed transaction that is invisible until an unrelated
  event.** Core B holds a window above core A's. B commits `t = 12300`;
  A's view carries `up_to_trx_id = 9000`, so `t >= up_to_trx_id` answers
  `false` (`read_view.hpp:84`) and the row is invisible. **Every core but
  the one holding the highest block is exposed**, not only an idle one,
  and what ends the exposure is not the commit becoming older: it is A
  burning the rest of its 4,096-id block and being carved a new one above
  B's. A row's visibility then depends on how much unrelated write
  traffic its reader's core has done.
- **H2 — a dirty read the union does not catch.** Core B holds a window
  *below* A's, with `peek = 5000` unspent when A mints at `up_to = 9000`.
  B then begins `t = 5000` and writes. `t` was not live at the mint, so
  no union of in-flight sets contains it; `t < up_to_trx_id`, so
  `Visible` answers `true` for a transaction that had not started.

Both hold because *issue order across cores is not id order*. A
predicate that reads a snapshot out of a trx id needs the two to agree,
and the block lease is the design decision that makes them disagree.
**This is the whole ground of AN-R7's mark**, and AN-R8 is the same fact
reaching one thing the mark did not remove.

**F. The watermark.** `Session::NoteParticipantWatermark`
(`session.hpp:402`) refuses a reply whose `up_to_trx_id` differs from the
one first reported; the comment at `session.hpp:381-390` gives the reason
in the same terms E does — the quantity is *"a high-water mark over the
ids that core has issued"*, so two of them order nothing. Under the mark
the quantity itself stops existing, which is why AN-R5 is now a deletion
rather than a redesign.

**G. What the checkpointer sees.** `wal::ActiveTransactions::Snapshot()`
(`checkpointer.hpp:77`) is per manager. **Nothing folds N of them** — the
draft said AL-S3/S4 did, and `workorder-al-m0-single-wal.md` is explicit
that the anchor fold carries one core's whole record and never a
field-wise minimum; what merges N cores' checkpoint records is recovery's
analysis pass. AN changes neither: the live set a checkpoint records and
the window a view consults are the same facts read through two doors, and
AN does not merge the doors.

**H. `base/latch.hpp` has exactly one includer in the tree** —
`wal/stream.hpp` — which is AM's survey finding and applies here
unchanged: AN-R1's slot latch would be its second, and AR0-M2 records
that `spin_latch.hpp` was deleted at `7839a29`, so there is no third
primitive to reach for.

**I. Where a commit becomes visible today, and what that does *not*
prove.** `TransactionManager::Commit` calls
`wal_->Commit(txn.id_, durability)` at `manager.cpp:230` and only then
clears `txn.active_` at `:243`, which is what takes the transaction out
of the next `MintReadView`'s in-flight set (`manager.cpp:77`).

**Visibility stands behind durability only under `kStrict`.**
`WalManager::Commit` (`wal/manager.cpp:358-388`) syncs on `kStrict`
alone; `kGroup` records the LSN for the drain and returns, `kRelaxed`
returns at once, and **`kGroup` is the default**
(`tcp_server.hpp:297`, `wal.md` §3's "default operating mode"). So under
the shipping default a reader already sees a commit the platter has not
got, and AN-R9's ruling does **not** rest on changing that. What `:243`
is, and all AN needs it to be, is the point where the in-flight set stops
containing the transaction — so publishing the window entry there keeps
the two facts in step through the cutover instead of moving visibility
earlier by a side effect of where a map insert went.

## AN-4 — Rulings AN-R1..AN-R11

**AN-R1 — `txn::InstanceVisibility`.** One per instance, owned by the
`Expeditor`, handed to every `CoreRuntime` on `Config` beside
`shared_stream`/`shared_writer` (`core_runtime.hpp:268-269`). Three
parts:

- **The window** — `trx_id → commit_lsn` for every transaction committed
  and not yet reclaimed. Written by the committing core at AN-R9's
  publication point, read by `Visible` on every core.
- **The floor** — one instance-wide trx id, monotonically rising, below
  which every transaction is resolved *and no id can ever be issued
  again* (AN-R8). A version on a page whose writer is below the floor was
  committed, because a loser's page changes are physically undone before
  the database is served — `txn.md` §4.1's existing load-bearing
  assumption, reused rather than replaced.
- **A slot per core**, holding two integers the owning core publishes and
  every core reads: `min_snapshot_lsn` (this core's oldest live view, or
  `UINT64_MAX` with none) and `issue_cursor` (this core's `ids_.peek()`).
  The first is AN-Q4's global horizon; the second is AN-R8's bound.

**It is not null at `cores = 1`.** That is the visible difference from
the drafted mechanism, which was an add-on to a working per-core view:
this *is* the view, so it is present at every core count and the
`cores = 1` path runs the same code with one slot. What AR0 requires at
`cores = 1` is unaffected — no on-disk format changes, no WAL record
changes, and every reply stays byte-identical. **The in-memory `ReadView`
POD does change shape, and no rule promised it would not**; AN-S1's first
cell is the behavioural identity that replaces the byte-identity cell the
drafted mechanism could have claimed.

A core writes only its own slot; the window and the floor are written by
the committing core and by reclamation. Serialization: the slot's
integers are `std::atomic<std::uint64_t>` with release/acquire and no
latch — each is written by one core and read by many.

**The argument for that is not "a stale read is conservative", and the
draft's was wrong.** `issue_cursor` and `oldest_unresolved` only ever
constrain the floor downward, so a stale read of either is conservative.
`min_snapshot_lsn` is **not monotone** — `UINT64_MAX` is its *maximum*,
meaning "no snapshot", and registering a reader lowers it — so a stale
read is too *high*, and a horizon computed from it sits **above** a live
reader's snapshot, which is the purge race AN exists to close. What makes
the plain atomic sound is a different fact: **a newly minted snapshot
takes the current published ceiling, and the ceiling is at or above every
live snapshot**, so a purge that ran while this core's slot still read
`UINT64_MAX` can only have retired versions superseded at or below a
bound the new snapshot is itself at or above. The store is still made
before the view is returned, which costs nothing and is what AN-S3 needs.

**AN-S3 is the case where the argument fails**, and it is stated here
rather than discovered there: a participant that *adopts* a coordinator's
snapshot (AN-R5) is minting a snapshot **below** the current ceiling, so
adoption must publish and register before the first read, and the floor
and the window must both already be bounded by it. AN-S3's second cell is
that.

The window needs a real latch
(`base/latch.hpp`'s, AN-3 H); its acquisition order is stated at AN-R9,
and it is **never** taken under the WAL stream latch. Declared as a new
`rules.md` §3 row (AN-2), with AN-1 as the argument §3 asks for.

**AN-R2 — Publication.** Four points, all in `TransactionManager`:

- `Commit` publishes `trx_id → commit_lsn` **at `manager.cpp:243`**, the
  line that clears `active_` today, so the window entry becomes readable
  exactly where the in-flight set stops containing the transaction
  (AN-3 I, AN-R9).
- `Abort` publishes **nothing**. A loser has no window entry, is
  therefore invisible by absence, and its page changes are physically
  undone — the same fact the floor rests on.
- `Begin`, `Commit` and `Abort` republish this core's `issue_cursor` and
  `oldest_unresolved` through one private `PublishCoreBounds()`, which
  the constructor calls too: a core must publish a cursor **before** it
  runs a transaction, because a floor raised while the core looked
  unattached would be a floor it could then issue below.
- **`min_snapshot_lsn` is AN-S2's, and nothing publishes it at AN-S1.** A
  read view still carries a trx-id bound and has no LSN to publish, so
  the field stays at "no snapshot" and reclamation is unconstrained by
  readers — correct precisely while nothing reads the window, and the
  first thing AN-S2 changes.
- **`[design]` resolved at AN-S1, against both of the draft's options.**
  Neither `TrxIdSequence::Next` nor `InstallWindow` needs a hook:
  `ids_.Next()` has **exactly one caller in the tree**, `manager.cpp:90`
  inside `Begin`, so republishing from `PublishCoreBounds()` catches
  every advance the cursor can make and the id path itself is untouched.
  Recorded because the draft priced a store on the id path that does not
  have to exist.
- **Topology gate.** The structure exists only under `kSingleStream`
  (`expeditor.cpp`, beside `shared_stream`): commit order *is* the commit
  record's LSN, and under per-core streams LSNs are stream-local and
  never compared, so there is no order to record. A pre-M0 volume keeps
  the per-core `ReadView` it has, which is
  `ratification-an-commit-order.md` AN-D4's fourth constraint wired
  rather than restated.

**AN-R3 — The mint, the predicate, and the horizon's change of unit.**

`ReadView` loses `up_to_trx_id`, `in_flight` and `in_flight_count` and
gains `snapshot_lsn`. `MintReadView` reads AN-R9's published ceiling and
stores it; the sorted-merge of the drafted mechanism does not exist, so a
mint is one atomic load rather than a walk over `live_`. `Visible`
becomes `ratification-an-commit-order.md` AN-Q2's four branches, in this
order:

    t == kAlwaysVisibleTrxId  -> true      (txn.md §4.2, unconditional and permanent)
    t == own_trx_id           -> true
    t <  committed_floor      -> true      (AN-R1: resolved, and still on the page)
    otherwise                 -> window lookup: entry present && commit_lsn <= snapshot_lsn

**The floor is read live, not copied into the view.** A copied floor is a
lost-row hazard and the reason is worth stating: reclamation drops a
window entry *because* the floor has risen past its transaction, so a
view holding a stale lower floor would take branch 4 for that
transaction, find no entry, and answer `false` for a row committed long
ago. Reading the shared floor makes branch 3 and reclamation agree by
construction, and a floor that only rises can only move an answer from
`false` to `true` for a transaction every live snapshot already includes.

`ReadView::Everything()` (`read_view.hpp:131`) keeps its meaning and
loses its mechanism: with no `up_to_trx_id` there is nothing to set to
`UINT64_MAX`. `[design]` a `sees_everything` flag short-circuiting to
`true` is one predictable branch and keeps the POD; the alternative — a
sentinel floor — reintroduces the copied floor this ruling just refused.
AN-S2's cell is that all ten sites in AN-3 B answer exactly as they do at
`004f949`.

**`ReadHorizon()` answers an LSN**, `min` over every core slot's
`min_snapshot_lsn`, which is AN-Q4 built rather than argued. Its two
consumers (AN-3 D) change with it: a superseded version is unreachable
when its superseding writer is below the floor, or when that writer's
`commit_lsn` is at or below the horizon LSN — two branches replacing one
comparison, and the same two the predicate's branches 3 and 4 already
are.

**One retention duty does not survive the change of unit, and it is the
reason AN-S2 is L.** `MinVisibleBound()` (`read_view.hpp:119-124`) folds
`own_trx_id` into today's horizon, which is what keeps an *active*
transaction's own undo records — the ones its rollback needs, not the
ones a reader needs — from being retired under it. An LSN horizon over
snapshots says nothing about that, and an RC transaction re-minting every
statement contributes nothing through `min_snapshot_lsn` at all. The load
moves to AN-R8's first floor term, `oldest_unresolved`: an active
transaction holds the floor below its own id, and the purge's first
branch then cannot retire under it. **That is sufficient and it is not
obvious**, so AN-S2 carries it as a cell — a rollback after a purge pass
taken while the transaction was live — rather than as an assumption.

**AN-R4 — Struck by the mark.** The bound on a union in-flight array —
32 KiB per mint, or an instance `OutOfSpace`, or a compile-time core cap
— has no subject: there is no union and no array. Recorded rather than
deleted because it is the clearest single statement of what the mark
bought.

**AN-R5 — Global consistency for cross-owner RR. [operator, spec change]**
Under the mark this is small: the coordinator's `snapshot_lsn` is one
`uint64` copied into the enrolment message, and a participant adopts it
instead of minting. What it retires: the per-participant watermark
(`session.hpp:373-411`), `txn_watermark_refusals`, and the client
manual's "consistent-per-core" sentence — replaced by "globally
consistent under REPEATABLE READ". **Proposal: take it in AN** (AN-S3),
because AN is the one milestone whose whole subject is the view, and
leaving the watermark standing beside an instance view means keeping a
check whose premise no longer describes the engine. The adopted snapshot
is safe against reclamation for the reason AN-R3 gives — the adopting
participant registers it, so the floor and the window's reclamation are
both bounded by it — and AN-S3's cell says so rather than assuming it.
The drafted mechanism's lease-ceiling reconciliation cell does not exist
under the mark.

**AN-R6 — What AN does not do.** No commit-LSN stamp on tuple headers:
invariant 12 is untouched, which is
`ratification-an-commit-order.md` AN-D4's first constraint, and the
window is what carries commit order instead. No change to what the
`Everything()` views *mean*. No fast path — the per-statement cost is
paid and measured at AN-S5. No change to the checkpointer's `Snapshot()`.
No change to where a tuple's writer id comes from: a participant keeps
its own leased id in every header it writes (AN-D4's second constraint),
which is why AN-3 E is a constraint on the *predicate* and never a
proposal to retire the leases. No second counter — under `kSingleStream`
the sequence is the commit record's LSN, per AN-D4's third constraint and
`CLAUDE.md`'s rule against a second name for an existing quantity.

**AN-R7 — The mechanism. [marked (B), operator, 2026-09-03]** AN-3 E
ruled out the drafted mechanism. Three exits stood: (A) the cut vector —
union in-flight plus a per-core `[peek, ceiling)` pair in every view,
correct and a resurrection of the one structure AR0-3 declined, paying 64
range checks per tuple; **(B) commit order** —
`ratification-an-commit-order.md` AN-Q2's scalar snapshot LSN over a
window above a floor. **Immune to H1 and H2 in branch 4, and not "by
construction" everywhere** — the draft said no id ordering is consulted,
and branch 3 (`t < committed_floor`) is one. AN-R8 is what makes that
branch safe, and without it (B) fails H2 exactly as (A) does; (C)
retiring the block lease for one shared atomic counter,
which reaches `cross-owner-txn.md` §1's shared-id rejection,
`CoreRuntime::Open`'s mount check and every peer's `TrxIdLease` wiring.

**The operator marked (B).** What the mark carries: `ratification-an-commit-order.md`
AN-Q1, AN-Q2 and AN-Q4 are settled and are AN-S1/S2's build; AN-Q3,
AN-Q5 and AN-Q6 are not, and are AN-R9, AN-R10 and AN-R11 below. What it
does not carry: AN-D4's four constraints already bound it and none of
them moved, and AN-R8 is a correction the mark inherits rather than a
reopening of it.

**AN-R8 — The floor's second bound, which corrects AN-Q1.**
`ratification-an-commit-order.md` AN-Q1 defines the floor as *"a single
instance-wide trx id below which every transaction is resolved"*. **That
is not maintainable under block leases, and it fails as H2 fails.** Core
A holds `[8192, 12288)` with `peek = 9000`; core B holds
`[12288, 16384)` and every transaction either core has *issued* has
resolved, so a floor defined on resolution alone advances past 12500. A
then issues `t = 9001` from its own unspent range, writes, and stays
uncommitted — and branch 3 answers `t < committed_floor` → **true**, a
dirty read through the branch that was supposed to be the safe one.

The floor therefore carries two bounds, not one, and it is the lower of
them:

    floor = min( oldest unresolved trx id across cores,
                 min over cores of that core's issue_cursor )

The second term is why AN-R1's slot publishes `issue_cursor`: below it,
no id can ever be issued again, so "resolved" stays true once it is true.
At mount both terms sit at the post-recovery high-water — every core
leases its first block from at or above the superblock's `next_trx_id`,
so nothing below it will be issued — which is why AN-Q1's *mount* case
was right while its steady-state definition was not. The window stays
empty at mount and nothing is persisted; no format event, exactly as
AN-Q1 says.

**AN-R9 — The publication point, and why AN-Q3's window closes.**
`ratification-an-commit-order.md` AN-Q3 names the interval between a
commit LSN being fixed under the append latch and its window entry
becoming readable, and calls it the one place this design can be
implemented wrongly and pass every test. **Publishing inside the append
latch would close it and is refused, on AN-R2's ground rather than on a
durability one.** The draft argued that visibility stands behind
durability and that publishing early would let a reader see a commit the
platter has not got; AN-3 I shows that is true only under `kStrict`, and
`kGroup` is the default, so the engine already does exactly that. The
ground that survives is the one that does not depend on a durability
class: `manager.cpp:243` is where the transaction leaves the in-flight
set, so publishing there keeps the window and the set saying the same
thing at every instant of the cutover, and a map insert's position stops
being able to move visibility on its own. Publishing under the append
latch would also put an unbounded-lookup structure inside the hottest
latch in the engine, which AL-S8 measured and AN-S5 must not undo.

**Ruling: publish at `manager.cpp:243`, and maintain AN-Q3's ceiling.**
The log core keeps the set of commit LSNs *reserved and not yet
published*; the snapshot ceiling `MintReadView` reads is the lowest such
LSN, or the newest assigned LSN when the set is empty. The set is small
by construction — it holds one entry per commit in flight between its
append and its publish — and it is the window's own latch that protects
it. **Acquisition order: the window latch is taken with the WAL stream
latch released, never under it**, which is the same shape
`wal/writer.hpp`'s wait mutex already has and is stated in the subsystem
header per `rules.md` §3.

**AN-R10 — Undo retention, global. [operator, and the mark was taken
without it]** `ratification-an-commit-order.md` AN-Q5 proposed that
AN-D4 *not* be marked until this was resolved, and the mark came first.
Recorded plainly rather than quietly inherited: `txn.md` §4.1 declines a
byte-cap retention and never raises `SnapshotTooOld`, priced explicitly
as *one long-running transaction holds reclamation for its lifetime* —
**per core**. AN-R3 makes the horizon instance-global, so one idle
session's open `BEGIN` now holds the whole instance's undo, and
`shipped_statement_executor.hpp:157` already names the five-minute
abandoned-transaction case against the per-core cost. CLA proposes no
resolution and proposes that **AN-S2 not land without one**: the stage
that changes the horizon's scope is the stage that changes this cost, and
the two cannot be separated afterwards. It blocks nothing before AN-S2.

**AN-R11 — Snapshot acquisition at first read. [operator, separable]**
`ratification-an-commit-order.md` AN-Q6: `manager.cpp:101` mints inside
`Begin` and `:125` is the branch that declines to re-mint for REPEATABLE
READ. Under a global horizon, minting at `BEGIN` turns `BEGIN`-then-idle
into an instance-wide cost. Proposal: mint at first read (PostgreSQL's
rule). **A user-visible semantic change, and so the operator's rather
than CLA's.** It is separable from every stage below and is not scheduled
here.

## AN-5 — Stages

Every stage: `critics-developer` review, full suite, sync with
`origin/main`, stop.

| # | Stage | Cells | Size |
|---|---|---|---|
| AN-S0 | This document; `docs/inflight/known-gaps.md` gains the entry AN-1 describes, verified against `read_view.hpp:81-90` and `manager.cpp:559` | the files at the commit | the hour |
| AN-S1 | `txn::InstanceVisibility` — window, floor, per-core slot (AN-R1); publication at all four points (AN-R2); the floor's two bounds (AN-R8). No reader consults it yet: the existing predicate still runs, so the stage is additive and the suite is the regression | `cores = 1`: the `ReadViewTest` cells in `tests/visibility_test.cpp` and every cell in `tests/txn_manager_test.cpp` and `tests/txn_session_test.cpp` pass unchanged (**there is no `read_view_test.cpp`** — the draft named a file that does not exist), and the window holds one entry per committed transaction with the LSN `Commit` returned; **`Abort` leaves no entry**; the floor at mount equals the post-recovery high-water and the window is empty; the floor does **not** advance past a core's `issue_cursor` — the H2 shape as a cell, built here because AN-R8 is where it is closed; reclamation drops an entry only below the floor. `cores = 2` is **not** cellable here: AM-S0(a)'s assembly is unbuilt (`index.md` records M1 as not started), so the multi-core cells are written against a direct two-slot fixture over `InstanceVisibility` itself, and the assembled two-core case is AN-S2's | M |
| AN-S2 | The cutover: `ReadView` loses `up_to_trx_id`/`in_flight`, gains `snapshot_lsn`; the four-branch `Visible` with the floor read live; `MintReadView` reads AN-R9's ceiling; `ReadHorizon()` answers an LSN and both consumers change with it (AN-3 D); `Everything()`'s replacement (AN-R3). **Does not land without AN-R10** | **The two that fail on the drafted mechanism**: H1, a commit on a core holding a higher id window is visible to a lower core's next view; H2, a transaction begun after the mint from a lower core's unspent range is invisible to the pinned view. Then: a view minted on core 1 while core 0's txn is live does not see it, before its commit **and** after (the view is pinned); at RC the next statement does; **a commit is visible exactly when it leaves the in-flight set**, not when its LSN is fixed (AN-R9); **a rollback still finds its own undo after a purge pass taken while the transaction was live** — the duty `MinVisibleBound()` discharges today and AN-R8's `oldest_unresolved` discharges after; all ten `Everything()` sites answer as at `004f949`; a reclaimed window entry never turns a committed row invisible (AN-R3's live floor); core 0's undo purge does not pass a reader on core 3; the delete-mark purge gate (`command_dispatcher.cpp:8249`) still refuses on a peer | L |
| AN-S3 | (AN-R5, if taken) The coordinator's `snapshot_lsn` forwarded in the enrolment message; participant adopts it; watermark and `txn_watermark_refusals` removed | the case `cross-owner-txn.md` §3 states as *possible* — two cross-owner RR transactions disagreeing on the order of two commits on two cores — as a cell that must **not** reproduce; **an adopted snapshot publishes and registers before its first read**, which is the one case AN-R1's lock-free argument does not cover, and the floor and the window's reclamation are both bounded by it afterwards | S–M |
| AN-S4 | Prose: `txn.md` §4.1 and §4.2 — the `ReadView` block, the four branches, the "why no commit table is needed" paragraph and the horizon's scope; `rules.md` §3's new row; `cross-owner-txn.md:35-37`, `:308-310` and `:332-336` per `ratification-an-commit-order.md` AN-D6; `crosscore.md` §5 rewritten with the mechanism; `client-manual.md` **last**; the `command_dispatcher.cpp:8239` comment ("blind") rewritten to name the gate as defence in depth only; `CLAUDE.md`'s WAL and Transactions rows | no spec conditions soundness on a reader reading only its own core's versions; no spec states a snapshot as a bound on trx ids; `ratification-an-commit-order.md` keeps AN-D1..D4, D6, D7, D9 and its Q-items point here | M |
| AN-S5 | The number: point-lookup and 8-statement RC cells at `cores = 1` and `cores = 8`, delta against AL-S8, under `bench/v3.0.0/` | one file per cell, `git describe --tags`, p0–p100 with p25, the delta column; the mint's cost as ns per statement — **which the mark is expected to make smaller, not larger**, since a mint is now one atomic load where it was a walk over `live_` — and against it the window lookup's cost per tuple at 0, 8 and 64 live transactions per core, which is where the mark spends what the mint saved | S–M |

**Order**: S0 → S1 → S2 → (S3) → S4 → S5. S3 is separable and can be
declined without disturbing the rest. AN-R10 gates S2; AN-R11 gates
nothing and is not scheduled.

## AN-6 — Row status (CLA, appended as rows land)

| row | status |
|---|---|
| AN-S0 | **landed at `b5abab6`.** This document and the `known-gaps.md` entry, source-read at `004f949`, rewritten twice the same day — on the source read, then on AN-R7's mark — and corrected a third time against the first `critics-developer` pass recorded in AN-7 |
| AN-S1 | **landed at `b5abab6`, suite green.** `include/kds/txn/instance_visibility.hpp` and `src/txn/instance_visibility.cpp`; publication from `TransactionManager` at three points plus its constructor; `CoreRuntime::Config::visibility` and the `Expeditor`'s `visibility_`, gated on `single_stream()`; ten cells in `tests/instance_visibility_test.cpp`. **3263/3263 pass** in Debug — the additive claim holds, since every existing cell reads the unchanged per-core predicate. **Its `critics-developer` pass was still running when it landed**, on the operator's word, and AN-7 gains its record when it returns; a finding against S1's code is therefore a fix on top of this commit rather than a change to it |
| AN-S2 | not started; **gated on AN-R10** |
| AN-S3..S5 | not started |

## AN-7 — Review record

Two passes, both at `004f949`. **CLA's own source read** produced the
first table: **four findings** — the leased cursor, the two-document
collision, AN-R8's floor, AN-R9's ordering — and **seven corrections**.
(The draft's preamble said "six wrong, two findings" over eleven rows,
and `index.md` said eleven corrections; three documents, three numbers,
none right. 4 + 7.) **A `critics-developer` pass** then read the result
against the tree and produced the second table, which is where the
design actually moved.

### First pass — CLA's source read

| finding | what it refuted | where it landed |
|---|---|---|
| `up_to_trx_id = ids_.peek()` (`manager.cpp:74`) is a cursor into **this core's leased block** (`trx_id.hpp:151/157`, `kTrxIdBlockSize = 4096` at `:74`) | AN-3 E's "the predicate is indifferent to block interleaving provided the set is the union", and with it the drafted AN-R3's "the predicate is untouched" and AN-2's "the snapshot is a scalar HWM" | AN-3 E rewritten with H1 and H2; **AN-R7 raised**, and the operator marked (B); the whole of AN-R1..R5 rewritten to the marked mechanism |
| `ratification-an-commit-order.md` rules on the same object with an incompatible mechanism, under the same letter | the draft's AN-2 table, which named AR0-3, AM-R1, `rules.md` §3, D1 and `cross-owner-txn.md` §3 and **not the one document that had already ruled on visibility ordering** | the header's two-documents paragraph; AN-2's four rows; the Q-items moved in as AN-R9/R10/R11 on the mark |
| The same block-lease fact reaches **AN-Q1's floor**, not only the high-water mark: a floor defined on resolution alone advances past a lower core's unissued range, and branch 3 then answers `true` for an uncommitted writer | `ratification-an-commit-order.md` AN-Q1's *"a single instance-wide trx id below which every transaction is resolved"* | **AN-R8**, the floor's second bound, with `issue_cursor` added to AN-R1's slot and the H2 shape as an AN-S1 cell |
| `Commit` clears `active_` at `manager.cpp:243`, **after** `wal_->Commit` returns at `:230` — visibility already stands behind durability, by ordering | the obvious closure of AN-Q3, which is to publish the window entry inside the append latch where the LSN is fixed | **AN-R9**: publication stays at `:243` and AN-Q3's ceiling is built; AN-S2 carries the cell that would pass if publication moved |
| `ReadHorizon()` has **four** trx-id-valued consumers — `undo_log.hpp:207`, `catalog.cpp:936`, `catalog.hpp:1283`, `varheap.hpp:46` | the draft's treatment of the horizon as one number with one consumer | AN-3 D lists them; AN-R3 rules the change of unit and AN-S2 is sized L rather than M because of it |
| `Everything()` sets `up_to_trx_id = UINT64_MAX` (`read_view.hpp:133`), a field the mark deletes | AN-3 B's "AN does not touch them" | AN-3 B and AN-R3's last paragraph: the meaning is untouched, the mechanism is not, and the ten sites are an AN-S2 cell |
| There is no `IsVisible`; the method is `ReadView::Visible` (`read_view.hpp:81-90`) | two citations, in AN-1 and AN-R6 | corrected; the line range widened from `84-87` to the whole predicate |
| `Begin`'s `OutOfSpace` is `manager.cpp:86-88`; `read_view.hpp:97` is `AddInFlight`'s | AN-3 C's single guard | AN-3 C names both, and the mark retires both |
| Ten `Everything()` sites, not eleven — nine calls, one comment (`assertion_catalog.hpp:315`), and the definition at `read_view.hpp:131` is the eleventh grep hit | AN-3 B's count | corrected, with the definition named so the number is checkable |
| `rules.md` §3 assigns no number to the next row, and AR0-M2 already queues **two** candidates — D2's lock table and AN-Q1's window | AN-2's "AN adds the fourth row" | AN-2's row rewritten: a row, not the fourth, with all three candidates named |
| The mint sites are `manager.cpp:101` and `:127`; `:559` is `ReadHorizon` | AN-3 A's "`manager.cpp:559`'s comment" for where `Begin` and `StartStatement` mint | AN-3 A names all three lines, which are also AN-R11's two |

Not changed by the mark: AN-0's first paragraph, AN-1, and AN-5's shape.
AN-1's argument never depended on the mechanism — the dirty read it
describes is real under any per-core view — which is why the direction
stood while the ruling moved twice in one day.

### Second pass — `critics-developer`, 2026-09-03, 62 tool calls

It confirmed H1, H2 and AN-R8 against source, deriving AN-R8
independently before reading it, and sharpened H1: the exposure is not
idleness but *every core below the highest block*, ending when that core
burns its own block rather than when the commit ages. It then refuted
five things, of which the first two are design changes and not wording.

| finding | what it refuted | where it landed |
|---|---|---|
| **`min_snapshot_lsn` is not monotone**: `UINT64_MAX` is its maximum, so a stale read is too *high* and yields a horizon **above** a live reader's snapshot | AN-R1's "a stale value is an older horizon, which is conservative" — the justification, for one of the three slot fields | AN-R1's argument replaced: what makes the plain atomic sound is that a newly minted snapshot takes the current ceiling and the ceiling is at or above every live snapshot. **AN-S3's adopted snapshot is the case that argument does not cover**, and it is now a stage cell |
| `WalManager::Commit` (`wal/manager.cpp:358-388`) syncs on `kStrict` **only**; `kGroup` is the default (`tcp_server.hpp:297`) | AN-3 I's "visibility already stands behind durability" and AN-R9's whole stated ground — under the shipping default the engine already makes a commit visible before the platter has it | AN-3 I scoped to `kStrict`; **AN-R9 re-grounded on AN-R2** — `:243` is where the in-flight set drops the transaction, so the window stays in step with it — plus the latch-cost argument, neither of which depends on a durability class |
| `ReadHorizon()` has **two** consumers, not four: `catalog.hpp:1283` is a comment describing the `catalog.cpp:936` call already counted (its other half names the mount sweep, which passes `UINT64_MAX` at `catalog.cpp:907`), and `varheap.hpp:46` is a comment stating a rule its own sentence says that file does not implement | AN-3 D's count, AN-R3's "largest piece of AN-S2", and AN-S2's sizing rationale | AN-3 D corrected with why the miscount happened; **AN-S2 stays L on the predicate cutover and on the retention duty below**, not on the count |
| `MinVisibleBound()` folds `own_trx_id`, which is what keeps an **active transaction's own undo** from being retired; an LSN horizon says nothing about it, and an RC transaction re-minting per statement contributes nothing | AN-R3's silent assumption that changing the horizon's unit moved only the reader duty | AN-R3 states that the load moves to AN-R8's `oldest_unresolved` and that this is sufficient but not obvious; AN-S2 carries a rollback-after-purge cell |
| `crosscore.md:288-290` states the false premise as a fact — *"the trx-id domain is global, so ids compare cleanly"* | the `known-gaps.md` entry, which described the gap without naming the one spec sentence that asserts its negation | named in AN-1 and in the gap entry, with the line number |

Also applied: `AddInFlight`'s refusal is unreachable today (AN-3 C);
nothing folds N `ActiveTransactions::Snapshot()` tables (AN-3 G);
`txn.md` §4.1's clause is stated of the **horizon**, and `Visible` has no
stated condition at all, which makes AN-S4's prose task an addition and
not only a deletion (AN-1); AL-S8's −46–48% is the motive, not a third
mechanism (AN-1); four documents now claim `rules.md` §3's fourth row,
`workorder-am-m1-shared-pool.md`'s AM-S5 among them (AN-2); there is no
`read_view_test.cpp` and the cells live in `tests/visibility_test.cpp`,
`tests/txn_manager_test.cpp` and `tests/txn_session_test.cpp` (AN-S1);
`manager.cpp:75` not `:74`, `trx_id.hpp:153-155` not `:157`,
`trx_id.hpp:159`/`trx_id.cpp:51` not `:167`, `manager.cpp:71-83` and
`:559-579`; and "landed at `004f949`" struck from the header, since this
document was untracked at that commit and AN-6 is where a landing is
recorded.

**Rejected: the review's cut of AM-S0(a) as AN-S1's `cores = 2` bench is
not in the list because it is not a rejection — it is applied.** What is
rejected is nothing; the pass produced no finding CLA disagrees with.
The two it graded most severe were both real, and the first of them
(`min_snapshot_lsn`) would have shipped a purge race into the structure
whose whole purpose is closing one.
