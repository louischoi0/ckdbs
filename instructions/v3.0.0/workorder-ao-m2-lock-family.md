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
| 4 | `IndexBuildPending`, `src/server/core_affinity.cpp:65-74`, caller `src/server/command_dispatcher.cpp:6306` | `TxnConflict` | cross-core: the catalog half on core 0, the build on the owner | **wait** on the relation lock: `CREATE INDEX` holds relation `X`, a writer's `IX` waits | S5, then S6 |
| 5 | `RelationWriteRightsPending`, `src/server/core_affinity.cpp:52-63`, caller `command_dispatcher.cpp:6484`; `MayWrite`'s lease/grant arm, `src/storage/device_page_store.cpp:479-488` (the `TxnConflict` branch) and `:804-817` | `TxnConflict` | cross-core: a grant from core 0 | **retired**, not waited: AR2 §8 retires the grant arm "with AM-R1" at M2 — read as *alongside* it, the routing staying (AO-1). What replaces a page's write-right is the page latch and the shared pool (M1); M2 retires the refusal and its grant demand (AO-R14) | S5 |
| 6 | `CrossCoreWriteRefused`, `src/server/core_affinity.cpp:19-31`, callers `command_dispatcher.cpp:6257`, `:6277`; counted by `CrossCoreWriteCounters` (`include/kds/server/core_affinity.hpp:65-83`, "the residue") | `TxnConflict` | cross-core | **not M2's**: the residue is a statement spanning two owners (M3, AR2 §5.7) and a write inside an explicit transaction, which already ships and enrols (`include/kds/server/command_dispatcher.hpp:2560-2573`). Execution locality is R12, M3's; M2 builds the borrow that makes it legal | M3 |
| 7 | `CrossCoreReadNotImplemented`, `core_affinity.cpp:33-50`, caller `command_dispatcher.cpp:6914` | `NotImplemented` | cross-core | **not M2's**: local reads are M1's and AN-S2's; the read shapes are AR2 §5.7's | M3 |
| 8 | `PeerDdlRefused`, `core_affinity.cpp:109-114`, caller `command_dispatcher.cpp:1330` | `Unsupported` | cross-core | **kept**: DDL ships to core 0 (CC13, R12) | — |
| 9 | `MayWrite`'s system-range arm, `device_page_store.cpp:480-488` (the `InvalidArgument` return at `:488`) and `:811` | `InvalidArgument` | cross-core | **kept** until E13 (M3) | — |
| 10 | the three spent leases: `src/storage/extent_lease.cpp:139`, `include/kds/catalog/row_id_lease.hpp:114`, `include/kds/txn/trx_id_lease.hpp:65` — all `TxnConflict` since `include/kds/base/status.hpp:145-155` | `TxnConflict` | cross-core: a refill from core 0 | **kept**: allocator authority, retained by AR0-4. The range's id block is R5's borrow and its refill is a ring ask — a message wait, not a lock wait. C2's "6–7 refused `INSERT`s per 10,000 while a range opens" is **not** this class — every logged line is #6's `CrossCoreWriteRefused` (`bench/v3.0.0/results-ar2-c2-spreading-v2.7.0-178-g92cb654.md:252-266`), and AR2 §9 item 1 files it under E13 (M3) | — |
| 11 | assertion admission: no busy answer at all — `docs/spec/assertion.md:308-320`, `:324-332`; the "bounded false rejection" (`:330-332`, `status.hpp:90-99`) refuses `AssertionViolation`; the build refuses `TxnConflict` on an in-flight writer (`assertion.md:456`, `src/exec/assertion_build.cpp:203`) | `AssertionViolation` / `TxnConflict` | core-local (AS4) | the false rejection becomes a **wait** on the reserving transaction under D8's slice fence (`S` while checking, `X` when changing the group's state); AS4 struck | S6 |
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
the waiter's next poll proceeds. Cross core: the decide sends one ring
message (`kLockWake`, a new `ring_message.hpp` kind with no payload of
substance) to the waiter's core, whose handler flips the slot — the message
is a wake, never the decision, so a delayed message costs latency and never
liveness. After every park the waiter **re-checks** (finding G).

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
| AO-S5 | **Cross core**: the table across reactors, `kLockWake`/`kLockAbort`, `MayWrite`'s grant arm and `RelationWriteRightsPending` retired; **a deterministic two-`CoreRuntime` rig over `SimRingTransport`, first** (finding L). Gate: AM-S1 (latch order), AM-S2 (shared pool), AN-S2 (view) | a waiter on core 1 woken by a decide on core 0 while core 1's reactor sleeps (`sched_wakes_received` moves); with every message delayed to the transport's maximum the waiter still proceeds after the wake; at the store, `MayWrite` admits a page its lease/grant arm refused at `9e5068c`, under the lock — owner routing still in force (AO-1), so the cell is the store's, not dispatch's | L |
| AO-S4b | **D12, cross core**: core 0's detector over edges from every core; a victim on a peer aborted by message | a 2-cycle across two cores resolved within one cadence | M |
| AO-S6 | **The units**: relation `X` for DDL (`IndexBuildPending` → wait), D8's slice fence (AS4 struck), `IS` at the slice (R14) with R13's gate, the range key | `CREATE INDEX` waits for an open writer and proceeds after its commit; a writer arriving during the build waits; an assertion's bounded false rejection admits after the reserver aborts; `DROP TABLE` waits for a positioned reader on a peer; a slice fence survives a leaf division | L |
| AO-S7 | **C3** (AR2 §9 step 5) under `bench/README.md`'s five rules, and the price of R3's relation-level key | one results file per cell under `bench/v3.0.0/`, `git describe --tags` in each; E7's default and E12's price read from them, not decided | M |
| AO-S8 | **Prose**: `txn.md` §1 (AR0-M1's four texts) and §5; `heap-and-tuple.md:95`; `assertion.md` AS4/§6; `foreign-keys.md` F3 and §5's first bullet; `sched.md` §9-2; `rules.md`'s row loses its "not built" parenthetical and moves to `txn.md` §5, and the four documents that still say "the fourth row" are corrected (`ar0-architecture-revision.md:370`, `workorder-an-read-view.md:84`, `ratification-an-commit-order.md:168-169`, `workorder-am-m1-shared-pool.md:66,226`); `ring_transport.hpp:16-19`; `include/kds/txn/manager.hpp:22-28`; `keystone.hpp:23-29`; AR2-R2's "CAS on the byte"; `client-manual.md`, `server.md:100`; `CLAUDE.md`'s Transactions row | no spec says "no lock manager"; every lock in the reactor is in §9-2's list | M |

**Order**: S0 → S1 → S2 → S3 → S4a → S3b → **[the word, and AM-S1..S6 +
AN-S2]** → S5 → S4b → S6 → S7 → S8. S1–S4a and S3b depend on neither M1 nor
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
| AO-S1..S4a, S3b | not started; gated on the operator's word (AR2-A §6) |
| AO-S5, S4b, S6..S8 | not started; gated on AM-S1..S6 and AN-S2 closing, then the word |

---

## AO-0 — Items for the operator

The ruling table's constants and quiet-wrong entries, per AR0's standing
rule. Every item names the ruling it moves.

| # | item | class | CLA proposal / state |
|---|---|---|---|
| 1 | E2: the cap's value and its refusal's detail code | constant; user-visible | 65,536; `ResourceExhausted` with a new append-only detail (AO-R10) |
| 2 | The partition count | constant | 64 × cores, re-measured in S7 (AO-R2) |
| 3 | The fault net and the detector cadence | constant | the net `kTxnPhaseDeadlineNs + 1 s` while 2PC is in the tree and 1 s after (finding J), aborting the waiter; the cadence 100 ms; both `constexpr` in M2, the knob merge at M3 (AO-R7, AO-R8) |
| 4 | The Keystone byte | persisted format | **decided 2026-09-03: no persisted bit** (AO-R3); the masked-image arm declined |
| 5 | The subsystem's name | spec | `lock` (AO-R1); `borrow` is the alternative — **not answered in plan mode, still the operator's** |
| 6 | Waits at `cores = 1` | user-visible | AO-R9's reading of R9 converts a refusal into a wait at one core |
| 7 | The in-doubt block's clock-end goes | user-visible | `server.md:100`'s contract changes (R10, AO-R8) |
| 8 | D1's RU is not in this order | spec | confirm, or name the order that carries it (AO-2) |
| 9 | The FK split: F3's wait half in M2 (S3), D9(a)'s fence in M3 | spec | confirm (AO-R14) |
| 10 | The `rules.md` §3 row's "declared in" names this work order, not a spec, until AO-S8 moves it — a new class of row against `rules.md:24`'s "the declaration lives in the owning spec" | spec | accept the interim, with the move written into the row |
| 11 | The partition latch: `base/latch.hpp`'s `std::mutex` (AO-R2's S1 default) against D2(a)'s own words "spinlocks (atomics)" (`ar0-architecture-revision.md:87`), which AR0-M2 left undecided | design; measurement-gated | mutex first, a spin primitive only if S7's numbers ask for it |
| 12 | The deadlock victim's `TxnConflict` message | user-visible (wire-contract text, `src/txn/manager.cpp:153`'s own rule) | one new message naming "deadlock", the code and its retryable bit unchanged (AO-R7) |

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
