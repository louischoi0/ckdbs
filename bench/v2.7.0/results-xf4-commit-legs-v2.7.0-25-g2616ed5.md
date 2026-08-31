# XF4 — the commit chain's seven legs, and H-XF3 answered by being refused

**Headline. H-XF3 offered two mechanisms for XE1's b=8 saving and asked
which dominates. Both are real, and they are not independent — the timers
show the second is caused by the first.** With the ack moved to the
append, the participant's decide→ack leg falls **80.1%** (1,615.0 → 321.7
µs), which is the wait leaving the chain and is by construction. What the
instrument adds is the half nobody could see: the participant's own record
also becomes durable **550.7 µs sooner** (`decide→durable` 2,093.0 →
1,542.3 µs, −26.3%), with `syncs_per_commit` falling 1.511 → 1.184. A
participant that stops blocking on the device runs more commits into each
drain, so the sync it no longer waits for is also a sync it reaches
earlier. Sharing is a **consequence** of the deferral, not an alternative
to it, and the dichotomy H-XF3 was built on does not survive measurement.

**Two findings the order did not ask for and this file will not bury.**
First, **the three coordinator legs are the chain**: they account for
5,786.6 µs of a 5,795.4 µs commit, an unaccounted remainder of **+0.15%**
(−0.40% on the control arm). Nothing hides in the sends, the outcome reads
or the refusal arms, and every future claim about this protocol can now be
made about a leg instead of a total. Second, **the largest leg is now
`prepare`, at 46% of the commit, and it is not the device**: the
coordinator waits 2,674.6 µs for a prepare the participant makes durable
in 1,186.9 µs, so **1,487.7 µs — 26% of the whole commit — is transport
and scheduling on a leg nothing has ever attacked**. XE1 optimised the
decide leg; the prepare leg is what is left.

Measured on the worktree `xf` (`/home/cdkbs/ckdbs/.claude/worktrees/xf`)
at **`2616ed5`** (`git describe --tags` → **`v2.7.0-25-g2616ed5`**),
branch `xf-shipped-read`, executing `instructions/v2.7.1/workorder-xf.md`
row XF4.

## 1. Provenance

| | |
|---|---|
| Date/time (UTC) | 2026-08-31, 14:05-14:12 |
| Commit measured | `2616ed5` (`v2.7.0-25-g2616ed5`) — the commit that adds the timers |
| Tree state | Clean at both arms' `git archive`. HEAD did not move during the run |
| Arm — `atappend` | `build-release/kds_server` at `2616ed5`, copied to the run's own directory before the first cell (ck-tester rule 5), `sha256` `d57c141e634c2fe8…`, source mtime `2026-08-31 14:04:49Z` |
| Arm — `ackdurable` (control) | **the same commit with XE1's one ternary reverted** — `CommitAck::kAtAppend` → `kWhenDurable` on `StartDecision`'s commit arm, and nothing else — built from a clean `git archive` of `HEAD` with that one `sed`, `sha256` `36bd7d0ecfc9f89a…`. The build script is archived beside the cells |
| **Why the control is not `85d2bda`** | XF3's arms are the real pre/post binaries and are the right pair for a *latency* A/B, but neither carries the timers, so neither can report a leg. The question here is which leg moved, and that needs **both arms instrumented**. Reverting the one line that XE1 changed gives exactly one variable, both arms carrying the identical instrument. It is a controlled arm, not a historical one, and this file never calls it "pre-XE1" |
| Driver | `bench/xe4_crossowner_commit_probe.py`, `--cores 2 --durability group --concurrency 8 --txns 4000` — byte-identical arguments to `results-xe-ack-at-append` §4.3's `group, pl, b=8` cells and to XF3's addendum |
| Repeats | three per arm, fresh server and data file per cell, `bench/wait_quiet.sh` before each |
| Device | `/dev/root`, **ext4**, checked with `df -T` on the actual workdir. Never tmpfs |
| Host | 8 logical CPUs = 4 physical × 2 threads, AMD EPYC 9V74, Linux 6.17.0-1022-azure. One unrelated `kds_server` from another checkout was resident at 0.8% CPU, named here rather than omitted |
| Test suite | **3,087 of 3,087 passing**, `build-release/tests/kds_tests` at `2616ed5` (3,086 before this row; the new cell is `CoreRuntimeTest.EachCrossOwnerCommitLegIsTimedAndAOneOwnerCommitTimesNothing`) |
| Raw output | `bench/v2.7.0/archive/xf4-commit-legs-v2.7.0-25-g2616ed5/` — six JSON summaries and both harness scripts |

**Absolute latencies here are this host's and this client's.** They are
read against XF3's addendum on the same host, never against
`results-xd-commit-decomposition`'s (different host, pre-fix client).

## 2. The instrument, and what it costs

Seven legs, count/total/max each, exposed per core in `SHOW META` and
absent until a core has walked one (`commit_phase_stats.hpp`).

**Count, total and maximum — not a histogram.** A histogram needs bucket
bounds; the order's conclusion 4 says a constant that wants choosing is
stopped on and reported. Count/total/max needs no bound, is the shape
`SHOW META` already prints for the lease refills beside it, and answers
the question three files asked.

**A one-owner commit pays nothing.** Every `Note` sits inside the
cross-owner path — the coordinator's parked commit block and the
participant's prepare/decide handlers — so a transaction with no
participants reads no clock it did not read before. The prepare site's net
addition is *zero*: its deadline is now anchored on the append stamp
instead of a second `Now()`. The test asserts it from the other end — one
dispatcher, a one-owner commit then a cross-owner one, and only the second
records anything.

**The one cost this file charges itself.** The participant's third leg
needs a park the erased context cannot hold, so it is a coroutine holding
an LSN and two stamps. It costs one task per decide **on both arms**, and
its submit-plus-poll lag is visible in the control arm's own numbers:
there, durability is already reached when the observer is submitted, so
`part_durable − part_ack` = **478.0 µs** is pure observer overhead. It
appears on both arms and therefore **cancels in every delta below** — but
it means `part_durable` is an upper bound on its arm, never a tight one.
Stated here because a reader would otherwise take 1,542.3 µs as the
XE1 arm's durability latency, when the honest figure is ~1,064 µs.

## 3. The cells

Three repeats per arm; medians of the repeats' own summaries, with each
arm's peak-to-peak spread over its median beside it.

### 3.1 What the client saw

| | ackdurable (control) | atappend (XE1) | Δ | floors |
|---|---:|---:|---:|---|
| commit p50 (µs) | 6,578.8 | 5,317.4 | **−19.17%** | 2.1% / 9.9% |
| commit p95 (µs) | 11,200.4 | 9,434.4 | −15.77% | 7.4% / 3.1% |
| commit p99 (µs) | 16,223.7 | 14,342.8 | −11.59% | 12.2% / 18.1% |
| commit mean (µs) | 7,054.2 | 5,888.4 | −16.53% | 3.9% / 3.6% |
| TPS | 545.8 | 679.6 | **+24.53%** | 7.4% / 9.9% |
| syncs per commit | 1.511 | 1.184 | −21.62% | 3.6% / 10.5% |

**Consistent with XF3's addendum on the same host** (−16.60% p50,
+27.66% TPS, spc 1.560 → 1.240) — which matters, because it says the
timers did not move the thing they measure. The p99 delta sits inside the
p99 floors and is not read as a finding.

### 3.2 The legs (mean µs per commit)

| leg | control | atappend | Δ | Δ% | floors |
|---|---:|---:|---:|---:|---|
| **coordinator** | | | | | |
| prepare sent → all settled | 2,813.8 | 2,674.6 | −139.2 | −4.95% | 3.3% / 2.2% |
| local commit → decision durable | 1,267.1 | 1,257.9 | −9.2 | −0.73% | 6.0% / 3.1% |
| **decide sent → all acked** | **2,906.7** | **1,854.1** | **−1,052.6** | **−36.21%** | 5.0% / 6.2% |
| the whole commit | 6,959.6 | 5,795.4 | −1,164.2 | −16.73% | 4.1% / 3.5% |
| **participant** | | | | | |
| PREPARE appended → durable | 1,199.5 | 1,186.9 | −12.6 | −1.05% | 4.9% / 0.5% |
| **decide → acked** | **1,615.0** | **321.7** | **−1,293.3** | **−80.08%** | 4.3% / 17.6% |
| **decide → own record durable** | **2,093.0** | **1,542.3** | **−550.7** | **−26.31%** | 6.5% / 5.1% |

**The three coordinator legs are the chain.** Their sum is 5,786.6 µs
against a whole of 5,795.4 (**+0.15%** unaccounted) on the XE1 arm, and
6,987.6 against 6,959.6 (−0.40%) on the control — the sign flips because
these are independent medians of means, and both remainders are far inside
the legs' own floors. Nothing material lives outside the three.

## 4. H-XF3, answered

> *If parking discipline dominates, the decide-to-ack leg's own time
> barely moves while commit total falls; if sync-sharing dominates, the
> leg time itself falls.*

**The hypothesis is a false dichotomy and the timers are what shows it.**

**Both terms are present.** The ack leg falls 1,293.3 µs — larger than the
whole commit's 1,164.2 µs saving, so the deferral alone more than accounts
for it. And the participant's record reaches durability **550.7 µs
sooner**, not later: 2,093.0 → 1,542.3 µs from the same origin, with the
observer's 478 µs overhead cancelling between the arms. Under the control
the record is durable at ≈1,615 µs after the decide; under XE1 at ≈1,064
µs. The sync did not merely move — it got faster.

**And the second is caused by the first, which is why they are not
alternatives.** `syncs_per_commit` falls 1.511 → 1.184: fewer, larger
drains for the same committed work. That is what a participant reactor
does when it stops parking every decide on the device — it admits more
commits per drain, and each one's record therefore reaches the platter
sooner. Parking discipline is the mechanism; sharing is its effect. A
reading that treats them as competing explanations, which is what the
hypothesis asked us to choose between, has the causality wrong.

**What XE §5 offered and could not prove is now proven, with a
correction.** §5's reading — *"a sync counted is not a sync waited for"* —
is right and is not the whole of it: under XE1 a sync is also a sync
performed sooner.

**Where the saving lands, leg by leg.** The participant's ack leg gives up
1,293.3 µs; the coordinator's decide leg recovers 1,052.6 of it, the
missing 240.7 being transport and scheduling that grew because the
coordinator is now resuming into a busier reactor (`decide − part_ack`
rises 1,291.7 → 1,532.4 µs). The prepare leg gains a further 139.2 µs of
congestion relief it did nothing to earn, and the decision leg does not
move at all (−0.73%, inside its floor) — correctly, since XE1 does not
touch the coordinator's own record.

## 5. What the instrument found that nothing asked for

**The prepare leg is now the largest single cost of a cross-owner commit,
and most of it is not the device.** At 2,674.6 µs it is **46%** of the
5,795.4 µs commit — more than the decide leg XE1 just cut by a third. The
participant makes its PREPARE durable in 1,186.9 µs, so **1,487.7 µs, or
26% of the whole commit, is the round trip and the scheduling around it**.
The same gap on the decide leg is 1,532.4 µs. Together, **just over half
of a cross-owner commit at eight concurrent coordinators is neither core's
device time**.

That is a large number that no previous file could see, and it is stated
without a proposal attached: what to do about it is a design question
(batching the phase messages, a credit-shaped decide, the read-only
participant lever XD6 named) and none of it is this row's.

**The prepare leg barely moved between arms** (−4.95%, against floors of
3.3%/2.2%), and the participant's own half of it did not move at all
(−1.05%). Whatever XE1 changed, it did not change what a prepare costs —
which is the null result the instrument owed and the confirmation that
these legs measure what their names say.

## 6. What this file does not answer

**The control arm is not the historical pre-XE1 binary.** It is `2616ed5`
with one line reverted, chosen because H-XF3 needs both arms
instrumented. The latency A/B against the real binaries is XF3's addendum,
on this host, and the two agree within their floors — which is the check
that the substitution did not change the subject, not a proof that it
could not have.

**The b=1 shape was not measured here.** XE §4.3 found no resolvable
saving at one coordinator and XF3 did not revisit it; the legs would very
likely show why (a serial loop has no other commit to share a drain with,
so the sharing term is absent by construction) and that is a prediction,
not a measurement.

**`part_durable` is an upper bound**, by the observer's 478 µs on both
arms (§2). Every delta in §3.2 is clean; no absolute in that row is.

**Nothing here is a scenario-2 booking.** This is the substitute shape
`results-xe-ack-at-append` §4.1 introduced because the shipped-read
refusal blocks the real one; XF0's survey
(`docs/inflight/blocked/workplan-shipped-read-typed.md`) and its
ratification ask stand in front of closing it, and XF2 is the measurement
that reopens when it does.
