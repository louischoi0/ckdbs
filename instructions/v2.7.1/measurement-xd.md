# Work order XD — cross-owner commit cost decomposition

Drafted 2026-08-31 by CLA against `main` at `85eff7d`
(`v2.2.1-165-g85eff7d`). Operator direction, this session: **the
optimization target is the durable classes — D2 `group` first, D1
`strict` beside it. D3 `relaxed` is a control instrument, not a
target.** The classes are `wal/manager.hpp:29-40`.

## Background

`bench/v2.7.0/results-scenario2-cores-v2.4.0-83-g57110cf.md` measured
scenario 2 across cores and found the cross-owner commit where the
booking's time goes. Its increments, re-read for this order
(all measured, that file's cells):

| comparison | commit mean | increment |
|---|---|---:|
| group, b=8: `a-c1-b8` → `a-c8-b8` (peer listeners) | 1,716.9 → 4,913.5 µs | **+3,196.6 µs** |
| relaxed, b=8: `f-c8-b8` → `f-c8-b8-pl` | 76.6 → 3,053.3 µs | **+2,976.7 µs** |
| group, b=1: `a-c1-b1` → `a-c8-b1` | 975.7 → 2,910.3 µs | **+1,934.6 µs** |

The increment is ~3 ms in **both** durability classes — the cost is not
a relaxed artifact, and the b=1 row (no queueing possible: one booker
contends with nothing) puts the protocol's fixed part at ~1.9 ms under
group.

**The serialized leg chain, source-read at `85eff7d`.** One cross-owner
COMMIT takes, in sequence:

1. prepare send — one ring slot per participant
   (`txn_2pc_service.hpp`, 24-byte payload);
2. participant prepare: `wal::LogTxnPrepare` append, then
   `wal_->RequestDurable` and a park on `IsDurable`
   (`src/server/shipped_statement_executor.cpp:575`, `:596` via
   `AwaitPrepared`, `:585`) — **device sync #1**;
3. prepare reply, coordinator's `WaitUntil{Settled}` park ends;
4. coordinator decision: `CommitLocal`, then `RequestDurable` and a
   park (`src/server/command_dispatcher.cpp:453`), with the comment
   stating it holds **"whatever the durability class"** — **sync #2**;
5. decide send; the participant's own COMMIT is made durable **before
   the ack** ("`DispatchAsync` parked on `IsDurable` before this
   callback ran", `shipped_statement_executor.cpp` ~`:869`) —
   **sync #3**;
6. coordinator parks for acks.

`IsDurable` means the platter (`wal/writer.hpp:77-80`), in every class.
All three waits are the same primitive — R6-B established that and
measured the aggregate under group at 1.975x a one-owner commit
(`bench/v2.5.0/results-r6b-cross-owner-cost-v2.4.0-28-g53f6aae.md`),
against D7's predicted "two durable syncs in sequence"
(`instructions/v2.4.0/2pc.md:200-215`).

**Two claims this chain contradicts, to be settled by measurement, not
edited first:**

- The scenario-2 file's headline *"3.0 ms of two-phase commit with no
  device in it"* — the relaxed arm removes the ordinary commit's wait,
  not the protocol's three. XD3 tests it; XD7 corrects the file only if
  the test says so.
- This project's standing note that the **read-only-participant reply**
  is the largest measured cross-owner cost and the obvious next lever.
  Source-read doubt: scenario 2's booking ships **writes** to core 0
  (`operations.booked_cbm`, `organizations.outstanding` — BTREE
  relations core 0 owns), so its sole participant is a *writing*
  participant and the RO reply would never fire for it. XD6 settles
  this from the workload source.

## Conclusions (standing, from this session's ratifications and rules)

1. Target classes: group, then strict. One relaxed cell survives as the
   sync-ablated control.
2. No protocol change is built under this order. XD1's verdict, if it
   finds slack, becomes a ratification ask — the enactment is a later
   order.
3. No constant is chosen. Anything wanting one stops and reports.
4. Session↔owner affinity and DA2's placement question stay out of
   scope; this order prices the commit, not the routing.
5. `cores = 1` and every one-owner transaction must be untouched by
   XD0's counter exposure — the path is shared, so XD0 is read-only
   surfacing of a counter that already increments
   (`src/wal/writer.cpp:98`, existing `fetch_add`; Guideline 1 gains no
   new atomic).

## Hypotheses

- **H-XD1 (the chain is the increment).** Under group, the cross-owner
  increment ≈ two extra serialized sync latencies + ring hops:
  ~1.9 ms at b=1 on this device. On a WAL whose sync latency is ~0
  (tmpfs), the increment collapses to the hop/park residue —
  predicted < 500 µs. **Falsifier:** the increment survives tmpfs.
- **H-XD2 (batching helps throughput, not latency).** The three legs'
  `RequestDurable`s ride the same drain as everything else, so at b=8
  concurrent bookings share syncs — syncs-per-booking well under 3 —
  while per-commit latency stays three sync-latencies deep, because the
  legs are sequential per transaction. **Falsifier:** syncs-per-booking
  ≈ 3 at b=8 (no sharing), or b=8 latency materially below b=1's chain
  (sharing somehow shortens the chain).
- **H-XD3 (strict ratio).** Under D1, a one-owner commit is one sync
  and a cross-owner one is three in sequence: ratio ~3x at b=1,
  against group's measured 1.975x. **Falsifier:** materially off 3x
  either way — off high means a cost the chain model missed.
- **H-XD4 (the queueing term).** The b=8-minus-b=1 increment
  (~1.26 ms, group) is core 0 reactor occupancy — 2PC service waiting
  behind shipped-statement execution — and should grow with b and move
  with core 0's `shipped_wait_us_max` / foreground-poll occupancy.
- **H-XD5 (RO reply inapplicable here).** The booking ships at least
  one write, so its participant can never answer read-only. Verdict
  from `tools/scenario2_freight.py` + ownership, counters as
  confirmation only.

## Rows

**XD0 — expose the sync counters.** `wal_syncs` (and `wal_sync_failures`)
into `SHOW META`, per core, from the existing `Writer::syncs()` /
`failures()`. Absent where the core has no writer. The one engine change
in this order; lands first, alone, so every later cell can read it.

**XD1 — the third leg's slack, source verdict only.** Question: may the
participant ack a decide after the COMMIT **append**, letting its
durability ride the next drain? The decision is already durable in the
coordinator's stream before the decide is sent (step 4), and §2c's
fourth-outcome recovery resolves a participant against that stream —
read `prepared_resolver.cpp`, `mount_recovery.cpp`, `wal.md` §11-3 and
`cross-owner-txn.md` §2c and answer whether an
appended-but-undurable participant COMMIT at crash re-resolves to the
same outcome by redo-or-resolution, or opens a window
(`participant.decide_applied_preack`'s partly-published state is the
site to reason from). Deliverable: a written verdict with path:line; if
sound, a drafted spec amendment for ratification — **nothing built**.
If unsound, the chain's floor is three syncs and the order says so.

**XD2 — sync accounting (H-XD2, H-XD5 confirmation).** Group, cores=8,
peer listeners, b ∈ {1, 8}, plus the two no-pl controls. Read
`wal_syncs` per core before/after a fixed booking count; report
syncs-per-booking and which cores take them. Requires XD0.

**XD3 — the ablation (H-XD1).** WAL directory on tmpfs vs ext4, group,
cores=8, pl, b ∈ {1, 8}, plus one relaxed-pl tmpfs cell as the bridge
to the scenario-2 file's claim. Same driver, same scale (20,000
cargos), fresh file per cell. The ablation changes device latency only;
durability semantics and code paths are untouched — state that in the
results file so the tmpfs numbers are never quoted as engine numbers.

**XD4 — the queueing curve (H-XD4).** Group, cores=8, pl,
b ∈ {1, 2, 4, 8}; commit percentiles against core 0's sched and
shipped counters per cell. No new instrumentation — the counters
exist.

**XD5 — the strict pair (H-XD3).** D1, b=1: `c1` baseline and `c8-pl`.
Two cells, three repeats each, ratio reported with the group and
relaxed ratios beside it.

**XD6 — RO-reply applicability (H-XD5).** Source-read the booking's
statement list and each relation's owner; verdict in the results file.
If the booking ships no write (verdict against expectation), the
RO-participant reply returns to the lever list with this workload as
its case; either way the standing note in CLAUDE.md is amended to say
*which* workloads it prices.

**XD7 — docs closure, after the numbers.** If H-XD1 holds: correct the
scenario-2 file's "no device in it" sentence (amendment note, not a
silent edit), cross-note R6-B and this order's results from it, and
add the decomposition summary to `cross-owner-txn.md` §5 or §6 as
measured sizing. If H-XD1 falls: the file stands and this order's
results say what the increment actually is.

## Measurement

Build-release only; every number named `git describe --tags`; fresh
server and data file per cell; three repeats on every cell a claim
rests on, spreads before medians; noise floor stated per host before
any ratio; baseline is our own prior numbers (the scenario-2 file's
cells and R6-B's), never PostgreSQL. Rule 4b: the b and cores extremes
reachable on the host are in the matrix, and the host's own ceiling
(8 logical / 4 physical) is stated where it binds, per DA-c's
precedent. Results to
`bench/v2.7.0/results-xd-commit-decomposition-<describe>.md`, archive
beside it. Claims tagged measured (invocation) or source-read
(path:line at commit) throughout.

## Improvement

What this order buys: the ~3 ms cross-owner increment stops being one
number and becomes named parts — sync chain, batching efficiency,
queueing, protocol residue — under the durability classes that ship;
the third leg's slack is answered rather than suspected; the RO-reply
lever is priced against a workload that may not use it; and the next
performance decision (drain-sharing, ack timing, or routing/affinity)
is made against parts, not a total. What it does not buy: no
transaction gets faster by this order alone — it is the measurement
that says which order should exist next.
