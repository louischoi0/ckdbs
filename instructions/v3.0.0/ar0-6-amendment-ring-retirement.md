# AR0-6 — Amendment to AR0: the ring transport retired; the wake is the one cross-core primitive

Status: DRAFT amendment to `instructions/v3.0.0/ar0-architecture-revision.md`,
following AR0-5; pending operator ratification
Author: CLA, 2026-09-05, against `4bd73d7`
Scope: `include/kds/sched/ring_transport.hpp`, `sim_ring_transport.hpp`,
`ring_message.hpp`, `waker.hpp`, the scheduler's phase 3, `sched.md` §5,
`rules.md` §3's declared-shared-state table, AO-R4 and the AO-S5 row, the
two-core rig order
Claim tags: `[source-read]` with `path:line` at `4bd73d7`; the rest `[design]`.

**AR0-6-V is appended**: the source read of 2026-09-05 at `4bd73d7`, which is
what the tree says wherever the body disagrees. It found the draft's kind
count wrong by a factor of three, which is load-bearing — the body below
carries the corrected inventory.

---

## 0. The decision (operator, 2026-09-05, verbal)

> Every SPSC-ring function whose need is certainly gone is retired. The
> one cross-core need that remains — waking a sleeping reactor — is met
> by the per-reactor waker directly, not by a message. A per-core inbox
> for handing *work* to another core is **deferred until R4-S actually
> demands it**; no structure is built without a consumer.

## 1. What the ring carries today

**Thirty-five kinds, not twelve.** `ring_message.hpp` declares 36
enumerators — `kUnset` plus 35 — valued to 44 with gaps. The draft of this
amendment tabulated twelve and concluded "eleven of twelve payloads are
ownership's"; that sentence is withdrawn, and the full inventory is below,
grouped by what retires it.

| group | kinds | what replaces it | fate |
|---|---|---|---|
| Catalog broadcast | `kCatalogInvalidate` | schema version word (AR0-5 §2.1) | AT |
| Allocators | `kExtentLease`, `kTrxIdLease`, `kRowIdLease` | shared allocators (AR0-5 §2.2) | AT |
| Page rights | `kRelationFaultGrant`, `kRelationWriteGrant`, `kRelationGrantRequest` | `MayFault` at M1, `MayWrite`'s grant arm at M2 (AM-R2, AO-R14) | AM-S2 / AO-S5 |
| Checkpoint | `kAnchorWrite` | one instance checkpoint task (AR0-5 §3) | AT |
| Statistics | `kAccessStatsBatch` | local write under lock (AR0-5 §4) | AT |
| Remote steps | `kStepOpen`, `kStepBatch`, `kStepEof`, `kStepCredit`, `kStepCancel`, `kStepError` | shipping retired (AR0-5 §4) | AT |
| Shipped statements | `kShippedStatementRequest`, `kShippedStatementReply`, `kShippedRowDesc` | same | AT |
| Index build | `kIndexBuildRequest`, `kIndexBuildReply`, `kIndexBuildDone` | **§1a** | AT |
| Assertion build | `kAssertionBuildRequest`, `kAssertionBuildReply`, `kAssertionBuildDone` | **§1a** | AT |
| Foreign keys | `kFkProbeRequest`, `kFkProbeReply`, `kFkReverseProbeRequest`, `kFkReverseProbeReply` | **§1a** | AT |
| 2PC | `kTxnPrepareRequest`, `kTxnPrepareReply`, `kTxnDecideRequest`, `kTxnDecideReply`, `kTxnResolveRequest`, `kTxnResolveReply` | one stream's own scan (AR0-5 §4; AO-R8's net drops to 1 s "once M3 retires it") | AT |
| Lifecycle | `kShutdown` | atomic flag + kick | **AU-S3, now** |
| *(planned)* | `kLockWake`, `kLockAbort` | the slot is already shared; only the kick is needed | **never built** (§2) |

### 1a. The argument the draft omitted, and which the retirement rests on

**A kick carries no payload, so it cannot replace a message that carries
data.** Eighteen of the kinds above are request/reply protocols with real
operands — index build, assertion build, FK probes forward and reverse, 2PC,
shipped statements and their row descriptors. Saying "the one remaining
cross-core need is the wake" is only true if those protocols *cease to
exist*, and the draft asserted it without saying why they do.

They cease to exist because **AR0-5 retires ownership**, not because the
transport is replaced. Every one of them is a core asking *another* core to
act on a relation it owns: probe the parent's owner, build the index on the
owner, enforce the assertion on the owner, ship the statement to the owner,
prepare the participant. With no owner and one shared pool, the asking core
reads and writes the pages itself — the request has no recipient because it
has no subject. That is why they are AT's, and why AT rather than AU strikes
them: **each dies with its ownership, not with the ring.**

The corollary matters for sequencing: **the ring cannot be retired before
AT**, and AU is therefore not "retire the ring" but "build the wake and stop
the ring acquiring new consumers".

## 2. The wake primitive

`Waker` already exists: one `eventfd` per reactor, registered with its
`IoBackend`, `Wake()` a single 8-byte write callable from any thread
(`include/kds/sched/waker.hpp:27-30`, declared `:66`). It was built as *the
wake a ring message needs* — the message carried the meaning, the eventfd
carried the interrupt. AR0-6 keeps the interrupt and drops the message:

> **AR0-6-R1 — A cross-core wake is: write the shared state under its own
> latch, then kick the destination reactor.** The woken reactor learns *why*
> from the structure it parks on, never from a payload. A lost or coalesced
> kick costs the idle block's expiry (`max_idle_block_ms`,
> `include/kds/sched/scheduler.hpp:82`) — slow, never wrong, which is
> `waker.hpp`'s stated contract today ("N wakes that arrive before the
> reactor looks are one wake, which is exactly right"). The `sleeping`-flag
> protocol that avoids a syscall to a busy reactor (`waker.hpp:38-45`) moves
> from `ring_transport.hpp` into `Waker` itself, since the ring will not be
> there to carry it.

For AO-R4 this means: the decide flips `LockWaitSlot` under the partition
latch (already so, AO-S2), then kicks the waiter's core. The waiter's
`WaitUntil` predicate reads the slot (already so). Nothing new crosses cores
except the 8-byte write.

## 3. What is deferred, and the condition

A **per-core MPSC inbox** — the shared-memory successor to the ring for
handing work to another core — is **not built**. `docs/spec/sched.md:43` ("A
task created on a core runs and completes on that core. Moving *work*
between cores happens only by sending a message that causes the peer to
create its own task") stays as the rule, with "message" read as "inbox
entry" when the inbox exists. Condition to build it, stated as a test: a
stage lands a cell in which one statement's plan creates a task on a core
other than the session's. Until such a cell exists, that sentence describes
a path with no caller, and says so.

## 4. Impact

| item | before | AR0-6 |
|---|---|---|
| AO-R4 | cross core: "the decide sends one ring message (`kLockWake`…)" (`workorder-ao-m2-lock-family.md:316-319`) | the decide kicks (AR0-6-R1). Text amended; ruling otherwise unchanged |
| AO-S5 row (`:436`) | `kLockWake`/`kLockAbort` kinds; rig "over `SimRingTransport`" | the kick; rig over a sim waker. `kLockAbort` is likewise a slot write plus a kick |
| the two-core rig order | reproduces ring delivery order by seeded delay | reproduces **kick delivery ticks** by seed; the log is `(tick, dst)`; `SimRingTransport` is replaced by a `SimWaker`, and the rig gets smaller. **The order does not exist yet and is not `AS`** — see AR0-6-V |
| `rules.md` §3 declared shared state | three rows: WAL stream, reactor wake flag, data-file capacity | the wake-flag row names `Waker` and AR0-6-R1 |
| `sched.md` §5 | "per-core-pair SPSC lock-free rings, N² rings" | becomes "Cross-core wake"; the N² topology is a history note |
| G1 (revised) | "no atomics outside ring indices and the lock/latch primitives" | "outside the wake flag and the lock/latch primitives" |
| `SimRingTransport` | the only deterministic cross-core path | `SimWaker` replaces it for new consumers at AU-S1; the transport and its two tests go at AT |
| Scheduler phase 3 | one null test per `RunOnce` with no transport (`scheduler.hpp:59`, `:130`) | removed at AT; unchanged until then |

## 5. Sequencing

The retirement is **two-ended**. The wake end is built now, before AO-S5
gives the ring a new consumer it would then lose. The transport end is struck
at AT, one kind per replacement, because **every remaining kind is alive
until its ownership is retired** (§1a) — a lease kind cannot go before the
shared allocator, and a probe kind cannot go before the probe has no
recipient. The work order is AU.

## 6. Items for the operator

| # | item | class | CLA proposal |
|---|---|---|---|
| D23 | `kShutdown` now vs at AT | sequencing | now (AU-S3): it is a flag, it touches nothing ownership-bound, and it is the one kind a rig cell exercises on every run |
| D24 | the deferred inbox's rule sentence in `sched.md:43` | prose | keep, with the "no caller" note (§3) |
| D25 | freezing the kind enum | discipline | a `static_assert` on the count with a comment naming AR0-6 — **at 35, not 12** (AR0-6-V) — so that adding a kind is a deliberate edit of a line that says "do not" |
| D26 | the letter for the two-core rig order | naming | **not `AS`**, which SUS-1 holds (`workorder-as-sus1-heap-suspended.md`, AS-S1 landed `1f77e87`). CLA proposes **AV** |

---

## AR0-6-V — the source read, 2026-09-05 at `4bd73d7`

**One finding is load-bearing and three are citations.**

**The kind count was wrong by a factor of three.** The draft said "twelve
kinds (`kUnset` aside)" and its table listed twelve. `ring_message.hpp`
declares **36 enumerators — `kUnset` plus 35** (`grep -oE 'k[A-Z][A-Za-z]+ =
[0-9]+'` over the file). The eighteen the draft never named are whole
cross-core protocols with real operands: index build, assertion build, FK
probes forward and reverse, 2PC's six, the shipped-statement pair and
`kShippedRowDesc`, `kAccessStatsBatch`, and two of the three page-rights
kinds. Three consequences, each of which would have surfaced as a build
failure or a wrong plan rather than as a discussion:

- AU-R4's `static_assert(kMessageKindCount == 12)` **would not compile**.
- AU-S3's "the kind struck, count 11" arithmetic is meaningless.
- §1's conclusion — "the one cross-core need that remains is the wake" —
  did not follow from its own table, because a kick carries no payload and
  those eighteen carry data. §1a now supplies the argument that makes it
  true (they die with ownership, per AR0-5), and it changes the sequencing:
  the ring cannot be retired before AT.

**`workorder-as-s1-two-core-rig.md` does not exist, and `AS` is taken.**
The draft cited it and referred to "the AS-S1 rig order" throughout. In this
tree `AS` is SUS-1's letter — claimed 2026-09-05 in
`workorder-as-sus1-heap-suspended.md`, whose **AS-S1 is SUS-1's code stage**,
landed at `1f77e87`. This is the collision `CLAUDE.md` means by *cite the
file, never the bare number*, and the second instance found in two
amendments (AR0-5-V found the `M5` one). D26 proposes `AV`.

**Two counts and one line number.** `RingTransport` is referenced by **42**
files under `include/`, `src/`, `tests/` and `sim/`, not 28. `Wake()` is
declared at `waker.hpp:66`; the draft cited `:55-58` and the AU order
`:46-60`. `SimRingTransport` has four referencing files, of which two are
tests — the draft's "the two transport tests only" is right about the tests.

**Exact, unchanged.** `docs/spec/sched.md:43` carries the no-work-stealing
rule verbatim. `include/kds/sched/scheduler.hpp:82` is
`int max_idle_block_ms = 10;`. `workorder-ao-m2-lock-family.md:316-319` is
AO-R4's cross-core arm as quoted. The `sleeping`-flag protocol and its
"why the flag cannot be missed" pointer are at `waker.hpp:38-45`, and the
coalescing contract the amendment leans on is `waker.hpp:32-35`.
