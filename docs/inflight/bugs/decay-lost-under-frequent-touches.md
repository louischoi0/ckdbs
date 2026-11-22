# A score touched more often than one sixteenth of a half-life never decays

**The wrong behaviour.** The lazy-decay score (`include/kds/stats/decay.hpp`,
`docs/spec/decay.md` R1) is meant to be a time-weighted count: a shape run
at a steady rate settles at `rate × half_life / ln 2`. A pair touched at
intervals shorter than one fraction bucket - `half_life / 16`, 37.5 s at the
default `decay_half_life` of 600 s - **does not decay at all**: it is a raw
count of every touch since it was first seen, until it saturates.

`Accumulate` decays the stored score to `now`, adds the points, and restamps
`last_bump = now`. `DecayedScaledAt` quantizes the elapsed fraction into 16
buckets a half-life, and bucket 0 is the factor 256 - no decay. So an
elapsed time under one bucket decays nothing, and the restamp then throws
that elapsed time away: the next touch starts from zero elapsed again. The
decay that was due is lost at every touch, not deferred.

**Verified** on worktree `dl-statistics-queue-no-statement` at `b0a824db`
plus DL-S1's working tree, by a probe over `decay.hpp` alone (the code is
unchanged since before DL; DL-S1's `MergeDecay` restamps the same way and
inherits it, no worse: a fold restamps the folded pair only when a part
noted the shape). Found by DL-S1's review, 2026-11-20. **Re-read at
`bf289a82`**: `decay.hpp` last changed at `18664808` (DL-S1), and
`Accumulate` still restamps `last_bump = now`.

**Smallest reproduction.** A `ManualClock`, a 600 s half-life:

| touches | span | reads | a time-weighted count would read |
|---|---|---|---|
| one every 1 s | 100 half-lives (60,000 touches) | **60,000 points** | about 866 points |
| one every 40 s (just past a bucket) | 100 half-lives (1,500 touches) | 22 points | about 21.6 points |

The second row is what the precision contract promises (an overestimate of
at most 4.4 %); the first is the defect. `tests/decay_test.cpp` pins whole
half-lives and single reads, and no cell touches a pair repeatedly inside
one bucket.

**What it costs.** No statement's answer: the score's one consumer is the
Cabin controller's signal collector (`stats/optimizer_signals.hpp`), which
is advisory and off by default (`cabin_optimizer`, experimental). With the
controller on, a shape that runs more often than once a bucket carries its
whole history as if it were current - its frequency (S1) and page sum (S2)
are overstated by up to the ratio above, and it does not cool when its rate
falls, only when it stops for a bucket or more. So the controller's
cost-benefit margins are computed from a count where they assume a rate: a
CREATE can be decided for a shape whose present rate would not earn it, and
a hot Cabin's DROP is delayed. A wrong decision, never a wrong row.

**The fix, known and unscheduled.** Advance `last_bump` by the decay that
was actually applied - whole half-lives plus whole buckets - and not to
`now`, so the remainder stays owed; or carry the remainder in the pair. It
changes every measured threshold the controller's defaults were chosen
against (`cabin-controller.md` §II.4), and is the controller's, not DL's:
DL changes how a signal reaches the controller and nothing about what the
controller decides from it. Owner: `docs/spec/decay.md`,
`docs/spec/cabin-controller.md`; no work order carries it.
