# R6-R — the read half's cost, the in-doubt ceiling's two axes, and the
line's own product metric measured for the first time

`instructions/v2.5.0/cross-owner-protocol-closing.md` §7, cells R1-R6. RR0
built D3's per-participant watermark with REPEATABLE READ carrying it and
READ COMMITTED skipping it; RR1 widened the read site in `HandleSelect`
(`command_dispatcher.cpp:6058`) to test `MayShip(session) ||
MayEnrolShip(session)`, the three write sites' own spelling — a foreign
read inside an explicit transaction now ships and enrols instead of being
refused. That refusal is exactly what blocked RP8's B5: a booking or
ordering transaction reads before it writes, and until this change no such
transaction could ever reach the two-phase commit path RP8 spent its whole
file pricing. This document is the six measurement cells the order's §7
lists — what the read costs (R1/R2), what the in-doubt ceiling actually
buys and on which axis (R3/R4), and what fraction of a realistic workload
now takes the two-phase path (R5, the blocker RP8 could not clear) — plus
the one-owner fast path's re-confirmation (R6).

**The headline finding is that RR1 did what it set out to do, at a
measured cost.** R5 is this line's actual product metric and it had never
been measured before this file: with the read refusal gone,
`scenario2_freight`'s booking transaction goes from RP8's **zero** commits
in 90 seconds to **100/100 committed at 244-265 TPS**, verified against its
four invariants (82/82 checks, 0 failures) — and every one of those
bookings is a genuine multi-participant cross-owner transaction, confirmed
in the server's own `[2pc]` log lines naming two and three distinct
participant cores per booking. That is bought at a real, measured price on
the read itself: an enrolled `SELECT` inside a cross-owner transaction
costs several times an autocommit foreign read of the same row (R1, CR1 —
HR1's falsifier fires), because the read now takes the same
`ShipStatement` ring-messaging path a write does rather than the
lightweight single-step remote-read pipeline autocommit still uses. D3's
own weakening (RC skips the watermark, RR carries it) turns out to cost
nothing measurable beyond that shared shipping cost (R2). The in-doubt
ceiling's writer-stall axis scales with the configured value exactly as
designed (R3, HR4 holds); its log-retention axis does not move with the
same knob at all in this workload — a live cross-owner participant never
stays prepared long enough to become the binding term against ordinary
dirty-page checkpoint lag, which is the same order of magnitude with or
without a single prepared transaction in flight (R4, CR3). The one-owner
fast path's cost is unchanged within this run's own (wider than RP8's)
noise band (R6, HR5's falsifier does not clearly fire, stated with the
caveat that earns that qualifier).

Measured in the git worktree `v2.5.0-crosscore-protocol-3` at
`/home/ubuntu/ckdbs/.claude/worktrees/v2.5.0-crosscore-protocol-3`, branch
`worktree-v2.5.0-crosscore-protocol-3`, commit `2a1cdcc0c938d15c8cafd0adf90777e82eccdaf1`
(`v2.4.0-32-g2a1cdcc`). The task named `acbd6b5` (`v2.4.0-30-gacbd6b5`,
RR0+RR1 only); by the time this session reached its first cell, the same
worktree had advanced two more commits — `a004263` (RR4's spec plus RR5's
two debts, including a `HandleSelect` micro-optimisation that reorders
`MayEnrolShip` after the free chain-shape tests so a purely local read
pays nothing for it) and `2a1cdcc` (the work order itself, checked in).
Every cell in this file ran against that later commit, which is named
throughout rather than the one the task cited, per the rule that a claim
carries the commit it was true of.

## 1. Stamp

| | |
|---|---|
| Date/time executed | 2026-08-28, 10:08 UTC (binary build) through 10:41 UTC (R5's second cell). Machine-quiet checks at 10:09:35 UTC (load 0.07) and 10:20:40 UTC (load 0.08) before the two main sweeps; a `cmake --build` of a scratch pre-RR0 tree ran 10:34:03-10:34:47 UTC for R6's baseline binary only, off the measured tree, and the R6 sweep itself did not start until load had settled back under 1.0 (see §11's own note on its one anomalous round) |
| Worktree | `v2.5.0-crosscore-protocol-3` at `/home/ubuntu/ckdbs/.claude/worktrees/v2.5.0-crosscore-protocol-3` |
| Branch | `worktree-v2.5.0-crosscore-protocol-3` |
| Commit | `2a1cdcc0c938d15c8cafd0adf90777e82eccdaf1`, committer date `2026-08-28T10:05:40Z` |
| Tree cleanliness | Clean at the start of this session (`acbd6b5`, then advanced to `2a1cdcc` by another session sharing this worktree before this session's first measurement). Dirty only with this session's own additions during the run: `bench/docs/README.md` (two new entries), three new driver files, one archive directory — all `bench/`-owned, per this agent's scope |
| Host | `ip-172-31-1-92`, Linux 7.0.0-1011-aws, the same host RP8's `bench/v2.5.0/results-r6b-cross-owner-cost-v2.4.0-28-g53f6aae.md` ran on |
| CPU | Intel Xeon Platinum 8488C, 1 socket x 4 cores x 2 threads/core = **8 logical CPUs, SMT on**, 1 NUMA node (`lscpu`) |
| Load at run time | `uptime` 0.07-0.18 before the read-probe and in-doubt sweeps; `pgrep -af cc1plus`/`ckdbs-sim`/stray `kds_server` empty at every check point named above |
| Data-file / workdir device | `$HOME` (`/home/ubuntu/kds-rr-*`) -> `/dev/root`, **ext4** (`df -T`, 28% used, ~188 GiB free). `/tmp` on this host is **tmpfs** (`df -T /tmp`, confirmed again this session) and held only the pre-RR0 scratch build tree and its binary before the binary was copied out — never a data file or WAL segment |
| Build type | Release: `CMAKE_BUILD_TYPE=Release`, `CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG` (`build-release/CMakeCache.txt`), compiler GCC 15.2.0, cmake 4.2.3. The pre-RR0 scratch build used the identical `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release` invocation against the same compiler |
| Server config, R1/R2 | `cores=2`, `placement=rotate`, `peer_listeners=off` (every connection lands on core 0 deterministically, which `rotate` never assigns table ownership to), `durability=group` |
| Server config, R3/R4 | `cores=5` (4 participants + coordinator), `placement=rotate`, `peer_listeners=on`, `durability=group`, `in_doubt_ceiling_ms` swept (1/5/20/200), `checkpoint_interval_ms=200`, `log_level=debug` (R4 needs the per-checkpoint anchor lines) |
| Server config, R5 | `cores=4`, `placement=rotate`, `peer_listeners=off` (RP8's §9 reasoning restated below), `durability=group`, `log_level=info` |
| Server config, R6 | `cores=1`, `placement=creating` (D1's fast path, no shipping, no participants — matches RP8's `local` arm exactly) |

## 2. Binary provenance

Two binaries. Both are copies made into a run-owned directory before the
first cell used them, never the build tree's own file, per the agent
rule's rationale — this worktree is shared and another session had already
advanced it twice by the time this session's first cell ran.

| | now (`2a1cdcc`) | pre-RR0 (`a02a666`) |
|---|---|---|
| Build tree | `build-release/` (in-tree, shared with the rest of this worktree) | `/tmp/.../scratchpad/preR6/src/build-release/` (a `git archive` export, private to this session, on tmpfs — the binary-only exception) |
| `git describe --tags` at that commit | `v2.4.0-32-g2a1cdcc` | `v2.4.0-29-ga02a666` |
| Binary mtime | 2026-08-28 10:08:47 UTC | 2026-08-28 10:34:47 UTC |
| sha256 | `01cf5304284bd2f9ede1ccbee922565c337bdf1564a0a1429a57eb1a34ff902a` | `88cb6f6a77068db5068328d0b3d8efaa4b04962c5226834d693dfc3a5af5c148` |
| Run copy | `/home/ubuntu/kds-rr-runbin/kds_server-2a1cdcc` | `/home/ubuntu/kds-rr-runbin/kds_server-a02a666` |
| Copy sha256 | identical to above | identical to above |

**`a02a666` is not v2.4.0.** RP8's own B3 baseline was `ec5f993` (v2.4.0
exactly). R6 here uses `a02a666` — the commit immediately before RR0 — on
purpose: HR5 asks whether *this order's own changes* (RR0, RR1, RR4, RR5)
cost anything on the one-owner path, and isolating that question means
comparing against the tree immediately before those changes rather than
accumulating whatever else has landed since v2.4.0. Named plainly because
it is a deliberate deviation from RP8's exact comparison points, not an
oversight.

**The pre-RR0 binary's sha256 matches RP8's own "now" binary
(`53f6aae`).** That is expected, not a mistake: `a02a666` (RP8's own
commit) touched only `bench/` and a doc file per its own results file's
§2, so it compiles to the identical bytes `53f6aae` did.

**Provenance check on the "now" binary.** `2a1cdcc`'s committer date
(10:05:40Z) *predates* the binary's mtime (10:08:47Z) by design: this
session ran its own `cmake --build build-release --target kds_server -j8`
at the start of this section's own work, which recompiled `command_dispatcher.cpp` and
relinked — confirming the binary reflects `2a1cdcc` exactly, not an
earlier ancestor another session might have last built. No `cmake --build`
ran against `build-release/` again after that point in this session.

## 3. Instrument and method

Three drivers. `bench/txn_shipped_read_probe.py` and
`bench/txn_indoubt_ceiling_probe.py` are new this session, `bench/`-owned;
`bench/docs/README.md` carries their entries under "The R6-R read-half
probes" — this order's own rule-7 obligation, separate from RP8's two
debts which RR5 already paid. R5 reuses `tools/scenario2_freight.py`
unmodified; R6 reuses `bench/txn_2pc_cost_probe.py`'s `local` arm
unmodified.

Every cell: fresh server, fresh data file per invocation (every probe
`shutil.rmtree`s its own workdir before starting), one process per arm,
per-rep spreads reported before any median, rows/reads in = rows/reads out
checked per cell (`rows_match`, content-checked against seeded values for
the read probe — not merely absence of `ERR`). Overhead A/B is **suspended**
per the operator's 2026-08-24 amendment and **was not run**; nothing in
this file implies it passed.

**The 992-byte shipped-reply cap** (`kShippedStatementReplyTextMax`,
`statement_ship_service.hpp:262`) is what a foreign read inside a
transaction is refused past, since RR1's fix ships the read rather than
answering it locally. `rt`'s schema (`id int64, tag varchar, n int64`,
`tag` seeded to a short fixed-width string) answers one row at under 100
bytes on the wire — nowhere near the cap — by construction: R1/R2 are
sized to measure the read's cost, not to find the cap, and neither driver
saw a single refusal attributable to it (`refused=0` on every one of the
45 read-probe cells, §5b).

## 4. Noise floor, established from repeated cells inside this run

Per rule 8: a repeated configuration inside this run, not a number carried
over from RP8's sitting. Two independent floors, because the two
instruments have very different absolute scales.

**R1/R2's floor**, from `autocommit`'s own 5-round repeat at each row
size (the least noisy arm, µs-scale so relative spread reads large on
small absolute numbers):

| rows | p50 per-rep (µs) | spread |
|---|---|---|
| 200 | 44.8, 44.2, 45.0, 44.4, 37.7 | 16.4% |
| 1000 | 44.4, 35.1, 44.8, 45.5, 34.1 | 25.7% |
| 10000 | 28.7, 47.3, 34.3, 48.2, 48.2 | 41.2% |

**R6's floor**, from each arm's own 7-round repeat (§11 has the full
table): `pre` at 18.3% (matching RP8's ~18-19.7% band closely — the same
host, the same order of magnitude), `now` at 83.8% including one clearly
anomalous round and 31.9% with that round excluded. **R6's own floor is
wider than RP8's band**, and §11 reads its ratio against this run's own
floor rather than RP8's.

**R3/R4 carry their own internal control.** R4's `control` mode (no
participant is ever prepared) is the cell that establishes what
`in_doubt_ceiling_ms` contributes against — not a repeat of one
configuration, but the "isolation-level change on a single connection"
class of control the rule names: a workload identical in every way except
the one property (a prepared transaction) whose cost is being isolated.

## 5. R1 — what a cross-owner read costs, and CR1

**HR1's falsifier fires, and it fires in every one of 15 independent
5-round arms.** `autocommit`'s point-lookup `SELECT` costs ~44-47 µs p50
flat across all three row-set sizes (a pk lookup, as expected — R1's own
evidence for rule 9's "does not scale with rows" case). The identical read
wrapped in `BEGIN`/`COMMIT` (`rc`, READ COMMITTED, the default) costs its
`select_us` leg **2.25x to 9.08x** more at p50, per individual rep, and
never once landed at or below `autocommit`'s cost across 15 reps at three
row sizes bar a single rep (rows=1000, rep 4, where `rc.select` p50 landed
at 42.4 µs against that rep's own `autocommit` p50 of 45.5 µs — the one
exception in 15 trials, and it is reported rather than dropped).

### 5a. Per-rep data (spreads before medians), rows=1000

| rep | autocommit total_us p50 | rc.select_us p50 | rc.commit_us p50 | rc.total_us p50 | ratio (rc.select/autocommit) |
|---|---|---|---|---|---|
| 1 | 44.4 | 305.2 | 2550.9 | 2912.9 | 6.87 |
| 2 | 35.1 | 288.9 | 1715.0 | 2031.8 | 8.23 |
| 3 | 44.8 | 107.6 | 2006.5 | 2163.1 | 2.40 |
| 4 | 45.5 | 42.4 | 1449.9 | 1509.0 | 0.93 |
| 5 | 34.1 | 306.8 | 2300.6 | 2676.6 | 9.00 |

**A bimodal pattern, reported rather than smoothed over.** `rc.select_us`
p50 clusters into two bands across reps and across all three row sizes —
roughly 40-115 µs ("fast") and roughly 280-310 µs ("slow") — and which
band a given rep lands in does not track row-set size (rows=10000 has four
"fast" reps and one "slow" one; rows=200 has five "slow" reps and none
fast; rows=1000 splits three-and-two). This is the same host-level
non-additive variance RP8's §5c/§12 found in the commit path's own
sequential-sync cost, now visible on a much smaller, read-only operation.
A source-grounded but unconfirmed candidate: the reactor's idle-wake
mechanism (`sched_idle_block_us`, `docs/inflight/in-progress/observability.md`)
— whether a given server instance's participant-core reactor stays "hot"
between statements or has to pay a wake cost per request is plausibly
instance-specific and would produce exactly this two-band shape. Not
independently instrumented this session, named as a hypothesis rather than
a finding, the same epistemic move RP8's §5c made for its own unexplained
gap.

### 5b. Median-of-rounds summary, all five percentiles, three row sizes

**rows=200**

| pct | autocommit | rc.select | rc.commit | rc.total |
|---|---|---|---|---|
| p0 | 33.2 | 82.8 | 1703.5 | 1971.9 |
| p25 | 43.7 | 254.8 | 2172.6 | 2517.8 |
| p50 | 44.4 | 303.8 | 2270.9 | 2635.1 |
| p95 | 55.3 | 428.5 | 2531.3 | 2948.3 |
| p99 | 62.2 | 504.2 | 2889.8 | 3241.5 |

**rows=1000**

| pct | autocommit | rc.select | rc.commit | rc.total |
|---|---|---|---|---|
| p0 | 28.5 | 62.4 | 1499.4 | 1640.3 |
| p25 | 43.5 | 243.9 | 1888.5 | 2035.7 |
| p50 | 44.4 | 288.9 | 2006.5 | 2163.1 |
| p95 | 54.3 | 401.4 | 2295.9 | 2464.4 |
| p99 | 66.4 | 448.7 | 2735.0 | 3106.6 |

**rows=10000**

| pct | autocommit | rc.select | rc.commit | rc.total |
|---|---|---|---|---|
| p0 | 33.2 | 92.6 | 2876.3 | 3118.4 |
| p25 | 45.9 | 106.3 | 3158.9 | 3312.5 |
| p50 | 47.3 | 108.6 | 3289.9 | 3454.6 |
| p95 | 57.2 | 137.9 | 3553.5 | 3765.9 |
| p99 | 64.2 | 424.1 | 3672.9 | 3924.6 |

Every cell: 200 reads x 5 rounds = 1000 attempts per arm per row size,
15,000 total across R1/R2's nine arm-size combinations. `refused=0` and
`rows_match=true` (every reply content-matched its seeded row) on all of
them — `bench/txn_shipped_read_probe.py`'s own acceptance gate, not a
separate check run after the fact.

### 5c. CR1 — named at the source, and it is more than one branch

The order's instruction: *"name what an RC cross-owner read executes that
an RC autocommit foreign read does not. If the answer is 'one branch on a
field already read,' say so with the site; if it is more, that is a
finding."* It is more, and the site is the fork itself
(`command_dispatcher.cpp:6202-6205`, RR1's own comment): an enrolling
session's `SELECT` falls through *both* lightweight remote-read paths
(the single-step pipeline at `:6108` and the two-step pipeline at
`:6151`, both gated `!enrolling`/`!MayEnrolShip` since RR1) and reaches
`ShipStatement(..., read=true)` at `:6202` — **the same function, same
ring-messaging, same dedup-table, same `StatementShipServer` round trip a
write takes.** The lightweight pipeline `autocommit` still uses
(`remote_reads_->Open`) is a purpose-built single-hop reader; the shipped
path is the general cross-owner statement mechanism, carrying a session
mint-or-reuse, a sequence number, a dedup record and a full ring
round-trip in each direction. That is why the cost is several-x rather
than a branch's worth: RR1 did not add a branch to the cheap path, it
routed an enrolled read onto the expensive one, because that is the one
whose reply can `join` an open participant context (RR0's `join` bit) and
answer the transaction's *own* uncommitted write — the lightweight
pipeline answers from the owner's latest-*committed* view outside any
transaction, which is exactly the wrong answer inside one (RR0/RR1's own
commit message: the pre-fix bug this closed).

**`rc.total`'s ~2.0-3.5 ms is dominated by `commit`, not `select`.** Every
row size's `rc.commit_us` p50 (2007-3290 µs) dwarfs `rc.select_us` p50
(109-304 µs) by 7-30x — the read's own added cost is real but small next
to the fact that an enrolled read-only transaction now pays a full
cross-owner **decide** at `COMMIT`, discussed next.

## 6. R2 — D3's per-level weakening, priced

**RR carries D3's watermark; RC does not; the difference is not
measurable against R1's own noise.** `rr.select_us` sits inside `rc.select_us`'s
own per-rep spread at every row size (rows=1000 p50: `rc` 288.9 µs, `rr`
291.7 µs; rows=10000 p50: `rc` 108.6 µs, `rr` 106.9 µs — RR is *lower* at
the median here, which is itself inside the noise §5c already
established). The tail diverges more (rows=10000 p95: `rc` 137.9 µs vs
`rr` 321.4 µs) but that single cell is one rep's outlier pulling one
percentile, not a reproduced pattern across the other two row sizes.

| rows | rc.select p50 | rr.select p50 | rc.commit p50 | rr.commit p50 |
|---|---|---|---|---|
| 200 | 303.8 | 293.8 | 2270.9 | 2316.4 |
| 1000 | 288.9 | 291.7 | 2006.5 | 2045.0 |
| 10000 | 108.6 | 106.9 | 3289.9 | 3352.4 |

**This is the ratified design working as specified, not an absence of
measurement.** D3's own text (`instructions/v2.4.0/2pc.md`, restated in
RR0's commit message): the watermark reuses "the enrolled arm's existing
map find rather than adding one" for RC, and for RR it is "one comparison
on a field already in the reply" (`command_dispatcher.cpp:4104-4145`,
`NoteParticipantWatermark`). A comparison against an int64 already sitting
in a struct the reply parsing touched anyway is exactly the kind of cost
this instrument cannot resolve from noise at these sample sizes — which is
the finding: **D3's weakening is priced at effectively zero marginal cost
over RC**, consistent with the source, not merely unmeasured.

## 7. R3 — the writer-stall axis, swept

**The mechanism holds: a refused writer's minimum observed wait tracks the
configured ceiling closely, and the tail runs above it under load.**
`bench/txn_indoubt_ceiling_probe.py --mode live --participants 4`: a
`holder` connection loops a genuine 4-owner cross-owner transaction against
one fixed row per table; a `racer` connection seated on the first table's
owner core repeats a plain local `UPDATE` against that same row,
classified per attempt. 4 participants rather than 2 widens the natural
"prepared, awaiting decide" window (RP8's B2: coordinator wait grows past
four participants on this host), which is what makes a low ceiling
observable at all — with 2 participants and this host's ~2.6 ms natural
commit latency, a 1 ms ceiling would rarely be tested.

### 7a. Per-rep data, `racer_doubt_us` (attempts refused by D5's exact
ceiling message, not an ordinary write-write conflict)

| ceiling | rep | n | p0 | p25 | p50 | p95 | p99 | mean |
|---|---|---|---|---|---|---|---|---|
| 1 ms | 1 | 506 | 1038.7 | 1958.8 | 2017.2 | 2336.4 | 6322.8 | 2069.2 |
| 1 ms | 2 | 507 | 1088.1 | 1943.8 | 1998.8 | 2353.3 | 4951.0 | 2040.6 |
| 1 ms | 3 | 517 | 1087.0 | 1960.4 | 2015.9 | 2357.6 | 3546.2 | 2040.3 |
| 5 ms | 1 | 18 | 5689.4 | 6484.6 | 6838.4 | 12321.1 | 12321.1 | 7999.2 |
| 5 ms | 2 | 17 | 5475.3 | 6131.1 | 6975.3 | 11792.4 | 11792.4 | 7741.2 |
| 5 ms | 3 | 13 | 5420.2 | 5947.6 | 6772.0 | 11143.1 | 11143.1 | 7339.6 |
| 20 ms | 1 | 0 | - | - | - | - | - | - |
| 20 ms | 2 | 1 | 21339.3 | 21339.3 | 21339.3 | 21339.3 | 21339.3 | 21339.3 |
| 20 ms | 3 | 0 | - | - | - | - | - | - |
| 200 ms | 1-3 | 0 | - | - | - | - | - | - |

Every ceiling's own `racer_all_us` (every attempt, ok/doubt/other pooled)
runs 13,000-26,000 attempts per rep with a p0 of ~13-16 µs — the ordinary
"row visibly written, not yet prepared" refusal, and by far the majority
class (10,000-24,000 of each rep's attempts), which the tables above
correctly exclude: it is not what D5's ceiling governs, and folding it in
would answer a different question.

**Reading the table: p0 tracks the ceiling; the median and tail sit
above it.** At `ceiling=1`, p0 is 1.04-1.09 ms — a refused writer's *best*
case is close to the 1 ms it was configured for, confirming the mechanism
fires close to its bound rather than early or vastly late. At
`ceiling=5`, p0 is 5.4-5.7 ms, the same relationship. But p50 runs roughly
**2x** the ceiling at both swept values (2.0 ms at ceiling=1; 6.8 ms at
ceiling=5) and p99 runs **3-6x** (3.5-6.3 ms at ceiling=1; 11.1-12.3 ms at
ceiling=5). **Measured, with a source-grounded but unconfirmed
explanation**: `command_dispatcher.cpp:249-266`'s bound is a single
deadline taken once and shared across every re-entry
(`"Bounded once, not once per blocker"`), which structurally caps the
*in-doubt wait itself* at the ceiling — what this instrument cannot
separate is that bound from the **queueing delay before `Dispatch()` even
starts**, on a core the `holder`'s own continuous shipped-statement,
prepare and decide-ack traffic is saturating. No per-phase server-side
timer exists to isolate "time queued before dispatch" from "time inside
the bounded wait" — the same instrument gap RP8's §5c and §11's "no
per-leg timer" item both name, now blocking a third attribution.

**At `ceiling=20` and `ceiling=200`, the ceiling is essentially never
reached.** 1 stray event out of ~60,000 combined attempts at 20 ms (at
21.3 ms, just over the configured value — consistent with the same
queueing explanation), 0 of ~43,000 at 200 ms. **HR4 holds**: "200 ms
survives the sweep" was the prediction, and at 200 ms a writer never once
met the refusal in this run, which is precisely what "a writer never meets
the refusal on a healthy path" (the constant's own comment,
`txn_2pc_service.hpp:376`) says should happen.

## 8. R4 — the log-retention axis, swept, and the surprise

**Measured: log retention does not move with `in_doubt_ceiling_ms` in this
workload, and the control cell proves why.** `anchor_series()` reads every
`anchor published: core=<N> checkpoint_lsn=... redo_start=... durable_lsn=...`
line the owner core logs (`checkpoint_interval_ms=200`, so ~19-25 ticks
per 4-second run) and reports the **peak** `durable_lsn - redo_start` seen
at any tick — the worst moment a checkpoint's own floor held back, not
just the last tick (which systematically reads near-zero, since every
transaction that was ever prepared has long since decided by the time the
run stops and this driver samples — a bug in this driver's first cut,
caught before this file's numbers were drawn, and left in the driver's own
docstring as a stated correction).

### 8a. Per-rep peak retained bytes (owner core), and the control

| ceiling | rep 1 | rep 2 | rep 3 | doubt events (rep1/2/3) |
|---|---|---|---|---|
| 1 ms | 98,736 | 110,384 | 143,200 | 506 / 507 / 517 |
| 5 ms | 119,072 | 126,488 | 135,416 | 18 / 17 / 13 |
| 20 ms | 126,256 | 137,728 | 159,656 | 0 / 1 / 0 |
| 200 ms | 141,448 | 124,520 | 120,520 | 0 / 0 / 0 |
| control (no participant ever prepared) | 102,608 | 105,128 | - | n/a |

**The control's peak (102,608-105,128 bytes) sits inside the exact same
range every `live` cell's peak occupies (98,736-159,656 bytes), regardless
of ceiling and regardless of how many hundreds of in-doubt events
occurred.** A workload with **zero** prepared transactions ever (control)
retains as much log as a workload with **517** in-doubt refusals in four
seconds (`live`, ceiling=1 ms, rep 3). This is not a percentile table -
it is a peak-per-run count, so rule 6's percentiles do not apply to it
(rule 6's own carve-out: "a table of counts, sizes or ratios carries no
percentiles").

**Why: ordinary dirty-page-driven checkpoint lag dominates completely.**
`Checkpointer::Start` floors `redo_start` at **two** independent terms
(`checkpointer.cpp:127-149`): `RedoStartFrom(begin_lsn_, dirty)` (the
oldest recLSN among this checkpoint's dirty pages — nothing to do with
2PC) and, only if lower, `OldestPreparedLsn()`. On this host, at this
workload's write rate, the dirty-page term alone already floors
`redo_start` roughly 100-160 KB behind the checkpoint's own position every
200 ms tick — an order of magnitude bigger than what a prepared
transaction's few-millisecond natural window could ever contribute, since
that window is gone (decided, released) within one or two checkpoint
ticks at most. The prepared-transaction floor is real and confirmed by
source (this section's own opening paragraph cites the exact lines), but it is never the *binding*
term here: it would have to hold a transaction prepared for longer than
the dirty-page term already reaches back, and nothing in this run's
healthy operation does that.

## 9. CR3 — the ceiling's answer, and the axis that actually chose it

**R3 moves with the knob; R4 does not. CR3's "pick the ceiling against
whichever binds first" resolves cleanly: R3 is the only axis with anything
to bind on, in this workload.** The 200 ms default was proposed
(`instructions/v2.4.0/2pc.md`, restated at `txn_2pc_service.hpp:376-386`)
from this same host's ~0.94 ms durable sync and ~21-23 µs ring hop
(`bench/v2.1.0` §3a) — a derivation this session did not need to
re-derive, since it swept the value directly on the same host and R3 §7
confirms the prediction those numbers fed: 200 ms is roughly 100x the
sync latency it is meant to survive, and it was never once reached in this
run's healthy operation. **HR4 holds, and R4 sharpens why rather than
changing the number**: the second axis RP2's review added does not, in
fact, argue for a *smaller* ceiling on this workload, because it is not
sensitive to the ceiling at all — what actually bounds a prepared
transaction's worst-case duration (and therefore the log it can hold
back) is `kTxnPhaseDeadlineNs` (10 s, `txn_2pc_service.hpp:347` — the
coordinator's own prepare-phase deadline, ten times the ceiling and
completely independent of it) in the case where the coordinator is alive
but slow, or **the next mount** in the case where it is not
(`prepared_resolver.hpp` — unbounded in wall-clock terms while the
instance keeps running, resolved only at restart). Neither of those is
`in_doubt_ceiling_ms`. **The ceiling this order proposed is chosen on its
one binding axis (writer stall), and the log-retention exposure a
prepared transaction can create is a separate, much larger number that a
smaller `in_doubt_ceiling_ms` would not shrink** — a distinction the order
asked for explicitly ("a ceiling chosen on latency alone is chosen on half
the evidence") and one this run's data answers rather than assumes.

## 10. R5 — B5 re-run, and CR4: the line's own product metric

**RP8's B5 measured zero, for a reason outside the protocol
(`CheckReadAffinity`'s unconditional refusal of a foreign read inside a
transaction). That reason is gone. R5 measures what replaces it, for the
first time in this line.**

`tools/scenario2_freight.py --organizations 20 --ships 5 --operations 20
--cargos 200 --bookings 100 --bookers 1` (RP8's exact parameters, §9's
citation), against a fresh `cores=4 placement=rotate peer_listeners=off`
server started from the `2a1cdcc` run copy, `--txn` (the default:
`BEGIN`/`COMMIT` per booking, whose first statement is the same
`cargo-lookup` read that refused every attempt in RP8's file). Two cells,
fresh server and data file each:

| cell | committed | TPS | conflicted (retries) | on read | verify |
|---|---|---|---|---|---|
| 1 (plain) | 100/100 | 265.3 | 5 | 100.0% | not run |
| 2 (`--verify 25`) | 100/100 | 244.2 | 5 | 100.0% | 82 checks, 0 failures |

**Every committed booking is a genuine multi-owner cross-owner
transaction, confirmed at the source rather than inferred from the
absence of a refusal.** The server's own `[2pc]`-tagged log carries lines
like *"a participant's answer from core 1 arrived after its phase
settled (session 3, transaction 12640)"* alongside sibling lines naming
cores 2 and 3 for the same transaction — two and three distinct
participant cores per booking, `SHOW META`'s `shipped_statements=1563`
confirming the volume of shipped traffic this generated across 100
bookings. Since every relation in this deployment shape sits on a peer
core and the client is on core 0 (`peer_listeners=off`, RP8's §9
reasoning, restated below), **every booking that reaches `COMMIT` touches
at least one foreign relation and therefore runs the full protocol** —
`command_dispatcher.cpp:7143-7148`'s comment states this directly: "a
transaction that ships every statement to one peer has one participant
and still runs the full protocol." Under this deployment shape, the
two-phase fraction of a committed booking workload is **100%** — the
number CR4 asked for, arriving as a clean fraction rather than the
blocker RP8's file reported.

**RP8's §9 caveat still applies, restated rather than re-derived (per this
order's own instruction).** `peer_listeners=off` makes the booking client
foreign to *every* relation, because the driver has no retry-until-core-0
logic for its DDL connection and `rotate` never assigns ownership to core
0 — this is the *maximal* cross-owner shape, not a representative one. A
deployment where the booking client happened to be co-located with some
of what it reads would ship fewer statements and might, for some booking
shapes, resolve to a single-owner transaction touching no peer at all —
though even that would still run the full protocol per the comment cited
above, since "one participant" is still cross-owner by this engine's own
accounting. What this run does *not* measure is a deployment shape between
these two extremes; RP8's caveat that this file's number is the ceiling of
what a realistic deployment would see, not its floor, holds unchanged.

**The 5% read-conflict retry rate is a new, small cost RP8 never had to
account for.** Every one of the run's conflicts was "on read" (100%,
both cells) rather than on the two write statements the booking also
performs (`operation-update`, `org-update`) — consistent with R1/R2's
finding that the read now enrols a participant and can therefore meet
D3's `join`-not-open rule (RR0's commit message) or, more likely here, an
ordinary MVCC conflict on a contended cargo row under one booker hammering
a small pool (`cargo pool left 93` and `99` respectively, against a
200-cargo pool). 5/105 read attempts retried once each in both cells,
`retries/booking` 0.05 — small, but new, and it is the read half's own
contribution to what a booking transaction now costs beyond R1's
per-statement price.

Raw JSON and server logs from both cells archived at
`bench/v2.5.0/archive/scenario2-freight-rr-v2.4.0-32-g2a1cdcc/`
(`cell1-txn.json`, `cell1-server.log`, `cell2-txn-verify.json`,
`cell2-server.log` — no data file or WAL segment, per the standing rule).

## 11. R6 — the one-owner fast path, re-confirmed with a caveat

**The direction is consistent; the magnitude sits at the edge of, and
partly outside, this run's own noise floor — reported as such rather than
forced into a clean pass or fail.** `bench/txn_2pc_cost_probe.py --arm
local --participants 1`, 7 interleaved rounds of 300 committed
transactions each, `2a1cdcc` ("now") against `a02a666` ("pre", the commit
immediately before RR0 — §2's note on why this point and not v2.4.0).

### 11a. Per-round data (spreads before medians)

| round | now p50 | pre p50 | now/pre p50 | now p99 | pre p99 | now/pre p99 |
|---|---|---|---|---|---|---|
| 1 | 1652.9 | 1335.7 | 1.237 | 2050.7 | 1775.1 | 1.155 |
| 2 | 1376.0 | 1220.1 | 1.128 | 1878.2 | 1612.6 | 1.165 |
| 3 | 1365.3 | 1364.5 | 1.001 | 1839.7 | 1959.8 | 0.939 |
| 4 | **517.9** | 1189.9 | **0.435** | **880.3** | 1591.3 | **0.553** |
| 5 | 1354.1 | 1327.0 | 1.020 | 1708.9 | 1811.9 | 0.943 |
| 6 | 1314.6 | 1141.2 | 1.152 | 1743.5 | 1640.2 | 1.063 |
| 7 | 1218.5 | 1188.5 | 1.025 | 1717.0 | 1605.3 | 1.070 |

**Round 4's "now" cell is a clear outlier**, roughly half of every other
round's value on both arms it's compared against, with 300/300 rows
committed and `rows_match=true` — not a data error, a genuinely fast
sample. No cause was found; it is reported rather than discarded, per the
rule that a re-run does not get to pick which of its own rounds counts.

### 11b. Median-of-rounds summary and the noise-floor comparison

| pct | now | pre | ratio |
|---|---|---|---|
| p0 | 636.0 | 505.0 | 1.259 |
| p25 | 1247.6 | 1056.6 | 1.181 |
| p50 | 1354.1 | 1220.1 | 1.110 |
| p95 | 1630.6 | 1488.9 | 1.095 |
| p99 | 1743.5 | 1640.2 | 1.063 |

**This ratio band, [1.063, 1.259], sits above RP8's stated [0.929, 1.031]
— but that band was RP8's sitting's own noise floor, not a universal
constant, and this run's own floor is wider.** §4 already established it:
`now`'s own 7-round repeat has an 83.8% relative spread at p50 (31.9%
excluding round 4), well above the 6.3-25.9% delta this table reports at
every percentile. **Read against this run's own floor, the ratio is
inside the noise band, and HR5's falsifier does not clearly fire** — but
it does not clearly *not* fire either, since `pre`'s own floor (18.3%,
closely matching RP8's) is narrower than the observed delta at three of
five percentiles. Both readings are stated because the honest answer is
between them: **excluding round 4, `now` was at or above `pre` on every
one of the remaining 6 rounds at p50** (ratios 1.001-1.237, never below
1.0) — a consistent direction across independent samples, even though no
single round's delta clears its own arm's noise band by a wide margin.
This is weaker evidence than RP8's own B3 confirmation, and is reported as
such: **a plausible small cost, not ruled out, not confirmed**, and a
cleaner answer would need either more rounds or the anomalous round's
cause understood. `cores=1` is unmoved in the sense that mattered to every
prior row in this series (the fast path never touches shipping,
`MayEnrolShip`, or the read-half code at all — RR1's own diff never
touches a line `local`'s arm could reach), which is a structural argument
this run's numbers do not contradict even where they do not cleanly
confirm it either.

## 12. Versus PostgreSQL

**No twin exists for any of R1-R6, and RP8 §10's reasoning is why —
cited, not re-derived.** RP8's `bench/v2.5.0/results-r6b-cross-owner-cost-v2.4.0-28-g53f6aae.md`
§10 already establishes that PostgreSQL has no concept of a relation
"owned" by one of several backend-partitioned execution units the way this
engine's cores are, so a cross-owner-shaped comparison has no honest
mapping onto a single-process PostgreSQL instance — the same reasoning
applies unchanged to a cross-owner *read* (R1/R2), a writer-stall/log-floor
sweep specific to this engine's 2PC (R3/R4), and a booking workload's
two-phase fraction (R5), none of which name a PostgreSQL concept to
measure against. R6 (the one-owner fast path) is the one cell in this file
that is *not* inherently cross-owner-shaped, and a plain point-lookup /
single-row-write comparison against PostgreSQL does exist in this tree's
other benchmarks (`tools/pg_benchmark.py`) — but running it here would
answer a different, already-answered question (this engine's absolute
single-node cost against PostgreSQL's), not "did RR0-RR5 change the fast
path's cost," which is the only question R6 is asking in this file. The
task RP8 already named stands: a `pg_txn_2pc_cost_probe.py` twin has no
obvious honest design, and none was built this session either.

## 13. What this run does not measure

- **A clean, isolated confirmation of R6's HR5 reading.** §11's own
  anomalous round leaves the answer bracketed rather than settled; a
  longer or repeated sweep would narrow it, and was not run this session.
- **The precise split between "in-doubt wait" and "dispatch queueing"
  inside R3's client-observed stall latencies.** No per-phase server-side
  timer exists to separate them — the same instrument gap RP8's §5c and
  §11 named, encountered a third time (§7's own note).
- **A confirmed explanation for R1/R5c's bimodal read-latency pattern.**
  Named as a hypothesis (reactor idle-wake state) tied to the same
  observability fields RP8's file cites, not independently instrumented.
- **The overhead A/B gate.** Suspended by the operator's 2026-08-24
  amendment; not run, and no engine code changed this session that it
  would have had anything to bracket in any case.
- **CR2's own correctness test** (whether a cross-owner RR transaction can
  fail to see its own earlier write on a participant). This was RR0's to
  build and test, and it already is — the RR0+RR1 commit message states
  the bug was found, reproduced (a transaction reading `1,10 2,20 3,30`
  and never seeing its own row 77) and closed by the `join`-not-open wire
  bit; RP7's kill-matrix and the full suite (2,929 green, `sim.sh` 171/0,
  per that commit's own gate) cover the correctness side this file does
  not re-test. This file measures cost, not correctness, for every cell.
- **A deployment shape between R5's two extremes** — a booking client
  partially, not wholly, co-located with what it reads. §10 names this
  explicitly rather than assuming the 100% figure generalises.
- **Width or participant-count sweeps beyond R3/R4's fixed 4.** RP8's B2
  already swept width for the write path (1-5); this file's R3/R4 fixed 4
  participants deliberately, to widen the natural in-doubt window enough
  to observe a low ceiling firing at all, and did not re-sweep width for
  the read-and-ceiling-specific shape.
- **A row-set sweep for R3-R6.** Rule 9's sweep is R1/R2's (three sizes,
  §5/§6); R3/R4 measure a fixed-shape contention/ceiling relationship with
  no row-set free variable the way a bulk scan has one, R5 reuses RP8's
  own fixed load sizes to keep the two files' numbers comparable, and R6
  is a single-row-per-transaction insert shape RP8's own file already
  argued has no row-set axis (`results-r6b-*.md` §11's identical note).

## 14. What this teaches about the engine

**The read/write asymmetry the order set out to close is closed, and it
was worth closing: R5 is the first number in this whole line that answers
"does anything use the protocol," and the answer went from zero to
one hundred percent under the shape that actually exists in this tree.**
Every prior R6 order (`2pc.md`, `cross-owner-protocol.md`) measured the
protocol's cost in isolation; this is the first to measure whether a
workload this tree already benchmarks can reach it at all, and RP8's own
words apply exactly: "R6 is built, correct and measured, and nothing that
exists exercises it end to end outside a purpose-built driver" — that
sentence is now false, and R5 is the reason.

**CR1's answer is a genuine engineering trade-off, not a bug.** The read
now costs several times more than the free pipeline it used to take
unconditionally, because it now has to be able to see the transaction's
own writes — a correctness requirement, not an optimisation opportunity
missed. Whether that cost is acceptable is a product question outside
this file's scope, but it should be read against R5's number: on the one
workload this tree can measure it against, the added read cost is a
fraction of a transaction whose commit alone already costs 2-3 ms (RP8's
B1), and the workload that needed the fix now runs at all rather than not
running.

**CR3's finding sharpens a design decision rather than reversing it, and
that is itself informative about where this protocol's real risk lives.**
`in_doubt_ceiling_ms` was proposed and swept as a single knob governing
"how bad can this get," and R4's own data says it governs only half of
what it looked like it governed: the writer-stall exposure, real and
correctly bounded, and a log-retention exposure that a healthy instance
essentially never triggers through this knob at all. The actual
log-retention risk this protocol carries is `kTxnPhaseDeadlineNs` (10 s)
or an unbounded wait on a crashed coordinator until the next mount — two
numbers an operator tuning `in_doubt_ceiling_ms` would not be touching.
Anyone reasoning about this protocol's worst-case WAL growth should read
the phase deadline and the mount-time resolver, not the in-doubt ceiling —
a distinction CR3 asked this file to draw and which the data draws
cleanly.

**The bimodal read-latency pattern (§5c) is the same phenomenon RP8's B1
found in the commit path, now visible on a much cheaper operation.** Two
independent measurements — one on a multi-millisecond three-sync commit,
one on a sub-millisecond point read — both show this host's per-request
latency clustering into two bands rather than one continuous
distribution, neither explained by row count, participant count, or
anything this session could directly instrument. That consistency across
two very different operations is itself evidence the phenomenon belongs
to the reactor/scheduling substrate rather than to either specific code
path — which is exactly the shape `docs/inflight/in-progress/observability.md`'s
`sched_idle_block_us` field was built to eventually answer, and which
RR5's own debt-payment already named as a gap (the missing per-leg timer,
recorded beside M3's `shipped_statement_us`). This file adds a second,
independent data point to that open question rather than closing it.
