# CQ-S2 - the column reservation spike: a debit that does not hold its row

**Measured at `v3.4.8-11-g3dea53c2`** on `worktree-cq-lock-free-reservation`,
2026-11-12, pass 2 at 01:04-01:10 UTC. Order:
`instructions/v3.2.0/workorder-cq-column-reservation.md` section 6, CQ-S2,
at CQ-Q15 (a)'s reduced scope. Raw outputs:
`archive/cq-s2-spike-v3.4.8-11-g3dea53c2/`. The spike is on the work
branch only (CQ-Q2 (a)).

**Every number below is pass 2's.** Pass 1 (00:44-00:50 UTC, at
`v3.4.8-10-gf72b8232`) is archived beside it and agrees - `k = 8` medians
under `group`: hot `1,744 tps`, reserved `4,587 tps`, piped `4,669 tps`,
disjoint `4,490 tps`. Pass 2 exists for two reasons: pass 1 showed a
false refusal in the spike (4 debits of about 250,000 refused because a
head committed between two questions the check asked; fixed in
`3dea53c2`), and its base cells ran a shorter schedule than the spike's,
which made the off-path comparison unequal.

## What was asked

Whether a column reservation realises, **without the client reordering
its transaction**, the throughput S0 found a client gets by writing the
shared row last; whether the bound holds and the balances stay exact;
and what the spike's code costs a relation that has no marked column.

The transaction is S0's, 8 sessions, 6 s a cell:

    BEGIN
    UPDATE cq_acct SET c = c - 1 WHERE id = 1            -- the debit, first
    UPDATE cq_work SET v = v + 1 WHERE id = <own row>    -- k of these
    COMMIT

| arm | server | what the debit is |
|---|---|---|
| hot | the spike's binary, no mark | today's `UPDATE`: the row held in `U` from the statement to the decide |
| **reserved** | the spike's binary, `cq_acct.c` marked | the same statements; the debit journals a delta, holds the row in `S` and writes nothing, and the row is written at `COMMIT` |
| hot-last piped | no mark | the client's remedy (A2): the debit last, in one socket write with `COMMIT` |
| disjoint | no mark | each session debits a row of its own |
| base | the binary of `e698fce8`, the commit the spike started from | hot, hot-last piped and disjoint again, for the off-path comparison |

Three servers alternate - plain, marked, base - each a fresh instance for
one driver call, three times over.

## Results

### T1 throughput, `group`, 8 sessions (median of 3, and the range)

| arm | `k = 8` | range | `k = 2` | range |
|---|---|---|---|---|
| hot (debit first), today | `1,760 tps` | `1,732-1,769 tps` | `4,384 tps` | `4,268-4,468 tps` |
| **reserved** (debit first, the column marked) | `4,584 tps` | `4,567-4,618 tps` | `6,981 tps` | `6,817-7,037 tps` |
| hot-last piped with `COMMIT` | `4,584 tps` | `4,565-4,670 tps` | `6,950 tps` | `6,928-7,056 tps` |
| disjoint | `4,474 tps` | `4,235-4,588 tps` | `7,326 tps` | `7,113-7,450 tps` |

**The reserved arm commits `2.60x` the hot arm at `k = 8` and `1.59x` at
`k = 2`, and recovers `100 %` and `101 %` of the gap to the piped arm** -
it cannot be told apart from the client's own remedy, with the debit
still the first statement.

### T2 refusals and latency, `group`, `k = 8` (median over the repetitions)

| arm | refused | debit p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| hot | decided-newer `469` (`1.5 %` of attempts) | `47 us` | `725 us` | `1,307 us` | `12,194 us` | `20,688 us` | `51,798 us` |
| **reserved** | **none** | `41 us` | `75 us` | `88 us` | `148 us` | `213 us` | `3,236 us` |
| hot-last piped | decided-newer `3,991` (`4.6 %` of attempts) | not a separate statement | - | - | - | - | - |
| disjoint | none | - | - | `99 us` | - | `257 us` | - |

| arm | `COMMIT` p0 | p25 | p50 | p95 | p99 | max | transaction p50 | p99 |
|---|---|---|---|---|---|---|---|---|
| hot | `357 us` | `637 us` | `758 us` | `1,170 us` | `1,465 us` | `3,870 us` | `2,687 us` | `21,984 us` |
| **reserved** | `370 us` | `703 us` | `837 us` | `1,240 us` | `1,576 us` | `277,119 us` | `1,619 us` | `2,606 us` |
| hot-last piped | - | - | `849 us` | - | `1,579 us` | - | `1,501 us` | `2,425 us` |
| disjoint | - | - | `881 us` | - | `1,793 us` | - | `1,648 us` | `2,828 us` |

- **The wait moved from the debit to the `COMMIT`, and shrank.** The hot
  arm's debit waits a median `1,307 us` for the row; the reserved arm's
  answers in `88 us`, and its `COMMIT` is `79 us` slower than the hot
  arm's at the median - the apply, and its wait for the row's `U`.
- **The lock family's waits**: the reserved arm records `5,318-5,737`
  waits a cell, all `tuple/U`, all granted, p50 `18-19 us`, p99
  `141-164 us` - the applies serialising at `COMMIT`. The hot arm fills
  the `16,384`-row ring in every cell, all granted, p50 `493-502 us`.
- **No decided-newer refusal** in any reserved cell (B2): `0` of about
  `250,000` reserved transactions, against `1.5 %` for the hot arm and
  `4.6 %` for the piped one, whose refusals each fail a transaction.
- One `277 ms` `COMMIT` in the reserved arm's maximum; the piped arm has
  one of `287 ms`. Not the spike's.

### T3 `strict`, `k = 8` (two repetitions, both shown)

| arm | committed | refused |
|---|---|---|
| hot | `845 tps`, `713 tps` | decided-newer `1` |
| **reserved** | `1,184 tps`, `1,198 tps` | none |
| hot-last piped | `1,230 tps`, `1,240 tps` | decided-newer `49` (`0.3 %` of attempts) |
| disjoint | `3,718 tps`, `3,425 tps` | none |

As the model said (K3): under a deferred `strict` commit the apply's `U`
spans the sync, so the reserved arm reaches the last-writer arm and no
further - a third of disjoint.

### T4 a 1.2 s pause after the debit, `group`, `k = 0`, 20 s, one cell each

| server | committed | refused | lock waits |
|---|---|---|---|
| today | `18` transactions | fault net `134` | `115` granted, `134` at the net |
| **reserved** | `136` transactions | **none** | `123` granted |

Today the pause holds the row and the eight use it one at a time. With
the row reserved every session commits each time its pause ends: `136`
of the `133` a 1.2 s pause allows eight sessions in 20 s (the last
commits land as the cell closes). One cell: a mechanism, not a rate.

### T5 off the path: the spike's binary against the base's, no mark, `group` (median of 3)

| arm, `k` | base | range | spike, unmarked | range | spike / base | server CPU a transaction, base | spike |
|---|---|---|---|---|---|---|---|
| hot, `8` | `1,753 tps` | `1,702-1,762 tps` | `1,760 tps` | `1,732-1,769 tps` | `1.004` | `554 us` | `555 us` |
| hot, `2` | `4,420 tps` | `4,420-4,447 tps` | `4,384 tps` | `4,268-4,468 tps` | `0.992` | `280 us` | `284 us` |
| hot-last piped, `8` | `4,642 tps` | `4,603-4,667 tps` | `4,584 tps` | `4,565-4,670 tps` | `0.988` | `413 us` | `415 us` |
| hot-last piped, `2` | `6,980 tps` | `6,920-6,980 tps` | `6,950 tps` | `6,928-7,056 tps` | `0.996` | `210 us` | `211 us` |
| disjoint, `8` | `4,586 tps` | `4,528-4,589 tps` | `4,474 tps` | `4,235-4,588 tps` | `0.976` | `399 us` | `404 us` |
| disjoint, `2` | `7,410 tps` | `7,265-7,419 tps` | `7,326 tps` | `7,113-7,450 tps` | `0.989` | `185 us` | `186 us` |

**Not separated, and not zero.** The ranges overlap in every cell, but the
spike's median is the lower in five of six and its server CPU a
transaction the higher in all six, by `1-5 us` (`0.2-1.4 %`). Three
repetitions of 6 s cannot tell a cost of that size from the spread. The
interleaved overhead measurement the project owes a milestone at its
close has not run.

## The bar (CQ-Q6 (a))

| clause | result |
|---|---|
| the spike's arm recovers at least half of the gap between hot and the piped last-writer arm | **met**: `100 %` at `k = 8`, `101 %` at `k = 2`, the ranges apart from hot's |
| every run's final balances equal the serial oracle's, no committed state below the floor | **met** as far as it was measured: every row of both relations equals what the sessions counted as committed in `52` cells of `52`. The floor was set far below any balance these cells reach, so the bound itself is exercised only by the test cells (`tests/column_reserve_rig_test.cpp`), not here |
| an unmarked relation is inside the spread | **inside the spread, with a consistent lean** (T5); not shown to be zero |

## Reading

- **The proof of concept holds**: a debit written first, in a transaction
  that then does eight more statements, costs its row nothing until
  `COMMIT`. What a client gets today only by reordering its transaction,
  it gets here without doing so, and with no decided-newer refusal.
- **It is not more than the client's remedy on throughput**: the reserved
  arm equals the piped arm at both `k`, and under `strict` both stop at a
  third of disjoint. What it adds is the case reordering cannot reach -
  T4's transaction that pauses after its debit, `136` commits against
  `18` - and the absent refusals.
- **Against S0's numbers** (`v3.4.7-13-g92449e65`: hot `1,571 tps`, piped
  `4,437 tps`, disjoint `4,402 tps` at `k = 8`): today's unmarked arms run
  `2-12 %` higher on the same host and schedule shape. The base binary
  beside them runs as high (T5), so the difference is the day's and CP's
  close on `main`, not the spike's. PostgreSQL's floor was not measured.

## What was cut, and what was not measured

- **Scope, by CQ-Q15 (a)**: a floor on one `int64` column; every other
  write of the column refused; a debit that does not fit behind
  undecided reservations refused at once (CQ-Q5 (b)). So **K9** (an
  ordinary writer of the column paying the reserve) and **K11** (the wait
  on the reserve) are not measured, and no cell here draws a balance
  near its floor.
- `strict`: one `k`, two repetitions. The pause: one cell each.
- Not measured: client CPU, `sys.ash`'s shares, a crash, a marked
  relation's unmarked statements.

## Run conditions

| | |
|---|---|
| Binaries | spike: `build-release/kds_server` at `3dea53c2`, copy hashed `7bc5e0fd316f572a...`; base: a release build of `e698fce8`'s tree (`git archive`), `0022ebc850d14ed0...`. Pass 1's spike binary: `36009c8cc879125f...` |
| Config | `cores = 4`, servers pinned to CPUs 2-5, drivers to 0, 1, 6, 7; `buffer_pool_frames = 65536`; `durability = group` (T1, T2, T4, T5) and `strict` (T3); ports 15550-15556, chosen per server |
| Mark | the marked servers ran with `KDS_CQ_SPIKE_MARK=cq_acct.c:-1000000000000` in their environment |
| Device | `/dev/nvme0n1p1`, ext4, under `/var/tmp/cq-s2-run/` |
| Host | 8 CPUs, 1-minute load `0.35` at the start. The quiet-host wait ran its full 480 s because its process pattern matched its own launcher's command line (the suite had just ended in the same shell); nothing was building or testing |
| Sessions' cores | recorded at both ends of every cell; `12` of `24` reserved-arm sessions and `11` of `24` hot-arm sessions at `k = 8` ended on another core than they started on (BL-S3) |
| Time | pass 1 5 min 10 s, pass 2 6 min 31 s: 11 min 41 s of the stage's 20 |
| Watchdog | 60 s, on the schedule's heartbeat file; `pass2/watchdog.txt` |
