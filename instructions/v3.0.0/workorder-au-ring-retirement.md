# Work order AU — Ring retirement: the wake first, the transport last

Written 2026-09-05 by CLA against `4bd73d7`, under AR0-6. Every `path:line`
is source-read at that commit unless tagged `[design]`. Nothing is
`[measured]`, and nothing in this order runs a benchmark.

**Corrected before filing.** The draft of this order was written against
AR0-6's twelve-kind table. There are **35 kinds**, and AR0-6-V records what
that changes: AU-R4's `static_assert` count, AU-S3's arithmetic, and the
stage list, which now ends at AT rather than reaching zero on its own.

## AU-1 — The direction

Build the one cross-core primitive AR0-6 keeps — write shared state, kick the
reactor — as a public operation of `Waker`; re-base the two stages that were
about to consume the ring (AO-S5, the two-core rig) onto it; move `kShutdown`
off the ring; freeze the kind enum; and hand AT a checklist of the remaining
33 kinds rather than a discovery.

**What this order is not.** It is not "retire the ring". AR0-6 §1a settles
that: eighteen kinds are request/reply protocols carrying real operands, and
they die when **ownership** dies, not when the transport does. The ring
outlives AU by construction, and AU's job is to build the wake and to stop
the ring acquiring new consumers it would only lose again.

## AU-2 — Survey at `4bd73d7`

- `Waker` (`include/kds/sched/waker.hpp`): `Create()`, `handle()`, and
  `Wake()` declared at `:66` — a single 8-byte write, safe from any thread,
  failure swallowed by contract. Owned by the scheduler; reached today only
  through the transport's send path, which reads the destination's
  `sleeping` flag first (`:38-45`, the protocol living in
  `ring_transport.hpp`). The coalescing contract AR0-6-R1 leans on is at
  `:32-35`: "N wakes that arrive before the reactor looks are one wake,
  which is exactly right."
- Phase 3 of `RunOnce` drains the inboxes (`scheduler.hpp:59`, `:130`); with
  no transport attached it costs one null test. **42 files** reference
  `RingTransport` across `include/`, `src/`, `tests/` and `sim/` — the draft
  said 28.
- `LockWaitSlot` (AO-S2): the waiter parks on `WaitUntil` reading the slot,
  the decide flips it under the partition latch, same-core needs no wake.
  AO-R4's cross-core arm is unbuilt (`workorder-ao-m2-lock-family.md:316-319`).
- `SimRingTransport` is referenced by four files, of which **two are tests**
  (`tests/sim_ring_transport_test.cpp`, `tests/ring_transport_test.cpp`).
- **35 kinds**, inventoried by group in AR0-6 §1.

## AU-3 — Rulings

**AU-R1 — `Waker::Kick(core_id)` is the operation; `Wake()` is its
implementation.** One public entry point on the scheduler's waker table,
callable from any thread, that reads the destination's `sleeping` flag and
writes the eventfd only if set. The flag protocol moves from
`ring_transport.hpp` into `waker.hpp` verbatim, with its "why the flag cannot
be missed" argument. At `cores = 1` a kick on one's own core is the
scheduler's existing self-wake and writes no eventfd.

**AU-R2 — Every cross-core wake is write-then-kick, and the write is under
the structure's own latch.** The kick carries no meaning; a consumer that
needs to know *what* woke it reads its structure. This is AO-R4's existing
shape with the message removed, and it is what `rules.md` §3's wake-flag row
will state.

**AU-R3 — `SimWaker`.** A deterministic waker for the rig: `Kick(core)`
records `(tick, dst)` and schedules the destination's idle block to end at
`tick + delay(seed)`. Replaces `SimRingTransport` for new consumers; the two
transport tests keep theirs until AT.

**AU-R4 — The enum is frozen at its true size.** `ring_message.hpp` gains a
`static_assert` on the kind count — **at 34 since AU-S3 struck `kShutdown`**,
and at 35 before it — with a comment naming AR0-6 and this order. A kind is removed by decrementing the count in the same commit as
its replacement lands; a kind is never added. **The draft's 12 would not have
compiled**, which is the argument for the assert rather than against it.

**AU-R5 — Striking order.** A kind goes when its replacement's stage lands,
in that stage, with a cell showing the replacement carries the traffic the
kind carried. The transport object goes last, when the count reaches zero —
which is inside AT, not here.

## AU-4 — Stages

| # | stage | cells | size | gate |
|---|---|---|---|---|
| AU-S0 | This order; AR0-6 filed; AO-R4's text and the AO-S5 row amended; the rig order re-lettered per D26 | the files at the commit | the hour | — |
| AU-S1 | `Waker::Kick`, the `sleeping` protocol moved in; `SimWaker` | `cores = 1` byte-identical (no eventfd write on a self-kick, asserted); a kick to a sleeping peer ends its idle block within one `RunOnce` (real threads, the existing two-core `core_runtime_test` fixture); a kick to a busy peer writes nothing (counter); `SimWaker` reproduces a seed's `(tick, dst)` log across three runs | S–M | — |
| AU-S1b | **The two registries collapsed into one.** AU-S1 built `WakerTable` *beside* `RingTransport::WakeTarget` rather than in place of it, so every reactor registered the same two pointers twice and a send kicked through one copy while a stop kicked through the other. `WakeTarget`, `SetWakeTarget`, `RealRingTransport::wake_`, its `wakes_sent_` and `RingTransport::wakes_sent()` are deleted; `RealRingTransport` holds a `const WakerTable*` and its send calls `Kick`; `TrySend`'s explicit seq_cst fence moves into `Kick`, where every caller gets it; `Scheduler::wakes_sent()` (`SHOW META`'s `sched_wakes_sent`) reads the table. AU-S5 then removes a *user* of the wake rather than unpicking a registry | the instance's transport holds the instance's table (mutation: deleting the one wiring line leaves the whole suite green but for this cell — measured); at one core neither object is built; the four existing wake cells re-pointed at the table's counters | S | AU-S1 |
| AU-S2 | **AO-S5 re-based**: cross-core lock wake and victim notification as write-then-kick; no `kLockWake`/`kLockAbort` kind is ever added | AO-S5's cells unchanged in statement — the waiter on core 1 proceeds at the kick rather than at idle-block expiry (`sched_wakes_received` moves); the victim's refusal **reaches the client**, which is `c168acb`'s lesson (a report reaching nobody is the defect class) | M | the rig, on `SimWaker` |
| AU-S3 | `kShutdown` → atomic stop flag + `Kick`; the kind struck, count **34** | `STOP` at `cores = 4` stops every reactor within one idle block; the expeditor's join ordering re-read against the flag | S | D23 |
| AU-S4 | The three page-rights kinds struck with their grants — `kRelationFaultGrant` at AM-S2, `kRelationWriteGrant` and `kRelationGrantRequest` at AO-S5. Count **31** | no `GrantFaultPages`/`GrantWritePages` caller remains | S | AM-S2, AO-S5 |
| AU-S5 | **At AT**, one sub-stage per replacement, covering the remaining 31: catalog broadcast, the three allocator kinds, checkpoint, statistics, remote steps, shipped statements, index build, assertion build, foreign-key probes, 2PC | per group: the replacement carries the traffic, and the kind's handler is unreachable (a call-site grep in the cell) | inside AT | AT |
| AU-S6 | Count reaches 0: `RingTransport`, `RealRingTransport`, `SimRingTransport`, `ring_message.hpp`, phase 3, the N² preallocation, `AttachTransport` and the two transport tests removed; `sched.md` §5 rewritten; `rules.md` §3's row; G1's sentence | the suite; the golden log CRC unchanged (the transport never touched the log); `SHOW META` loses its ring counters and `client-manual.md` says so | M | AU-S5 |

**Order**: S0 → S1 → S1b → S2 (needs the rig, which now needs only S1) → S3
→ S4 (with its two milestones) → S5, S6 inside AT. **S1, S1b and S3 are
independent of every other lane** and are the three that can land without AT.

**S1b is a correction, not a plan**, which is why it carries a letter rather
than a number: S1 was written as "the pair moves house" and built as "the
pair gets a second house", and the order did not catch the difference because
its cells asked whether a kick works, never whether there is one registry.
The cell that would have caught it is S1b's own - the instance's transport
holds the instance's table - and it is the shape every wiring step here
should have: not "does the path work" but "is it wired to the one object
this instance has".

## AU-5 — What this changes in orders already written

- **The two-core rig order** (unwritten; **not `AS`**, which SUS-1 holds —
  D26 proposes `AV`): `SimRingTransport` → `SimWaker`; its H1 becomes "a kick
  delivered at tick+0 is the real path"; the delivery log is `(tick, dst)`.
- **AO-S5**: loses two kinds that were never added, and gains AU-R2's
  write-then-kick at two sites. Its rig dependency is unchanged.
- **AT (Uniformity, unwritten)**: inherits AU-S5 and AU-S6 as its last two
  stages, and AR0-6 §1's table as the checklist — 31 kinds at that point,
  each named with what retires it.

## AU-6 — Not in this order

The MPSC inbox (AR0-6 §3, deferred on its stated condition). Any change to
how a *task* is created — `sched.md:43` stands. Any benchmark.

## AU-7 — Row status

| row | status |
|---|---|
| AU-S0 | **filed 2026-09-05** on `worktree-ar2-borrow-model-2` at `4bd73d7`: this order and AR0-6, both corrected against the tree before filing (AR0-6-V). No engine code changed; the suite was not run for this row and is not claimed. AO-R4's text, the AO-S5 row and the rig order's letter are **not yet amended** — they are this stage's remaining work |
| AU-S1 | **Built 2026-09-05** from `cdc2d46`, landed `5ea3ac0`. `WakerTable` is the instance's map from core id to the destination's `sleeping` flag and its `Waker` — both halves already belonged to the destination and only their *home* moved, off `RingTransport`'s `WakeTarget`. **The survey found what this order did not anticipate**: `Scheduler`'s `may_sleep` read `transport_ != nullptr`, so a reactor with no transport never raised its flag and could not be woken at all — it only ever timed out. AU-S5/S6 would have deleted the transport and taken **every cross-core wake with it, silently**, the symptom being latency rather than a failure. The gate is gone and the waker's creation is extracted so either attach point arms it without ordering. **The race-closer has no analogue and none is built**: the transport closed the flag-raise window with `HasPending`, and with no queue there is no such question — the general form is "re-evaluate the predicate you are about to park on", which belongs with AU-S2's consumer. Until then a lost kick costs one idle block, which is AR0-6-R1's stated cost rather than an oversight. Three cells against a real `EpollIoBackend` (a `NullIoBackend` never blocks, so a wake test against one asserts nothing). Mutation-checked, and the mutation corrected the *cell*: restoring the transport gate made it **hang** rather than fail, because the teardown spun waiting for a reactor that would never wake — bounded now, so the assertions fire first. **Suite: 3348/3348 plain and armed** |
| AU-S3 | **Built 2026-09-05** from `5ea3ac0`. **The stage is one type change and its consequences**, because the message existed for exactly one reason and the tree said so twice: `Scheduler::Stop()` wrote a plain bool, so only the reactor's own thread could flip it — `scheduler.hpp` called that "the same [race] one kShutdown exists to avoid", and `core_runtime_test` opened with "plain bool owned by the reactor's thread, so core 0 may not call it". `stopped_` is `std::atomic<bool>` now, relaxed on both sides because nothing is published through it, and the kind is struck. **What went with it is more than the kind.** The hand-pumped drain: core 0 sent the message then pumped its own ready queue for up to a thousand turns, because its reactor had already left `Run()` and nothing drained the sends — its own comment conceded that a peer whose ring was full and whose reactor had stopped "would otherwise hang the shutdown", and a flag has no queue to be full. And the routing detour: a peer's `STOP` sent `kShutdown` to core 0 purely so core 0's thread would flip its flag, which is now one `instance_stop` hook — a single verb, rather than handing peers pointers to core 0's scheduler and to the waker table, neither of which is their business. **Value 20 is struck, not reused**: a number a stale peer might still send must not come to mean something else, and the gap is the record that it was spent. The routing cell asserted a `kShutdown` reached core 0's ring — testing the transport rather than the routing — and now observes the instance stop firing. `scheduler.hpp`'s unsynchronised-accessor paragraph names `stopped()` as **the one exception** and says the next accessor must earn it the same way, so nobody reads the whole block as now safe. Kind count **35 → 34** |
| AU-S2, S4..S6 | not started. S2 needs the rig; S4 rides AM-S2 and AO-S5; S5 and S6 are inside AT |
