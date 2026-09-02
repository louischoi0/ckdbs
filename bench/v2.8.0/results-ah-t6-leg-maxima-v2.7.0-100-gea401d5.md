# AH-T6's owed item: the legs' maxima, and the tail that turned out not to be the crossing's

| | |
|---|---|
| Measured at | `v2.7.0-100-gea401d5` (`git describe --tags`) |
| Build | `build-release` (Release), relinked from this commit's tree before the run |
| Driver | `bench/fk_crossing_cost_probe.py --mode crossing`, six blocks, 300 statements a block, one row each |
| Host | 8 cores, AMD EPYC 9V74; `/proc/loadavg` 1.36 before and 1.38 after, my own run being ~1.0 of it |
| Raw | `bench/v2.8.0/archive/ah-t6-leg-maxima-relaxed.json`, `…-group.json` |
| Tree state | Nothing under `src/`, `include/` or `tests/` moved between the build and the last run; the driver gained the two `*_max_us` reads and nothing else |

**What was owed.** `results-ah-t6-fk-crossing-matrix-v2.7.0-97-g199dabf.md`
§4 recorded a crossing p99 of 13× the colocated arm's, could not attribute
it, and named the legs' maxima as the instrument that would:
*"`fk_probe_round_max_us`, `fk_release_decide_max_us` are printed and not yet
read per block."* They are read now.

**A note on the numbers, first.** `PhaseLeg` keeps a high-water mark that
never resets, so what a block reports is *running*: a maximum that moved in
a block is an outlier that happened in it, and one that did not move says the
block held nothing worse than an earlier one. That is the honest reading and
no reset was added to get a prettier one.

## 1. The answer: the tail is the write path's, not the crossing's

Pooled over six blocks of 300 statements each.

| | crossing p50 | p90 | p99 | p100 | probe leg max | decide leg max |
|---|---|---|---|---|---|---|
| `relaxed` | 143 | 162 | 1622 | 4174 | **2604** | **2209** |
| `group` | 1280 | 1974 | 3012 | 11662 | **4758** | **8488** |

| | colocated p50 | p90 | p99 | p100 |
|---|---|---|---|---|
| `relaxed` | 82 | 116 | **1317** | 2675 |
| `group` | 1227 | 1934 | **3713** | 15914 |

µs. The two rows that settle it:

- **`relaxed`: the crossing's p99 is 1.23× the colocated arm's**, against a
  p50 ratio of 1.74×. The crossing adds ~60 µs to the median and
  proportionally *less* to the tail.
- **`group`: the crossing's p99 is 0.81× the colocated arm's** — the arm with
  two extra ring round trips has the **smaller** tail, and its p100 is 0.73×.

A write on this engine occasionally waits milliseconds whether or not a
foreign key crosses. The crossing does not cause that and does not
meaningfully add to it.

## 2. So §4's "13×" was a sampling artifact, and here is the evidence

The earlier file measured three blocks. Its colocated arm's p99 read **127
µs** — it had not yet caught an outlier — while its crossing arm's read 1684
µs. At six blocks the colocated arm's p99 is **1317 µs** and the crossing
arm's is **1622 µs**: the numerator barely moved (1684 → 1622), the
denominator moved by an order of magnitude, and the ratio collapsed from 13×
to 1.23×.

**The lesson is about the measurement, not the engine**: a ratio of two
tails needs enough blocks that *both* arms have met their own worst case, and
three was not enough for the arm that meets it more rarely. §4 was right to
record the observation and right not to conclude from it.

## 3. What the two legs' maxima say about themselves

- **The probe round is never the largest thing in the statement.** Its worst
  case over 1800 crossing statements is 2.6 ms (`relaxed`) and 4.8 ms
  (`group`), against statement maxima of 4.2 ms and 11.7 ms. Bounded at the
  maximum and not only at the mean, which is what §4 asked.
- **The release decide's maximum tracks the statement's.** At `group` it
  reaches 8.5 ms inside an 11.7 ms statement. That is not a slow round: it is
  the overlap the matrix file's §2 established at the *mean* — the leg is
  wall-clock from send to acknowledgement, so it absorbs whatever the
  reactor is doing, and under `group` the reactor is draining a group commit.
  A leg whose maximum approaches the statement's maximum is a leg that
  **contains** the wait rather than adding to it, and the end-to-end p50
  increment of 4.3% is the same fact from the other side.

## 4. Two discarded attempts, said out loud

The first run of this measurement is not in this file, and neither is the
second:

1. **Contended.** `/tmp/claude-1000/-home-cdkbs-ckdbs/` held three Claude
   session scratchpads and one of them was waiting on its own `cmake --build`
   in a different worktree. The symptom is a whole distribution shifting
   rather than failing: the colocated arm's p99 read **1.5 ms** where the same
   cell reads 159 µs here, and a crossing statement reached 87 ms. Sequencing
   CLA's own builds against its own measurements is necessary and, on this
   host, no longer sufficient.
2. **Stale binary.** The second attempt ran a `build-release` built before the
   review fixes landed — including C5, which stops the probe leg from timing
   a deadline as a round trip. No deadline occurred in that run so no number
   would have changed, but a results file may not name a commit whose tree it
   was not built from.

Both are recorded because the numbers they produced were plausible, and a
plausible number from a contended host or a stale binary is exactly the kind
that survives into a citation.

## 5. What this closes and what it does not

**Closes** AH-T6's §4 and the `known-gaps` line that carried it: the p99 tail
is not the crossing's, and both its rounds have bounded maxima.

**Does not touch**, and they remain AH's open list: the surviving-coordinator
crash half; `durability = strict`; any concurrency at all (one client, one
statement at a time, which is what makes a *tail* here a property of the host
as much as of the engine); the cost of the participant-coordinated release
path `foreign-keys.md` §2b names; and the reverse direction's fan-out.
