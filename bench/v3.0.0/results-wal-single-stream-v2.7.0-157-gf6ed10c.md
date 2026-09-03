# The single WAL stream's own cost — AL-S8's M0-specific cells

AR0 M0 replaced one WAL stream per core with one stream for the instance:
core 0 opens it and owns it, every peer attaches and appends through
core 0's latch, and asks core 0's writer thread for a sync instead of
issuing one (`docs/spec/wal.md` §3). That latch and that hand-off are the
cost the revision accepted deliberately, and this document prices them —
the M0-specific quarter of the AL-S8 baseline; the scenario matrix is
`results-scenario0-stockmarket-v2.7.0-157-gf6ed10c.md` and
`results-scenario2-freight-v2.7.0-157-gf6ed10c.md`.

**Fresh series (AR0 D15, AL-R8) — no delta against any `v2.x` number.**
There is no prior measurement of a single-stream engine's peer-attach
cost to compare against; this run is that baseline.

## 1. Stamp

| Field | Value |
|---|---|
| Date/time | 2026-09-03, 01:39–01:47 UTC (per-cell times in §2) |
| Worktree | `v3.0.0-arch-revision` (branch `worktree-v3.0.0-arch-revision`) |
| Commit measured | `f6ed10c`, `git describe --tags` = `v2.7.0-157-gf6ed10c` |
| Tree cleanliness | Clean at every cell; sibling commit `f027a3c3` landed on this branch at 01:46:01 UTC — after every cell below except the noise-floor repeat in `results-scenario0-...md` §8, which is unaffected (`tools/`/`bench/` untouched). §6 below notes one place this run's own raw data independently reproduces the exact defect that commit fixes. |
| Binary provenance | `/home/cdkbs/bench-runs/al-s8-f6ed10c/kds_server`, `sha256 2ab1960bc056e7cc5c59be4946a2cf1250b4e65b941934c96ebf736e80435af3`, source mtime `2026-09-03 01:17:56.28 UTC`. |
| Device | `/home/cdkbs`, `ext4`, `/dev/root`. |
| Build type | `build-release`; not rebuilt. |
| Host | 8 logical CPUs, AMD EPYC 9V74, 1 socket × 4 cores × 2 threads/core (SMT). |
| Server config (common) | `cores = 8`, `peer_listeners = on` in every cell of this document (needed to read a peer's own `SHOW META` and to let a client land on a chosen core). `placement = rotate` — a deliberate departure from the scenario matrix's `namespace` default, explained in §2. |

## 2. Why `placement = rotate` here, and not the matrix's default

Both scenario drivers issue no `CREATE NAMESPACE`, so under `placement =
namespace` (the shipped default, and what the scenario matrix documents
use) every relation is owned by core 0 (`AssignOwnerCore`,
`docs/spec/namespace.md` NS10 clause 1 — confirmed by `DESCRIBE` in the
scenario0 document, §2). Under that placement, **no peer core ever owns a
relation, so no peer ever appends to the WAL or asks core 0's writer for
anything** — the three cells this document exists to measure would read
`wal_syncs=0` everywhere for the uninteresting reason that nothing ever
tried. `placement = rotate` spreads a relation over the non-system cores
in creation order (`kSystemCore + 1 + relation_seq % (core_count - 1)`,
`include/kds/catalog/core_placement.hpp`), which is the mechanism that
actually produces a peer-owned, peer-appending relation without touching
either driver. Confirmed for this run's own tables (`DESCRIBE
{users,assets,accounts,trades,user_periodic_profit}_brg` on the
`b-rotate-group` server): `users→core 5, assets→core 6, accounts→core 7,
trades→core 1, user_periodic_profit→core 2` — five distinct peer cores,
none of them core 0.

## 3. What was run

Two `scenario0_stockmarket.py` runs supply the concurrent write load §4
and §5 read `SHOW META` against (fresh server, fresh data file each,
`--verify 0` since correctness is already covered by the scenario0/2
documents and this stage is instrument-only):

| Cell | Port | durability | Flags | Precheck (UTC) | loadavg (1/5/15) | Build check |
|---|---|---|---|---|---|---|
| `b-rotate-group` | 15580 | group | `--users 300 --accounts-per-user 3 --assets 30 --traders 16 --txn-per-user 40 --seed 1` (target 11,960 txns) | 01:39:57 | 0.30/0.52/1.01 | none |
| `b-rotate-strict` | 15581 | strict | same, `--txn-per-user 15` (target 4,395 txns, sized down so strict's per-commit cost still finishes in this stage's time budget) | 01:43:39 | 0.32/0.52/0.91 | none |

`--traders 16` on 8 reactors (over `--traders 8` used in the scenario
matrix) so more than one client can be in flight per core, which is what
makes group batching and ring pressure possible to observe at all — a
single committer per core cannot batch with itself. A third server
(`b-commit-tail-group`, port 15582, `placement = namespace`, §4) runs a
purpose-built micro-probe rather than a scenario driver, for the reason
given there.

Section 4's `wal_ring_full`/`wal_ring_full_refusals` and §5's per-core
scheduling accounting are both read from `b-rotate-group`'s and
`b-rotate-strict`'s own `SHOW META`, one connection pinned per core by
opening sessions until `SO_REUSEPORT` hands one to each of the 8
listeners (the same technique `tools/multicore_benchmark.py`'s
`collect_connections` uses, reimplemented as a standalone read-only probe
— `tools/`'s own drivers were not touched, per this stage's constraint).

## 4. A peer's commit tail under `group` — the p99 claim, re-measured

`src/wal/manager.cpp`'s `Sync()` comment: on a 2-core host, handing a
waited-on sync to another thread "doubled `group`'s p99 while barely
moving its median" for the caller that had to wait. Under one stream,
every core but the one running the drain now pays that hand-off; this is
whether the tail moved on this host, at this core count.

**Method.** A dedicated server (`b-commit-tail-group`, `placement =
namespace`, `durability = group`) with two tables: `probe0` (undeclared
namespace → core-0-owned, `AssignOwnerCore`'s creating-core answer) and
`probe_ns.probe1` (a declared namespace's first relation → rotates onto a
peer, NS10 clause 3). One session pinned to each table's own owning core
(so neither statement is ever shipped — each executes locally, through
that core's own WAL manager, owning on core 0 and attached on the peer),
with 4 background filler connections per side continuously inserting so
group durability has concurrent committers to batch at all. 2,000 timed
`INSERT`s per side, interleaved with the fillers' continuous load.

| Session | core | ops | p0 | p25 | p50 | p95 | p99 | max (µs) |
|---|---|---|---|---|---|---|---|---|
| core-0 owner (`probe0`) | 0 | 2,000 | 74.1 | 314.5 | 455.9 | 1,019.1 | 1,358.3 | 3,957.8 |
| peer owner (`probe_ns.probe1`) | 1 | 2,000 | 72.8 | 339.2 | 480.4 | 1,039.8 | 1,379.3 | 2,536.4 |

Repeated once (fresh tables on the same server, a different peer core by
the kernel's own draw) as this stage's noise-floor control:

| Session | core | p50 | p99 | max (µs) |
|---|---|---|---|---|
| core-0 owner (repeat) | 0 | 483.6 | 1,458.7 | 173,555.3 (one outlier; see below) |
| peer owner (repeat) | 2 | 460.1 | 1,407.1 | 2,219.6 |

**The tail does not move.** Across both runs, the peer's p50 and p99 sit
within 1–5% of core 0's own — well inside the ~5–8% the repeat itself
shows as this measurement's own run-to-run floor. The peer's `max` is
*lower* than core 0's in both runs (core 0's first run carries a
3,958 µs outlier and its repeat a 173,555 µs one — a single stall,
plausibly the 5 s checkpoint interval firing mid-run; neither recurs on
the peer side in either run). **On this 8-core host, under `group`, a
peer's commit-latency distribution is statistically indistinguishable
from core 0's own** at both the median and the tail. The 2-core-host
result the `Sync()` comment records is not reproduced here — read
alongside §5, the reason is visible: `group`'s batching means the actual
wait a committing task pays is the time until the *next scheduled drain*,
not a per-caller fsync, and on this host with real concurrent fillers on
both sides that wait is dominated by the same drain cadence regardless of
which core is asking for it. The comment's own host was 2 cores; this one
is 8, and whatever made the hand-off visible there (less concurrency to
batch with, most likely, since a 2-core host has at most one peer
generating traffic instead of seven) is not present here.

## 5. `fdatasync` share of reactor wall time, per core

Instrument: `sched_wall_us − Σ sched_*_polled_us − sched_idle_block_us`
(`docs/spec/sched.md` §4) is the reactor time charged to no scheduling
group and was not sleep. Read as a share of **busy** time
(`wall − idle_block`) rather than of total wall clock: every server in
this document sat mostly idle around a short burst of driver activity
(§3), and dividing by total wall time would mostly measure how long the
probe waited between connecting to each core, not what the busy core
actually spent its time on.

### `group` (`b-rotate-group`)

| core | relation owned | wal_syncs | group_commits | mean batch | busy (ms) | gap (ms) | gap/busy |
|---|---|---|---|---|---|---|---|
| 0 | (none — system core) | 7,375 | 5 | 1.000 | 833 | 206 | **24.7%** |
| 1 | trades (heavy, INSERT) | 0 | 23,938 | 177.3 | 7,871 | 5,056 | **64.2%** |
| 2 | user_periodic_profit | 0 | 298 | 1.000 | 688 | 622 | 90.4% |
| 3 | (none this run) | 0 | 0 | — | 641 | 251 | 39.1% |
| 4 | (none this run) | 0 | 0 | — | 430 | 174 | 40.4% |
| 5 | users | 0 | 299 | 1.000 | 641 | 450 | 70.3% |
| 6 | assets | 0 | 21 | 1.000 | 658 | 273 | 41.4% |
| 7 | accounts (heavy, UPDATE) | 0 | 24,845 | 23.8 | 8,928 | 6,359 | **71.2%** |

### `strict` (`b-rotate-strict`)

| core | relation owned | wal_syncs | busy (ms) | gap (ms) | gap/busy |
|---|---|---|---|---|---|
| 0 | (none — system core) | 18,231 | 364 | 66 | 18.2% |
| 1 | trades (heavy) | 0 | 19,885 | 120 | **0.6%** |
| 7 | accounts (heavy) | 0 | 21,199 | 129 | **0.6%** |
| 2, 3, 4, 5, 6 (light/idle) | — | 0 | 169–1,122 | 102–149 | 11.5–60.4% |

At least two peers, as asked: core 1 and core 7 are the two heavily
loaded ones in both cells (their `group_commits`/busy time make them the
statistically meaningful samples; the lightly-loaded cores' ratios above
are dominated by small-sample noise from a few hundred microseconds of
activity each and are shown for completeness, not as findings).

**The `fdatasync`-equivalent cost is not in the gap — it is inside
`polled_us`, on the peer that is waiting for it.** Core 1 and core 7's
gap share **collapses from ~65–71% under `group` to ~0.6% under
`strict`**, while `sched_foreground_polled_us` divided by the actual
number of commits each core processed (`trade-insert`/`account-update`
phase `ops` from the driver's own JSON — 23,940 under `group`, 8,802
under `strict`, both cores) tells the opposite story:

| core | relation | group: polled µs / commit | strict: polled µs / commit | ratio (strict ÷ group) |
|---|---|---|---|---|
| 1 | trades | 117.6 | 2,241.8 | **19.1×** |
| 7 | accounts | 103.4 | 2,389.7 | **23.1×** |

A peer's `RequestSyncNow`/`EnsureDurable` call is itself inside a poll,
so a task's blocking wait on core 0's writer condition variable is timed
as part of whatever poll called it — under `strict`, that wait costs a
peer roughly **20–23× more polled time per commit** than the same peer
pays under `group`, where many committers' waits resolve together on one
drain. This is the mechanism `docs/spec/wal.md` §3 describes, made
visible in the scheduler's own accounting: a peer's wait for the writer
is a real, synchronous block inside its own foreground task, which the
scheduler faithfully charges to `foreground` rather than losing it as an
unattributed gap — group batching's whole value is cutting how often any
one committer has to pay that block at all, which is exactly what §4's
"the tail does not move" result also shows from the client's side. Core
0's own gap (18–25% of a much smaller busy time, since `rotate` gives it
no user relation to commit for) is a different, smaller quantity:
whatever residual scheduling overhead (system-group checkpoint ticks,
timer callbacks) core 0 pays regardless of WAL activity, not the sync
path this section was built to find.

## 6. `wal_ring_full` at eight writers

`wal_ring_full` (a stall the appender paid a flush for and got through)
and `wal_ring_full_refusals` (an append that exhausted
`kRingDrainAttempts` and was refused `OutOfSpace`) — both new counters at
this commit, surfacing `WalStats::ring_full_drains`/`ring_full_refusals`
that existed uncounted before.

| Cell | core 0 | core 1 | core 2 | core 3 | core 4 | core 5 | core 6 | core 7 | sum |
|---|---|---|---|---|---|---|---|---|---|
| `b-rotate-group` — `wal_ring_full` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | **0** |
| `b-rotate-group` — `wal_ring_full_refusals` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | **0** |
| `b-rotate-strict` — `wal_ring_full` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | **0** |
| `b-rotate-strict` — `wal_ring_full_refusals` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | **0** |

**Neither counter fired anywhere, in either durability class, at up to
16 concurrent writers over 8 appending cores.** `wal_ring_full_refusals`
being 0 everywhere is the required outcome (a nonzero reading would be
the most important thing in this document); `wal_ring_full` also being 0
means this run never even found the *softer* stall — the 1 MiB shared
ring (`kDefaultRingCapacity`, `include/kds/wal/stream.hpp`) was never
close to full even with `trades` and `accounts` each committing on the
order of 24,000 times across the run. **This cell characterizes nothing
about the ring's limit; it only shows this workload's write rate did not
approach it.** The instructions for this stage are explicit that a cell
which never approached a limit should say so rather than report it as a
finding: sizing the ring against a rate that *does* pressure it — a much
larger `--traders` count, smaller rows, or a driver that removes the
per-statement round trip this Python client's own socket cost bounds
every number in this document by — is future work, not something this
run's numbers can be stretched to claim.

## 7. Verification

`--verify 0` in both `b-rotate-*` cells (correctness is scenario0's/
scenario2's document's job, not this instrument's); `torn = 2` of 11,968
committed in `b-rotate-group` and `torn = 1` of 4,400 in
`b-rotate-strict`, both from the documented, retryable `row-id lease ...
is spent; retry after the refill grant lands` condition racing an
autocommit statement mid-transaction at `--traders 16` on 8 cores — an
expected shape of running without `--txn` at this concurrency
(`tools/scenario0_stockmarket.py`'s own docstring: a failure between
statements with no transaction open leaves a torn trade, and this driver
counts and reports it rather than hiding it), not a WAL correctness
finding. The `peer_commit_tail` micro-probe's own `INSERT`s carry no
verification beyond "the reply was not `ERR`", which held for all 8,000
statements across both timed runs.

## 8. A provenance note from this run's own data

Every peer's `SHOW META` in every cell of this document printed
`version=0 create_time=0 last_mount_time=0` — the epoch, not this
volume's actual values (core 0 answers correctly: `version=16
create_time=1788399391 last_mount_time=1788399391` in the same capture).
This is the exact defect `f027a3c3` (§1) fixes, landed on this branch
*after* every cell in this document ran: a peer's `CoreRuntime` was
missing the volume's decoded superblock image. It does not touch the
`wal_*`/`sched_*` fields §4–§6 read — those come from `WalStats` and
`SchedulerView`, a different code path, populated correctly throughout —
so it does not change any finding above; it is noted because a results
file that quietly worked around an oddity in its own raw data would be
less trustworthy than one that names it.

## 9. What this stage teaches about the engine

**The latch AR0 M0 accepted deliberately is not where the cost shows up
on this host, at this concurrency.** §4 found no tail movement on a
peer's commit under `group`; §5 found the actual synchronization cost
fully attributed to whichever core's foreground task is waiting on it
(not lost as unaccounted reactor time), and collapsing by roughly 20–23×
per commit when group batching has concurrent committers to amortize
over; §6 found the shared ring nowhere near its limit at up to 16
writers. Read
together, the three say the single-stream design's `AL-2` argument —
that per-core WAL streams were charging `fdatasync` time to no
scheduling group, and a shared stream with one off-reactor sync point
would remove that — holds on its own terms here: the cost that used to be
invisible is now visible, attributed, and (under `group`) amortized, and
nothing in this run found a new cost the single stream introduced that
the per-core design did not already have. The one open question this
stage does not answer is what happens at a write rate that actually
pressures the shared ring (§6) or a host with less SMT-thread contention
per physical core (§1's stamp) — both are where this design's real limit
would show up next, not in the cells this baseline measured.

Raw driver JSON, server logs, `SHOW META` dumps and the standalone probe
scripts (`probe_meta.py`, `probe_peer_commit_tail.py` — read-only clients
built on the unmodified `tools/ckdbs_cli.ServerConnection`, not `tools/`
drivers themselves) are archived at
`bench/v3.0.0/archive/wal-single-stream-v2.7.0-157-gf6ed10c/`; the
`b-rotate-*` scenario0 runs' own JSON/logs are additionally under
`bench/v3.0.0/archive/scenario0-v2.7.0-157-gf6ed10c/` since they are
literally `scenario0_stockmarket.py` runs.
