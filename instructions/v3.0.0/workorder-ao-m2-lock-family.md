# Work order AO — AR0 M2: the lock family

Written 2026-09-03 on `ar2-borrow-model` at `9e5068c`
(`v2.7.0-181-g9e5068c`; no v3 tag exists, AR0-M6). The survey below is a
source read at that commit; citations into `index.md` and `rules.md` are at
`9e5068c` too, **before this order's own two edits** to those files shift
their later lines by one. Written against
`instructions/v3.0.0/ar2-architecture-revision-borrow-model.md` (AR2) and
the operator's ratification `raft-ar2-A.md` (AR2-A).

**Status: AO-S0 lands now; no code stage starts before the operator's
word.** AR2-A §6 (`raft-ar2-A.md:97-102`) starts no prototype, and AR2-A §1
(`:20-21`) opens M2 when M1 (AM) and AN-S2 close. At `9e5068c` AM-S1..S6 are
not started (`workorder-am-m1-shared-pool.md:235`) and AN-S2 is gated on
AN-R10 (`index.md:36`). The point of writing this now is AR2-A §6's own
sentence: *"so that AM and AN proceed against a stated M2 rather than an
unstated one."*

Three decisions were taken by the operator in plan mode on 2026-09-03 and
are recorded where they land: **no persisted lock bit** (AO-R3, AO-0 item
4); the `rules.md` §3 row lands with this document, marked not built
(AO-R2); AM-S1 runs its `cores = 1` A/B of the **page latch** (AM's row,
not this order's) — the lock table's own `cores = 1` A/B is S7's, a
different number.

---

## AO-1 — The direction, and what it is not

**What M2 is.** AR0 §8 step 5 (`ar0-architecture-revision.md:153`): the lock
manager (D2), async waits (D13), deadlock detection (D12), gap locks (D8).
AR2 §9 step 3 refines it into rules: R1–R4, R6, R9, R10, R13, R14 with
D2(a), D12, D13; the tuple lock, the slice fence, the relation lock. M2 is
evaluated on AR2-A §1's three axes in order — **refusal → wait first**,
flexibility, the physical optimizer's foundation — and not on throughput.

**The axis, made concrete.** AO-3 B is the census: every refusal the engine
issues today because a permanent owner or an in-flight writer is elsewhere,
each classed core-local or cross-core, each assigned the stage that turns
it into a wait, retires it, or keeps it. The one refusal the model *adds*
and keeps is R4's cap (AR2-A §3, `raft-ar2-A.md:59`).

**What it is not.** Not M3 — AO-2's table says which rules and items move
there (AR2 §9 step 4). Not the page latch (AM-S1). Not a mover — none exists; `physical-optimizer.md` is
shadow-only. Not `SERIALIZABLE` — only its *reason* text changes (AR0-M1,
`ar0-architecture-revision.md:303-317`). **Not execution locality**: AM-R1's
routing of writes to the relation's owner (`workorder-am-m1-shared-pool.md:
141-152`) stays through M2; M2 replaces the *guard* (write rights) with the
lock, and M3 changes *where a write runs* (R12). AO-R14 carries the split.

**What lands now.** This document; the `rules.md` §3 row (a spec change
first — AR0-M2 `:365-373`, AR2 §8); `index.md`'s row. No code, no comment
fix, no test: the stale comment at `src/txn/manager.cpp:59-61` that AR0-M1
flags is fixed in S3, when that file is opened.

---

## AO-2 — Where this sits against AR0's D-items and AR2's E-items

| item | mark at `9e5068c` | bearing on M2 |
|---|---|---|
| D2 | (a), AR0-M2 | **AO-R2**: the table, its partition constant, the declared-shared row |
| D12 | pending; its priority stated by AR2-A §5 item 1 → AR2 R10 | **AO-R7, AO-R8**: detection is the mechanism, the timeout a fault net |
| D13 | pending, `[source-read required: sched wake path]` | discharged in AO-3 C; **AO-R4** |
| D8 | as proposed, AR0-M3 | the slice fence, **S6**; AS4 struck there (`ar0-architecture-revision.md:416-419`) |
| D9 | (a), AR0-M4 | **M3's** (AR0 §8 step 6; AR2 §5.3). M2 turns F3's *busy* into a wait (S3) and builds no `S` fence — AO-R14 states the split |
| D1 | (b) conditionally, AR0-M1 | **AR0 D16 lists D1 with M2 (`:133`); AR2 §9 does not.** AO takes only the four SR reason texts AR0-M1 assigns to M2's work order. RU is not here — it needs its own ruling on the RU writer's first-updater-wins view (`ar0-architecture-revision.md:329-330`) and the operator's word on which order carries it. AO-0 item 8 |
| D14 | pending | **not needed**: M2 changes no format (AO-R13) |
| D16 | pending | the letter AO continues the series |
| E1, E4, E11, E12 | ratified (AR2-A §3) | built as ratified: modes, the slice key, scope-bounded, `IS` at R14's unit (S1, S6) |
| E2 | deferred to M2's opening (AR2-A §4) | **in this table**: AO-R10, AO-0 item 1 |
| E3, E5, E7, E8–E10, E13 | M3 / C1–C3 | not here; S7 produces C3 for E7 and E12's price |
| E6 | AN's | not here |
| R13, R14 | ratified (AR2-A §2) | S6 |

---

## AO-3 — The survey: what the lock family meets, at `9e5068c`

**A. Nothing of the lock family exists, and the tree's own texts say so in
the same words.** `include/kds/txn/manager.hpp:22-28` ("No lock manager, no waiting,
no deadlock detection, and the Keystone lock byte stays unused");
`docs/spec/txn.md:401-402`; `docs/spec/assertion.md:324-325` and `:336-338`
("Row locking (Keystone lock byte) is not used by this protocol");
`docs/spec/foreign-keys.md:25-31` (F3) and `:414`. The comment at
`src/txn/manager.cpp:59-61` is the one AR0-M1 already calls wrong on its
second half. Neither `include/kds/txn/` nor `src/txn/` holds a file with
lock or borrow in its name. `include/kds/storage/keystone.hpp:23-29`
describes a CAS that "happens at the call site, once a frame/page
abstraction exists" — never written; the only `Keystone::Encode` caller is
`src/exec/row_codec.cpp:834`, flags `0`. No page latch exists (AM-3 A,
still true; `base/latch.hpp`'s includers are `wal/stream.hpp` and
`include/kds/txn/instance_visibility.hpp:10`).

**B. The refusal census — axis 1.**

| # | site | code today | locality | fate | stage |
|---|---|---|---|---|---|
| 1 | first-updater-wins, `src/txn/manager.cpp:136-155` (the refusal at `:153`) | `TxnConflict` | core-local: every writer of a relation runs on its owner under AM-R1; two sessions on one core conflict at `cores = 1` | **wait** for the holder's decide, then re-check | S3 (statements that wrote nothing), S3b (mid-statement) |
| 2 | the in-doubt block, `src/server/command_dispatcher.cpp:284-363`; recorded at `:939-944`; the choice at `:10633` (`may_park_ && clock_ != nullptr && txn_->IsInDoubt(cur)`) | a **wait that ends by clock** at `in_doubt_ceiling_ms`, then `TxnConflict` (`:346-352`) | core-local | subsumed by #1: an in-doubt holder is a holder that decides late; the clock-end is exactly what R10 forbids and goes | S3 |
| 3 | FK forward busy: `include/kds/txn/visibility.hpp:143` → `src/exec/fk_check.cpp:24-25` → `src/server/command_dispatcher.cpp:4625-4630`; the probe's own busy at `src/server/fk_probe_service.cpp:118-121` | `TxnConflict` | a same-core parent: core-local; a shipped probe: cross-core | **wait** for the writer's decide, then re-check — the wait half of F3. D9(a)'s `S` fence is M3's | S3 (same core), S5 (the probe handler parks) |
| 4 | `IndexBuildPending`, `src/server/core_affinity.cpp:40`, caller `src/server/command_dispatcher.cpp:6560` under the `Covers` guard at `:6559` (this row said `core_affinity.cpp:65-74` and `:6306` until AO-S6e-d; both had moved) | `TxnConflict` | cross-core: the catalog half on core 0, the build on the owner | **wait — and not on the relation lock this row proposed.** Built at AO-S6e-a as a park on the owner's own window (`!Covers(oid)`), bounded by `kIndexWindowWaitNs` and falling back to this refusal. The lock cannot serve here: the window's close, the owner's catalog-cache drop and its next admitted write are one ordered event on the owner's reactor, and an `X` released by core 0 at its decide is seen before either ring message is drained | S5, then **S6e-a** |
| 5 | `RelationWriteRightsPending`, `src/server/core_affinity.cpp:52-63`, caller `command_dispatcher.cpp:6484`; `MayWrite`'s lease/grant arm, `src/storage/device_page_store.cpp:479-488` (the `TxnConflict` branch) and `:804-817` | `TxnConflict` | cross-core: a grant from core 0 | **retired**, not waited: AR2 §8 retires the grant arm "with AM-R1" at M2 — read as *alongside* it, the routing staying (AO-1). What replaces a page's write-right is the page latch and the shared pool (M1); M2 retires the refusal and its grant demand (AO-R14) | S5 |
| 6 | `CrossCoreWriteRefused`, `src/server/core_affinity.cpp:19-31`, callers `command_dispatcher.cpp:6257`, `:6277`; counted by `CrossCoreWriteCounters` (`include/kds/server/core_affinity.hpp:65-83`, "the residue") | `TxnConflict` | cross-core | **not M2's**: the residue is a statement spanning two owners (M3, AR2 §5.7) and a write inside an explicit transaction, which already ships and enrols (`include/kds/server/command_dispatcher.hpp:2560-2573`). Execution locality is R12, M3's; M2 builds the borrow that makes it legal | M3 |
| 7 | `CrossCoreReadNotImplemented`, `core_affinity.cpp:33-50`, caller `command_dispatcher.cpp:6914` | `NotImplemented` | cross-core | **not M2's**: local reads are M1's and AN-S2's; the read shapes are AR2 §5.7's | M3 |
| 8 | `PeerDdlRefused`, `core_affinity.cpp:109-114`, caller `command_dispatcher.cpp:1330` | `Unsupported` | cross-core | **kept**: DDL ships to core 0 (CC13, R12) | — |
| 9 | `MayWrite`'s system-range arm, `device_page_store.cpp:480-488` (the `InvalidArgument` return at `:488`) and `:811` | `InvalidArgument` | cross-core | **kept** until E13 (M3) | — |
| 10 | the three spent leases: `src/storage/extent_lease.cpp:139`, `include/kds/catalog/row_id_lease.hpp:114`, `include/kds/txn/trx_id_lease.hpp:65` — all `TxnConflict` since `include/kds/base/status.hpp:145-155` | `TxnConflict` | cross-core: a refill from core 0 | **kept**: allocator authority, retained by AR0-4. The range's id block is R5's borrow and its refill is a ring ask — a message wait, not a lock wait. C2's "6–7 refused `INSERT`s per 10,000 while a range opens" is **not** this class — every logged line is #6's `CrossCoreWriteRefused` (`bench/v3.0.0/results-ar2-c2-spreading-v2.7.0-178-g92cb654.md:252-266`), and AR2 §9 item 1 files it under E13 (M3) | — |
| 11 | assertion admission: no busy answer at all — `docs/spec/assertion.md:308-320`, `:324-332`; the "bounded false rejection" (`:330-332`, `status.hpp:90-99`) refuses `AssertionViolation`; the build refuses `TxnConflict` on an in-flight writer (`assertion.md:456`, `src/exec/assertion_build.cpp:208`, which this row cited as `:203` until AO-S6e-d) | `AssertionViolation` / `TxnConflict` | core-local (AS4) | the false rejection becomes a **wait** on the reserving transaction — built at AO-S6e-c, on the family's own channel so the wait-for graph sees the edge. **D8's slice fence is not built and is not owed**: the check and the reserve run inline in one statement with nothing between them on a cooperative core, the enforcer is per-core, and a relation's writes run only on its owner, so an `S`/`X` fence over the group would guard an interleaving that cannot occur. AS4's striking is `ar0-architecture-revision.md:416-419`'s, not this row's. **The row's other refusal is untouched**: `CREATE ASSERTION`'s own `TxnConflict` on an in-flight writer stands, and is not this stage's - a build that counted an in-flight row and lost the abort would overstate the group for ever | S6, **admission built at S6e-c; the build's refusal kept** |
| 12 | the multi-owner statement refusals: `include/kds/server/command_dispatcher.hpp:2602-2606`, `docs/spec/crosscore.md:369-394`, the split gates `include/kds/exec/range_eligible.hpp:83-91` | `NotImplemented` / `TxnConflict` | cross-core | **M3's** (AR2 §5.7, gate by gate) | M3 |
| 13 | **new, and kept**: R4's cap (E2); the deadlock victim — a new *reason* on `TxnConflict`, R1 aborts the waiter, no new code | `ResourceExhausted`; `TxnConflict` | — | AO-R10, AO-R7 | S1, S4a |

**C. The wait primitive exists, is polled, and is the statement's.**
`include/kds/sched/coro.hpp:446-457` (`WaitUntil`), `:183-190`
(`ConsumeWaitIfSatisfied`), `:354-364` (a parked coroutine costs one
predicate call per reactor iteration and is never entered);
`docs/spec/sched.md:52` (polled at most once per iteration), `:54` (a
parked task is not runnable; the reactor may sleep). Twelve `WaitUntil`
sites in `src/server/command_dispatcher.cpp`, among them the durability
park at `:725-727` (`wal_->IsDurable(decision_lsn)`), and
`src/wal/manager.cpp:424-436` says of it "the waiter is a parked task
polling `IsDurable`". `src/exec/step_vm.cpp:1986-1995` is the one
mid-statement park: no pin, no span. **Consequences for D13:** a same-core
grant needs no wake — the granting task runs on the reactor that polls the
waiter. A cross-core grant needs the waiter's reactor awake, and the only
wake whose sleep race is closed is the ring's (`sched.md:69`, `:97-110`;
`include/kds/sched/ring_transport.hpp:73-84` — `HasPending` re-checks the
*ring*, not a flag in a table), and the only cross-core path the simulator
can drive is the transport (`docs/rules/rules.md:39`; `sched.md:111` "the
simulated transport does not wake"). So D13's "via the existing ring wake
path" is read literally: the grant is a ring message that says "look at
the table"; the waiter's predicate reads the table. At `cores = 1` nothing
constructs a transport (`sched.md:72-73`), so the cross-core path is
unreachable by construction.

**D. The one existing wait is a statement restart, for statements that
wrote nothing.** `include/kds/server/command_dispatcher.hpp:521-541`; the
loop at `src/server/command_dispatcher.cpp:299-334` takes one deadline,
parks on `WaitUntil{decided}` (`:310-313`), re-runs via `DispatchAndStage`
(`:331-333`). `:10618-10621`: the conflict is detected "inside a page span
and a row callback, which is no place to park" — the park is on the
statement. `include/kds/server/session.hpp:37-43`: failure atomicity is per
transaction, not per statement, so a ten-row `UPDATE` that meets a held row
7 cannot be restarted; `HandleUpdate` is synchronous
(`command_dispatcher.hpp:1860`) while `DispatchAsync` is the coroutine
(`:875`). **This is the seam S3 widens, and the reason S3b is its own
stage.**

**E. Release-at-decide has two hooks and no list.** `src/txn/manager.cpp:
225-264` (`Commit`: `PublishCommit` at `:259`, `active_ = false` at `:261`)
and `:430-467` (`Abort`: compensations `:437-444`, `active_ = false` `:457`).
`Transaction`'s fields (`include/kds/txn/manager.hpp:194-201`) carry the
undo trail, not a borrow list. `MarkPrepared` (`:186-189`) keeps a prepared
transaction live until its decide, which is what makes #2 an ordinary lock
wait. AO-R6's ordering rule follows from `:259`.

**F. The Keystone byte is on disk, unused, and promised as something no
page has.** `include/kds/storage/keystone.hpp:33-35`;
`docs/spec/heap-and-tuple.md:95`: "Transaction/status byte, Oracle
lock-byte style; **may reference a per-page transaction slot**" — the page
format has no transaction slot (`include/kds/storage/heap/heap_page.hpp:
135-147` is the whole tuple header). The MVCC header's own `flags` byte
(offset 18, `:146`) is written `0` at `src/storage/heap/heap_page.cpp:
161-163` and `:405-407` and read nowhere; the delete mark is a *slot* flag
(`heap_page.hpp:119`; `:111` is the dead mark beside it). **Five paths carry a page byte to disk or
the log:** writeback stamps the checksum on the frame and copies it
verbatim (`src/storage/device_page_store.cpp:1147-1167`);
`FULL_PAGE_IMAGE` (`include/kds/wal/record.hpp:59`, `docs/spec/wal.md:137`);
`HEAP_INSERT`'s "tuple bytes incl. Keystone word" (`wal.md:100`);
`HEAP_OVERWRITE`'s image (`record.hpp:55`); the undo before-image and the
trail's `image` copy (`include/kds/txn/manager.hpp:116-120`). The four byte-for-byte suites
(`tests/CMakeLists.txt:38, 92, 93, 139`) and the golden log's CRC
(`tests/wal_golden_log_test.cpp`) observe all of them. The Bound Cabin
entry's `flags` (`include/kds/storage/cabin_bound_page.hpp:24, 60-87`;
`src/storage/cabin_bound_page.cpp:84-88, 102-104`) are the entry's own bits
in the same packing, not a copy of the tuple's byte — no hazard there.

**G. The `X` fast path already exists: the stamp.** The header's 48-bit
`trx_id` names the writer (`heap_page.hpp:130-133`); first-updater-wins is
a read of it under run-to-completion (`include/kds/txn/manager.hpp:63-68`: "nothing
suspends between reading a tuple's header and overwriting it"). AM-S1 turns
that premise into "under the page latch". A wait *is* a suspension between
the read and the write, so the re-check after the park is mandatory — the
premise M2 breaks, stated.

**H. `cores = 1`.** `include/kds/base/latch.hpp:33-49` (`Latch =
std::mutex`, `LatchGuard(nullptr)` = not shared); `src/server/expeditor.cpp:
809-810` (`single_stream() && core_count() > 1`); AM-R3's run-time-branch
measurement (`workorder-am-m1-shared-pool.md:162-172`, AM-S1's cell).
Finding B #1 fires at `cores = 1` — two sessions on core 0 conflict today —
so what R9 compiles out is synchronization, not the wait (AO-R9).

**I. The detector's home.** D12 says the log core; under M0 that is core 0
(AM-R5, `workorder-am-m1-shared-pool.md:187-192`). The `system` group
(`sched.md:50`) with `MaybeBurnIdleBlock` as the periodic-tick precedent
(`include/kds/txn/manager.hpp:573`). `Transaction` objects are core-local (`include/kds/txn/manager.hpp:65`), so core 0 cannot abort a peer's victim directly — a ring message
does it.

**J. The timeout knob.** `include/kds/server/txn_2pc_service.hpp:367-411`:
`kTxnInDoubtCeilingNs` = 200 ms, "one number, two waits" (the participant's
ask and the writer's stall); `kTxnPhaseDeadlineNs` = 10 s (`:365`); the key
`in_doubt_ceiling_ms` (`src/server/expeditor.cpp:83`,
`manual/server/server.md:100`, `docs/spec/cross-owner-txn.md:409`). AR0-V
(`ar0-architecture-revision.md:273-274`): "D12's 1 s constant duplicates a
knob … a lock-wait ceiling is its re-scope." 2PC is retired on paper (AR0
§4.5, AR2 §5.5) and present in the tree; a 1 s net beside a live 10 s
coordinator deadline would fire on a slow but honest holder. AO-R8.

**K. The shared-object precedent, and two stale sentences.**
`include/kds/server/core_runtime.hpp:271-278` hands every core an
`InstanceVisibility*` on `Config`; the lock table copies that wiring.
`include/kds/txn/instance_visibility.hpp:94-99` states the window latch's
order, yet at `9e5068c` that latch is in neither `rules.md` §3's table
(`docs/rules/rules.md:26-30`, three rows there; this order's lock-table row
is the fourth, and AM-S1's page latch the fifth) nor `sched.md` §9-2 (`:189`:
"those three are the whole list") — AN-S4 owes both
(`workorder-an-read-view.md:670`). `include/kds/sched/ring_transport.hpp:
16-19` still says "no shared engine state, no atomics outside ring
indices" — stale since `rules.md` §3's revision at AL-S9. S8 must not
repeat the omission for the partition latches.

**L. Tests and the simulator.** One executable, one source list
(`tests/CMakeLists.txt:4-173`); the fixture shape `tests/txn_manager_test.
cpp:50-57` (`SuperBlock::CreateFresh`, `TrxIdSequence`, `UndoLog`,
`TransactionManager` over `InMemoryPageStore`, `wal = nullptr`). Every
two-core test drives `RealRingTransport` on real threads
(`tests/core_runtime_test.cpp:297-304, 322, 388, 1002, 1097, 1388`);
`SimRingTransport` has exactly two users, both transport tests. **The
simulator has one session** (`sim/instance.hpp:11`, `:188`;
`sim/workload.hpp:31-33`), so no conflict is reachable in the corpus.
Consequences: same-core cells need a two-session fixture on one reactor;
cross-core cells need a deterministic two-`CoreRuntime` rig over
`SimRingTransport`, which does not exist — S5's first cell is the rig, the
way AM-S0 was the assembly. The nearest same-core shape is synchronous:
`tests/txn_session_test.cpp:136` runs two sessions on one dispatcher over
`Dispatch()`, which cannot park (finding D); S2's fixture is that pair
moved onto a reactor.

**M. E2's code.** `include/kds/base/status.hpp:59-69` (`kResourceExhausted`,
"a statement spent its per-statement work budget",
`include/kds/exec/budget.hpp:8-30`); `IsRetryable` one code wide (`:156`),
and `:141-156`'s rule already files "a cap" under `kResourceExhausted`;
`docs/spec/protocol.md:142-147` (categories pinned, details append-only,
`retryable` from `IsRetryable`); `tests/kwp_error_test.cpp:48`
(`RESOURCE_EXHAUSTED` = 13), `:118` (not retryable). A cap detail code is
append-only.

**N. AR0-M1's four SR texts** are still where it says: `docs/spec/txn.md:
31-34`, `src/txn/manager.cpp:63-65`, `manual/sql/sql.md:758`,
`docs/spec/client-manual.md:370`. S8.

---

## AO-4 — Rulings AO-R1..AO-R14 (CLA's proposals)

**AO-R1 — The subsystem is named by its family: `lock`.** `[design; AO-0
item 5]` `include/kds/txn/lock_table.hpp` + `src/txn/lock_table.cpp`
(partitions, unit keys, modes, compatibility, queues, grant and wake, the
cap) and `src/txn/deadlock_detector.{hpp,cpp}`. First line of the header:
"The borrow model's **lock family** (AR2 §2) — AR0 D2's lock manager." Why
not "borrow": AR2's "borrow" is the *genus* — a tenancy in either family —
and the latch family is AM's and already named; a subsystem called
"borrow" that holds only the lock family would make one word mean two
things, which is the second-name hazard wearing the spec's own vocabulary.
The key stays `max_locks_per_txn` as R4 spells it. The reverse
(borrow-named, lock in the first line) is the alternative; the operator
picks.

**AO-R2 — The table, what serializes it, and the declared-shared row.**
`[design; the count is [constant]]` D2(a): one table for the instance,
owned by the expeditor and handed to every core on `CoreRuntime::Config`
beside `visibility` (`core_runtime.hpp:271-278`). Partitions: **64 × cores**
`[constant, AR0 D2's, re-measured in S7]`, keyed by `(rel_oid, unit kind,
lo)` so a relation's entry and its tuples hash apart. Each partition: a
`Latch*` from `base/latch.hpp` — null at `cores = 1`, a `std::mutex` above
it (AR0-M2 `:355-363`: the spin primitive was deleted at `7839a29`;
re-introducing one is a decision S7's numbers can ask for, not one S1
takes) — over its chains; the relation entry's counters (`IX`/`IS` holders,
`S`-fence count, waiter count) are atomics. **Order:** a partition latch is
taken with no page latch held, with no other partition latch held (one
partition per operation), never under the WAL latch or the window latch,
and released before any park. The row `docs/rules/rules.md` §3 carries from
this document's commit:

> | The **lock table** — the lock family's partitioned table, one for the instance (**M2; not built at this row's commit**) | a `base/latch.hpp` latch per partition, null at `cores = 1`; relation-entry counters are atomics; taken with no page latch, no WAL latch and no window latch held, one partition at a time, released before any park | `instructions/v3.0.0/workorder-ao-m2-lock-family.md` AO-R2 until AO-S8 moves it to `docs/spec/txn.md` §5 |

The ordinal is not stated: "fourth" is already contested by four documents
(`workorder-an-read-view.md:84`).

**AO-R3 — No persisted bit: the tuple `X` lock's fast path is the stamp,
and the Keystone flags byte stays zero on disk and in memory through M2.**
`[decided by the operator 2026-09-03; a persisted-format question under
invariants 5 and 6]` Finding G: the header's `trx_id` under the page latch
already names the holder and is written by every `UPDATE`/`DELETE`; the
table adds the *wait* and the `S` mode, not the lock. Finding F: any bit in
the byte reaches disk by five paths, and since same-core waits exist at
`cores = 1` (finding H) a waiter bit would make the on-disk image depend on
interleaving — the byte-identical property `index.md:11` promises would
break in the golden log and the four contract suites. So: the table
carries `IX`/`IS`, `S` fences, slice fences and waits; a writer reads the
relation entry's `s_fences` count on the `IX` it takes anyway (R3) and
probes its tuple key only when that count is nonzero; a decide probes a
trail entry's tuple key only when its partition's waiter count is nonzero.
**Departure from AR2-R2's wording** ("a writer takes the tuple by CAS on
the byte under the page latch") is flagged and AR2's text is amended by
S8. The byte's promise at `heap-and-tuple.md:95` becomes "reserved and
zero" in S8. The arm the operator declined: use the byte, with the rule "a
lock bit is masked out of every persisted and logged image (the five sites
of finding F), and a set bit read from the device is `Corruption`" — an
L-size cross-cutting change with a byte-for-byte hazard.

**AO-R4 — The wait is the existing park, widened.** `[design]` A wait is a
`WaitUntil` on a per-wait slot the table owns; the waiter holds no pin, no
span and no latch when it parks (`SuspendAudit` is the proof in debug); the
predicate reads the table. Same core: the holder's decide flips the slot;
the waiter's next poll proceeds. Cross core: **the decide flips the slot itself and
kicks the waiter's core** — write-then-kick, AR0-6-R1, built at AU-S1 as
`WakerTable::Kick`. *(Amended at AU-S3. This read "the decide sends one ring
message (`kLockWake`, a new `ring_message.hpp` kind with no payload of
substance) to the waiter's core, whose handler flips the slot". AR0-6 retires
the transport and AU-R4 forbids adding a kind, so `kLockWake` is never built;
the slot is already shared state under the partition latch, and only the
interrupt was ever needed.)* The kick is a wake, never the decision, so a
delayed or lost kick costs latency and never liveness — at most one idle
block, which is the cost AR0-6-R1 accepts by name. After every park the waiter **re-checks** (finding G).

**The predicate is a disjunction, and the registration precedes the last
check.** A waiter that saw the holder in flight, then registered *after*
the holder's decide had swept its list, would park on a slot nothing will
ever flip — a lost wakeup whose only exit is the net, which is the clock-end
R10 forbids. So the predicate `WaitUntil::await_ready` evaluates before the
park (`coro.hpp:449`) is "the slot flipped **or** the holder is decided"
(the window cross-core, `IsInFlight` same-core — the same two sources the
post-park re-check reads), and the waiter registers its slot **before** it
re-reads the holder's state for the last time. Either order of decide and
register then ends the wait on the next poll.

**AO-R5 — Same core first, statement restart first.** `[design]` S3 widens
`command_dispatcher.cpp:10633` from `IsInDoubt(cur)` to "in flight" and
the loop at `:299-334` from "in-doubt decided" to "holder decided", for
statements that wrote nothing — the existing restriction
(`command_dispatcher.hpp:524-530`). A multi-row statement that meets a held
row after writing waits only once the write walk can park at a no-span
boundary (S3b, `step_vm.cpp:1986`'s shape, `HandleUpdate` onto the
coroutine path). Until S3b that case still refuses `TxnConflict`, and the
row status says so.

**AO-R6 — Release at decide, after visibility.** `[design]` `Transaction`
gains a bounded lock list (the cap bounds it); release is the last act of
`Commit` after `:259`/`:261` and of `Abort` after `:457`, so a woken
waiter's re-check reads a decided holder through the window (AN) or
`IsInFlight` (same core). An autocommit's scope is its statement (AR2 §2).
A prepared transaction releases nothing until its decide.

**AO-R7 — The detector: core 0, the `system` group, edges in the table,
the waiter that closed the cycle is the victim.** `[design; the cadence is
[constant]]` Every park writes a `waiter → holder` edge into the table; a
`system`-group task on core 0 walks them every **100 ms** `[constant]`; on
a cycle it aborts the transaction whose edge closed it (deterministic; the
choice PostgreSQL's detecting waiter makes), by a `kLockAbort` ring message
to that transaction's core when it is not core 0; the victim's statement
fails `TxnConflict` naming "deadlock" — no new code (`status.hpp:156`).
Same-core cycles are real at `cores = 1`, so the detector runs there too.

**AO-R8 — The timeout is a fault net, a `constexpr` in M2, and
`in_doubt_ceiling_ms`'s re-scope at M3.** `[constant]` `kLockWaitFaultNetNs`
is never the normal end of a wait (R10); when it fires it **aborts the
waiter** with `TxnConflict` naming the net — R1's "a timeout aborts the
waiter, never revokes the holder" — and logs the fault at `kWarn`, because a
wait that reached it means the detector missed a cycle or a holder is
stuck. **Its value answers finding J**: while 2PC is in the tree an honest
holder can sit inside a coordinator's 10 s phase (`kTxnPhaseDeadlineNs`,
`txn_2pc_service.hpp:365`), so the net is `kTxnPhaseDeadlineNs + 1 s`
(11 s) until M3 retires 2PC, and **1 s** (D12's value) after. Not a config
key in M2, because
`in_doubt_ceiling_ms` has two 2PC users (`txn_2pc_service.hpp:369-378`) and
a second key would be a second name; when 2PC leaves (M3) the key is
re-scoped to the net, renamed, and the old spelling refused at
`expeditor.cpp:70-91`'s known-key check naming its successor. S3 removes
the in-doubt block's clock-end, which changes `server.md:100`'s contract —
AO-0 item 7.

**AO-R9 — R9 compiles out synchronization, never semantics.**
`[measurement-gated; user-visible, AO-0 item 6]` At `cores = 1` the table
exists with null latches, uncontended atomics and no transport; a wait is
still a wait; byte-identical holds because a lock writes no bytes. AM-R3's
run-time branch is inherited; S7 carries the `cores = 1` A/B. **Flag:**
AR2-R9's "every borrow primitive is a no-op at one core" read literally
would keep first-updater-wins refusals at one core, against axis 1.

**AO-R10 — The cap.** `[constant; the refusal ratified by AR2-A]`
`max_locks_per_txn` = **65,536**, counting table entries a transaction
holds (not stamps); refuses `ResourceExhausted` with a new append-only
detail (finding M), non-retryable, aborting the statement under the
session's poison rules; `status.hpp:59-69`'s comment widens.

**AO-R11 — Modes and units in M2.** `[ratified by AR2-A; design]`
`IS`/`IX`/`S`/`X` (E1); units relation, range (CC8's `[lo, hi)`), slice
`(rel_oid, [lo, hi))` (E4), tuple; no escalation. The relation `IX` on
every write and `IS` on every positioned read is the one cost (R3); S7
prices it.

**AO-R12 — The read borrow and the mover's gate.** `[ratified by AR2-A;
design]` S6 takes `IS` at the slice for a positioned statement at page
entry and moves it at each boundary (the `step_vm.cpp:1986` seam); no
mover exists, so R13's M2 consumer is DDL's relation `X` (`DROP TABLE`,
`CREATE INDEX`); the precondition `physical-optimizer.md:66` needs becomes
one compatibility check, which also closes the stale premise AR2-V flagged.

**AO-R13 — M2 logs nothing and changes no format.** `[design]` No WAL
record type, no page bit, no superblock field; a crash releases every lock
because the loser is rolled back at mount (R1). The golden log's CRC and
the four contract suites are the proof, and D14 is not invoked.

**AO-R14 — The two splits finding B's table does not carry.** `[design]`
The census (AO-3 B) is the list of what M2 retires, waits on and keeps;
this ruling adds only what a row cannot say. **The guard goes in M2, the
route in M3**: `MayWrite`'s grant arm and `RelationWriteRightsPending` are
retired at M2 because the lock is the guard, while AM-R1's owner routing
stays until R12 (M3) moves the write — AR2 §8's "retired with AM-R1" is
read here as *alongside*, not *instead of*. **D9(a)'s fence is M3's**: S3
changes *when* the FK check runs (after the writer decides, not busy), not
what protects the row after it.

---

## AO-5 — Stages

Sizes as AM-5: S ≤ ½ day, M ≤ 2 days, L more. Every stage: a
`critics-developer` review, the full suite, sync with `origin/main` on the
branch, stop.

| # | Stage | Cells (definition of done) | Size |
|---|---|---|---|
| AO-S0 | This document; `rules.md` §3's row (AO-R2's text); `index.md`'s row | the three files at the commit that carries them; the suite not executed and not claimed | the hour |
| AO-S1 | **The pure core**: `lock_table.{hpp,cpp}` — partitions (64 × cores, null `Latch*` at `cores = 1`), the four unit keys, the compatibility matrix, the relation entry's counters, queues without a scheduler, the cap | one cell per mode pair; an uncontended `X` registers nothing; the cap refuses at 65,537 with `ResourceExhausted` and the new detail, `kwp_error_test`'s golden list extended; `cores = 1` constructs no latch; two threads on two partitions never serialize; a slice keyed in key space is found after its page's bounds change | M |
| AO-S2 | **The wait and the wake on one core**: `WaitUntil` on a table slot; the lock list on `Transaction`; release after `:259`/`:457`; a two-session fixture on one reactor (none exists) | T2 parks on T1's row, T1 commits, T2's next poll proceeds and its re-check sees T1 through the window; T1 aborts, T2 proceeds on the prior version; `Release` leaves no entry; `SuspendAudit` trips on a park with a span; the golden log's CRC does not move | M |
| AO-S3 | **Same-core cutover, zero-write statements**: `:10633` widened; the in-doubt loop generalised and its clock-end removed; F3's forward busy on a same-core parent; `src/txn/manager.cpp:59-61` fixed | the first-updater-wins cells that assert `TXN_CONFLICT` now assert the wait's outcome; an autocommit `UPDATE` against a row an open transaction holds returns after its `COMMIT` with the new value; a child `INSERT` against an in-flight parent waits, passes after commit, `FkViolation` after abort; the `txn_2pc_*` in-doubt cell waits past 200 ms | M–L |
| AO-S3b | **The mid-statement wait**: `HandleUpdate`/`HandleDelete` on the coroutine path, the park at a no-span boundary | a ten-row `UPDATE` meeting a held row 7 keeps rows 1–6 and waits; `SuspendAudit` clean; the same statement inside an explicit transaction | L |
| AO-S4a | **D12, same core**: edges in the table; the detector task (core 0, `system`, 100 ms); the victim's abort; the net as a logged fault | a 2-cycle between two sessions on one core: one aborted `TxnConflict` naming deadlock within one cadence, the other proceeds; a 3-cycle; with the detector disabled in the cell, the net fires at 1 s and the log line is asserted | M |
| AO-S5 | **Cross core**: the table across reactors, the wake and the victim notification as **write-then-kick** (AR0-6-R1 — `kLockWake`/`kLockAbort` are never built, since AU-R4 forbids adding a kind and `WakerTable::Kick` landed at AU-S1), `MayWrite`'s grant arm and `RelationWriteRightsPending` retired; **a deterministic two-`CoreRuntime` rig over a `SimWakerTable`, first** (`SimWaker` until AV-R1's mark of 2026-09-07; the seam is the table) (finding L; `SimRingTransport` was the pre-AR0-6 plan). Gate: AM-S1 (latch order), AM-S2 (shared pool), AN-S2 (view) | a waiter on core 1 woken by a decide on core 0 while core 1's reactor sleeps (`sched_wakes_received` moves); with every kick delayed to the sim waker's maximum the waiter still proceeds; at the store, `MayWrite` admits a page its lease/grant arm refused at `9e5068c`, under the lock — owner routing still in force (AO-1), so the cell is the store's, not dispatch's | L |
| AO-S4b | **D12, cross core**: core 0's detector over edges from every core; a victim on a peer aborted by message | a 2-cycle across two cores resolved within one cadence | M |
| AO-S6 | **The units** as proposed: relation `X` for DDL (`IndexBuildPending` → wait), D8's slice fence (AS4 struck), `IS` at the slice (**AO-R12** — this row said "(R14)" and §AO-S6e said "R13's gate"; R13 is "M2 logs nothing and changes no format" and has no gate, corrected at AO-S6e-d), the range key. **What landed, across S6a..S6e**: the tuple and range units decide rather than record, and two refusals became waits — and **none of the three units this row names was built**, each for a reason its sub-stage states rather than a gap. The range key was never this stage's; census row 10 keeps it | `CREATE INDEX` waits for an open writer and proceeds after its commit; a writer arriving during the build waits; an assertion's bounded false rejection admits after the reserver aborts; `DROP TABLE` waits for a positioned reader on a peer; a slice fence survives a leaf division | L |
| AO-S7 | **C3** (AR2 §9 step 5) under `bench/README.md`'s five rules, and the price of R3's relation-level key | one results file per cell under `bench/v3.0.0/`, `git describe --tags` in each; E7's default and E12's price read from them, not decided | M |
| AO-S8 | **Prose**: `txn.md` §1 (AR0-M1's four texts) and §5; `heap-and-tuple.md:95`; `assertion.md` AS4/§6; `foreign-keys.md` F3 and §5's first bullet; `sched.md` §9-2; `rules.md`'s row loses its "not built" parenthetical and moves to `txn.md` §5, and the four documents that still say "the fourth row" are corrected (`ar0-architecture-revision.md:370`, `workorder-an-read-view.md:84`, `ratification-an-commit-order.md:168-169`, `workorder-am-m1-shared-pool.md:66,226`); `ring_transport.hpp:16-19`; `include/kds/txn/manager.hpp:22-28`; `keystone.hpp:23-29`; AR2-R2's "CAS on the byte"; `client-manual.md`, `server.md:100`; `CLAUDE.md`'s Transactions row | no spec says "no lock manager"; every lock in the reactor is in §9-2's list | M |

**Order**: S0 → S1 → S2 → S3 → S4a → S3b → **[the word, and AM-S1..S6 +
AN-S2]** → S5 → S4b → S6 → S7 → S8.

**S3b's place in that order stands**, and the two prerequisites AO-6's row
names are its own to build rather than gates on it. The first of them
touches AO-S4a's graph, which is why the row is worth reading before S3b
is opened: a mid-statement park makes an autocommit scope a possible
member of a cycle, and the detector cannot see one it has no identity
for. S1–S4a and S3b depend on neither M1 nor
AN-S2 technically (same core, no shared page, no instance view) and are
gated on the operator's word alone (AR2-A §1, §6); S5 onward need the
shared pool and the view. S4a lands before S3b because a mid-statement
wait without a detector would leave the net as the only exit, which R10
forbids for a stage's lifetime.

---

## AO-6 — Row status (CLA, appended as rows land)

| row | status |
|---|---|
| AO-S0 | **written 2026-09-03** on `ar2-borrow-model` at `9e5068c`; the survey is source-read at that commit; no engine code changed; the suite not executed and not claimed |
| AO-S1 | **Built 2026-09-04** on `worktree-worktree-ar2-borrow-model` from `eb1e414`, on the operator's word (*"borrow model로의 전환을 위한 작업을 최우선"*), which is the word AO-5's order gates S1–S4a on. `include/kds/txn/lock_table.hpp` + `src/txn/lock_table.cpp`: four units, four modes, compatibility and covering as two `constexpr` tables (a table and not an expression, so that adding `SIX` fails to compile rather than falling through to a plausible default), `64 x cores` partitions with a **null `Latch*` at `cores = 1`**, the striped fence counter that stands in for AO-R3's declined lock bit, the queue as data, and the cap. 17 cells in `tests/lock_table_test.cpp`; `wire::ResourceDetail::kLockCap` appended and `kwp_error_test` gains the **detail** golden list, which did not exist — §11 has required append-only details since P16 and only the categories were guarded. **Three things this stage does not deliver, stated rather than implied**: the cap's refusal carries no wire detail yet (nothing maps a `Status` to one until a session exists, AO-S3); AR2-R2's "an uncontended write touches no table entry" is a claim about the Keystone stamp and is AO-S3's, so the cell here pins the weaker fact that one borrow makes one entry; and AO-R2's acquisition order is prose with nothing enforcing it, the shape AM-S1 needed a census for. The `critics-developer` pass found and fixed one live defect — `Release` lowered the fence counter unconditionally, so a double release underflowed it to 2^64-1 and left AO-R3's probe gate permanently open, sending every writer on that relation through an all-partition scan for the instance's life — and two design faults now corrected: a queued waiter no API could withdraw, which pinned its entry forever (a transaction is one thread of control, so `LockHoldings` records the single unit it waits on and `Release` dequeues it), and the cap checked *after* the conflict test, which left a capped transaction queued behind a grant the same check would refuse on arrival. Rejected from that review: nothing. **Suite: 3308/3308 before the fixes; re-run after them below** |
| AO-S2 | **Built 2026-09-04** on `worktree-worktree-ar2-borrow-model` from `36ccbd2`. A refused `Acquire` hands back a `LockWaitSlot`; the waiter parks on it with `sched::WaitUntil` and **re-asks** on every wake (finding G's mandatory re-check is the acquire loop going round again). A decide wakes everyone queued on what it released. `Transaction` gains `borrows_` (AO-R6) and the manager releases them **after** `PublishCommit` and `active_ = false`, so a woken waiter re-checks against a holder it can already see as decided. The two-session fixture finding L says does not exist now does: `LockWaitOnOneReactor` runs two transactions as coroutines on a real `sched::Scheduler`, and one genuinely parks. **A hang found in development**: a woken waiter re-asked, got its slot back still flipped, and `WaitUntil::await_ready` was satisfied on entry, so the loop spun the reactor instead of parking. The slot is now consumed **under the partition latch**, and the proof is not "the wake takes the same latch" (true but insufficient - `Release` takes it twice) but that the waiter's *observation of the holder* and its *clear* are one critical section, so any holder it saw runs its removal after that section and its wake after that. **A departure from AO-R4, recorded in the header**: the predicate is the slot alone rather than AO-R4's disjunction, sound because the last read of the holder's state is the table's own holder list in the same critical section as the registration - and **the premise dies at S3**, where the re-check becomes `CheckWriteConflict` outside this latch, so S3 must re-argue it rather than inherit it. **Cross-core a wake is late, not lost**: a slot flip is not one of the three things that end a sleeping reactor's idle block, so delivery waits out `max_idle_block_ms`; AO-R4's `kLockWake` ring message is AO-S5's. The `critics-developer` pass found four defects, all fixed: a transaction conflicting on a second key orphaned its first queued record forever (unbounded entries, AO-R3's gate stuck nonzero, and a false edge for AO-S4a's graph); `DequeueWaiter` erased a withdrawn waiter's record without flipping its slot, a lost wakeup that becomes reachable the moment S4a's victim abort exists; the `borrows()` accessors were inserted into the middle of `prepared()`'s rationale block; and a function-local `static` served as a loop counter in a concurrency cell. It also named the stage's own coverage gaps, now closed: the manager half had **no** executing cell (nothing constructed a manager with a table, so AO-R6 never ran) and the load-bearing fix was proved only by a test that would hang rather than fail. **Two cells stated as not delivered**: "the re-check sees T1 through the window" needs an `InstanceVisibility` in the fixture, and "`SuspendAudit` trips on a park with a span" is the only thing that would enforce AO-R2's "released before any park". **Reported and not fixed**: `Commit`'s WAL-failure return keeps the borrows deliberately (the transaction is still active and may be rolled back; the caller owes an `Abort`), and `command_dispatcher.cpp`'s autocommit write scope already leaks the `Transaction` on that path - AO-S3 inherits it as a borrow leak. **Suite: 3318/3318 before the review's fixes, re-run after** |
| AO-S3 | **Built 2026-09-04** on `worktree-worktree-ar2-borrow-model` from `c23527b`. The same-core cutover: `CheckWriteConflictBlocking` records a wait for **any in-flight holder** where it recorded one only for a transaction this core had prepared, and the loop's predicate follows. F3's forward busy joins it — `exec::CheckParentPresent` gains an optional `busy_trx` out-parameter and the local forward resolution records the holder, so a child insert waits for its parent instead of refusing (AO-3 B row 3's same-core half; the probe's half is AO-S5, untouched and still refusing). The in-doubt block is renamed `WriteBlock` across 29 sites, because the name would otherwise say "in doubt" for a wait that covers every undecided writer. `manager.cpp`'s SERIALIZABLE refusal gave two reasons that are now both false — no lock manager, no reader registration — and states the narrower true one. **AO-R8's fault net replaces the 200 ms ceiling**: `txn::kLockWaitFaultNetNs`, 11 s, logged at `kWarn` under the `lock` tag, with a `static_assert` in `txn_2pc_service.hpp` keeping it above `kTxnPhaseDeadlineNs` so M3's drop to 1 s cannot silently precede 2PC's retirement. **The review found the stage unsafe as first written and the fix is the stage's substance**: widening the wait to every in-flight holder opens a two-transaction deadlock that only the net ends and that kills *both* waiters — reproduced, and exactly what AR2-R10 forbids, since AO-S4a's detector does not exist yet. `NoteBlockingWriter` now gates every recording on two conditions. **A waiter must hold nothing**: a cycle needs an edge out of a holder, so if every waiter has written nothing the wait-for graph runs waiters → holders and cannot close — this stage is deadlock-free by construction rather than by detection, and AO-S4a lifts the guard once there is a detector to catch what it admits. **And the re-run must be able to answer differently**: under `kRepeatableRead` the view is minted at `BEGIN` and never re-minted, so a holder committing after it stays invisible and the re-run refuses on the same ground — waiting there was a stall ending in the identical refusal plus a poisoned session. Three more from the same review: a statement carrying a parked FK probe or a ship no longer also carries a write block (the re-run would have discarded the probe and sent a second one, stranding the first reply and its reference intents); `RunAsync` now fails instead of returning a half-outcome, a vacuity trap that had let one cell pass by reading an unfinished statement's stale reply; and six comments that the rename had left saying the opposite of their code. **`in_doubt_ceiling_ms` is now inert** — the writer's stall was its only reader — and `manual/server/server.md`, `cross-owner-txn.md` §5's row and its knob table say so; AO-0 item 7 is where the operator's word on its fate sits, and AO-R8 gives the re-scope to M3. Cells: 12 in `Txn2pcBlockedWriterTest`, six of them new — the autocommit round trip over a commit and over a rollback, the FK child over both, and the two guards above asserted as **refusals**, which is what proves them. **Suite: 3324/3324 before the review's fixes; re-run after** |
| AO-S4a | **Built 2026-09-04** on `worktree-worktree-ar2-borrow-model` from `cf711cb`, and it lifts AO-S3's guard where it runs. The wait-for graph lives in the lock table (`NoteWaitFor`, `ClearWaitFor`, `WaitEdgeCount`); the dispatcher records a `waiter -> holder` edge before it parks and refuses outright when that edge would close a cycle. **A departure from AO-R7, argued in the header**: detection happens at registration rather than on a 100 ms cadence, because a cycle can only close at the instant some waiter adds the closing edge, and that waiter is already holding the latch and can walk the chain itself — which makes AO-R7's victim rule ("the waiter that closed it") literally true instead of true up to a cadence, and spares the victim up to 100 ms of stalling. The review checked the argument and it holds same-core: the graph is **functional** (a re-registering waiter replaces its edge), so the walk is a unique chain and the test is decisive; a replacement cannot create an unseen cycle, because the walk returns at the waiter before it would follow the stale edge. **AO-S4b still owes the cadence, and not for the reason first written** — visibility is not the problem, since the table is the instance's; what a cross-core cycle can contain is a wait that registers *no* edge (the shipped-statement park, the FK probe park), and a link nobody recorded is one no registration can close. **The review found one live defect, in both branches**: the victim's refusal set only `DispatchOutcome::response`, and `KwpSession::OnStatementComplete` prefers the carried `status` — so on the default port the deadlock report reached nobody and an operator saw the ordinary pre-wait conflict. AO-S3's fault-net refusal had the identical defect. Both now go through one `RefuseParkedWrite`, which is the duplication that hid it. Three more from the same pass: the edge left by a coroutine destroyed *at* the park is cleared by the **decide** rather than by an RAII guard — the guard was tried first and segfaulted the three-cycle cell, because the table is instance-scoped and a destructor running during teardown reaches it after its owner is gone, and the decide is the right home anyway since a stale edge only matters while the transaction it names is in flight (`Commit`/`Abort` clear it with the borrows, which is why the manager needed the table too); the victim's message said "the other proceeds" when what is aborted is the statement and the transaction is poisoned still holding its rows, so it now says to ROLLBACK; and the header's "every wait is an edge" was false in its own file — `Acquire`'s queue wait registers none, which **AO-S6 must close** or a transaction will hold rows under this graph while waiting on a queue it cannot see, with the guard already lifted. AO-S6 also owes the container: the graph stops being functional the moment a waiter waits on *all* holders of a shared unit. **Wired at `core_count == 1` and only there** (`core_runtime.cpp`), which is where per-core and instance-wide are the same object and where AO-3 C makes the cross-core path unreachable by construction — so the stage ships behaviour rather than a fixture, without touching what AO-S5 gates. Above one core the table stays null and AO-S3's narrow rule runs, which needs no detector. Cells: 4 in `LockDeadlockTest` — a 2-cycle where only the closer is refused and the survivor proceeds on its ROLLBACK, a 3-cycle caught transitively, a non-closing chain left alone, and the no-table case where AO-S3's rule still applies. A test-helper defect found on the way is worth carrying: `DispatchAsync` takes a `std::string_view` and is a coroutine, so a helper binding a temporary `std::string` had the parked statement parsing freed memory and a valid `UPDATE` read as `ERR unknown command`; every existing cell passed a string *literal*, whose static storage hid the requirement. **Suite: 3330/3330 before the review's fixes; re-run after** |
| AO-S3b | **Built 2026-09-05** on `worktree-ar2-borrow-model-2` from `410377e`, landed at `07c94c7` and corrected after review. The row below the survey stands: nothing was open, and the three owed items are closed. **The shape is stop-and-re-enter, not park-in-place.** The row callback cannot park - it runs under a page span (AO-R2) - so it records the blocker and returns `kStop`; the page loop moved out of `heap::ChainVisit`/`btree::BtreeVisit` into `WalkHeapChains`/`WalkBtreeLeaves`, which is where the statement parks with no pin and no span, `step_vm.cpp:1943-2007`'s shape. The open scope, the snapshot, the position and R6-5's trail mark park on the **`Session`** (`session.hpp`), not on the dispatcher, because two sessions on one reactor can be parked at once. **Owed items 2 and 3 fall out of re-entry**: `check_view` is re-minted and the catalog borrows re-taken because `UpdateInner` runs again from the top, only the position being carried. Item 1 is real code - `DispatchAsync`'s waiter identity now comes from the parked scope, since a mid-walk park holds rows even in autocommit. **The gate that had to be found the hard way**: parking *every* blocked statement broke five AO-S3 cells, because a statement that wrote nothing is re-run under a **fresh** snapshot and so can see a holder that committed, where a resume carries the original view by construction and would end in the refusal it began with. The park is gated on the statement having written rows, and the two conditions turn out mutually exclusive without a lock table anyway (`NoteBlockingWriter` declines for a waiter whose trail is non-empty when `locks_ == nullptr`), so AO-S3's narrow rule still runs where no detector exists. **The btree arm is not the heap arm.** A heap cursor is `(range, page, slot)`; a btree leaf splits by moving its upper half right, so a split whose midpoint fell below such a cursor would carry already-written rows into a page the walk had yet to visit and write them twice. The btree arm therefore resumes **by key, descending afresh** (`BtreeSeekLeaf`), which is immune to the split rather than unlikely to meet it - and this is the arm SUS-1 (`workorder-as-sus1-heap-suspended.md`) makes the one every new relation reaches. **The `critics-developer` pass found three correctness bugs, all fixed, one of them already pushed**: (a) the pk **point-lookup** arm renders straight from the row counter and never tested the stop, so `UPDATE … WHERE id = k` against a held row answered **`UPDATED 0`** - a success line for a write that never happened, with the autocommit commit behind it - which is the quiet-wrong class and was live in `07c94c7`; (b) the park gate read `blocking_writer_` bare, but the foreign-key resolution writes a busy *parent* into that same statement-scoped member, so a row failing for a non-waitable reason could park on a transaction holding nothing it wanted - now gated on `blocking_writer_ == trx_id`; (c) `waiter_id` was cached before the wait loop, so a statement refused with nothing written (`0`, correct then), re-run, that then parked mid-walk holding rows kept the stale `0` and **registered no edge** - defeating the very item 1 it was added for. CLA then fixed the review's R1: an unreadable key at the btree stop left `cut_pk` at 0 with the cursor active, which resumes at the *leftmost* leaf and skips nothing - every written row written twice, `UPDATED 16` for a ten-row statement - now `Corruption`. **Two cells were vacuous and are recorded because the method matters more than the cells**: the leaf-split cell asserted `DESCRIBE` differed, but `DESCRIBE` carries `ids_issued`, which moves on every insert, and the split divided far above the resume key so no written row moved - the review proved it by making the resume positional and watching the cell still pass. CLA's first cell for `RefuseParkedWrite`'s scope cleanup was vacuous the same way: both victims were in explicit transactions, where `EndWrite` only poisons, which `RefuseParkedWrite` does anyway. The replacement uses an **autocommit** victim ended by the fault net and fails without the cleanup with `UPDATED 6` - the next statement on that session resuming into the refused one's cursor and skipping rows 1-4. **Every new cell here was checked by breaking the code it claims to test.** Cells: 10 in `MidWalkWaitTest`, plus `AStatementThatHasWrittenRowsIsRefusedEvenWithADetector` rewritten as `…NowWaitsInsteadOfBeingRefused` - it pinned the line this stage moves and said so in its own comment. **Not delivered, stated rather than implied**: RepeatableRead stays excluded from the wait (`NoteBlockingWriter`'s guard), so AO-S3's deferral of "the finer answer" to this stage is **not** discharged - lifting it changes refusal behaviour for a whole isolation level and needs its own cells; the review's R2 (the cursor's `range` is a positional index, an assumption now written down rather than removed), R3 and R4 (the resumed run re-executes the reverse-FK hoist, and the probe/ship arms would end a scope holding written rows - both no-ops at `core_count == 1` and neither says so in code), R5 (a parked statement is several statements to observability), and S1/S2/S4 (`rows_done` on the wrong struct; `HandleUpdate` and `HandleDelete` now share ~45 lines, which AO-S4a's row already names as the shape that hid its defect; `walk_cursor`'s round trip through `DispatchOutcome`) are open. A session dropped mid-park leaks its scope, bounded only by `tcp_server.cpp:732`'s in-flight deferral, which `ParkedWrite`'s comment does not name. **Suite: 3341/3341.** Overhead not measured |
| AO-S5 | **Started 2026-09-07** on `AU+AV-S0S1` from `62ab2e0`, on the operator's word to begin after AU-S2, and **split in two because its gate is half open**: AM-S1 and AM-S2 are landed, AN-S2 is not (another session is building it on `an-s2-read-view-cutover`). **AO-S5(a), built**: the rig - `workorder-av-two-core-rig.md`, its three cells and the AM-S2-P promotion - which is the first deliverable this row names; the wake as write-then-kick, which is AU-S2 (the table records each waiter's core and kicks it through the instance's registry after the partition latch drops; no `kLockWake`); and **the table across reactors**: `Expeditor::Open` builds one `LockTable` for the instance, sized `64 x cores`, its registry installed at `Start` beside the transport's, core 0's manager takes it, and every peer borrows it through `CoreRuntime::Config::locks` - `visibility`'s shape - so a decide on any core releases into the one table. `expeditor_test.cpp`'s two-core assembly cell asserts the table exists, is partitioned for both cores and kicks through *this* instance's registry (AU-S1b's lesson, applied a third time). This row's cells 1 and 2 - a waiter on core 1 woken by a decide on core 0 while its reactor sleeps, with `sched_wakes_received` moving; every kick delayed to the maximum and the waiter still proceeding - are hosted at the table's level (`tests/lock_wake_rig_test.cpp`). `MayWrite`'s grant arm and `RelationWriteRightsPending` were retired by AW-S1b/AW-a before this stage. **Two findings**: (1) **no server had a lock table before this** - `CoreRuntime` built one at `core_count == 1` and a server's core 0 is the `Expeditor`, so AO-S4a's detector, "wired at one core where per-core and instance-wide are the same object", ran in every fixture and in no instance; the manager takes the table now on every count, and **the dispatcher deliberately does not** - handing core 0's dispatcher the table at `cores = 1` is one line and would make a server admit a holding waiter under AO-S4a's rule for the first time, a behaviour change in a shipped configuration that this stage holds for the operator's word rather than riding in with the wake (`expeditor.cpp` says so at the site); (2) with the instance table on every core, `ClearWaitFor` - called by every decide - took `wait_latch_`, a real mutex above one core, on the commit path of every reactor for a graph nothing above one core can populate until AO-S4b; it is gated on an atomic edge count now, AO-R3's shape. **AO-S5(b), gated on AN-S2**: the dispatcher's cross-core wait through the table - the FK probe's busy answer becoming a park on the parent's slot (AO-3 B row 3's probe half), the re-check reading the holder through the window - and cell 3's restatement (the note in `workorder-av-two-core-rig.md` §6: `MayWrite` no longer consults a lease or a grant, so "admits a page its lease/grant arm refused" names a refusal that does not exist). Also not in (a), and stated: above one core the dispatcher keeps AO-S3's narrow rule and a null table, because a cross-core cycle can pass through a wait that registers no edge (AO-S4a's row) and only AO-S4b's cadence catches it |
| AO-S4b | **Built 2026-09-07** on `AU+AV-S0S1` from `5651edb`, on the operator's word ("AO-S5(b) waits for AN-S2; start AO-S4b" - AN-S2 landed in that same merge). The section at the end of this order is the design; its claim is that **the cadence is not the mechanism, the missing edge is**: AO-S4a's registration-time walk over the instance's graph is complete across cores once every wait records an edge, and the shipped-statement park - a coordinator waiting for the participant that runs its statement - was the one that recorded none. Only the owner knows both ids, so the request carries the coordinator's transaction id (`ShippedStatementRequestPayload::coordinator_txn`; the shippable ceiling falls 968 → 960 bytes - 976 → 968 on its own branch, AN-S3's eight for the coordinator's snapshot having landed in the same merge, `crosscore.md` §9 and `client-manual.md` restated), `ShippedStatementExecutor::Execute` records `coordinator -> participant` at enrolment and `Finish` clears it, and a registration that closes a cycle - the participant's row park, in practice - refuses the closer naming deadlock with the refusal riding the reply. The dispatcher takes the instance table on every core now, `Expeditor`'s core 0 included: the behaviour change AO-S5(a) held is landed here, the detector stage being its subject. **AO-R7's cadence is not built, and this is the departure to state**: a walk on a cadence finds nothing a registration did not; the fault net stays for a wait this stage did not foresee. **Two findings on the way**: (1) `FinishShippedStatement` rendered a shipped refusal's line and left the carried `status` OK, so every shipped refusal reached a typed client through the bare-line fallback - `TxnConflict` survives that fallback, the bare-arm set (`NotFound`, `Unsupported`, ...) did not; the status is carried now; (2) **`Expeditor::Start` left a thread-local suspend audit naming a freed store** on any instance started and dropped without `RunUntilStopped` - the `Start()`-then-look fixture shape - and a later cell parking a coroutine on that thread segfaulted; invisible to the gate, which runs each cell in its own process, and reproduced by two single-core `Expeditor` cells and a deadlock cell in one process. `~Expeditor` withdraws the audit now. **Cells**: `tests/deadlock_rig_test.cpp`'s two-core cycle through shipped statements (the rig order's §6 cell 5 delivered by it), and the two `Expeditor` wiring assertions - single-core and two-core dispatchers holding *the* table. The three-cell plan in the section is corrected in place: the enrolment registration never closes a cycle by construction, so its refusal branch is stated as the fault net's cousin rather than tested. **Not delivered**: the FK probe park still registers no edge and still refuses `busy` (AO-S5(b), un-gated now); an autocommit coordinator ships nothing while holding rows (a shipped statement is whole), so no cycle passes through one. **The `critics-developer` pass found six things and CLA took all six**: (C1, the reviewer's own edit) the two typed-answer refusal arms in `Execute` replied without reaching `Finish`, so the coordinator's edge outlived the statement - cleared on both arms now; (C2) `Finish`'s one-argument `ClearWaitFor(coordinator)` could erase a *live* edge the coordinator had registered for a later local park, because a coordinator gives up on a ship at 10 s while the participant's own wait runs to the 11 s net and a failed shipped read does not poison - the table gained `ClearWaitFor(waiter, holder)`, which erases only the edge that still names the participant, and every owner-side clear uses it; (C3) `ClearWaitFor`'s unlatched gate rested on "every edge is its waiter's own", which AO-S4b's foreign registration falsified - the comment now states the real invariant (an edge added after a zero read is one whose own clear has not happened) and the narrow residue (a spurious cycle, never a missed one); (C4) two comments still called the enrolment registration the closer when it cannot close a cycle in a graph with no stale edge - corrected, and the arm no longer counts an `enrolment_refusal` for an enrolment that succeeded; (C5) `~Expeditor`'s audit withdrawal names the same-thread later-instance case; (C6) `txn.md` §5 and `foreign-keys.md` §5 still said "no deadlock detection" - both corrected, minimally, ahead of AO-S8's prose pass. Of the cuts: the deadlock sentence is one function, `DeadlockVictim`, for both sites; the cell's seed returns a `Status` and its parties are declared ahead of the rig they are borrowed by; `ShipStatement` reads the level and the id off one pointer; three comment pairs trimmed. **Declined**: folding `Running::coordinator_txn` into `Enrolled::coordinator_txn_id` (set at enrolment, verified at prepare) - a mismatch would then be caught at the first statement rather than the second prepare, which is a change to 2PC's identity check and not this stage's to make. Suites and the cells after the fixes: the landing commit |
| AO-S5(b) | **Built 2026-09-07** on `AU+AV-S0S1` from `a5389cf`, on the operator's word after AO-S4b's push, AN-S2 and AN-S3 being on `main`. The section at the end of this order is the design. The forward probe's busy answer becomes a wait on the parent's core: `FkProbeServer::Answer` checks every parent under one fresh view, and a parent being written by an in-flight transaction of that core parks the probe (`WaitForHolder`, a coroutine on the parent's reactor polling `IsInFlight(holder)` - AO-S3's same-core wait run on the holder's core on the child's behalf) until the writer decides or the probe's own 5 s deadline passes; the park records `child -> holder` in the instance's graph (the child's transaction id is on the wire for the protocol now, where the sender put 0 and a comment said it named nothing on the owner) and clears it keyed on the holder; a registration that closes a cycle refuses the child naming deadlock through the probe's status; after the decide every parent is re-checked from the top - the re-check through the window - and the answer is pass or violation. Past the deadline the answer is busy as before, the child's busy arm now saying so. Intents are granted per pass as before; `FkIntentTable::Add` is a set insert, so a re-answer adds nothing twice. **Cells** (`tests/fk_probe_rig_test.cpp`): a child on core 1 waits out an in-flight parent on core 0 and passes when it commits (`probe_waits` moves, `probe_wait_expiries` does not); the same shape over a rollback is `FkViolation`. Mutation: with the probe answering busy at once, the first cell fails at "the parent's core did not park the probe". The reverse probe still answers busy (AO-S6). This row's cell 3 (`MayWrite`) is restated as absent, in the section and in the rig order's §6 note. **The `critics-developer` pass found nine things; CLA took six and states three as open, the tree landing on the operator's word with the hook skipped**: (C3, the reviewer's own edit) the child's busy arm claimed a probe deadline on two paths that never park - the pending-delete busy (AJ-T1) and the self-referencing local descent - so the sentence names the holder's state now, not a mechanism; (C4, the reviewer's edit to `lock_table.hpp`) a child whose foreign keys name parents on two owner cores can have two blockers at once and the functional graph keeps one - a missed detection ended by the deadline and the fault net, never a false one; the container is AO-S6's; (C5, C6, C7, the reviewer's edits) `foreign-keys.md` §5's two broken sentences, the design section's "intents on the reply" against the per-attempt grant the code makes (safe because the child enrols each owner at send time, so every owner gets a decide), and `fk_check.hpp` still naming the probe as a path that cannot wait; (C8) the two counters the rig cell reads across threads are atomic now; (S1-S3) the park's paragraph written twice, two comments restating their code, and `probe_waits` counting parks rather than probes - trimmed, and the accessor's comment says parks. **Open, and this stage's next step**: (C1) a cross-owner transaction that writes its own parent - the parent `INSERT` under `BEGIN` shipped to core 0, then the child on core 1 - parks on its own participant, which cannot decide before the coordinator's `COMMIT` that is itself parked on this probe, so the refusal that was immediate costs the full 5 s and closes no cycle the detector can see (`coordinator -> participant` is cleared at `Finish`); the interim is not to park on a holder that is the requester's own participant, the real fix to answer from that participant's view; (C2) the parent stamps its park deadline at receipt and the child its waiter deadline at send, so the parent's is later by transit plus drain lag - a park that runs to its deadline can re-answer after the child gave up and sent its abort decide, granting an intent nothing releases, and that parent row answers busy to every `DELETE` for the life of the process; the fix is the child's deadline on the wire or a park margin; (C9) `DeadlockVictim`'s 276-byte sentence truncates to `kFkProbeReplyMessageBytes` = 128 on the probe path and loses its remedy - the status and its retryable bit survive. Suites on the tree before C8 and the trims: plain, armed and armed+budget 3328/3328 each; the rebuilt tree ran the fk probe cells. Overhead not measured |
| AO-S5(b) C1, C2 | **Built 2026-09-08** on `ao-s5b-c1c2` from `25c5849`, on the operator's word ("go ahead with step 3, AO-S5(b) C1 and C2"). **C1, the real fix rather than the interim**: the probe's check view is minted with the requester's own participant on the parent's core as its writer - `ShippedStatementExecutor::enrolled_transaction_id(coordinator, session)` answers which local transaction, if any, the requester's session holds there, and `MintCheckView(own)` makes that participant's pending rows the transaction's own under `foreign-keys.md` §4's `own_trx_id` rule - so a transaction that shipped its parent `INSERT` and then writes the child at home is answered pass or violation at once: no park, no 5 s, no edge. Wired through `SetShippedStatements` on the runtime and the `Expeditor` alike. **C2, a flag rather than the child's deadline on the wire or a park margin**, because neither closes the race - the park's re-answer and the child's decide can still land in either order on the parent's reactor. The flag does: both run on that one reactor, so `ReleaseIntents` marks every park up for its holder abandoned (`parked_`, a multimap of holder to the park's own flag), the park's predicate ends on it, and an abandoned park clears its edge and answers nothing - no re-answer, no grant; `probe_wait_abandoned` counts them. **The finding underneath C2, a pre-existing defect fixed here**: the decide the refusal arm sends names transaction 0 by design (`DispatchAsync`'s "the id the decide will name is this statement's own or none"), and `Txn2pcClient::OpenPhase` refused transaction 0 for every phase - so that decide, and every `ReleaseIntentsWithoutWaiting`, was refused before a message left and at most logged. An autocommit statement refused after its probes went out never released the intents it had been granted - F1's defect on the arm F1 did not reach - and C2 was not a race but a certainty, since the decide that could have ended the park never came. The zero check moved to `Prepare` (always) and to `Decide` only over a target that prepared; a decide over intent holders alone opens no decision record, a holder never asking. The C2 cell is the cell for it: its decide is the first that arm ever sent. **Cells** (`tests/fk_probe_rig_test.cpp`, 4 and 5): a transaction on core 1 ships its parent `INSERT` to core 0 and inserts the child at home - the child completes `INSERTED` inside 3 s with `probe_waits` at 0 and the `COMMIT` inside 2 s; **mutation**: a writerless view, and the cell fails at "the child waited on its own participant" after the probe's deadline. And: core 0 held for 1 s at the probe's arrival so the park is stamped a second after the child's waiter; the child gives up, is refused `TxnConflict`, and its abort decide reaches the park; the parent then commits inside the park's own deadline; `DELETE FROM p WHERE id = 7` answers `DELETED 1`; **mutation**: let the park re-answer after the decide, and the `DELETE` is refused "relied on by a foreign key check running on another core". **A rig fact the cells surfaced**: a participant's prepare parks on its device sync and is polled, and the rig's tick is off by default under a 5 s idle block, so the first cross-owner `COMMIT` ever run on the rig took 5,008 ms - one block exactly; both cells run under production's 1 ms tick (`Ticking()`). **The `critics-developer` pass found the C2 fix as first built did not close its window**: the park registered itself inside the coroutine body, and a `sched::Coro` runs its body at its first poll - a reactor phase after the drain that decided to park - so a decide drained in the same batch as the probe (a core blocked past the child's deadline drains both together) found no park to flag, and the park then re-answered and granted. The registration moved into `Answer`, at the park site, with a shared flag rather than a pointer into the coroutine frame, and every exit of the park erases its entry. Second, once the refusal arm's decide actually left, `ReleaseIntentsWithoutWaiting` opened a waiter and closed it at once, counting a phase timeout and a late reply for an acknowledgement nobody read - it rides `AbortAndForget`'s no-waiter leg now, which gained `Decide`'s per-target intent byte and its zero-id rule. Third, C1 falsified `fk_intent.hpp`'s argument that the pending-delete pre-gate loses nothing (a transaction's own delete-mark would now answer violation through the view, and the pre-gate answers busy first) - the asymmetry is stated in `foreign-keys.md` §5 and filed in `known-gaps.md`; making the pre-gate own-aware was offered and **declined here for scope**, since it needs the coordinator's identity in a registration AJ-R5 keeps identity-free. Taken as well: the accessor's unstated soundness premise (`Session::Finish()` clears `ship_id_` after a transaction with participants, so a stale context is keyed on an id no later probe carries), the 2PC protocol cell's two new lines for the zero-id decide, the forward-only sentence in §5, "silently refused" corrected to what it was (an `Error` line, and the consequence silent), two dead ternaries, the O(n²) intent-only loop folded into two helpers, the plain counters dropped from the cells' failure messages, `EXPECT_LT(commit_ms, 2000)` dropped as proving nothing C1-specific, and the `DELETE`'s gate dropped so it races the park's window. **One pre-existing cell needed a wider drive**: `ACrossOwnerInsertNamingAnInFlightParentAnswersRetryable` now sends the refusal arm's decide after its 5 s probe wait, which put it past `ForeignIndexRig`'s 5 s ceiling - the ceiling is a parameter now and that one call names its own. C9 stays open. **Suites, on this exact tree**: plain 3330/3330; the armed and armed+budget runs were in flight when the tree was pushed on the operator's word with the pre-push hook skipped, and are recorded in the row below when they return. Overhead not measured |
| AO-S5(b) C1, C2 - suites | **Landed as `18e9ba2`** on `main`, pushed on the operator's word with the pre-push hook skipped. The two runs that were in flight at the push returned on the same tree: armed (`KDS_TEST_PAGE_LATCH=1`) 3330/3330, armed + `KDS_TEST_FRAME_BUDGET=8` 3330/3330, beside the plain 3330/3330 the commit states - each serial, 266-275 s; the one disabled cell is SUS-1's resume test, and the three the armed runs skip assert an unarmed store. Overhead not measured |
| AO-S6a | **Built 2026-09-08** on `ao-s6-units` from `0e1ed85`, and the survey that opened it changed what the stage is. **Nothing in `src/` called `LockTable::Acquire`** - 47 callers in tests, none in the engine - so the table built by S1..S2 and wired by S5(a) was serving as a wait-for graph and a wake registry only, and `Release` emptied a `borrows_` that was always empty. AO-S6 is therefore the stage where the engine first takes a lock at all, not one that adds units to a working protocol, and S6a is the acquire path itself. **`LockTable::TryAcquire`**: `Acquire`'s body with `queue_on_conflict` false, granting and recording exactly as `Acquire` does when the unit is free and, on a conflict, returning `false` having touched no waiter list, no slot and not `holdings.waiting_`. Non-queueing **because a second wait beside AO-S3's would put two sets of edges into one detector**, which is the shape S3's own review caught; S6b makes the lock the wait and deletes the other. **`CommandDispatcher::BorrowRowForWrite`**: relation `IX` then tuple `X`, at the two write sites before the conflict check, on the operator's choice of 2026-09-08 between the two orders `lock_table.hpp` left open. **The cap becomes reachable for the first time** and its config key `max_locks_per_txn` is wired (0 refused, `kds.conf.sample` documents it): one entry per relation and one per row, so the effective limit is `cap - relations` rows **per transaction**, 65,535 at the default, where `max_rows_touched` is 100,000,000. **The operator's decision of 2026-09-08 is to swallow it while the borrow is advisory** - a ledger that guards nothing must not fail a statement that would otherwise succeed - counted by `borrow_cap_stops()` so the truncation is visible; **S6b propagates instead**. Two cells with the mutation. **What the review changed**: the config key was never added to `KnownConfigKeys()`, so a file naming it made the server refuse to start and the whole wiring was dead code; and the ancestor chain was ordered but not enforced - a tuple `X` was taken even where the relation `IX` was refused, which `lock_table.hpp` calls a wrong answer given quietly. Five prose claims were corrected. **What is not done and is stated rather than implied**: the borrow is taken *after* the header value it uses is captured (`apply` reads `trx_id` ~40 lines above; `DeleteInner` writes it 99 lines above), so the operator's chosen order is not yet met in fact - S6b owes a re-read after the grant or the borrow moved above `ReadTuple`. `INSERT`, bulk `VALUES`, index maintenance, every catalog and DDL write, the assertion enforcer's reservations and var-heap appends take **no** borrow. `ResourceDetail::kLockCap` still has no setter, which the swallow makes moot until S6b. B3 - a failed autocommit commit never aborts and now leaks borrows too, which `manager.cpp` predicted this stage would inherit - is not fixed. **Two structural facts**: AR2-R2's "no partition latch under a page latch" is violated in letter, the first caller running inside `BtreeVisitLeafPage`'s span, and the only thing that makes it safe is that `TryAcquire` never parks - so **S6b cannot make this site the wait site** and must keep AO-S3b's stop-and-park-outside shape; and the partition's entry vector is scanned linearly per acquire and released with a mid-vector erase, so bulk writes go from linear to quadratic, which is AO-S7's to measure. **The unit a bulk write should borrow is left as AO-0 item 14, to be answered in S6b**, on the operator's word of 2026-09-08: the swallow is what makes it safe to defer, and S6b's propagation is what ends the deferral |
| AO-S6a - suites | **Landed as `3c53db9`** on `main`, pushed on the operator's word with the pre-push hook skipped. The two runs still executing at the push returned on the same tree: armed (`KDS_TEST_PAGE_LATCH=1`) 3333/3333 and armed + `KDS_TEST_FRAME_BUDGET=8` 3333/3333, beside the plain 3333/3333 the commit states - two more than the 3331 baseline, which are the cap's two cells. Overhead not measured |
| AO-S6b | **Built 2026-09-08** on `ao-s6b-lock-is-the-wait` from `4a4ecfd`, and like S6a the survey changed what the sub-stage is. **Two findings before a line was written.** (1) *S6a's stated debt is not a debt.* S6a reported that the borrow is taken after the header it judges is captured, and owed S6b "a re-read after the grant, or the borrow moved above `ReadTuple`". Neither is owed: the borrow's key **is** the row's pk and the pk comes only from the tuple, so moving it above the read is impossible in principle; and a re-read would be provably redundant, because both write sites - the scan's and the point path's - run inside a live write `PageRef` for the tuple's page (`store.Get()`'s handle spans `BtreeVisitLeafPage`'s whole slot loop, and the probe path holds its own across the `apply` call) and the entire walk carries no `co_await`, so no peer core can write the page and no coroutine on this core can interleave. The order S6a chose is met in fact; the reason S6a gave for doubting it was the wrong one. (2) *The units S6 was written to add could not be taken soundly.* `AcquireInner`'s conflict test was `e.key == key` and nothing else, and `FenceCoversKey` had **no caller in `src/`** - so a range `X` and a tuple `X` inside it were different keys in different partitions and both were granted, which `lock_table.hpp` calls a wrong answer given quietly. The header had already said so: *"closing that is AO-S6's"*. So S6b is the fence's two sides first, and the coarse unit second. **The fence's second side.** Both sides now **publish first and verify second**: the grant is written under its own partition latch, the latch is dropped, and only then does the acquirer look at the side its partition cannot see - a fence-taker scanning for conflicting tuple descendants (`ConflictingDescendant`), a tuple-taker reading AO-R3's striped counter and, only if it is nonzero, scanning for a covering fence (`FenceCoversKey`). A verify that conflicts unwinds through the new `ReleaseOne` and answers exactly as the in-latch conflict path does. **Why publishing first is the whole argument**, written into the header: if a fence F and a write T inside it both proceeded, T's scan ran before F published and F's scan before T published, giving `publish(T) < scan(T) < publish(F) < scan(F) < publish(T)` - a cycle, so at least one side always sees the other. Neither side may verify before it publishes and neither may hold its partition latch across the scan (AO-R2), which is why the verify sits outside the granting block. `Release`'s loop body was factored into `ReleaseHeld` so the unwind runs the same steps in the same order - a second copy of that order is how a fence counter drifts. `FenceCoversKey` gained a `LockMode want`, defaulted to `kExclusive` so every pre-S6b caller keeps its meaning, and the compatibility table now decides both sides rather than a fixed pair. **AO-0 item 14, marked by the operator on 2026-09-08 as CLA proposed it**: a write whose predicate covers a key window declares that window once instead of accumulating a row. `DeclaredWriteBorrow` reads it off the predicate at dispatch - no `WHERE` -> relation `X`; a pk-range-shaped `WHERE` -> range `X` over the derived `PkSpan`; anything else -> per-row, unchanged. A declaration and never an escalation, which is what the mark's §1 item 4 forbids. Two facts stated rather than implied: **a one-key window deliberately stays per-row**, because a fence there would raise the relation's counter and send every other writer of it through the all-partition scan AO-R3 exists to avoid - and the shape that would pay it is the commonest write there is, `WHERE id = k`; and **a predicate naming no pk window is left per-row**, so `WHERE <non-pk>` still accumulates and is the one shape the cap can refuse once S6c propagates it. Soundness of ignoring the other conjuncts: the `where` vector is an AND-list, so the window derived from the pk conjuncts alone is a **superset** of the rows written. **What S6b does not do, and it is a re-lettering worth stating**: S6a wrote that "S6b makes the lock the wait and deletes the other". It does not - the fence had to exist before any unit could be declared, and that filled the sub-stage. Making the lock the wait, propagating the cap in place of S6a's swallow, and the S6 row's DDL and `IS` units are **AO-S6c**. The borrow stays advisory here: a refused `TryAcquire` is still ignored and the cap is still swallowed, so this sub-stage changes what the ledger *records*, not yet what it *decides*. **The `critics-developer` pass found two real defects, both of them things this sub-stage's own declaration made reachable, and both fixed.** (C1) `ConflictingOverlap` - then `ConflictingDescendant` - looked at tuple units only, so two range fences over one relation conflicted **only where their bounds were byte-identical**: `WHERE id > 1` beside `WHERE id BETWEEN 2 AND 3` derive different keys in different partitions and both were granted, though both write row 2. The per-row borrow they replace met at `Tuple(rel, 2)`, so the declaration was *weaker in detection* than the thing it replaces - which is a wrong answer rather than the one-directional cost the header claimed. A tuple is the unit-length interval `[pk, pk + 1)`, so one `LockKey::Overlaps` test now serves tuples and fences alike, and a relation unit's empty `[0, 0)` correctly matches nothing. (C2) The per-row skip keyed on the declaration having been *made* rather than *granted*, and `BorrowDeclaredUnit` never looked at `TryAcquire`'s answer - so a statement whose coarse unit was refused recorded **nothing at all**, not even the relation `IX`, which is strictly less than S6a's path. `BorrowChain` answers `StatusOr<bool>` and the caller demotes to per-row; S6c parks on the coarse unit instead. **A third defect was found in the fix for C2 and not by the review**: returning `SwallowBorrowCap`'s `Status::OK()` as a `StatusOr<bool>` builds exactly the value `base/status.hpp` documents as its one hazard - `ok()` true, `value()` dereferencing an empty optional - so the cap path answers `false` explicitly. Four more the review named and CLA took: (C6) a window of the whole id space declares the **relation**, one entry and no fence counter, since `WHERE id >= 0` is the same statement as no `WHERE`; (C3) `AcquireResult::slot`'s "never null on a refusal" was no longer true and the reviewer corrected the contract text, the code hole staying `Acquire`'s and therefore S6c's, since no `src/` caller reaches it; (C4) the publish-first argument proves "never both granted" and **not** "never both refused", so the header now records that S6c owes a bias against mutual refusal; (S1, S2, S3, S4) `WriteBorrow` collapsed into `std::optional<LockKey>` and the two borrow functions into one `BorrowChain`, the pk-literal test shared with `PkEqualityTarget` as `PkLiteral` rather than copied a third time into the reader `ast.hpp` names as having burned two already, a duplicated comment block collapsed, and S6a's contract comment moved back off the struct CLA's header edit had orphaned it onto. **One rejected**: (S5) `ReleaseOne`'s backwards search of `held_` for a record that is always at the back, proposed as `pop_back()`. It is exact today and would be a silent trap for the second caller S6c adds; the search is correct under any caller and costs six lines. **And one carried, not fixed** (C5): the tuple-taker's probe is an all-partition scan taken under a page latch, and the relation's fence counter is now nonzero for the whole life of any transaction that declared a range - so an ordinary `UPDATE ... WHERE id > 1` puts every concurrent writer of that relation on that scan, per row. AR2-R2's letter was already violated at S6a and still is not a cycle (no partition latch is held across a page-latch acquisition); what is new is the size. **AO-S7 measures it.** **Suites, all three on this exact tree**: plain 3341/3341, armed (`KDS_TEST_PAGE_LATCH=1`) 3341/3341, armed + `KDS_TEST_FRAME_BUDGET=8` 3341/3341 - eight more than S6a's 3333, which are this sub-stage's cells, two of them the review's. The pre-change run of the same tree was 3337 with **one failure**, and it was the intended one: S6a's `AWritePastTheBorrowCapKeepsWritingAndCountsTheTruncation` used a `WHERE`-less `UPDATE`, which now declares the relation and takes one entry, so it can no longer reach a cap of three. It was moved to a non-pk predicate, which is the shape that still accumulates, and two cells were added for the declaration itself. **Mutation, four mutants and four kills**: reverting the declaration kills both declaration cells and leaves the two cap cells green (they test the per-row path); disabling the fence-taker's verify kills exactly the two cells that test it and leaves the writer-side cell green, so the two directions are independently covered; restoring the tuple-only scan kills the overlapping-fence cell alone; and trusting a refused declaration kills the fallback cell alone. Overhead not measured |
| AO-S6c-a | **Built 2026-09-08** on `ao-s6c-lock-is-the-wait` from `ea7b566`. The survey that opened S6c found the sub-stage cannot start where S6a and S6b left it: **`BorrowChain` had four callers and all four were `UpdateInner`'s and `DeleteInner`'s, so an `INSERT` borrowed nothing.** S6c's whole subject is moving the wait's blocker from the tuple header (`NoteBlockingWriter`, AO-S3) to the lock table, and a row whose only writer was an `INSERT` carries a header `trx_id` and no table entry - so a reader of the table alone would answer that the row is free and stop waiting for an in-flight inserter. That is a wrong answer, not a missing feature, and it is why the ledger has to be complete before it can be the authority. **So S6c-a is every writer taking its borrow, and it is a prerequisite rather than a stage of its own.** `InsertOneRow` takes `Tuple(oid, row_id)` once the id is settled and **before `EncodeRow`**, which appends this row's overflow values to the var-heap and is a write of its own - a borrow taken after it would leave bytes on a page the transaction had not yet claimed the right to write. Per row and with no coarse declaration: item 14's unit is read off a *predicate* and an `INSERT` has none, so a multi-row `VALUES` takes one entry per row and is a shape the cap can reach - counted while the borrow is advisory, refused once S6c-c propagates it. The refusal is not acted on yet, exactly as the UPDATE and DELETE sites do not act on theirs. **And the survey's second finding was a defect in the test surface, not the engine.** Turning the borrow on broke the cap cells, and the instrument said why: `EntryCount()` read **5 after four committed autocommit inserts** - one relation entry and four tuples, none released. `Txn2pcParticipantTest` built its `TransactionManager` with `locks = nullptr` while calling `dispatcher_->set_locks(...)`, and **the manager is what releases a transaction's borrows at its decide** (AO-R6), so the fixture had a lock family that took tenancies and never gave them back. Production passes the table to both (`core_runtime.cpp` constructs the manager with it, then calls `set_locks`). It stayed invisible for the whole of S6a and S6b because only UPDATE and DELETE borrowed and no cell ran two writes over the same rows; the first statement to meet a leaked tenancy was the one AO-S6c-a added. The base fixture now owns `locks_` behind a virtual `LockCap()` and the three derived fixtures name only their cap, which deleted three copies of the same `SetUp`. **Two wrong turns on the way, both caught by instruments rather than by reasoning**: the hook first landed in `Txn2pcCommitTest` rather than the base of the three (the compiler refused the `override`), and then `LockDeadlockTest`'s own `locks_` member shadowed the base's, so its cells read a null table and segfaulted - the host has no `gdb`, so an ASan build was stood up and its backtrace named `LockTable::WaitEdgeCount` directly. **The `critics-developer` pass found two real defects.** (C1, the reviewer's own fix) **this sub-stage silently hollowed out one of AO-S6b's mutation-killing cells.** Under a cap of one every setup `INSERT` now counts a cap stop of its own, so `ARefusedDeclarationFallsBackToThePerRowBorrow`'s absolute `>= 2` was already satisfied by the four inserts before the statement under test ran - S6b's fourth mutant passed it. It baselines the counter after the setup and asserts the **delta** now, which is 4 against the mutant's 1. A cell that stops discriminating without failing is the worst shape a regression can take, and it was this sub-stage's doing rather than S6b's. (C2) **`SortedFillInner` is an INSERT path that routes past `InsertOneRow` entirely and borrowed nothing**, while this sub-stage's own new comment claimed a multi-row `VALUES` takes one entry per row. The reviewer corrected the false sentence and proposed the code fix; CLA took it rather than carrying it, because "every writer takes its borrow" is this sub-stage's whole claim and a path that does not is the claim being false. One `Range` over the block `AllocateRowIdRange` just carved - **an exact interval rather than a superset**, since every row of the run takes an id from it - which is item 14's shape reached from the other direction, the ids being known instead of a predicate. SUS-1 keeps the path off every relation created since the suspension, but a pre-suspension heap relation reaches it and the test binary lifts the suspension, so the hole was live rather than theoretical; it now has the cell it never had. Two more the reviewer fixed and CLA kept: (C4) the fixture's `locks_` sits above the member block so the manager is destroyed first, which is **correct and was undocumented** - and the obvious tidy-up of moving it down with the others is a use-after-free at teardown, so it now says so; and (C5) an orphaned comment moved back onto the class it is true of. Three simplifications taken: the `scope.txn != nullptr` wrapper was a second copy of `BorrowChain`'s own guard, `(void)took.value()` went with five lines explaining that nothing happens, and the comment lost the two paragraphs recoverable from this row - which is where its one false claim and its one overstated justification had both been living. **One justification was overstated and is now corrected**: the borrow was said to be placed before `EncodeRow` so the row's var-heap append is covered, but a `Tuple` borrow confers no right over the `kVarHeap` page, which is shared with every row and which no `LockUnit` names. It is an ordering of this row's own writes and the comment says that. **Two carried, both stated rather than implied.** (C3) B3's leak - a failed autocommit commit never aborts, so `Commit` keeps the borrows by contract and `EndWrite` calls `Release`, which early-returns on a still-active transaction and leaves it in `live_` until the manager is torn down. Pre-existing and owned since S6a; what is new is that it now reaches the commonest write there is. **Not fixed here because the fix is a choice about failure atomicity** - release explicitly, or make that arm abort like the one above it - and AO-S6c-b, where a leaked `Tuple X` on a real row would block every later writer of it for the life of the process, is where that choice has to be made rather than guessed. (C5-cost) every `INSERT` now runs the tuple-taker verify: an acquire load of the fence counter, and a full all-partition scan whenever any transaction on that relation holds a range fence. That is S6b's carried cost extended from `UPDATE` to the hottest write path in the engine, and **AO-S7 prices it**. And one asymmetry S6c-b must not assume away, which the review named: the UPDATE and DELETE sites follow an ignored refusal with `CheckWriteConflictBlocking`, which still yields a verdict from the header, while an INSERT has no conflict check at all - so there is nothing here for S6c-b to convert and it must add a park outright. **Three cells, and the first is the one that would have caught the half-wiring**: an autocommit statement's borrows are gone from the table at its decide, asserted over two inserts and an update; an `INSERT` under a cap of one counts the cap stop its own tuple borrow reaches, which a statement borrowing nothing cannot; and the sorted fill counts the stop its declared range reaches, which is the review's C2. **Mutation, four mutants and four kills**: disabling the INSERT borrow kills the second alone; restoring the half-wiring kills the first - and also the pre-existing cap cell, which is exactly the path the defect first surfaced on; disabling the sorted fill's borrow kills the third alone; and S6b's own "trust a refused declaration" mutant kills the repaired `ARefusedDeclarationFallsBackToThePerRowBorrow` again, which is what C1's fix had to restore. **Suites, all three on this exact tree**: plain 3344/3344, armed (`KDS_TEST_PAGE_LATCH=1`) 3344/3344, armed + `KDS_TEST_FRAME_BUDGET=8` 3344/3344 - three more than S6b's 3341, the third being the review's C2 cell. An earlier three-configuration run at 3343 predates the review's edits and is not the number this row claims. Overhead not measured. **What is left of S6c**, unchanged by this sub-stage: (b) the lock becomes the wait - the blocker from `AcquireResult::blocking_txn`, AO-S3's header path deleted, C3's cross-unit wake and C4's bias against mutual refusal; (c) the cap propagates, which **must follow (b)** because a cap that refuses while the borrow is still advisory fails a statement for the size of a ledger that guards nothing - the very thing the operator's decision of 2026-09-08 forbade; and (d) the S6 row's DDL and `IS` units. The cap's wire detail has no route yet: `kLockCap` is attached by the session through `wire::ErrorFromStatus(status, detail)` and is not carried on the `Status`, so (c) owes a field on `DispatchOutcome` - the idiom that struct already uses for `write_block` |
| AO-S6c-b | **Built 2026-09-08** on `ao-s6c-lock-is-the-wait` from `817ac7a`. **The lock becomes what a statement waits for.** `LockTable::TryAcquire` gained an optional `blocker` out-param - the shape S6b gave `FenceCoversKey`, so none of the 23 test callers moved - written only on a refusal, since `blocking_txn` is zero on a grant and emitting that would let a caller read "granted by nobody" as "refused by transaction 0". `CheckWriteConflictBlocking` now takes the two things apart: `cur` is the row's own writer from the header and decides the **MVCC verdict**, `blocker` comes from the table and is **who to wait for**. **Why that division is the whole sub-stage**: a header can only ever name the writer of the row being looked at, so once item 14 put declared *ranges* in the table, a transaction holding a range over a key it had not yet written blocked nobody - the header named a long visible writer, the MVCC check passed, and the write went through a key another transaction had already claimed. So the refused borrow had to become **a conflict in its own right** rather than a name attached to somebody else's refusal: `CheckWriteConflictBlocking` synthesises a `TxnConflict` when the table refused and MVCC did not, in `CheckWriteConflict`'s own message shape, naming the holder rather than the writer because that is who the client waits for. **One thing the survey missed and the suite caught**: `BorrowChain` grants everything vacuously when `locks_ == nullptr`, so sourcing the wait from the table alone made `blocker` zero for every row and **every wait vanished** - 7 of `Txn2pcBlockedWriterTest`'s 12 cells failed at once. The table is the authority **where there is one**; with no lock family the header is all there is and AO-S3's narrow guard is what keeps that arm deadlock-free, which is what `WithoutATableTheNarrowGuardIsWhatKeepsTheStageSafe` has always said. **And one claim CLA made to the operator and then had to withdraw**: that this sub-stage and the cap's propagation are mutually dependent and must land together, on the argument that a truncated ledger leaves a row looking free. It does not. `CheckWriteConflict` still reads the header and still refuses, so a row past the cap is refused exactly as before - what a truncated ledger costs is the **wait**, which degrades to a retryable refusal. The lock adds refusals and removes none, so the two halves separate cleanly and the cap is its own sub-stage. **The review reached this row late and the sentence that stood here was wrong twice over - corrected at `<CORR>`.** A `critics-developer` pass was launched and was still running when the operator asked for the push; CLA did not wait, pushed `bd1c24c` with the pre-push hook skipped (`--no-verify`), and wrote here that *no review stands behind this row and none of its findings are applied*. **Both halves were false by the time they were written**: the reviewer was editing the working tree, so `git add -A` swept its edits - including a real defect's fix - into `bd1c24c`, and CLA published a commit containing code it had never read or built. And the three suite numbers quoted below were measured **before** those edits, so as published they described a tree that was never committed. They have since been re-measured on `bd1c24c` itself and are stated again below as measurements of it; they came back identical, which is luck rather than justification. The lesson is the mechanical one: **a review that may still be editing makes `git add -A` unsafe**, and the two must be ordered rather than raced. **What the review found, and it refutes this row's own argument.** (C1) reading the table as the *sole* source of the blocker deletes a wait wherever the ledger is incomplete - and at this sub-stage it still is, because `SwallowBorrowCap` has not gone. A transaction past `max_locks_per_txn` keeps writing and records nothing more, so its later rows carry a header naming an in-flight writer and no entry to find; `blocker` is zero, nothing is recorded, and a statement that waited at `817ac7a` is refused outright. **So the paragraph above claiming "the lock adds refusals and removes none" is wrong**: deleting a wait removes the success the re-run would have produced, which is not the retryable degradation it was called. The fix is in `bd1c24c` and is the reviewer's: the two sources are a **union**, `blocker` falling back to `cur` where the table has nothing - which also subsumes the `locks_ == nullptr` branch CLA had added as a special case, since `BorrowChain` grants vacuously there and `blocker` is always zero. **What this sub-stage still owes, both assigned to it by earlier rows and neither built.** (C2) **a range fence does not stop an `INSERT` into its window**: `InsertOneRow` and `SortedFillInner` still discard `BorrowChain`'s answer, so a declared unit protects its holder against writers of rows that *exist* and not against rows that *appear* - which is the one thing a range fence is for, and the AO-S6c-a row had already said "there is nothing here for S6c-b to convert and it must add a park outright". (C3) **a REPEATABLE READ session can never wait on a fence**: `NoteBlockingWriter` excludes every RR waiter, and its stated reason is about the row's *writer* - a holder that aborts restores the prior writer id - which says nothing about a fence holder that wrote no row. Correct-but-strict rather than wrong, and it is the refusal AR2-A §1 measures this milestone by removing. Both are AO-S6c-c's, with (C2) first. **One cell, and it is built to isolate the thing the header could not express**: A declares a window whose second conjunct matches nothing, so it holds a range and writes **no row** - leaving row 5's header naming the committed inserter, which MVCC passes. Only the fence refuses, B waits, and B resumes with `UPDATED 1` at A's commit. **Mutation, two mutants and two kills**: disabling the synthesised conflict lets B write through the fence; sourcing the blocker from the header again leaves B with nobody to wait for. Each kills that cell alone. **Suites, all three re-measured on `bd1c24c` itself** after the reviewer's edits turned out to be in it: plain 3345/3345, armed (`KDS_TEST_PAGE_LATCH=1`) 3345/3345, armed + `KDS_TEST_FRAME_BUDGET=8` 3345/3345, and the build clean - one more than S6c-a's 3344, which is the fence-wait cell. The first run of these three numbers predated those edits and is not what this row claims. Overhead not measured. **What is left of S6c**: (c) the cap propagates - `SwallowBorrowCap` goes, and `kLockCap` gets the setter it has never had, which needs a field on `DispatchOutcome` because a `Status` carries no wire detail and the session is what builds the error; it invalidates seven cap cells whose whole observation is the swallow counter, and their replacement observation is the better one - the statement is refused and says which limit it hit. (d) the S6 row's DDL and `IS` units. And still open from S6b and S6c-a: C3's cross-unit wake and C4's bias against mutual refusal, both of which are needed only to move the park from polling `IsInFlight` to a lock slot - `WakeWaiters` matches `e.key == key` exactly, so a waiter on a tuple is not woken by the release of a fence covering it, and `LockKey::Overlaps` is the symmetric fix S6b already put in the file; and B3's leak, whose fix is a choice about failure atomicity |
| AO-S6c-c (part) | **Built 2026-09-08** on `ao-s6c-lock-is-the-wait` from `01cbb67`, and it closes the AO-S6c-b review's C2: **a range fence now stops an `INSERT` into its window.** Both insert paths took a borrow at S6c-a and discarded the answer, so a declared unit guarded its holder against writers of rows that *exist* and not against rows that *appear* - which is the one thing a range fence is for. An insert has no conflict check of its own to convert (the row does not exist, so there is no header to judge), so this is a park added outright rather than a source swapped: `BorrowOrWait` records the holder through `NoteBlockingWriter` and returns the refusal, and `DispatchAsync`'s existing loop turns it into a wait and a re-run. **Two defects the cell found in CLA's own first draft, before any review saw it.** (1) *A named key's re-run was not idempotent*: a park re-runs the whole statement, and the first attempt's `AdmitExplicitRowId` had already advanced the relation's high-water mark past the key the statement names - so on a heap relation, whose ids must ascend, the re-run was refused `OutOfRange` for the very key it had waited for. (2) *A cap refusal was read as a conflict*: `SwallowBorrowCap` still turns `ResourceExhausted` into `granted == false`, so that bool has two causes, and without `blocker != 0` an insert past the cap was refused as though transaction 0 held its row - three `LockCapOfOneTest` cells caught it. **And the fix for (1) was itself a regression, which the review caught.** Moving the borrow ahead of `AdmitExplicitRowId` also moved it ahead of the **only validation a caller-named key ever gets** - `TableAccess` carries no `next_id` by design, so nothing upstream can refuse an illegal key. An insert that could never succeed then waited out a fence, burned ledger entries, and inside an explicit transaction could be aborted as a deadlock victim for a statement with no future - AR2-R10's shape exactly, reintroduced by an ordering chosen for a different and correct reason. There is only one point where both orderings hold at once, so `AdmitExplicitRowId` gained a **`before_mark` hook**: it runs after every refusal is decided and before any of the three writes, and its non-OK return aborts the admit with nothing changed. The rule stays in the catalog rather than being copied into the dispatcher, and a held row and an illegal key reach the client by the same path. **Three more the review named and CLA took**: the fill's "the wait costs nothing already done" was false - `AllocateRowIdRange` has burned the block by then, and the sentence now says so as the single-id path already did; a range refusal told the client that `lo` was held when the conflicting descendant may be any key in the window, and it now renders `rows id=[lo, hi)`; and the two park blocks became one `BorrowOrWait`, which also recovers the carried `Status` the lambda form had dropped, with the dead `rows != 0` guard removed. **One carried** (the review's C2): a statement resumed from an FK probe can leave an explicit transaction told `ERR` and still committable, because `DispatchAsync`'s write-block arm runs before the probe arm and the probe's resume can set the blocker for the first time. Pre-existing since AO-S3 for `UPDATE`/`DELETE`; what this sub-stage did was make it reachable from `INSERT`. The structural fix is one wait arm covering a probe-resumed outcome, and it is its own stage. **Four cells and four kills.** A fence-blocked insert waits and completes at the holder's decide; an illegal key is refused **without** waiting (reinstating the borrow-before-admit ordering fails exactly this one); the sorted fill waits on a fence over the block it carves; and S6c-a's three cap cells still pin the cap arm. **Suites, all three on this exact tree**: plain 3348/3348, armed (`KDS_TEST_PAGE_LATCH=1`) 3348/3348, armed + `KDS_TEST_FRAME_BUDGET=8` 3348/3348 - three more than S6c-b's 3345. Overhead not measured. **What is left of S6c-c**: the cap propagates (`SwallowBorrowCap` goes and `kLockCap` gets the setter it has never had, which needs a field on `DispatchOutcome` because a `Status` carries no wire detail); and a REPEATABLE READ session still cannot wait on a fence, because `NoteBlockingWriter` excludes every RR waiter for a reason that is about the row's *writer* and says nothing about a holder that wrote no row |
| AO-S6c-c (cap) | **Built 2026-09-08** on `ao-s6c-lock-is-the-wait` from `79d2fd7`: **the borrow cap refuses instead of truncating, and `kLockCap` gets the setter it has never had.** The operator's decision of 2026-09-08 to swallow it carried a condition - *while the borrow is advisory* - and that condition lapsed: S6c-b made a refused borrow refuse the write and S6c-c made a fence stop an insert, so a truncated ledger is rows with no fence over them and no wait behind them. AO-0 item 1's mark said refusal was the end state; this is it. A `Status` carries no wire detail, so `DispatchOutcome` carries one. **The demotion goes with it.** Falling back to per-row when the declared unit was refused restored a ledger that would otherwise have recorded nothing; with the lock as the wait it is strictly worse - a demoted statement writes the rows that do not conflict and then parks on one that does, which AO-S3b's re-runnability rule turns into a refusal, where waiting on the coarse unit parks having written nothing and re-runs cleanly. **And that surfaced a user-visible semantic change against a ratified spec sentence, put to the operator and marked "accept and fix the spec".** A coarse-declaring autocommit write now waits and then **succeeds** under a fresh view, where `txn.md` §5's first-updater-wins refused it - AR2-A §1's refusal-into-wait, delivered. §5 now records it with its three bounds (an explicit transaction keeps its view; a predicate naming no pk window still takes the per-row path and can park mid-walk; the wait is the ordinary one the detector governs), the three `TXN_CONFLICT` message shapes, and the cap as the family's one non-retryable member. **The review made CLA add the half the question had not named**: a `WHERE`-less write declares the *relation* in `X`, which is incompatible with every other writer's `IX` - a table-level serialization point on the commonest bulk shape, and one that can now join a deadlock cycle. That is in §5 too. **The read-only review found two defects that would have made this unlandable, both this sub-stage's own.** (B1) `InsertOneRow` answers a rendered string and leaves `status` OK, so the cap's refusal reached a KWP client as `InvalidArgument` - `kErrorSpellings` cannot recover `ResourceExhausted` from a line - **carrying `kLockCap` anyway**. Details are per category, so the client was handed a code that means nothing in the invalid-argument namespace: worse than no detail, on the path likeliest to reach the cap. The dispatcher carries the **`Status`** now, installed only over an OK one. (B2) `ExecuteInsert` is a public seam - `KwpLoadServer` drives it on the same dispatcher a session uses - that never reaches the copy-out, so the setter outran the clearer and a load chunk's cap refusal could label the next unrelated statement. Cleared at both ends now, as `blocking_writer_` already was and for the same reason. **Two coverage findings, both taken.** (B3) CLA's own `v >= 0` rewrite had emptied `ADeadlockBetweenTwoMidWalkParks...` of its subject: both walkers started at row 1, so the second met the first's rows immediately and wrote nothing, leaving `RowsWith(4) == 0` vacuous. The anchors' values carve the two walks apart now, and a second assertion proves B genuinely wrote before it was refused. (B4) the cap cells observed a rendered string, which is exactly why B1 was invisible to a green suite; they observe `status.code()` and `resource_detail`, and a new cell pins a declared unit the cap refuses - the behaviour that replaced the deleted demotion cell. **Six documents were saying something the engine had stopped doing**, `kds.conf.sample` among them - it described the swallow as current to an operator - along with "no lock to wait on" in two manuals and two specs, and a `SwallowBorrowCap` doc block orphaned above the member that replaced it. **Simplifications taken**: one ten-line comment had been pasted six times and became one fact on the fixture it is about; the declared-borrow argument was written twice and the DELETE site already pointed at the UPDATE site. **Suites, all three on this exact tree**: plain 3349/3349, armed (`KDS_TEST_PAGE_LATCH=1`) 3349/3349, armed + `KDS_TEST_FRAME_BUDGET=8` 3349/3349. Overhead not measured. **What is left of AO-S6**: a REPEATABLE READ session still cannot wait on a fence - `NoteBlockingWriter` excludes every RR waiter for a reason that is about the row's *writer* and says nothing about a holder that wrote no row; the FK-probe resume can leave an explicit transaction told `ERR` and still committable (pre-existing since AO-S3, reachable from `INSERT` since S6c-c); B3's leak; C3's cross-unit wake and C4's mutual-refusal bias, both needed only to move the park from polling to a slot; and the S6 row's DDL and `IS` units |
| AO-S6d | **Built 2026-09-09** on `ao-s6d-carried-defects` from `7e26a65`: the three defects S6c-b and S6c-c made reachable, closed before a fourth unit opens on top of them. **(1) B3, AO-0 item 15**: `EndWrite`'s autocommit arm returned on a failed `enforcer_.CommitTxn` or a failed `txn_->Commit` without unwinding anything, and since `Commit` fails only *before* `PublishCommit` the transaction stayed active - `Release` refuses to free an active one, so it sat in `live_` for the life of the process holding the floor, the in-flight count and, since S6c-a, **its borrows**, which turned every later writer of one of its rows into a fault-net defect report. `AbortOwnedScope` is the arm `CommitLocal` has taken since the DT9 review, applied to all three of `EndWrite`'s owned exits; the client's answer stays the commit's own failure. A commit record that reached the platter under a failed sync is followed by the compensations and by `TXN_ABORT`, and `analysis.cpp`'s `note_txn` lets `kAborted` overwrite `kWinner`, so the mount reproduces the outcome the client was told. **(2) AO-0 item 16**: the write-block wait ran before the foreign-key probe arm and never after it, so a statement resumed from a probe was refused where the same statement dispatched directly would have waited - and the premise the sub-stage was drafted on turned out to be **wrong in CLA's favour**: `may_park_` was false around the resume, so no blocker was recorded there at all and the `[quiet-wrong]` half was unreachable rather than live. It becomes reachable the moment the resume may park, so the two halves land together: `may_park_` around the resume, and the wait - extracted verbatim into `AwaitWriteBlock` - called over the resumed outcome. Verified by mutation: keeping `may_park_` and dropping the wait produces `ERR TXN_CONFLICT` for the `INSERT` and `COMMIT` for the `COMMIT` on the same transaction. The fault net is now taken once per **statement** rather than once per entry, and verdicts held from earlier rounds are dropped after a wait, because the wait's re-run is a whole statement and a held verdict is as of the view the wait exists to leave behind. **(3) AO-0 item 17**: `NoteBlockingWriter` takes a `RepeatableReadWait`, and the exclusion is asked per wait rather than per level - *can the re-run answer differently once this holder decides?* The row's own writer is `kFutile` and stays excluded; a holder of a coarser unit that wrote no version of the key is `kCapable`; and so is **every `INSERT`**, whatever refused it, because an insert's verdict is not a function of the waiter's view at all - a named key's uniqueness is a physical descent (`btree.cpp:648`, `heap_chain.cpp:117`) and an issued key comes from the relation's sequence. A site that cannot tell keeps the exclusion: a refused *declared* unit knows who refused it and not what they hold. **CLA's first build got this wrong** and a cell caught it before the review did - the role was derived from the unit **asked for** rather than from what refused it, so a repeatable-read `INSERT` stopped by a range fence was labelled "the row's writer" and refused at once. The unit asked for settles nothing in either direction. **Ten cells**, six in `txn_2pc_protocol_test.cpp` (a new `FailedCommitTest` under `kStrict` with `FailNextSync` - the fixture for a failing WAL commit that did not exist before this sub-stage - three repeatable-read cells and the forward check's) and four on the rig in `fk_probe_rig_test.cpp`; every one of them mutation-checked, the mutation named at the cell. **Overhead not measured** (AO-S7's, per every S6 row). **The second `critics-developer` pass found two more, both of them this change's own.** *B1, and it is the one that produced a wrong answer*: `may_park_` on the resume made a **mid-walk** park reachable there for the first time, and a mid-walk park cannot survive a probe round - the wait's re-run is a whole statement that re-resolves every parent, so it raises a fresh probe before it reaches the walk, `AbandonWriteForShipping` keeps the rows inside an explicit transaction, and the cursor is gone. Measured on the rig: the victim answers `UPDATED 2` and then `COMMIT` for a transaction that wrote three rows. Closed by `resumed_from_fk_probe_`, which withholds the mid-walk park on a resume and falls back to the refusal a resume got before AO-S6d; the whole-statement wait - item 16's actual deliverable - is untouched. *B2*: `ResolveForeignKeyParents` was labelled `kFutile` on the ground that a repeatable-read check view is minted at `BEGIN`, and `CheckView` says the opposite in its own first sentence - a constraint reads latest state, at every level. It is `kCapable`, which also makes the same-core half agree with the cross-core probe, which parks without asking the level at all. **Cell 3 of §(b) as drafted is not what was built**: a resume carrying a second probe *and* a blocker needs two overlapping exclusive fences on one relation, which the table cannot grant, so what pins the copy-out guard instead is that every rig cell's wait re-runs the statement, raises a fresh probe, and answers `INSERTED` - which it could only do if that second round was collected rather than discarded. |
| AO-S6e | **Opened 2026-09-09** on `ao-s6e-units` at `68fae89`: the stage document only - the survey of the four units, the split into S6e-a..d, the AO-S6 row's five cells mapped onto them, and AO-0 items 18-21. **No code**, and the suite not executed and not claimed. Every sub-stage is gated on one of the four items, which is why the document opens the stage rather than a first sub-stage doing it. **Revised the same day** after a `critics-developer` pass over the draft: the survey's load-bearing finding held, two of its claims about what S6a..S6c already wired were false and re-size (a) and (c), item 19's `[quiet-wrong]` justification was refuted three ways and its grounds are withdrawn, and item 20 loses its motivation with them - both go back to the operator. AO-S6e-a is **blocked** on a finding of CLA's own that the ratified item 18 does not reach: the owner's window close, its catalog-cache drop and its next admitted write are one ordered event on the owner's reactor (`core_runtime.cpp:685-687`), and a relation `X` released on core 0 at the decide does not reproduce it. The survey's finding that held: **no transaction spans a cross-core `CREATE INDEX`** - `BeginForeignIndexBuild` refuses inside an explicit transaction and runs outside `InDdlStatement`, phase 2 opens a scope of its own - so census row 4's "`CREATE INDEX` holds relation `X`" has nothing to hold it, and item 18 is what to do about that |
| AO-S6e-a | **Built 2026-09-09** on `ao-s6e-units` from `68fae89`: census row 4's **outcome** with none of its mechanism. A write on the owner that meets an open index-build window used to be refused `TxnConflict`; on a served connection it now parks on `!Covers(oid)` and runs the statement whole when the window closes, and the refusal survives only on the synchronous path that has no reactor to park on - `write_block`'s division exactly. **The relation `X` census row 4 names is not built**, and the reason is the stage document's §"AO-S6e-a is blocked": `OnDone` closes the window and *then* drops the catalog cache, in one handler, before the next task is polled, so the park cannot observe the close without the drop - while a lock released on core 0 at the DDL's decide is seen on the owner before either ring message is drained and would admit the unindexed write the window exists to prevent. The sub-stage came out **S, not L**: one member, one outcome field, one arm, one cell. `ddl-transactional.md` §5e and `crosscore.md` carry the behaviour. **The park is bounded by `kIndexWindowWaitNs`**, half `kShippedStatementDeadlineNs` and asserted against it, taken once for the statement and falling back to the gate's own refusal - the review's C1, C2 and C4 in one constant - and the two waits run in **one loop** rather than two arms (C3), with the foreign-key probe arm calling it beside item 16's (C5). One cell, mutation-checked (drop the record and the write is refused `PW1c-6b` while the window is open), and it now also asserts the row is **in the index** afterwards, which is the only reason the window exists; its control is the pre-existing `ACreateIndexOnAPeerRelationIsBuiltByTheOwnerAndPublishedByCore0`, whose synchronous refusal is unchanged. **One gap stated rather than closed**: the wait is invisible to `SHOW META` - an operator who saw an error line now sees a stall - and a counter is client-visible surface. **Overhead not measured** |
| AO-S6e-c | **Built 2026-09-09** on `ao-s6e-units` from `4efe0e7`, census row 11: a refused admission now **waits** for the transaction whose reservation refused it. `AssertionEnforcer::ReserverOn` names one from `pending_` - arrivals only, a departure having lowered the aggregate and so refused nobody, and never the writer's own - and the two admitting entry points hand it back on a refusal; the dispatcher records it through `NoteBlockingWriter`, so the wait rides the family's channel and gets the wait-for graph with it. **The graph is why that matters rather than being tidy**: two transactions can each hold a reservation the other's admission needs, and AO-S4a refuses the waiter that would close it. `kCapable` unqualified - an admission reads the live aggregate and never the waiter's view. **The `S`/`X` slice fence census row 11 names is not built**, and the reason is the one item 19 was withdrawn for: it has no contender. The check and the reserve run inline in one statement with nothing between them on a cooperative core - `InsertOneRow`'s own comment says so - the enforcer is a dispatcher member and so per-core, and a relation's writes run only on its owner. **A pk of 0 is the sentinel** for "the contended thing is not a row": `kFirstRowId` is 1, the `INSERT` path has no id at admission time by design, and the two wait refusals name the assertion's group instead of row zero. `assertion.md` §6.1 and §6.2 rewritten - three of the four properties, each keeping what it said before beside what it says now. **The `critics-developer` pass found two defects this change made, and they are closed here.** *B1*: `AdmitAndReserveUpdate` is per-assertion and not atomic across them, so a refusal by the second leaves the first already applied to its cabin and its chain - and `EndWrite`'s re-runnability test reads the transaction's **trail**, which a reservation never enters, so the statement read as re-runnable and the re-run counted the first assertion twice, durably (both entries are `kAssertReserve`, `header == Σ(entries)` still holds, a rebuild reproduces it). A call that has reserved now hands back no reserver and gives the violation it always gave. *B2*: `ReserverOn` skipped departures and took any arrival, but an `UPDATE` always writes the pair - so a transaction that *lowered* the group by 49 was found by its +1 and waited on, futilely in both arms and with a live edge that could make the innocent holder a deadlock victim; it nets the candidate's contributions now and names one only when the net is positive. Three more: the `UPDATE` arm named a row its holder may never have touched (`pk = 0` there too), the Debug line still printed `row id=0`, and the fault net told an operator to look for a stuck holder on what is ordinary group contention (AO-0 item 22 records what that leaves open). **Five cells**, mutation-checked - the two the review asked for are the `UPDATE`-arm cell, which is what would have caught B1, and the two-transaction reservation cycle, which pins the claim the sub-stage rests on; the control is `AssertionEnforceTest`'s own synchronous cell, which still gets the violation at once and is unchanged. Suite **3365/3365 green in 166.22 s**. **Overhead not measured** |
| AO-S6e-d | **Built 2026-09-09** on `ao-s6e-units` from `9b1dad3`, and it is prose only. Census row 4's two citations were stale and its fate half-wrong (the wait landed, the relation lock did not); row 11's `assertion_build.cpp:203` was inside a comment, its fate put the conversion under a fence that turned out to have no contender, and its *other* refusal - `CREATE ASSERTION`'s own - is untouched and was not said to be. The AO-S6 row named four units and **none of the three that were this stage's was built**; the fourth was never its own. The `IS` ruling was cited three ways - "(R14)", "R13's gate", and R12's own "R13's M2 consumer" - and R13 has no gate; all three now say **AO-R12**. AO-S6e-c gained the section the sizing table was already pointing at. **The close**: the stage delivered two waits and no new unit, and its three reasons are one reason - every unit it was asked to add had no contender, or one the unit could not have served. That question goes to AO-S7 beside the prices. No code; the suite not executed for this row and not claimed |
| AO-S6e-b, AO-S7..S8 | not started; (b) is deferred to AO-S7 with items 19 and 20 |

---

## AO-0 — Items for the operator

The ruling table's constants and quiet-wrong entries, per AR0's standing
rule. Every item names the ruling it moves.

| # | item | class | CLA proposal / state |
|---|---|---|---|
| 1 | E2: the cap's value and its refusal's detail code | constant; user-visible | 65,536; `ResourceExhausted` with a new append-only detail (AO-R10). **Marked 2026-09-08** (`raft-marks-2026-09-08.md` §1): 65,536 as `kds.max_locks_per_txn`, `[provisional]` until AO-S7 names it; `ResourceExhausted`, non-retryable, never escalated; per local `Transaction` until AT asks again |
| 2 | The partition count | constant | 64 × cores, re-measured in S7 (AO-R2) |
| 3 | The fault net and the detector cadence | constant | the net `kTxnPhaseDeadlineNs + 1 s` while 2PC is in the tree and 1 s after (finding J), aborting the waiter; the cadence 100 ms; both `constexpr` in M2, the knob merge at M3 (AO-R7, AO-R8) |
| 4 | The Keystone byte | persisted format | **decided 2026-09-03: no persisted bit** (AO-R3); the masked-image arm declined |
| 5 | The subsystem's name | spec | `lock` (AO-R1); `borrow` is the alternative. **Marked 2026-09-08** (`raft-marks-2026-09-08.md` §2): `lock` for the transaction-scoped family and its table, `borrow` for AR2's tenancy over either family, `latch` for the critical-section family; no rename |
| 6 | Waits at `cores = 1` | user-visible | AO-R9's reading of R9 converts a refusal into a wait at one core. **Marked 2026-09-08** (`raft-marks-2026-09-08.md` §3): confirmed as shipped behaviour since AO-S3/S3b/S4a/S5(a); AO-S7 measures the zero-overhead-at-one-core rule, AO-S8 verifies the manual says a conflicting write waits |
| 7 | The in-doubt block's clock-end goes | user-visible | **Done at AO-S3, and it went further than this row expected**: `in_doubt_ceiling_ms` has no reader left at all, so it is inert rather than re-scoped. `server.md:100` and `cross-owner-txn.md`'s two rows now say so. The open question is its fate — refuse it at startup naming its successor, or keep it inert until M3 re-scopes it to the fault net (AO-R8's plan). CLA kept it inert so a configuration carrying it still mounts |
| 8 | D1's RU is not in this order | spec | confirm, or name the order that carries it (AO-2) |
| 9 | The FK split: F3's wait half in M2 (S3), D9(a)'s fence in M3 | spec | confirm (AO-R14) |
| 10 | The `rules.md` §3 row's "declared in" names this work order, not a spec, until AO-S8 moves it — a new class of row against `rules.md:24`'s "the declaration lives in the owning spec" | spec | accept the interim, with the move written into the row |
| 11 | The partition latch: `base/latch.hpp`'s `std::mutex` (AO-R2's S1 default) against D2(a)'s own words "spinlocks (atomics)" (`ar0-architecture-revision.md:87`), which AR0-M2 left undecided | design; measurement-gated | mutex first, a spin primitive only if S7's numbers ask for it. **Marked 2026-09-08** (`raft-marks-2026-09-08.md` §4): mutex first, amending AR0 D2(a); the switch condition is written into AO-S7's order before it runs - the partition latch's share of a contended update's wall time against the same cell's cross-core wake cost |
| 12 | The deadlock victim's `TxnConflict` message | user-visible (wire-contract text, `src/txn/manager.cpp:153`'s own rule) | one new message naming "deadlock", the code and its retryable bit unchanged (AO-R7) |
| 13 | **AO-S3b's prerequisites** — withdrawn as an operator item, kept as a row so the withdrawal is on the record. It was raised as a read-view decision blocking the stage; the review refuted the premise (AO-6's S3b row), and what remains is buildable work with nothing to decide: an autocommit waiter's identity in the wait-for graph, a re-minted `check_view` on resume, and the coroutine hoist. **The one thing that would come back here** is a proposal to make the *commit* arm succeed rather than refuse — `txn.md` §5 decides that today, deliberately and in favour of refusing, so reopening it is the operator's and nobody else's | spec (settled; listed as withdrawn) | none needed. CLA's earlier proposal of a three-arm choice rested on a false premise and is withdrawn |
| 15 | **B3's arm: abort, or release the borrows and keep the leak.** failure atomicity; user-visible in the second writer's wait | **Ruled 2026-09-09 as CLA proposed: abort**, the arm above it verbatim, and the statement reports the commit's failure. Built in AO-S6d, and widened by one arm the draft did not name: `enforcer_.CommitTxn`'s failure leaked the same way and now unwinds too. |
| 16 | **The probe-resumed outcome is offered the write-block wait.** pre-existing since AO-S3; `[quiet-wrong]` inside an explicit transaction | **Ruled 2026-09-09 as CLA proposed**, and built in AO-S6d - but not as one loop over both arms. The wait became a function called from both, which is the same fix with the probe arm's own rounds loop doing the looping. The draft's claim that the resume could set a blocker "for the first time" was **wrong at `7e26a65`**: `may_park_` was false there, so the `[quiet-wrong]` was one line away rather than live. Both halves landed together and the mutation that separates them was run. |
| 17 | **REPEATABLE READ waits on a holder that is not the row's writer, and may then be refused first-updater-wins if that holder wrote the row.** user-visible; `txn.md` §5 gains a sentence | **Ruled 2026-09-09 by the operator: adopted** (*"#17 RR fence first-updater-wins 채택"*). Built as the question *can the re-run answer differently*, asked per wait: `blocker != cur` at the row level, every `INSERT` (whose verdict does not go through the view at all), and nothing else - a refused *declared* unit cannot tell a fence from a writer and keeps the exclusion. `txn.md` §5 records all four arms and the wait-then-refuse ending. |
| 22 | **Whether a wait on a bound assertion's group carries a bound of its own**, shorter than the lock-wait fault net | design; user-visible in what a contended group answers | **Raised 2026-09-09 at AO-S6e-c, not decided.** The net was written for a *row*, where reaching it is a defect report (AO-R8); a group is held for a transaction's length and every writer touching one account serialises on it, so the net is reached by ordinary contention. AO-S6e-c made the refusal say so rather than send an operator after a stuck holder, and left the bound alone - a second bound interacts with AO-R8's one-net-per-statement rule, which is AO-S7's to price |
| 18 | **What holds the relation `X` across a cross-core `CREATE INDEX`**, given that no transaction spans the build | design; user-visible in two deadlines, not one — `kIndexBuildReplyDeadlineNs` 60 s bounds the asker and the 180 s window ceiling bounds a peer writer, and a lock-wait fault net is 11 s | **Ratified 2026-09-09 as proposed: one transaction across both phases.** Its premise was confirmed by the AO-S6e review. Two things the proposal understated, both recorded at AO-S6e §survey: `InDdlStatement` is a synchronous template and splitting it around the park is most of the sub-stage, and a peer's `CREATE INDEX` ships to core 0, so the transaction is open across **two** round trips. **And AO-S6e-a is blocked on a finding this ruling does not reach** — the owner's window close, its catalog-cache drop and its next admitted write are one ordered event, which a lock released on core 0 does not reproduce |
| 19 | **The `IS`'s granularity, and so its cost on every read** | **was** `[quiet-wrong]`; **now** a cost question with no correctness argument behind it | **Ratified 2026-09-09 as proposed (per page, per AO-R12) — and the grounds are withdrawn the same day.** CLA argued the cheap reading was wrong across cores; AO-R12 itself says the mover does not exist, `drop-table.md` DT1 leaves an unparked reader reading correct rows, and `step_vm.cpp:1996-2009` already turns a dropped relation into a clean error. CLA now proposes **deferring 19 to AO-S7**, where the number is, rather than paying a per-page acquire on the hottest path for a hazard with no agent. **Back with the operator** |
| 20 | **What the `DROP TABLE` wait promises** | spec | **Ratified 2026-09-09 as proposed**, and it loses its motivation with item 19's: given DT1 and the post-park re-bind, a positioned reader already gets correct rows and a clean error, so CLA cannot state what the wait buys. **Back with the operator**, with the honest position that (b)'s cell may be unmotivated |
| 21 | **The assertion's bounded false rejection becomes a wait** | user-visible; spec rewrite | **Ratified 2026-09-09 as proposed**, and narrower than CLA put it: AS4's striking is already AR0's (`ar0-architecture-revision.md:416-419` — struck by whichever work order lands D8), and the rewrite is **three** of `assertion.md` §6.2's four properties plus §6.1's core-locality, not two. The ruling stands; the scope is corrected |
| 14 | **The unit a bulk write borrows. Marked 2026-09-08** as CLA proposed it and built in AO-S6b: a `WHERE`-less write declares the relation, a range-shaped predicate declares its window, everything else stays per-row. Two things the mark's own reasoning did not reach and the build had to settle - a one-key window stays **per-row**, because a fence there raises the relation's counter and puts every other writer of it on the all-partition probe; and two declared windows must meet **each other**, or the declaration is weaker in detection than the per-row borrow it replaces (the AO-S6b row's C1) | design; user-visible; **settled** | AR2 §3's table gives `UPDATE`/`DELETE` the **tuple** with `IX` on the relation, so a write borrows one entry per row and the cap binds at `max_locks_per_txn - relations` rows - 65,535 at the default, against `max_rows_touched`'s 100,000,000. **Escalating at the cap is already forbidden**: the mark of 2026-09-08 §1 item 4 refuses widening a transaction's tuple borrows into a relation `X`, because that turns one transaction's refusal into a wait other transactions pay for without a record of why, `[quiet-wrong]` on their side. **Choosing a coarse unit up front is a different thing and is not forbidden** - §3 already gives the relation to DDL and range split, the changed key interval to a mover, and the whole child relation to an FK reverse check with no covering structure, which AR2 calls "a refusal-class fact rather than a performance one". So the question is whether a `WHERE`-less `DELETE FROM t`, or a write whose predicate covers a range, declares its unit as the relation or the range rather than accumulating tuples. **Why it is S6b's and not S6a's**: S6a swallows the cap because an advisory ledger must not fail a statement, so nothing forces the answer today; **S6b makes the lock the wait, where a truncated ledger is an incorrect one and the cap must propagate** - and a bulk write then either declares a coarse unit or is refused. CLA proposes: extend §3 with a row for a predicate-covering write, taking the **relation** `X` for a `WHERE`-less delete and the **range** `X` where the predicate is range-shaped, decided at compile from the predicate rather than at run time from a count - which keeps it a declaration and not an escalation. It is user-visible either way: a coarse borrow blocks concurrent writers the fine one admitted, and the cap refuses statements that complete today |

---

## AO-7 — Where AR2's text and the tree disagree

Listed so the drift is amended — items 2, 6 and 7's `sched.md` half on
2026-09-04, the rest by S8 — rather than inherited by the next reader.

1. AR2-R2's "a writer takes the tuple by CAS on the byte under the page
   latch": no CAS exists, no page latch exists at `9e5068c`, the stamp
   already is the lock, and the byte is on disk — AO-R3.
2. AR2 §2's family table reads "D12: wait-for graph, timeout aborts the
   waiter" as if the timeout were a mechanism; R10 as amended by AR2-A says
   it never is — a drift inside AR2 itself. **Amended 2026-09-04**: the
   cell now reads as R10 does.
3. AR2-R9's "no-op at one core" against axis 1 at one core — AO-R9.
4. AR0 D16 puts D1 in M2 (`:133`); §8 step 5 (`:153`, D2/D13/D12/D8) and
   AR2 §9 do not — AO-2.
5. AR2 §8's "`MayWrite`'s grant arm retired with AM-R1 at M2" beside R12
   being M3's: the guard goes in M2, the routing in M3 — AO-R14.
6. AR2 §9 item 1 files C2's refused `INSERT`s under E13; every logged line
   is `CrossCoreWriteRefused` — a session bound to core 0 whose target range
   had migrated to a peer
   (`bench/v3.0.0/results-ar2-c2-spreading-v2.7.0-178-g92cb654.md:252-266`) —
   so the class is finding B #6, M3's under R12, not E13's named-key ship.
   **Amended 2026-09-04**: §9 item 1 now names the class.
7. `ring_transport.hpp:16-19` ("no shared engine state") and `sched.md:189`
   ("those three are the whole list") are already false by one lock (the
   window latch, AN-S4's); **`sched.md` §9-2 and `rules.md` §3's table
   corrected 2026-09-04** — both latches in §9-2, the window's row added
   to the table citing AN-R9 until AN-S4 moves it into `txn.md` — and
   `ring_transport.hpp:16-19` is still owed.
8. `include/kds/txn/manager.hpp:63-68`'s premise ("nothing suspends between reading a
   tuple's header and overwriting it") is what a wait breaks; the re-check
   after every park is the consequence — AO-R4.
## AO-S4b — D12 across cores: the graph made complete, the closer refused where it registers

Drafted 2026-09-07 on `AU+AV-S0S1` after `5651edb` (the merge that brought
AN-S2 in), on the operator's word: "AO-S5(b) waits for AN-S2; start
AO-S4b". Every `path:line` is `[source-read]` at that commit.

### The finding this stage rests on

AO-S4a's row says what AO-S4b owes and why: *"what a cross-core cycle can
contain is a wait that registers no edge - the shipped-statement park and
the FK probe park - and a link nobody recorded is one no registration can
close."* AO-R7's cadence cannot close it either: a walk over recorded edges
sees the same graph a registration sees. **The cadence is therefore not
the mechanism; recording the missing link is.** With every wait an edge,
AO-S4a's registration-time detection is complete across cores by the
argument AO-S4a already made - the table is the instance's and
`wait_latch_` orders every core's registrations, so the walk that runs
when the closing edge is added sees every foreign edge - and the victim is
the waiter whose registration closed the cycle, wherever that registration
runs.

### The missing link, and who can record it

A coordinator `T_a` (session on core 1, holding rows there) ships a write
to core 0 and parks on the reply (`command_dispatcher.cpp:640`,
`WaitUntil{settled}`). On core 0 the shipped statement runs as a
participant transaction `P_a`, enrolled under `(core 1, session)`
(`shipped_statement_executor.cpp:113`, `EnrolFor`). `T_a` waits for
`P_a`; nothing records it. The other half - `P_a` parking on `T_b`'s row -
*is* recorded, by core 0's dispatcher loop (`:363`), once that dispatcher
has the table (AO-S5(a) held it at one core; this stage lifts it).

**The owner records the coordinator's edge, at enrolment, and clears it at
`Finish`.** The owner is the only core that knows both ids: `P_a`'s is its
own session's transaction after `BEGIN`; `T_a`'s must be **carried in the
request** - `ShippedStatementRequestPayload` gains `coordinator_txn`
(0 for an autocommit coordinator, which holds nothing and needs no edge),
eight bytes among the u64s, so `kShippedStatementFixedBytes` is 56 and
the longest shippable statement falls **968 → 960 bytes** (976 → 968 on its own branch; AN-S3's eight landed in the same merge), client-visible
and stated in `crosscore.md` §9 and `client-manual.md`. Nothing on the
client wire changes.

`Execute`, after `EnrolFor` for an in-transaction statement: `NoteWaitFor
(coordinator_txn, participant_txn)`. If that registration closes a cycle,
the coordinator is the closer and the victim: the statement is refused
`TxnConflict` naming deadlock **without running**, and the refusal rides
the reply to `T_a`'s client - which is the rig order's §6 cell 5, "the
victim's refusal reaches the client", and AU-S2's second cell, delivered
here by the reply path rather than by any new message (AU-R4: no
`kLockAbort`). `Finish` clears the coordinator's edge. When instead `P_a`
parks on `T_b`'s row and *its* registration closes the cycle, `P_a` is
refused by the dispatcher loop as AO-S4a refuses any closer, and its
refusal rides the same reply.

### What lifts, and what stays narrow

- The dispatcher takes the instance table on every core (`core_runtime.cpp`
  `set_locks(locks_)` unconditionally; `Expeditor::Open` hands core 0's
  dispatcher its table), so AO-S3's "a waiter must hold nothing" guard
  lifts above one core the way AO-S4a lifted it at one - **including a
  server at `cores = 1`**, which is the behaviour change AO-S5(a) held: a
  server now admits a holding waiter under AO-S4a's rule. The operator's
  word to start AO-S4b, the detector stage, is what lands it.
- The FK probe park registers no edge yet and refuses `busy` today
  (AO-3 B row 3's probe half); its wait *and* its edge are AO-S5(b)'s,
  which AN-S2's landing has un-gated. Until then a child's probe cannot
  be in a cycle because it never waits.
- The 2PC prepare and decide parks wait on messages, not on rows, and a
  prepared participant releases nothing until its decide - a waiter on a
  prepared row waits for a coordinator that itself waits on no row, so no
  cycle passes through a prepare (finding J's shape is a stall, and the
  fault net's).
- **AO-R7's cadence is not built**, and this is the departure to state:
  registration-time detection over a complete graph is exact, and a walk
  on a cadence would find nothing a registration did not. The fault net
  (AO-R8) stays as the net for a cycle through a wait this stage did not
  foresee, and a net that fires is logged as the defect it is.

### Cells, on the rig

1. **A two-core, two-session cycle through shipped statements**: `T_a` on
   core 1 updates its own row on a core-1 relation and ships an update to
   core 0's relation; `T_b` on core 0 does the mirror. The second ship's
   *participant park* closes the cycle - its registration on core 1 walks
   `T_a -> P_a -> T_b -> P_b` back to itself - and the participant is
   refused naming deadlock by the owner's dispatcher loop, exactly as
   AO-S4a refuses any closer; the refusal rides the reply to the
   coordinator's outcome, rendered line and carried `status` both
   (`c168acb`'s lesson); the survivor proceeds when the victim rolls back;
   no edge is left behind. **The mutation**: with the enrolment edge not
   recorded the cell fails at its edge count, and with that gate relaxed
   it fails at "the cycle was not detected" inside its 6 s bound - only
   the 11 s fault net would have ended it.
2. **The enrolment registration never closes a cycle, by construction** -
   stated rather than tested: the coordinator's edge is the *first* edge
   a ship adds, so it closes a cycle only if a chain already runs from the
   participant back to the coordinator, which needs the participant to be
   parked while its coordinator ships - and a session runs one statement
   at a time, so a parked participant means a coordinator parked on its
   reply, not shipping. The refusal branch at enrolment stays as the
   fault net's cousin: a wait this stage did not foresee closing there is
   refused rather than netted.
3. **A server at one core holds the instance's table in its dispatcher**
   (the lifted guard's wiring): `Expeditor` at `cores = 1` builds one,
   latchless, with no registry, and `dispatcher().locks()` is it; the
   two-core cell asserts both dispatchers hold *this* table. The
   behavioural cell - a holding waiter admitted in a server - is
   `LockDeadlockTest`'s, on a core-0 runtime whose wiring this now
   matches.
## AO-S5(b) — the cross-core wait through the table: the probe parks on the parent's core

Drafted 2026-09-07 on `AU+AV-S0S1`, on the operator's word after AO-S4b's
push: "push, and start AO-S5(b)". AN-S2 and AN-S3 are on `main`, which is
what this half was gated on.

### What AO-S5(a) and AO-S4b left

AO-3 B row 3 has two halves. The same-core half is AO-S3's: a child whose
parent is being written by an in-flight local transaction *waits* for the
writer's decide and re-checks. The probe half still refuses: a parent on
another core answers `kBusy` (`fk_probe_service.cpp`, `OnRequest`), the
child's dispatcher turns that into `TxnConflict retryable=1` (the busy arms
of `ResolveForeignVerdict`), and the client retries on a schedule nobody
chose - the wait written in the wrong place, which AR2-A §1's first axis
rejects. And the wait registers no edge: a cycle through it is invisible to
the detector AO-S4b completed for the shipped park.

### The mechanism: the parent's core waits, the child stays parked

The wait belongs where the holder is. The parent's core knows the holder
(`CheckParentPresent`'s `busy_trx`, the tuple's `trx_id`) and the child's
transaction (`FkProbeRequestPayload::transaction_id`, already carried), so
on a busy verdict the probe server, instead of replying:

1. records `child -> holder` in the instance's wait-for graph
   (`NoteWaitFor`), the edge the cycle needed - and if that registration
   closes a cycle the child is the closer, refused naming deadlock through
   the probe's own status, which the child's `CollectProbeReplies` already
   turns into the statement's refusal;
2. parks a coroutine on the parent's reactor until the holder decides or
   the probe's deadline passes (`kFkProbeReplyDeadlineNs`, the child's own
   5 s - a longer wait would answer nobody, since the child's waiter has
   given up by then); the park polls `IsInFlight(holder)` on the core that
   owns the holder, which is the same-core wait AO-S3 built, run on the
   holder's core on the child's behalf;
3. clears the edge keyed on the holder (`ClearWaitFor(child, holder)`,
   AO-S4b's guarded form), re-checks **every** parent under a fresh check
   view - the re-check through the window, which is what AN-S2 delivered:
   a decided holder is visible or gone, never in doubt - and replies
   `kPass`/`kViolation`; past the deadline with the holder still in flight,
   `kBusy` as before, and the child's busy arm is then the fault-net shape
   rather than the ordinary answer.

Intents are granted per attempt, on each pass the re-check computes -
`FkIntentTable::Add` is a set insert, so a re-answer records nothing twice
and only `stats_.recorded` counts the repeat. What makes that safe is that
the child decides for every owner it enrolled at *send* time, whatever the
answer, so an intent granted on an attempt whose reply is a violation is
released by that decide.

The child's side changes only in wording: its busy arms name the wait the
parent's core made and did not see end.

### What this does not do

- The reverse probe (a `DELETE` of a parent meeting a child being written)
  still answers busy without waiting; AO-S6's relation and slice units are
  where the delete side's waits belong.
- No `MayWrite` cell: AO-S5's third cell named a refusal AW-a retired
  (the note in `workorder-av-two-core-rig.md` §6). What replaced it is the
  page latch and the shared pool (M1), and the store's `MayWrite` refuses
  only a system page asked for by a peer - `device_page_store_test.cpp`'s
  cells pin that, and nothing under the lock changes it. The cell is
  restated as absent rather than rewritten to assert a refusal that does
  not exist.

### Cells, on the rig

1. **A child on core 1 waits out an in-flight parent on core 0 and passes
   when it commits**: the parent row inserted inside a transaction on core
   0; the child's `INSERT` on core 1 parks rather than refusing; the parent
   commits; the child's insert completes `INSERTED`. **The mutation**: with
   the probe replying busy at once, the child is refused `TxnConflict` and
   the cell fails at "the child was refused instead of waiting".
2. **A child waiting on a parent that rolls back is a violation**: the same
   shape, the parent rolled back, the child refused `FkViolation`.
3. **A cycle through a probe is refused at the registration that closes
   it**: the parent-side edge `child -> holder` makes a cycle through a
   probe visible to AO-S4b's walk - stated as the design's consequence;
   the cell is owed if the shape can be built on the rig within the probe's
   deadline, and AO-S4b's cell already proves the walk over a foreign edge.

### C1 and C2 - closed 2026-09-08 on `ao-s5b-c1c2`, from `25c5849`

**C1: answer from the participant's view, not "do not park".** The
interim the row proposed - not parking on a holder that is the requester's
own participant - would answer `busy` at once, which is the pre-stage
refusal with a shorter wait. The real fix is the one §4 already states
for one core: a transaction's own pending image is visible to its own
check through `own_trx_id`. The crossing broke that because the probe's
view was minted with no writer. Now `Answer` asks the shipped-statement
executor which local transaction the requester's session holds here as a
participant (`enrolled_transaction_id`, keyed the way the intent's holder
and the decide are - coordinator core and session) and mints the check
view with it. The parent row that participant inserted is the
transaction's own: pass. One it delete-marked: violation. No park, so no
edge, and nothing for the detector to have to see.

**C2: the decide flags the park.** The row offered the child's deadline
on the wire or a park margin. Neither is a proof: with the same clock on
both sides the park's re-answer and the child's decide can still be
ordered either way on the parent's reactor - a reactor woken by the
decide's kick drains it before it polls the park. What closes it is that
they *are* on one reactor: `ReleaseIntents` finds every park up for its
holder in `parked_` and sets the park's own flag; the park's wait
predicate reads the flag; an abandoned park clears its edge and returns
without answering. Nothing is granted after the child has decided, in any
interleaving - **provided the park is on record from the drain that
decided to park**, which the first build got wrong: it registered inside
the coroutine, whose body runs at its first poll, one phase after that
drain, so a decide in the same drain batch found nothing to flag. The
`critics-developer` pass caught it; `Answer` registers now, before it
submits, with a flag both sides share.

**What was under C2.** The decide C2 turns on was never being sent. The
refusal arm of `DispatchAsync` names transaction 0 in its decide on
purpose - the statement ran no transaction, and the session's previous id
belongs to a committed one - and `ReleaseIntentsWithoutWaiting` does the
same. `Txn2pcClient::OpenPhase` refused transaction 0 for every phase, so
both decides were refused before a message left and the refusal was a log
line at most. Every intent an autocommit statement was granted before it
was refused stayed held for the life of the process: F1's defect, on the
arm F1 did not reach. Fixed in this change - the zero check is `Prepare`'s
always and `Decide`'s only over a target that prepared, and an intent-only
decide opens no decision record. The C2 cell exercises that decide, which
makes it the first one that arm ever delivered.

**Cells 4 and 5.**

4. **A transaction writing its own parent across cores passes its child
   without a park**: one session on core 1 - `BEGIN`, the parent `INSERT`
   shipped to core 0, the child `INSERT` at home, `COMMIT`. The child
   completes inside 3 s with `probe_waits` at 0; the `COMMIT` inside 2 s.
   **Mutation**: a writerless view, and the cell fails at "the child
   waited on its own participant" after the probe's 5 s.
5. **A decide that arrives during a park abandons it and strands no
   intent**: core 0's reactor held for a second at the probe's arrival,
   so the park is stamped a second after the child's waiter; the child
   gives up, is refused `TxnConflict`, decides; `probe_wait_abandoned`
   moves; the parent commits inside the park's own deadline; a `DELETE`
   of the parent row answers `DELETED 1`. **Mutation**: let the park
   re-answer after the decide, and the `DELETE` is refused "relied on by
   a foreign key check running on another core".

Both cells run under production's 1 ms tick, because a participant's
prepare parks on its device sync and only a tick polls it: on the rig's
default - tick off, 5 s idle block - cell 4's `COMMIT` took 5,008 ms, one
block exactly. It was the first cross-owner `COMMIT` any cell had run on
the rig.

---

## AO-S6d — S6c's carried defects, closed before the units

Drafted 2026-09-09 against `main` at `7e26a65`; built the same day on
`ao-s6d-carried-defects` from `7e26a65`, on the operator's word ratifying
AO-0 item 17 (*"#17 RR fence first-updater-wins 채택"*) and, with it, items
15 and 16 as CLA proposed them.

### Why this sub-stage came before S6's units

S6a's survey turned S6 into "the stage where the engine first takes a
lock", and S6a..S6c-c spent the stage getting the tuple and range units to
*decide* rather than record. What S6c-c's last row left is not a list of
small items; it is three defects that S6c-b and S6c-c **made reachable or
made worse**, and the discipline that closed M1 (`index.md:32`, "three
simultaneously half-landed critical-path stages") says they close before a
fourth unit opens on top of them. The three are AO-0 items 15, 16 and 17,
and each is recorded there with its ruling.

### The one thing the draft got wrong, and why it did not change the fix

The draft argued item 16 was live and `[quiet-wrong]` at `7e26a65`: a
probe's resume "can set `blocking_writer_` for the first time", `EndWrite`
then withholds the poison, and nothing re-runs the statement. **The source
read says otherwise.** `may_park_` is set true around the statement's own
dispatch and around the write-block re-run and nowhere else, and
`NoteBlockingWriter` returns immediately without it - so a resume recorded
no blocker at all, `EndWrite` poisoned normally, and what the resume
actually lost was the *wait*, not failure atomicity.

That correction narrows the defect and leaves the fix identical, because
the wait cannot be offered without the allowance: the moment `may_park_` is
set around the resume, the `[quiet-wrong]` the draft described becomes
real. So the two halves land in one change, and the mutation that separates
them was run rather than reasoned about - keeping the allowance and
dropping the wait produces, on one transaction, `ERR TXN_CONFLICT` for the
`INSERT` and `COMMIT` for the `COMMIT`.

### What the wait becoming a function bought

The draft asked for "one loop over both arms". What landed is the
write-block arm extracted verbatim into `CommandDispatcher::AwaitWriteBlock`
and called from two places: after the statement's own dispatch, and after
each resume inside the probe arm's rounds loop. That loop is already the
outer loop the draft wanted - a wait's re-run is a whole statement, so the
probe it raises is the next round - and the arm needed only two things it
did not have:

- **The fault net is the statement's**, taken once and threaded through
  both callers. Re-taking it per entry would make "bounded once, not once
  per blocker" a property of an arm, and the total then is the net times
  the number of rounds.
- **Held verdicts are dropped after a wait.** A verdict is as of the view
  its probe was answered under, and the whole point of the wait is that the
  holder has since decided; applying a held `pass` to the re-run would
  answer from the view the wait exists to leave behind.

The copy-out's "never beside a probe or a ship" guard is unchanged and is
now stated as the guard rather than as an ordering.

### Cells

Ten, every one mutation-checked, and the mutation named at the cell.

1. `FailedCommitTest.AnAutocommitWriteWhoseCommitFailsReleasesEverythingItHeld`
   - `kStrict` plus `MemoryLogDevice::FailNextSync`, which is the fixture
   for a failing WAL commit that did not exist before this sub-stage. The
   transaction is gone, `EntryCount()` is back where it was, the row reads
   its prior value, and a second session's write of it completes without
   parking. **Mutation**: the release-only arm, and the second writer never
   finishes.
2. `FailedCommitTest.AFailedStatementInsideATransactionStillPoisonsAndStillHolds`
   - the arm that must not widen: rows stay, borrows stay, the session is
   poisoned, `ROLLBACK` releases. Green under mutation 1, which is what
   makes it the control.
3. `LockDeadlockTest.ARepeatableReadWriterWaitsOnAFenceHolderThatWroteNoRow`
   - `UPDATED 1` after the fence holder commits. **Mutation**: the
   whole-level exclusion, refused at once.
4. `LockDeadlockTest.ARepeatableReadWaiterIsStillRefusedIfTheFenceHolderWritesTheRow`
   - the holder writes row 5 during the wait and the waiter is refused
   `TxnConflict` after it, which is the operator's ruling made observable.
5. `LockDeadlockTest.ARepeatableReadInsertThatWaitsOnAFenceStillCannotDuplicateAKey`
   - the boundary the lift had to be checked against, and the cell that
   caught the first build's mis-derived role: a repeatable-read `INSERT`
   waits out a fence whose holder then takes the very key it named, and is
   refused `duplicate primary key` rather than writing one. Safe because
   the proof is physical and not through the view.
6. `Txn2pcBlockedWriterTest.ARepeatableReadChildWaitsOutItsParentBecauseTheCheckViewIsFresh`
   - B2 made observable: a repeatable-read child waits out its in-flight
   parent and passes at its commit, where the first build refused it at
   once and the identical statement one core over waited.
7-9. `FkProbeRigTest.AChildResumedFromItsProbe*` - a parent committed on
   core 0, a range fence on core 1 declared by a `DELETE` that deletes
   nothing, and the child's `INSERT` parking on its **resume**. Commit,
   rollback, and the same inside an explicit transaction. **Mutation**:
   either half of item 16's fix, and all three fail at "refused instead of
   waiting"; with the allowance kept and the wait dropped, cell 7 also
   commits a transaction it told `ERR`.

10. `FkProbeRigTest.AResumedWriteThatHasAlreadyWrittenRowsIsRefusedRatherThanParkedMidWalk`
   - B1's, and the only cell here whose mutation produces a wrong answer
   rather than a missing wait: three child rows on core 1, a holder taking
   the middle one, and a victim whose `SET` names a foreign parent so that
   its walk runs on the resume. Refused, and its transaction poisoned.
   **Mutation**: `UPDATED 2` and `COMMIT`, for three rows written.

The suite ran **3359/3359 green in 166.66 s at `v2.7.0-300-g142431e`**,
the tree that landed, with the pre-push gate running it again there; and
3359/3359 in 173.17 s after the third review's cuts, which change no test.
The count is 10 above the 3349 the tree carried at `7e26a65`, which is this
sub-stage's cells and nothing else.
**Overhead not measured**, deliberately and for every S6 row's reason: the
three fixes touch failure and resume arms rather than the per-row acquire,
and AO-S7 is where the numbers are named before they are run.

### What §(b)'s third cell became

The draft asked for a cell where the resume's outcome carries a **second
probe** as well as a blocker, pinning the copy-out guard. It is not
constructible on the rig: it needs two overlapping exclusive fences on one
relation held at once, which is exactly what the table refuses. What pins
the guard instead is the shape every rig cell already has - the wait's
re-run raises a fresh probe, and a child that answers `INSERTED` is a child
whose second round was collected rather than discarded, which is the guard
read from the other side.

### AO-S6e

The AO-S6 row's units, unchanged: relation `X` for DDL
(`IndexBuildPending` → wait), D8's slice fence and `IS` at the slice under
**AO-R12**'s gate (this said R13's, which has none — AO-S6e-d), the
`DROP TABLE` wait on a positioned peer reader. It inherits
nothing carried, which is the point of ordering them this way. C3's
cross-unit wake and C4's mutual-refusal bias are still not here: both exist
only to move the park from polling `IsInFlight` to a lock slot, and whether
that move is worth making is a number AO-S7 has not produced.

---

## AO-S6e — S6's units: the relation lock, the read borrow, the slice fence

Drafted 2026-09-09 on `ao-s6e-units` at `68fae89`, which is `origin/main`.
**Revised the same day**, before any code, after a `critics-developer` pass
over the draft refuted two of its claims and CLA's own reading of
`core_runtime.cpp` blocked its first sub-stage. What that pass found is
kept here rather than quietly fixed: a plan is cited by everything built
from it, so a claim it got wrong is worth as much on the record as one it
got right.

**Status: closed 2026-09-09 at AO-S6e-d.** AO-S6e-a and AO-S6e-c built, AO-S6e-b deferred. AO-0 items 18–21 were ratified
2026-09-09 (*"18-21 전부 제안대로 채택"*), and two of them were ratified on
reasons that did not survive the review — §"What the review took back"
says which, and those two go back to the operator. AO-S6e-a is blocked on
a separate finding of its own.

### The survey, at `68fae89`

**1. `IndexBuildPending` — census row 4.** `src/server/core_affinity.cpp:40`
builds the refusal and `command_dispatcher.cpp:6560` calls it, under the
`Covers` guard at `:6559`, inside `CheckWriteAffinity`'s peer arm. **Census
row 4's own citations are stale** and are corrected here rather than
followed: it names `core_affinity.cpp:65-74` and `command_dispatcher.cpp:6306`,
neither of which is the site any more.

The window is memory-resident on the owner (`core_runtime.hpp:648`), opened
and closed by the index-build service, and expired at
`kIndexBuildPendingCeilingNs` = 180 s (`index_build_service.hpp:157`).
**It has two consumers besides the write gate**, and retiring it retires
both: `index_build_service.cpp:131` refuses a *second* concurrent build of
the same relation through the same `Covers`, and
`include/kds/exec/range_eligible.hpp:60` names `PendingIndexBuilds::Covers`
as one of the two admission windows RD5 owes a close on. Neither is a cell
the AO-S6 row enumerates; both are work any retirement inherits.

**And no transaction spans a cross-core `CREATE INDEX`.**
`BeginForeignIndexBuild` (`command_dispatcher.cpp:3145-3186`) refuses inside
an explicit transaction at `:3153`, is called at `:3020` **before** any
`InDdlStatement`, and returns `pending_index_build`; phase 2,
`FinishIndexBuild` (`:3188`), opens an `InDdlStatement` of its own at
`:3230`. Holders are keyed by `std::uint64_t txn` (`lock_table.hpp:527`,
`:693`) over one table for the instance (`:116`). So census row 4's
"`CREATE INDEX` holds relation `X`" has nothing to hold it. That is item
18, and it is the one load-bearing claim of this document the review
confirmed outright.

**2. What the lock family already takes, correctly stated.** The draft said
S6a..S6c "wired only `IX` at the relation and `X` beneath it". That is
false twice over at `68fae89`:

- `command_dispatcher.cpp:11350-11354` takes a **relation-unit borrow in
  `kExclusive`**, so a `WHERE`-less `DELETE FROM t` already holds relation
  `X`. What AO-S6e-a needs is not the mode, it is a holder whose lifetime
  covers a build.
- `command_dispatcher.cpp:11462` and `:7560` take `LockKey::Range(...)` in
  `X`, and `lock_table.cpp:23-26` defines a fence as "a range- **or
  slice**-unit borrow in `S` or `X`", `LockKey::IsFenceUnit()`
  (`lock_table.hpp:372`) treating the two alike. **The engine already takes
  a fence**, with AO-R3's counter and `FenceCoversKey` live since S6b.

What is genuinely absent is narrower: **no `IS` anywhere** — `kIntentionShared`
appears only in the table's own name switch — and **no slice-unit borrow**,
and no `S` mode on any unit. That is what (b) and (c) add, and it re-sizes
both.

**3. The read borrow's seam, and it is two seams.**
`src/exec/step_vm.cpp:1985` is the page **boundary**, reached at every step
index; `:1994` is the **park**, guarded by `resume_gate_ != nullptr &&
index == 0`, so only the outermost walk reaches it. AO-R12's per-page
reading puts the `IS` at the first; the cheap reading puts it at the
second. They are not one line range and the draft cited them as one.
Unsurveyed and also positioned reads that never reach either:
`RunPointStep`, `RunIndexStep`, `RunCabinStep`, `ServeFromCabin`.

**And a mechanism that already covers the cell (b) exists for.**
`step_vm.cpp:1996-2009`: after a park the runner re-`Bind`s, because any
DDL anywhere clears the whole `TableAccess` cache, and "a relation dropped
while we were parked surfaces here as a clean error instead of a read
through freed memory". So a positioned peer reader meeting a `DROP TABLE`
already gets a bounded, clean outcome with no `IS` at all.

**4. `DROP TABLE`.** `command_dispatcher.cpp:3523`, under `InDdlStatement`
at `:3525`, on core 0 (the peer-DDL gate is `:1514-1558`), RESTRICT against
foreign keys and assertions, then `Catalog::DropTable`. It waits for
nothing. Its pages **orphan** — `drop-table.md:8-19`, "they stay allocated
and unreachable — leaked space" — and the oid is never reissued, so no page
is reused under a reader. `ddl-transactional.md:121` makes the drop atomic
and deliberately **not** isolated, and `:135` says its readers "are not
wrong about the rows — the data pages are untouched — they are early about
the schema".

**5. `DROP INDEX`, which the draft put in the split with no survey line.**
It is the harder half of (a), not the easier: there is no owner-side window
to convert, and `command_dispatcher.cpp:3034-3070` documents a live hole —
`BumpVersion` broadcasts at the delete-mark, *before* the commit, and DT9's
in-flight predicate is core-local, so a peer owner stops maintaining the
index before `COMMIT` and a `ROLLBACK` restores it missing every row
written meanwhile. Inside a transaction that is refused by name; autocommit
is "left admitted" with the window open. A relation `X` here has to address
that hole or meet it mid-build.

**6. The assertion's admission protocol.** `assertion.md` §6.2 lists four
properties; the two the draft quoted are verbatim (`:336-345`). The draft
then said the false rejection becoming a wait changes two of them. It
changes **three**: `:338-339`'s "**Deterministic failure.** The loser of a
race **fails immediately**" is falsified by any wait, and only its trailing
"no retry storm and no livelock" survives. And §6.1 (`:274-277`) states "No
latches, no atomic CAS loops, no cross-core sharing. … the entire protocol
is core-local", which waiting through the instance-wide, mutex-partitioned
table (`lock_table.hpp:117-137`) breaks as well. The build's own refusal is
separate and stays: `assertion_build.cpp:208` (the draft said `:203`, which
is inside the comment above it).

### The split, re-sized

| # | sub-stage | gated on | size |
|---|---|---|---|
| AO-S6e-a | The index-build window's refusal becomes a wait (census row 4's outcome, none of its mechanism) | **built 2026-09-09** | S, not L |
| AO-S6e-b | `IS` and the slice unit on the positioned read path, and the `DROP TABLE` wait | **deferred to AO-S7 with items 19 and 20** | L |
| AO-S6e-c | The assertion false rejection becomes a wait; **the slice fence is not built, and the sub-stage says why** | **built 2026-09-09** | S, not M |
| AO-S6e-d | The row, and the spec edits these make | — | S |

(a) was sized M on the belief that the relation `X` was new machinery. It
is not — the mode is already taken — but splitting `InDdlStatement`
(`command_dispatcher.cpp:8538-8545`, a synchronous template:
`BeginWrite` → `body(scope)` → `FinishDdlStatement`) around the park at
`:1038`, so a live `WriteScope` rides `PendingIndexBuild`, is most of the
work, and `DROP INDEX` brings its own hole. L.

(c) was sized on "the engine taking a fence", which it already does. What
is left is the slice **unit** and the `S` **mode**, plus a spec rewrite.
Still M, for the rewrite rather than the code.

### The cells the AO-S6 row names, mapped

- *`CREATE INDEX` waits for an open writer and proceeds after its commit* —
  (a).
- *A writer arriving during the build waits* — (a).
- *An assertion's bounded false rejection admits after the reserver aborts*
  — (c).
- *`DROP TABLE` waits for a positioned reader on a peer* — (b).
- *A slice fence survives a leaf division* — (c). `lock_table_test.cpp:330`
  pins the table-level half already (`ASliceIsFoundAfterThePageItCameFrom`
  `ChangesItsBounds`, the quoted "identity is the bounds and nothing else"
  at `:345`); what (c) adds is a slice borrow taken by the engine.

### AO-S6e-a was blocked, and item 18's ruling is not what unblocked it

**CLA's own finding, 2026-09-09, reading the paths item 18's ruling names.**
The ruling is sound about the coordinator. What it does not reach is why
`IndexBuildPending` exists, and the wiring site says so in its own words
(`core_runtime.cpp:685-687`):

> `done(committed)` drops the catalog cache so the published index is seen
> by the first admitted write.

`IndexBuildServer::OnDone` (`index_build_service.cpp:243-256`) closes the
window and then calls `on_committed_`, which `CoreRuntime` binds to
`InvalidateCatalog()`. **The window's close, the owner's catalog-cache drop
and the admission of the next write are one ordered event on the owner's
own reactor.** A relation `X` held by core 0's transaction releases at that
transaction's decide — a write to a shared-memory table, visible to the
peer immediately and before it has drained either the `kIndexBuildDone`
message or the `kCatalogInvalidate` broadcast. So a writer woken by the
lock can be admitted with a catalog cache that does not yet carry the
index, and writes a row into nobody's index: the defect the window exists
to prevent (`index_build_service.hpp:40`), reintroduced by the mechanism
meant to replace it.

Census row 4's fate is therefore **half a fate**. The lock converts the
refusal into a wait, which is what AR2-A §1 measures; it does not order the
wake against the owner's publication, and that ordering is the whole of
what the window buys.

**The obvious repair collides with a deferred item.** Holding the `X` under
a non-transaction "build" holder released by the owner in `OnDone` puts the
release where the ordering is correct by construction — but
`DispatchAsync`'s wait predicate is `!txn_->IsInFlight(block.trx_id)`, so a
holder that is not a transaction is never in flight, the wait ends on its
first poll, and the woken writer is refused again. Waiting on a **lock
slot** instead is C3, deferred until AO-S7 prices it.

Three ways out, and CLA proposes the third:

1. **C3 first**, pulled ahead of AO-S7.
2. **A transaction on the owner** spanning the build, so the holder is a
   transaction and its decide is the owner's own event. The owner builds
   under `system` tasks and has none.
3. **Keep the window and convert its refusal to a wait without a lock**:
   the peer writer parks on `!pending_index_builds_->Covers(oid)` instead
   of being refused. Census row 4's *outcome* with none of its mechanism,
   no new unit, and a wait predicate of the shape this file already has
   eleven of. The cost is that the relation `X` does not arrive at S6e-a,
   and the AO-S6 row's first two cells are met by the window rather than by
   the lock.

**(3) was taken, and AO-S6e-a is built on it** (2026-09-09, the operator's
word *"CLA 제안을 따르고 이후 작업 수행"*). **Its first build was not safe and a
`critics-developer` pass said so**, which is recorded below rather than
quietly fixed: the park took no deadline at all, and on the shipped path -
which is the main population, `ShippedStatementExecutor` running every
shipped statement through `DispatchAsync` - it could outlive the arrival
core's `kShippedStatementDeadlineNs` by up to 170 s and commit a row the
client had been told `UnknownOutcome` about. Three more with it: the
statement's *total* park was unbounded because the loop re-entered per
window; the window's own expiry, which the bound leaned on, runs from a
timer armed only where `wal_drain_interval_ns > 0`, which is not the
default; and the new arm sat *after* the write-block arm, so a re-run
that met a held row was refused where a first dispatch would have waited
- item 16's defect on a new pair. **`kIndexWindowWaitNs` closes the first
three**, derived as half `kShippedStatementDeadlineNs` with the relation
asserted at its declaration and taken once for the statement, falling
back to the refusal the gate already produced; **one loop over both
waits closes the fourth**, and the foreign-key probe arm gained the same
call beside item 16's. The sub-stage came out **S, not
L**: the whole of it is one member, one outcome field, one arm and one
cell, because the ordering that made a lock wrong here is the same
ordering that makes the window's own predicate sufficient. What did not
arrive is stated as a fact rather than deferred quietly — **the relation
`X` is not in the engine's DDL path**, census row 4 is met by a window, and
the AO-S6 row's first two cells are met by it too.

The strengthened form of the argument, which the build confirmed at the
site: `IndexBuildServer::OnDone` calls `pending_.Close` and *then*
`on_committed_`, inside one handler that runs to completion in the drain
before the next task is polled. So the park cannot observe the close
without the cache drop. That is not a property a lock could be given
without C3.

### AO-S6e-c, and where the fence went

`AO-S6e-a` has a section above because it was **blocked** and the block
needed arguing. `AO-S6e-c` was not blocked, and its section exists for the
opposite reason: the sizing table says the slice fence "is not built, and
the sub-stage says why", and a row in a table is not where a reader looks
for a why.

**The wait was built; the fence was not, and is not owed.** Census row 11
put the false rejection's conversion *"under D8's slice fence (`S` while
checking, `X` when changing the group's state)"*. The conversion landed on
the lock family's own wait channel instead, which is what gives it the
wait-for graph — and the graph is load-bearing here in a way it is not for
the other units, because two transactions can each hold a reservation the
other's admission needs and that cycle is real.

The fence has no contender, and this is the third time this stage reached
that sentence:

- The check and the reserve run **inline in one statement** with nothing
  between them on a cooperative core. `InsertOneRow`'s own comment says so
  where the two calls sit: *"nothing runs between the two on a cooperative
  core, so the answer holds"*. `AdmitAndReserveUpdate` does both inside one
  synchronous call.
- The enforcer is a `CommandDispatcher` member, so **one per core**, and
  the affinity gate keeps a relation's writes on the core that holds its
  live cabin.
- There is no fourth path: the sorted fill is gated on
  `!enforcer_.AnyOn(oid)`, and `ExecuteInsert` shares `InsertOneRow`.

So an `S`/`X` fence over the group would serialise against an interleaving
that cannot occur. What the sub-stage's review *did* find is that the
absence of a fence is not the same as the absence of an ordering
obligation: `AdmitAndReserveUpdate` reserves for one assertion before it
can refuse for the next, and the transaction's trail — which is what
`EndWrite` reads to decide whether a statement is re-runnable — cannot see
a reservation. That is a real ordering fact, and it is closed by refusing
rather than waiting once anything has been reserved, not by a lock.

### What the review took back

Two of the four ratified items were ratified on CLA's reasoning, and the
reasoning is gone. The rulings are the operator's; these are the grounds
they were given, withdrawn.

**Item 19's `[quiet-wrong]` is withdrawn.** CLA argued a per-page `IS`
because the cheap reading "is wrong across cores: a reader that never parks
still runs on its own reactor while core 0's DDL runs on another, so 'no
park, no lock' leaves exactly the reader the mover must wait for
unprotected." Three refutations, and CLA accepts all three:

- **AO-R12, the ruling it defends, says the mover does not exist**
  (`:405`: "no mover exists, so … the M2 consumer is DDL's relation `X`").
  The argument invokes an agent the ruling says is absent.
- **DT1**: pages stay allocated and unreachable and the oid is never
  reissued, so an unparked reader reads correct rows.
- **`step_vm.cpp:1996-2009`** already re-binds after a park and surfaces a
  dropped relation as a clean error.

So the cheap reading is not `[quiet-wrong]` for anything on this stage's
list, and the per-page reading is a **cost question with no correctness
argument behind it** — on the hottest path in the engine, priced by a
stage that has not run. What CLA now proposes: **defer item 19 to AO-S7**
and take the cheap reading, or none at all, until there is a number and a
consumer.

**Item 20 loses its motivation with it.** CLA proposed the `DROP TABLE`
wait as narrowing §5a for one shape. Given DT1 and the re-bind, what that
shape gets today is correct rows and a clean error. **What the wait buys
has to be stated before it is built**, and CLA cannot state it: the honest
position is that (b)'s cell may be unmotivated, and that saying so is
better than building it and discovering it.

**Item 21 is narrower than CLA put it, and part of it was never open.**
`ar0-architecture-revision.md:416-419` already rules that AS4 "is struck by
whichever work order lands D8", so AS4's striking is AR0's, not this
item's. What is open is the rewrite, and it is **three** §6.2 properties
plus §6.1's core-locality, not two — §"survey 6" above.

**Item 18 stands.** Its premise is confirmed. Its *class* was understated:
`kIndexBuildReplyDeadlineNs` is 60 s (`index_build_service.hpp:156`) and
bounds the **asker**, while the 180 s ceiling bounds the **peer writer**
when no `done` arrives, so the user-visible change is two facts (60 s → 11 s
and 180 s → 11 s), not one. And "a DDL transaction open across a ring round
trip" understates the shape: a peer's `CREATE INDEX` ships to core 0
(`command_dispatcher.cpp:1514-1558`), so it would be open across **two**.

### AO-S6e's close

**The stage delivered two waits and no new unit**, and that sentence is the
whole of it. `IS` is still taken nowhere, there is still no slice-unit
borrow and no `S` mode on any unit, which is exactly what the survey found
absent when the stage opened.

The three reasons turned out to be one reason. Every unit this stage was
asked to add had **no contender, or a contender the unit could not have
served**:

- The relation `X` for `CREATE INDEX` had a contender — a peer writer — and
  could not serve it: the ordering the window has, close-then-invalidate on
  the owner's own reactor, is not a property a lock released on core 0 can
  be given without C3. AO-S6e-a took the wait and left the lock.
- The `IS` at the slice was argued for a **mover**, and AO-R12 says in its
  own text that no mover exists; the reader it would protect already gets
  correct rows (DT1) and a clean error (the post-park re-bind). Deferred to
  AO-S7 with items 19 and 20, where a number can decide it.
- The slice fence for the assertion check had no interleaving to fence: the
  check and the reserve run inline in one statement with nothing between
  them on a cooperative core, and the site's own comment says so. AO-S6e-c
  took the wait and left the fence.

What that leaves for AO-S7 is not only the price of what was built. It is
the question this stage kept meeting from three directions: **which of
AR2's units has a contender on this engine at all**, now that ownership
routing puts one writer on a relation and the reactor serialises what runs
on a core. C3 is on that list too, since it is what a lock would need
before it could hold anything across a round trip.

### What AO-S6e does not do, stated so it is not read as forgotten

**The AO-S6 row's fourth unit, "the range key", is not in this stage** —
the draft dropped it silently and this line is the correction. It is census
row 10's "the range's id block is R5's borrow and its refill is a ring ask
— a message wait, not a lock wait", whose fate that row already records as
**kept**. Nothing in S6e-a..d touches it.

C3's cross-unit wake and C4's mutual-refusal bias, still — and C3 is now
load-bearing for (a), which is finding enough to note here rather than only
above. D9(a)'s `S` fence stays M3's (AO-R14). `MayWrite`'s grant arm and
`RelationWriteRightsPending` are AO-S5's. And no measurement: this stage's
rows will say "overhead not measured" until S7.

**One citation this document does not inherit.** The `IS`-at-the-slice
ruling is **AO-R12**. `:443` calls it "(R14)", `:942` calls it "R13's
gate", and R12's own body says "R13's M2 consumer"; R13 is "M2 logs nothing
and changes no format" and has no gate. Three spellings for one ruling,
corrected here and left for AO-S8 to fix at the other two sites.
