# Work order AX — the instance in-flight publication

Opened 2026-09-08 on `ax-inflight-publication` from `bc1040e`, on the
operator's acceptance of a proposal. Not part of AR0's milestone chain: it
depends on AN-S1's `InstanceVisibility` (landed) and gates nothing that is
open, though AT (Uniformity) inherits it.

## 0. The decision (operator, 2026-09-08, chat session)

Verbatim: *"[proposal accepted] [in-flight] - CLA 권장: active 비트만
publish, single-writer per core. InstanceVisibility가 이미 이 형태입니다 —
코어별 슬롯에 발급 코어만 쓰고 모든 코어가 atomic으로 읽습니다. 같은 패턴을
in-flight에 적용합니다."* — proposal accepted, in-flight: CLA's
recommendation, publish only the active bit, single writer per core.
`InstanceVisibility` already has this shape — per-core slots written only
by the issuing core and read atomically by every core. Apply the same
pattern to in-flight.

**The proposal this accepts is not in the tree.** No document under
`instructions/` or `docs/` carries "active bit" or "single-writer" beside
in-flight, and `git log --all` finds no commit that does; the operator's
own marks document of the same day (`raft-marks-2026-09-08.md` §5) records
it as *"discussed 2026-09-07, not proposed in any order yet; its bearing on
AO-S6's cross-core half is stated in that discussion and nowhere in the
tree"*. This order therefore takes the operator's sentence as the whole of
the contract, in four clauses: (i) per
core, one slot; (ii) written only by the core that issued the transaction;
(iii) read atomically by every core; (iv) the active bit and nothing else.
Everything below is CLA's proposal *under* that mark, and each ruling says
which clause it serves.

## 1. Survey at `bc1040e`

**The predicate is per-core.** `TransactionManager::IsInFlight(trx_id)`
(`src/txn/manager.cpp:725`) walks this core's `live_` and answers
`active_`; a transaction on another core answers `false`, and the header
says so ("Per-core, and the caller must know it"). `IsInDoubt` and
`OldestActiveTrxId()` (`:717`) walk the same list. `active_` is set at
`Begin` (`:127`) before the push into `live_`, cleared at `Commit` after
the window entry is published (`:334-335`: "the window entry goes in
before the in-flight set lets go") and at `Abort` after the undo (`:538`);
`Release` erases from `live_` only once inactive (`:687-692`).

**Its consumers, and which one is wrong across cores.**

| consumer | site | cross-core today |
|---|---|---|
| the blocked-writer wait and its guard | `command_dispatcher.cpp:382,385` (`WaitUntil` on `!IsInFlight(holder)`), `:11040` (`NoteBlockingWriter`) | holders are this core's by CC3; the predicate is asked on the right core |
| the FK probe's park | `fk_probe_service.cpp:236,242` | runs on the holder's core by construction (AO-S5(b)); right core |
| the lock family's woken re-check | `lock_table.hpp:58` (AO-R4's disjunct "or the holder is decided": the window across cores, `IsInFlight` on one) | a cross-core waiter sees a *commit* through the window and an *abort* through nothing but the slot flip; the disjunct's second arm has no cross-core reading of an abort |
| **DT9's delete-mark gate** | `catalog.cpp:118-127` (`ScanAll`: `gone = deleter < oldest_active \|\| !IsInFlight(deleter)`) | **wrong**: a peer reading a catalog page asks its own manager about core 0's DDL transaction, gets `false`, and counts a delete-mark whose deleter is still in flight as settled — an uncommitted `DROP INDEX`/`DROP TABLE` reads as done on every core but 0. `command_dispatcher.cpp:2996` names it: "the claim this may carry is core-0-scoped"; `ddl-transactional.md` §5a carries the caveat |
| `OldestActiveTrxId()` as `ScanAll`'s short-circuit | `catalog.cpp:88-89` | per-core; a peer's value ignores core 0's running DDL, which is the same hole one line earlier |

**The pattern already in the tree.** `InstanceVisibility`
(`include/kds/txn/instance_visibility.hpp:182-202`) keeps one
`CoreVisibilitySlot` per core — `min_snapshot_lsn`, `issue_cursor`,
`oldest_unresolved`, `pending_commit_bound`, each a
`std::atomic<std::uint64_t>` — written by that core alone
(`PublishBounds`, `PublishSnapshotBound`) and loaded by every core's floor
and horizon. Its header carries the store-buffer argument the slots rest
on. That is clauses (i)–(iii); what in-flight adds is that a core's
in-flight state is a *set*, not four scalars.

## 2. Rulings — CLA's proposals under the mark

**AX-R1 — The structure: a per-core table of ids, dense, single-writer.**
`[design; serves (i), (ii), (iii)]` `InstanceVisibility` gains one
`CoreInFlightSlot` per core beside the visibility slot:

```
struct CoreInFlightSlot {
    std::atomic<std::uint32_t> count{0};                       // live entries, at the front
    std::array<std::atomic<std::uint64_t>, kInFlightSlotsPerCore> ids{};  // 0 = empty
};
```

The issuing core is the only writer. `Begin` stores the id at `ids[count]`
(release) then raises `count`; a decide swap-removes: the last entry's id
is stored into the freed position **before** the tail is cleared and
`count` lowered, so a reader scanning `[0, count + 1)` — its own snapshot
of `count`, plus one for the entry a removal may be moving — never misses
a live id. A reader loads with acquire and finds an id or does not; a
stale read answers "not in flight" only for an id that has decided on its
core, which is the answer's meaning. No latch, no CAS: single writer per
slot is what makes a plain store enough, the visibility slots' argument
applied to a table.

**AX-R2 — The active bit, and when it moves.** `[design; serves (iv)]`
What is published is presence: an id is in its core's table exactly while
its `Transaction::active_` is true. `Begin` publishes after `active_ =
true` and before the handle is returned; `Commit` retires **after**
`PublishCommit` puts the window entry in — AN-R2/AN-R9's "no instant in
neither record" now holds for a reader on any core, not only this one;
`Abort` retires where `active_` falls, after the undo. Nothing else is
published — not `prepared_`, not the isolation, not the session — which
is the clause the mark states by name.

**AX-R3 — The query goes instance-wide.** `[design]`
`TransactionManager::IsInFlight(trx_id)` answers from the tables when a
visibility is attached: this core's first (the common case and the one
`cores = 1` ever takes), then every attached core's — O(cores × live),
against today's O(live) on one core. `IsInDoubt` stays per-core: prepared
is a local fact and its one caller asks about local contexts.
`OldestActiveTrxId()` becomes the instance's: the minimum of
`oldest_unresolved` over attached visibility slots, which AN-S1 already
publishes, so no second structure is needed for it.

**AX-R4 — The cap, and the only honest overflow.** `[constant;
quiet-wrong if degraded]` `kInFlightSlotsPerCore` bounds the table; CLA
proposes **1,024** (8 KiB per core, 64 KiB at eight). A transaction that
cannot be published would answer "not in flight" on every other core
while running on its own — the DT9 gate would then count its drop as
settled — so `Begin` past the cap is refused `ResourceExhausted` naming
the core and the cap, never admitted unpublished. AX-Q1 below.

**AX-R5 — The consumers.** `[design]` `ScanAll`'s gate and short-circuit
read the instance predicate (AX-S2), which closes the core-0-scoped clause
at `command_dispatcher.cpp:2996` and `ddl-transactional.md` §5a's caveat
as facts rather than restating them. The wait sites and the FK park call
the same predicate — their holders are local, and the instance query finds
a local id in its own slot first, so they pay one load more than today and
no scan. The lock family's disjunct gains what it lacked: a cross-core
waiter's re-check can read the holder's active bit and see an abort.

**AX-R6 — `cores = 1` is byte-identical in what it answers.** The slot
exists and the query finds every id at core 0's table; no cross-core scan
runs. The cap refusal is the one new answer at one core, and only past
1,024 open transactions.

## 3. Stages

| stage | what | exit | size |
|---|---|---|---|
| AX-S0 | This order; the index row | the files at the commit | the hour |
| AX-S1 | `CoreInFlightSlot`, the three publication points, the instance-wide `IsInFlight`, the cap refusal | on the two-core rig: a peer answers **true** for core 0's open transaction, **false** after its `COMMIT` and, in a second cell, after its `ROLLBACK`; the swap-remove's ordering as a unit cell (a reader between the copy and the clear finds the moved id); the cap refusal at `cores = 1` names the cap; every existing cell unchanged | M |
| AX-S2 | The consumers: `ScanAll`'s gate and `OldestActiveTrxId()` instance-wide; the wait sites and the park on the one predicate | on the rig: a peer's catalog read during core 0's uncommitted `DROP INDEX` still resolves the index, and after the commit does not — the cell `ddl-transactional.md` §5a could not have; the same over `DROP TABLE`; **mutation**: the per-core walk back, and the first cell fails | M |
| AX-S3 | Prose: `txn.md` §4.1 and §5, `ddl-transactional.md` §5a, `crosscore.md` CC11 and §5, `rules.md` §3 (a new declared row: the in-flight tables), the dispatcher's two comments | no spec says the predicate is per-core | S |

## 4. Items for the operator

| # | item | class | CLA proposal |
|---|---|---|---|
| AX-Q1 | **Marked 2026-09-28 as proposed** (`raft-marks-2026-09-28.md` §9), built at AX-S1. `kInFlightSlotsPerCore` | constant | 1,024; a refusal past it rather than an unpublished transaction |
| AX-Q2 | **Marked 2026-09-28 as proposed** (`raft-marks-2026-09-28.md` §9); AX-S2 builds it. DT9 on peers changes behaviour: a peer stops seeing an uncommitted drop as done | user-visible | take it — it is the isolation §5a promised and could not keep across cores |

## 5. Sequencing

A build must not overlap a measurement on this host (`bench/README.md`
rule 4), and AM-S6's re-baseline is queued behind the two branches landing
today. AX-S1 therefore starts when that measurement returns, unless the
operator orders it ahead.

## 6. Row status

### AX-S1 — built 2026-09-28

On `worktree-ax-s1-inflight-publication` from `8627f0d`
(`v2.7.0-462-g8627f0d`), on the operator's word *"take 1, AX-S1 — Q1 and Q2
as proposed"* (`raft-marks-2026-09-28.md` §9), which supersedes §5's
sequencing condition.

**What landed.** `CoreInFlightTable` (`kds/txn/instance_visibility.hpp`): one
per core, 1,024 ids (AX-Q1), allocated when the core's manager attaches,
single writer, no latch. `InstanceVisibility::AttachInFlight`,
`PublishInFlight`, `RetireInFlight`, `InFlight`, `InFlightFull`. The three
publication points in `TransactionManager` (`src/txn/manager.cpp`): `Begin`
publishes after the begin record and the push and before the handle
returns, and refuses `ResourceExhausted` naming the cap **before** it issues
an id or appends; `Commit` retires after the window entry and the pending
marker are both in and before the schema word and the borrows; `Abort`
retires after the compensations, before the same two. `~TransactionManager`
retires an id still running. `IsInFlight` answers from every core's table,
its own first. `CoreRuntime::transactions()` is the rig's way to ask a
core's manager.

**Cells.** `instance_visibility_test.cpp`: the table's own contract, the
swap-remove interleaving twice (the retire landing before each place of one
scan; and a script of three retires and a publish reusing a freed place,
inside one scan), the cap per core; the manager's peer-sees-open-until-
commit and until-rollback, the cap refusal at one core spending no id and
no record, a torn-down manager's retire. `inflight_publication_rig_test.cpp`:
the exit's two rig cells, commit and rollback. **Six mutants, six kills**:
an upward scan (both swap-remove cells), `IsInFlight` walking `live_`
(five cells), no retire at commit (three), none at abort (two), no cap
check (the cap cell, by the out-of-bounds abort a Debug build takes), none
in the destructor (one).

**What it corrected in this order.**

1. **AX-R1's scan was wrong.** A reader scanning `[0, count + 1)` upward
   misses a live id: over `[A, B, X]` it reads A at 0, the writer retires A
   (X copied to 0, place 2 cleared), and it reads B at 1 and 0 at 2. A
   swap-remove only moves an id *down*, copy before clear, so the scan runs
   **downward from `count - 1`**; the header states the argument and the
   first swap-remove cell drives the miss. The review tried consecutive
   retires, a reused place, a stale count, the cross-core bound and the
   table pointer against it, and did not break it.
2. **§1's survey describes a tree that is gone.** At `8627f0d` there is no
   `fk_probe_service.cpp` (AT-S5f) and no `IsInDoubt` (AT-S6), and the
   dispatcher's row waits are `NoteBlockingWriter`'s. AX-R3's `IsInDoubt`
   clause has nothing to apply to.
3. **AX-R3's `OldestActiveTrxId()` cannot become the instance's.**
   `PublishCoreBounds` feeds it into this core's slot as the core's own
   oldest unresolved id, which the floor reads per core; an instance-wide
   value there would put one core's transactions in every core's term.
   AX-S2 needs a query of its own beside it for `ScanAll`'s short-circuit.
4. **S1 moves consumers the order put in S2**, because they call the one
   predicate. `NoteBlockingWriter` now parks a writer on a holder on
   another core rather than refusing it - the `known-gaps.md` Locks entry,
   rewritten with this row - with no kick behind it and no cell driving
   it. `ScanAll`'s `IsInFlight` arm is the instance's and its
   `deleter < oldest_active` short-circuit is still the core's, so on a
   peer with nothing running DT9 still counts core 0's open drop as done:
   AX-Q2's behaviour is not built until AX-S2. Every answer is at least as
   right as before the stage.

**The review** (`critics-developer`, one pass) found no defect in the table
and eight things around it. Taken: the commit's retire moved from before the
pending marker's release to after it - retiring first let a woken peer mint
a view still capped below the commit and refuse the write it had waited to
make - with the comment saying why both neighbours are load-bearing; the
table allocated at attach, so a publication allocates nothing, is
`noexcept` and follows the push; the two cross-core loops made one rotation;
the destructor retiring before it releases, as a decide does, and saying the
one place "not in flight" does not mean compensated; the longer interleaving
cell; the known gap and `txn.md` §5's sentence, which this stage makes
false. **Not taken**: a runtime guard on a publication past the cap - a
silent one would make the unpublished running transaction AX-R4 refuses,
and `Begin` asks and publishes in one synchronous call on the only core
that writes the count. **Recorded, not fixed**: the wait has no wake and a
re-run can meet the holder's borrow before its release (`known-gaps.md`,
Locks: AX-S2's); `manager.cpp`'s comment that the autocommit write scope
leaks on a failed commit is stale from before this stage. **Left for AX-S3**,
per its row: the comments saying `IsInFlight` is per-core, in
`lock_table.hpp`, `command_dispatcher.hpp`/`.cpp`, `catalog.hpp`/`.cpp`,
`manager.hpp`'s `OldestActiveTrxId`, four test headers and `txn.md` §5's
later paragraphs.

**Suite**: 3008/3008 in Debug after the review's changes, the one disabled cell pre-existing (`HeapSuspensionIsLifted`); 3007/3007 before them. The six mutants were re-run after the review and killed again. Overhead not measured.
