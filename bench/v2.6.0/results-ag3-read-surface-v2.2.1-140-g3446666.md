# AG3 — RS4's enumeration re-run, and the six shapes that moved

`workplan-insert-spreading.md` §12d (R4-A/AG3): the fan-in's shape gate now
admits a projection and a fold beside the star read. **RS4's own driver is
what states the result** — `bench/spread_read_surface.py`, unchanged, the
same 16 shapes × every core against an unsplit twin on the same server that
RR3 and RS ran. This file is that enumeration at `3446666`, and nothing
else: no latency cell, no throughput cell, no A/B. What AG3 costs on the
wire is named in §5 as unmeasured, not estimated.

**The headline: the surface goes from 5 reachable / 11 refused to 11
reachable / 5 refused**, and the six that moved are exactly the six AG3
targeted — `projection`, `projection-multi`, `count`, `sum`,
`count-distinct`, `group-by`. The five that remain are the two non-pk-
ascending sorts and the three quota shapes, each refused by the same
sentence that refused it before AG3.

**And one finding nobody asked for** (§4): on a peer, a **spread** relation
is now readable in shapes its **unsplit** twin is not. The fan-in streams
its rows under credit; statement shipping — the route an unsplit foreign
relation takes — returns one reply and loses it above 992 bytes. Five
shapes read `reply-lost` on the control and `ok` on the split relation.

## 1. Provenance

| | |
|---|---|
| Date/time (UTC) | 2026-08-30, 04:29–04:30 |
| Worktree | `r4s` (`/home/cdkbs/ckdbs/.claude/worktrees/r4s`) |
| Branch | `worktree-r4s` |
| Commit measured | `3446666` (`git describe --tags` → **`v2.2.1-140-g3446666`**) |
| Tree state | **Clean** at measurement — the commit was made before the run, and `git status --short` was empty when the binary was staged |
| Binary provenance | `build-release/kds_server`, linked 2026-08-30 04:23 UTC from the sources this commit holds (the only edits between that link and the commit were to `docs/`). Copied to `/home/cdkbs/ag3_run/kds_server` and never touched again; `sha256sum` = `0383bcbb11019ed3982565a3a393dc8ede4aede2e90a213e9d1f5fbc791b480a`. The server in this file started from that copy, per `.claude/agents/ck-tester.md` rule 5 |
| Device | `/home/cdkbs` on `/dev/root`, **ext4** (`df -T`) — a real block device, not tmpfs |
| Build type | `CMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG`, `KDS_WITH_TLS=ON` |
| Host | AMD EPYC 9V74, **2 logical CPUs = 2 physical cores, 1 thread/core** (no SMT; `lscpu` reports `Thread(s) per core: 1`), 1 socket, 1 NUMA node, Microsoft Hyper-V guest, 15 GiB RAM, Ubuntu 24.04.4 LTS, Linux 6.17.0-1022-azure |
| Host quiet | `uptime` load average 0.28 at the run; `pgrep cc1plus` empty before it (the release build had finished and was not overlapped — this host has two CPUs and a build alongside a run measures the build) |
| Server config | `cores = 2`, `placement = creating`, `range_size_ids = 512`, `durability = relaxed`, `peer_listeners = on` |
| Suite / sim | Both re-run for this commit, `build-release`, one sitting: **3042/3042** tests, and `scripts/sim.sh` **171 runs, 0 failures**. The Debug `build/` suite was also 3042/3042 |
| Raw output | `bench/v2.6.0/archive/read-surface-ag3-v2.2.1-140-g3446666/` — the grid and the driver's JSON |

## 2. The rig, and what it is asking

`bench/spread_read_surface.py --server <staged binary> --workdir
/home/cdkbs/ag3_run --cores 2 --range-size-ids 512 --rounds 300
--placement creating`. Two relations of `(id int64, v int64)` on one
server:

- **`spread`** — 600 rows written round-robin from both cores' sessions, so
  it takes a range per peer. `SHOW META`: `split_relation_detail=4000:2@2`
  — two ranges, two owners, confirmed on this run.
- **`whole`** — the control, 600 rows from core 0 alone, never split.

Each of 16 shapes is then asked of both relations from **each** core. A
shape refused on the control as well is refused for a reason that has
nothing to do with ranges, which is the distinction that makes this a
surface rather than a list of errors. `placed=600` — every row landed.

## 3. The surface at `3446666`

**11 reachable from every core, 5 refused BY THE SPLIT.**

| verdict | shapes |
|---|---|
| reachable everywhere (11) | `star`, `star+where-pk`, `star+where-nonpk`, `star+between`, `star+order-pk-asc`, **`projection`**, **`projection-multi`**, **`count`**, **`sum`**, **`count-distinct`**, **`group-by`** |
| refused BY THE SPLIT (5) | `star+order-pk-desc`, `star+order-nonpk`, `limit`, `limit+offset`, `order+limit` |

Against RS's run of the same driver
(`results-self-directed-stage-and-read-surface-v2.2.1-135-g9eae44a.md` §4.1,
`9eae44a`, 5 reachable / 11 refused):

| shape | at `9eae44a` | at `3446666` |
|---|---|---|
| `projection`, `projection-multi` | refused by the split | **reachable everywhere** |
| `count`, `sum`, `count-distinct`, `group-by` | refused by the split | **reachable everywhere** |
| `star+order-pk-desc`, `star+order-nonpk` | refused by the split | refused by the split |
| `limit`, `limit+offset`, `order+limit` | refused by the split | refused by the split |
| the five star shapes | reachable everywhere | reachable everywhere |

**Six shapes moved and no shape regressed**, which is the whole of AG3's
claim on this surface. The five that stay carry the same refusal text they
carried before — *"has ranges on another core and this shape cannot fan in
over them; reading it here would answer short"* — and the two reasons are
the ones `command_dispatcher.cpp`'s gate comment states: a sort and a quota
both apply at **emission**, and the remote side emits everything in its own
order, so shipping either would answer the statement in an order or a
cardinality the client did not ask for. Neither is an oversight and neither
is deferred work of AG3's; both are `crosscore.md` §9's if they are ever
lifted.

**Measured, not derived.** The 11/5 split above is the driver's own
classification of 32 replies (16 shapes × 2 cores) against 32 control
replies, archived in full.

## 4. The finding this run added: a split relation is now readable where its twin is not

The control column is not uniform, and the asymmetry is worth stating
because it inverts what the whole R4 line has been reporting. On **core 1**,
against the **unsplit** `whole`:

| shape | `spread` from core 1 | `whole` from core 1 |
|---|---|---|
| `projection` | ok | **reply-lost** |
| `projection-multi` | ok | **reply-lost** |
| `group-by` | ok | **reply-lost** |
| `star+order-pk-desc` | no-route | **reply-lost** |
| `star+order-nonpk` | no-route | **reply-lost** |

`reply-lost` is the driver's tag for `UNKNOWN_OUTCOME`. **Source-read, with
the site**: an unsplit relation another core owns is reached by *statement
shipping*, whose reply rides one message and is capped — the 992-byte reply
cap `CLAUDE.md` already carries as an open item on the cross-owner line. A
600-row projection or a 600-group `GROUP BY` does not fit. A **spread**
relation takes the fan-in instead, whose batches stream under credit and
have no such cap, so the same shape over the same 600 rows answers.

Two consequences, both stated rather than acted on here:

- **The 992-byte cap is now the narrower limit of the two**, on a peer, for
  every wide-reply shape. It was previously masked: before AG3 these shapes
  were refused on the split relation too, so the control's failure had no
  contrast to be measured against.
- **It does not affect §3's verdicts.** The driver reads its verdict off
  **core 0**, where the control is local and clean, precisely so that a
  failing control cannot be mistaken for the split's doing. That choice
  (RR3's, documented in the driver) is what keeps this run's 11/5 an honest
  count.

## 5. Not run, and why — each confirmed rather than assumed

- **`--placement rotate`.** Not a valid cell at this core count, and the
  reason is source-read and already established: `core_placement.hpp`'s
  `AssignOwnerCore` computes `relation_seq % (core_count - 1)`, which at
  `core_count = 2` is 0 for every relation, so `rotate` places everything on
  core 1 and core 0's foreign writes never open a second range
  (`results-self-directed-stage-and-read-surface-v2.2.1-135-g9eae44a.md`
  §4.2 traces the second half through `PeekRowId`/`NoteRowIdDemand`). It
  would produce **no split relation to enumerate**. Reported as not run, not
  as agreement between placements — and this is a narrowing of what
  `known-gaps.md` carried from RR3's 4-core run, which could say "both
  placements" and this run cannot.
- **k > 2, for anything.** `kds_server` refuses `cores 3` on this host
  (`CheckCoreCount`: two logical CPUs, reactors are pinned one per core and
  never block). So the 4-core grid RR3 ran is not reproducible here, and no
  cell in this file is a k = 4 claim.
- **What AG3 costs.** A fold over a fan-in ships **every row it folds** —
  `SELECT COUNT(*)` moves the whole relation across the ring to count it —
  and no cell here prices that. `aggregate.hpp`'s `Merge` (AG-M) is the
  reserved answer and is not built (§12d). The number this wants is a
  latency cell against the local walk and against statement shipping, which
  belongs to whatever order takes the aggregate workload.
- **Per-statement overhead A/B.** Suspended by operator amendment since
  2026-08-24. Not measured, and not implied.
- **Scenario 2.** §12b's branch stands: three of its four blocked shapes are
  unblocked by AG3 and the fourth — a join with the spread relation on one
  side — is the two-step pipeline's, so s2 whole still does not run. This
  file does not claim it does.
- **Any comparison against a pre-`3446666` build of this mechanism.** Every
  number here is this commit's engine, per `.claude/agents/ck-tester.md`
  rule 1's "current state only". The `9eae44a` column in §3 is quoted from
  that commit's own results file, not re-measured.

## 6. Versus PostgreSQL

**No counterpart concept exists, and none is claimed.** This enumeration
asks which shapes a relation *split across this engine's core-owned page
ranges* answers, from which core. PostgreSQL has no analogue to a relation
owned by one of several core-pinned execution units, so there is no
PostgreSQL twin of the question, let alone of the answer — the same
reasoning `bench/v2.5.0/results-r6b-cross-owner-cost-v2.4.0-28-g53f6aae.md`
§10 gives for the cross-owner line. Stated rather than the section omitted.

## 7. What this run does not measure

Everything in §5, and one thing more: **it measures a surface, not
agreement.** A shape reading `ok` here means the engine answered it, not
that it answered it *correctly*. Row-level equivalence against the same
rows unsplit is the unit suite's — at this commit,
`CoreRuntimeTest.AFoldAndAProjectionOverASpreadRelationAnswerAsTheUnsplitTwinDoes`
compares seven shapes byte-for-byte against a locally-walked twin with
duplicate group keys straddling the cut, and §12d's vacuity matrix reports
what each reversion of the mechanism costs. The driver cannot do that job:
`spread` and `whole` cannot hold identical ids, because `spread`'s come
from per-core leased blocks (the driver says so itself, and it is why its
control column is read for verdicts and never row-for-row).
