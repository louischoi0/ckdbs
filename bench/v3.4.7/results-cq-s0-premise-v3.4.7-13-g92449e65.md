# CQ-S0 - the premise of column reservations: a row held by an explicit transaction

**Measured at `v3.4.7-13-g92449e65`** on `worktree-cq-lock-free-reservation`
(build inputs are `766f6a6d`'s: every commit after it is documents only),
2026-11-11, pass 2 at 10:10-10:18 UTC. Order:
`instructions/v3.2.0/workorder-cq-column-reservation.md` section 6, CQ-S0.
Raw outputs: `archive/cq-s0-premise-v3.4.7-13-g92449e65/`. The driver,
`tools/cq_s0_premise.py`, was not yet committed when it ran; it is
committed with this file, byte-identical to `archive/.../tools-at-run/`.

**Every number below is pass 2's.** Pass 1 (09:48-09:54 UTC, an earlier
driver, `k` in `{0, 8}`) is archived beside it and agrees: its `k = 8`
medians under `group` were hot `1,558 tps`, hot-last `4,230 tps`, piped
`4,229 tps`, disjoint `4,347 tps`. Pass 2 exists because the stage's
review found that pass 1's driver did not record the sessions' cores,
checked only the hot row, and ran the hot arm first in two repetitions of
three.

## What was asked

Whether writers of one row lose throughput to the row's hold inside an
explicit multi-statement transaction - the window a column reservation
would close - and how much of the loss a transaction that writes the row
last already gets back. No engine code is compared: one binary, and the
arms differ only in what the client sends.

The transaction, 8 sessions, 6 s a cell, three repetitions, each starting
from a different arm and the odd one run backwards:

    BEGIN
    UPDATE cq_acct SET c = c - 1 WHERE id = <target>     -- the debit
    UPDATE cq_work SET v = v + 1 WHERE id = <own row>    -- k of these
    COMMIT

| arm | the debit | what it stands for |
|---|---|---|
| hot | first statement, every session on row 1 | today: the row held across `k` statements and `k + 1` round trips |
| hot-last | last statement, row 1 | the client orders its transaction; one round trip inside the hold |
| hot-last-piped | last, in one socket write with `COMMIT`, row 1 | the order's A2 as its section 3 states it, and the hold C1's commit-time apply would have: from the write to the decide |
| disjoint | first statement, a row of the session's own | no row shared |
| disjoint-piped | last and piped, own row | the control for piping itself |

## How it was run

- **Binary**: `build-release/kds_server` built at `92449e65`, copied into
  each server's directory and started from the copy; sha256
  `dadb0b0763d477d3...` (whole in the sidecars), the same for both
  servers and both passes.
- **Server**: `cores = 4`, `taskset 2-5` with `reactor_cpus = 2,3,4,5`,
  `buffer_pool_frames = 65536`, ASH on; one server at `durability = group`
  (M1, M4'), a second at `strict` (M2). Data and WAL on `/dev/nvme0n1p1`,
  ext4 (`/var/tmp`). Ports 15540 and 15542, chosen. The launcher is
  CP-S0's, unmodified
  (`archive/cp-s0-premise-v3.4.7-1-gcc54a296/scripts/m1m4_srv.py`).
- **Driver**: one process per session (eight threads under one GIL would
  bound the client, not the server), pinned to CPUs 0, 1, 6, 7. **Those
  are the SMT siblings of the server's CPUs** (pairs (0,4) (1,5) (2,6)
  (3,7) on this box), so client and server share physical cores; every arm
  shares them alike.
- **Sessions' cores**: read from `SHOW META` at each session's start and
  end and kept per cell in the JSON. Every cell opens eight fresh
  connections, so the spread is redrawn per cell: of M1's 45 cells two
  drew an even 2-2-2-2, ten drew 1-2-2-3, eleven 1-1-2-4, and the most
  uneven put six sessions on one core. `205` of M1's `360` sessions ended
  their cell on another core than they began on (BL-S3 re-decides at each
  transaction's head). The spread is the placer's, not an arm's, and it is
  part of the spread between repetitions.
- **Host**: the schedule waited for no compiler or test process and a
  1-minute load under 1.5 (`scripts/wait_quiet.sh`, 145 s) and started at
  load `1.48`. Every cell records `/proc/loadavg` and the `pgrep` list of
  competing builds, which is empty in all 61; the load in the cells runs
  from `1.41` to `5.69`, which is the run's own eight clients and server.
- **Budget**: pass 2 took 7 min 10 s and pass 1 5 min 09 s, 12 min 19 s of
  the stage's 20; the build took about 4 min. A watchdog polled the
  heartbeat file each minute and never found it stale.
- **Oracle**: before and after every cell the driver reads every row of
  both relations. `cq_acct` fell and `cq_work` rose by exactly what each
  session counted as committed, row by row, in **61 cells of 61** - the
  disjoint arms and the work rows included. Every statement is also
  required to report one row affected.

## What bounds a cell

The server used at most `1.62` of its four cores in any cell (the `server
CPU` column: the process's CPU time over the cell). No cell is bound by
server CPU. A session's transaction is `k + 3` round trips and one commit
acknowledgement, and `COMMIT` alone is p50 `0.68-0.87 ms` under `group`:
the cells are bound by round trips and the sync. Whether the eight client
processes on four shared hyperthreads cap the disjoint arms was not
measured, so **disjoint is a floor on what the server could do, and the
shares of the gap below are shares of that measured figure**. The rows
also share leaf pages (nine `cq_acct` rows on one leaf): disjoint means no
row conflict, not no contention.

## M1 - `group`: throughput

| k | arm | committed TPS, the three repetitions | median | refused decided-newer | share of attempts | other refusals | oracle | server CPU |
|---|---|---|---|---|---|---|---|---|
| 0 | hot | `7,281` / `7,825` / `7,887 tps` | `7,825 tps` | `12,808 txns` | `8.5 %` | `0 txns` | exact, `3 of 3 cells` | `1.06 cores` |
| 0 | hot-last | `7,441` / `7,668` / `8,062 tps` | `7,668 tps` | `12,992 txns` | `8.5 %` | `0 txns` | exact, `3 of 3 cells` | `1.05 cores` |
| 0 | hot-last-piped | `7,794` / `8,304` / `8,597 tps` | `8,304 tps` | `21,780 txns` | `12.8 %` | `0 txns` | exact, `3 of 3 cells` | `0.98 cores` |
| 0 | disjoint | `8,905` / `9,078` / `9,502 tps` | `9,078 tps` | `0 txns` | `0.0 %` | `0 txns` | exact, `3 of 3 cells` | `0.88 cores` |
| 0 | disjoint-piped | `8,381` / `9,261` / `9,370 tps` | `9,261 tps` | `0 txns` | `0.0 %` | `0 txns` | exact, `3 of 3 cells` | `0.80 cores` |
| 2 | hot | `3,599` / `3,742` / `3,801 tps` | `3,742 tps` | `3,120 txns` | `4.5 %` | `0 txns` | exact, `3 of 3 cells` | `1.00 cores` |
| 2 | hot-last | `6,063` / `6,301` / `6,458 tps` | `6,301 tps` | `6,771 txns` | `5.7 %` | `0 txns` | exact, `3 of 3 cells` | `1.24 cores` |
| 2 | hot-last-piped | `5,992` / `6,751` / `6,756 tps` | `6,751 tps` | `11,126 txns` | `8.7 %` | `0 txns` | exact, `3 of 3 cells` | `1.22 cores` |
| 2 | disjoint | `6,179` / `6,356` / `6,823 tps` | `6,356 tps` | `0 txns` | `0.0 %` | `0 txns` | exact, `3 of 3 cells` | `1.01 cores` |
| 2 | disjoint-piped | `7,144` / `7,261` / `7,516 tps` | `7,261 tps` | `0 txns` | `0.0 %` | `0 txns` | exact, `3 of 3 cells` | `1.10 cores` |
| 8 | hot | `1,511` / `1,571` / `1,593 tps` | `1,571 tps` | `408 txns` | `1.4 %` | `0 txns` | exact, `3 of 3 cells` | `0.76 cores` |
| 8 | hot-last | `4,443` / `4,447` / `4,490 tps` | `4,447 tps` | `2,088 txns` | `2.5 %` | `0 txns` | exact, `3 of 3 cells` | `1.60 cores` |
| 8 | hot-last-piped | `4,317` / `4,437` / `4,460 tps` | `4,437 tps` | `2,719 txns` | `3.3 %` | `0 txns` | exact, `3 of 3 cells` | `1.54 cores` |
| 8 | disjoint | `4,277` / `4,402` / `4,410 tps` | `4,402 tps` | `0 txns` | `0.0 %` | `0 txns` | exact, `3 of 3 cells` | `1.49 cores` |
| 8 | disjoint-piped | `4,274` / `4,690` / `4,738 tps` | `4,690 tps` | `0 txns` | `0.0 %` | `0 txns` | exact, `3 of 3 cells` | `1.57 cores` |

- **The held row costs `64 %` of the throughput at `k = 8` and `41 %` at
  `k = 2`**: hot commits `1,571 tps` against disjoint's `4,402 tps`
  (`2.80x`), and `3,742 tps` against `6,356 tps` (`1.70x`). The
  repetitions do not overlap (at `k = 8` hot's best is `1,593 tps` and
  every other arm's worst `4,274 tps`).
- **Writing the debit last gets the whole gap back with no engine
  change**: hot-last is `4,447 tps` at `k = 8` and `6,301 tps` at `k = 2`,
  level with disjoint (`102 %` and `98 %` of the gap).
- **Piping the debit with `COMMIT` cannot be told apart from hot-last**:
  `4,437 tps` at `k = 8`, `6,751 tps` at `k = 2`, inside the spreads.
  Against its own control the piped arm is at `94.6 %` and `93.0 %`.
- **At `k = 0` there is no window to close**: hot is `86 %` of disjoint,
  hot-last is hot (the same shape), and the piped arm's `8,304 tps`
  overlaps both.
- **Every refusal is decided-newer** (`row id=N was written by transaction
  N`): no deadlock and no fault net in any M1 or M2 cell. It fails the
  whole transaction. Under `group` its share of attempts runs from `1.4 %`
  (hot, `k = 8`) to `12.8 %` (piped, `k = 0`), and **ordering the
  transaction raises it**: at `k = 8`, `1.4 %` hot, `2.5 %` hot-last,
  `3.3 %` piped.

### Transaction latency (begin to commit acknowledged; each cell the median of the three repetitions' percentiles)

| k | arm | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| 0 | hot | `447 us` | `796 us` | `925 us` | `1,507 us` | `2,088 us` | `14,302 us` |
| 0 | hot-last | `435 us` | `791 us` | `944 us` | `1,558 us` | `2,097 us` | `14,090 us` |
| 0 | hot-last-piped | `424 us` | `768 us` | `883 us` | `1,320 us` | `1,657 us` | `10,610 us` |
| 0 | disjoint | `458 us` | `732 us` | `833 us` | `1,239 us` | `1,672 us` | `7,219 us` |
| 0 | disjoint-piped | `424 us` | `730 us` | `818 us` | `1,170 us` | `1,538 us` | `27,291 us` |
| 2 | hot | `587 us` | `1,236 us` | `1,637 us` | `4,381 us` | `6,731 us` | `279,576 us` |
| 2 | hot-last | `565 us` | `976 us` | `1,163 us` | `1,704 us` | `2,558 us` | `13,393 us` |
| 2 | hot-last-piped | `542 us` | `867 us` | `1,039 us` | `1,504 us` | `1,972 us` | `286,412 us` |
| 2 | disjoint | `565 us` | `943 us` | `1,133 us` | `1,605 us` | `2,131 us` | `269,108 us` |
| 2 | disjoint-piped | `546 us` | `850 us` | `1,017 us` | `1,498 us` | `1,999 us` | `62,087 us` |
| 8 | hot | `879 us` | `1,878 us` | `2,655 us` | `16,402 us` | `30,691 us` | `275,733 us` |
| 8 | hot-last | `920 us` | `1,490 us` | `1,687 us` | `2,438 us` | `3,096 us` | `10,663 us` |
| 8 | hot-last-piped | `850 us` | `1,426 us` | `1,607 us` | `2,299 us` | `2,988 us` | `277,297 us` |
| 8 | disjoint | `834 us` | `1,457 us` | `1,648 us` | `2,396 us` | `3,125 us` | `278,458 us` |
| 8 | disjoint-piped | `875 us` | `1,436 us` | `1,623 us` | `2,337 us` | `3,089 us` | `18,572 us` |

### Where the time sits: the wait breakdown, as the client sees it

The debit statement alone, over debits that succeeded (a piped arm has no
separate debit):

| k | arm | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| 0 | hot | `45 us` | `113 us` | `191 us` | `624 us` | `1,026 us` | `8,858 us` |
| 0 | hot-last | `45 us` | `113 us` | `190 us` | `625 us` | `991 us` | `7,534 us` |
| 0 | disjoint | `46 us` | `81 us` | `96 us` | `168 us` | `390 us` | `5,842 us` |
| 2 | hot | `52 us` | `349 us` | `717 us` | `3,358 us` | `5,700 us` | `22,879 us` |
| 2 | hot-last | `38 us` | `75 us` | `130 us` | `452 us` | `749 us` | `5,267 us` |
| 2 | disjoint | `45 us` | `86 us` | `108 us` | `204 us` | `308 us` | `5,451 us` |
| 8 | hot | `49 us` | `631 us` | `1,331 us` | `14,806 us` | `28,724 us` | `90,970 us` |
| 8 | hot-last | `39 us` | `67 us` | `91 us` | `440 us` | `847 us` | `5,666 us` |
| 8 | disjoint | `45 us` | `80 us` | `96 us` | `191 us` | `410 us` | `6,930 us` |

`COMMIT` alone (for a piped arm, the debit and the commit together):

| k | arm | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| 0 | hot | `299 us` | `579 us` | `687 us` | `1,019 us` | `1,274 us` | `7,880 us` |
| 0 | hot-last | `293 us` | `568 us` | `676 us` | `1,015 us` | `1,299 us` | `10,592 us` |
| 0 | hot-last-piped | `393 us` | `719 us` | `833 us` | `1,240 us` | `1,550 us` | `10,573 us` |
| 0 | disjoint | `321 us` | `578 us` | `675 us` | `997 us` | `1,297 us` | `6,961 us` |
| 0 | disjoint-piped | `387 us` | `680 us` | `770 us` | `1,087 us` | `1,381 us` | `26,604 us` |
| 2 | hot | `299 us` | `574 us` | `678 us` | `997 us` | `1,392 us` | `279,248 us` |
| 2 | hot-last | `318 us` | `607 us` | `731 us` | `1,107 us` | `1,461 us` | `12,968 us` |
| 2 | hot-last-piped | `357 us` | `647 us` | `789 us` | `1,194 us` | `1,550 us` | `284,929 us` |
| 2 | disjoint | `319 us` | `629 us` | `768 us` | `1,165 us` | `1,634 us` | `268,575 us` |
| 2 | disjoint-piped | `357 us` | `634 us` | `754 us` | `1,144 us` | `1,468 us` | `61,133 us` |
| 8 | hot | `339 us` | `578 us` | `687 us` | `1,042 us` | `1,380 us` | `274,852 us` |
| 8 | hot-last | `350 us` | `684 us` | `818 us` | `1,245 us` | `1,602 us` | `8,921 us` |
| 8 | hot-last-piped | `393 us` | `732 us` | `868 us` | `1,319 us` | `1,670 us` | `276,435 us` |
| 8 | disjoint | `364 us` | `690 us` | `822 us` | `1,296 us` | `1,876 us` | `277,652 us` |
| 8 | disjoint-piped | `380 us` | `732 us` | `868 us` | `1,323 us` | `1,745 us` | `11,878 us` |

The hot arm's loss is all in the debit statement: at `k = 8` p50
`1,331 us` against `96 us` disjoint, p99 `28.7 ms`. `COMMIT` costs about
the same in every arm; CLA reads that as the group sync, which the cell
does not isolate.

### The lock family's own record

A sample: `sys.lock_waits` is a ring of 4096 a core, 16,384 for the
instance, and the hot cells fill it - a full ring holds each core's last
4096 waits, so its percentiles lean to the cell's end. Each percentile is
the median of the three repetitions' values.

| k | arm | new `sys.lock_waits` rows, per repetition | outcomes | unit/mode | waited p50 | waited p99 |
|---|---|---|---|---|---|---|
| 0 | hot | `16,384` / `16,384` / `16,384 rows` (ring full) | granted `49,152` | tuple/U | `53 us` | `335 us` |
| 0 | hot-last | `16,384` / `16,384` / `16,384 rows` (ring full) | granted `49,152` | tuple/U | `54 us` | `330 us` |
| 0 | hot-last-piped | `16,384` / `16,384` / `16,384 rows` (ring full) | granted `49,152` | tuple/U | `26 us` | `286 us` |
| 0 | disjoint | `0` / `0` / `0 rows` | none | - | - | - |
| 0 | disjoint-piped | `0` / `0` / `0 rows` | none | - | - | - |
| 2 | hot | `16,384` / `16,384` / `16,384 rows` (ring full) | granted `49,152` | tuple/U | `196 us` | `423 us` |
| 2 | hot-last | `16,384` / `16,384` / `16,384 rows` (ring full) | granted `49,152` | tuple/U | `50 us` | `199 us` |
| 2 | hot-last-piped | `14,961` / `16,228` / `16,187 rows` | granted `47,376` | tuple/U | `30 us` | `200 us` |
| 2 | disjoint | `0` / `0` / `0 rows` | none | - | - | - |
| 2 | disjoint-piped | `0` / `0` / `0 rows` | none | - | - | - |
| 8 | hot | `16,384` / `16,384` / `16,384 rows` (ring full) | granted `49,152` | tuple/U | `547 us` | `1,112 us` |
| 8 | hot-last | `15,118` / `15,252` / `15,013 rows` | granted `45,383` | tuple/U | `47 us` | `578 us` |
| 8 | hot-last-piped | `3,843` / `3,343` / `3,455 rows` | granted `10,641` | tuple/U | `21 us` | `350 us` |
| 8 | disjoint | `0` / `0` / `0 rows` | none | - | - | - |
| 8 | disjoint-piped | `0` / `0` / `0 rows` | none | - | - | - |

## M2 - `strict`, `k = 8`

| k | arm | committed TPS, the three repetitions | median | refused decided-newer | share of attempts | other refusals | oracle | server CPU |
|---|---|---|---|---|---|---|---|---|
| 8 | hot | `805` / `823` / `869 tps` | `823 tps` | `0 txns` | `0.0 %` | `0 txns` | exact, `3 of 3 cells` | `0.48 cores` |
| 8 | hot-last | `1,174` / `1,300` / `1,322 tps` | `1,300 tps` | `88 txns` | `0.4 %` | `0 txns` | exact, `3 of 3 cells` | `0.68 cores` |
| 8 | hot-last-piped | `1,222` / `1,233` / `1,295 tps` | `1,233 tps` | `80 txns` | `0.4 %` | `0 txns` | exact, `3 of 3 cells` | `0.66 cores` |
| 8 | disjoint | `3,785` / `3,827` / `3,874 tps` | `3,827 tps` | `0 txns` | `0.0 %` | `0 txns` | exact, `3 of 3 cells` | `1.38 cores` |
| 8 | disjoint-piped | `3,827` / `3,852` / `4,043 tps` | `3,852 tps` | `0 txns` | `0.0 %` | `0 txns` | exact, `3 of 3 cells` | `1.38 cores` |

- **Under `strict` the hold spans the sync, and writing the row last does
  not escape it**: hot-last is `1,300 tps` and the piped arm `1,233 tps`,
  `1.58x` and `1.50x` hot and a third of disjoint (`3,827 tps`). The piped
  arm's `COMMIT` p50 is `2,425 us` against `1,015 us` for its control.
  This is the order's K3, measured; a commit-time apply inherits it.
- hot is `823 tps`, `22 %` of disjoint (`4.65x`).
- Decided-newer refusals are rare here: none in hot, `0.4 %` in the two
  last-writer arms.

Transaction latency:

| k | arm | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| 8 | hot | `1,712 us` | `3,638 us` | `6,151 us` | `28,924 us` | `46,933 us` | `98,300 us` |
| 8 | hot-last | `928 us` | `1,671 us` | `2,692 us` | `23,275 us` | `43,150 us` | `115,173 us` |
| 8 | hot-last-piped | `957 us` | `1,983 us` | `3,009 us` | `22,474 us` | `39,931 us` | `98,458 us` |
| 8 | disjoint | `1,090 us` | `1,764 us` | `1,928 us` | `2,564 us` | `3,313 us` | `284,713 us` |
| 8 | disjoint-piped | `1,082 us` | `1,741 us` | `1,924 us` | `2,590 us` | `3,165 us` | `278,199 us` |

The debit statement:

| k | arm | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| 8 | hot | `246 us` | `2,430 us` | `4,914 us` | `27,656 us` | `45,825 us` | `92,913 us` |
| 8 | hot-last | `45 us` | `380 us` | `1,295 us` | `22,064 us` | `41,773 us` | `113,920 us` |
| 8 | disjoint | `52 us` | `103 us` | `123 us` | `195 us` | `257 us` | `6,114 us` |

`COMMIT`:

| k | arm | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| 8 | hot | `335 us` | `521 us` | `588 us` | `832 us` | `1,016 us` | `3,870 us` |
| 8 | hot-last | `339 us` | `610 us` | `709 us` | `1,009 us` | `1,242 us` | `5,685 us` |
| 8 | hot-last-piped | `463 us` | `1,443 us` | `2,425 us` | `21,916 us` | `39,434 us` | `97,884 us` |
| 8 | disjoint | `424 us` | `816 us` | `978 us` | `1,322 us` | `1,628 us` | `274,732 us` |
| 8 | disjoint-piped | `454 us` | `850 us` | `1,015 us` | `1,405 us` | `1,728 us` | `184,467 us` |

The lock family's record:

| k | arm | new `sys.lock_waits` rows, per repetition | outcomes | unit/mode | waited p50 | waited p99 |
|---|---|---|---|---|---|---|
| 8 | hot | `16,370` / `16,384` / `16,384 rows` | granted `49,138` | tuple/U | `1,119 us` | `1,773 us` |
| 8 | hot-last | `16,384` / `16,384` / `16,384 rows` (ring full) | granted `49,152` | tuple/U | `715 us` | `1,260 us` |
| 8 | hot-last-piped | `16,384` / `16,384` / `16,384 rows` (ring full) | granted `49,152` | tuple/U | `721 us` | `1,336 us` |
| 8 | disjoint | `0` / `0` / `0 rows` | none | - | - | - |
| 8 | disjoint-piped | `0` / `0` / `0 rows` | none | - | - | - |

## M4' - a holder that pauses 1.2 s (`group`, `k = 0`, hot, one 20 s cell)

| committed | TPS | refused at the fault net | `sys.lock_waits` | waited p50 | waited p99 |
|---|---|---|---|---|---|
| `18 txns` | `0.9 tps` | `134 txns` | `250 rows`: `116` granted, `134` `fault_net`, all `tuple/U` | `598 ms` | `1,001 ms` |

- **The row is used serially at its bound**: 20 s of 1.2 s holds is about
  17 transactions, and `18` committed. The fault net costs no throughput
  here; it costs **refusals**: `134` transactions waited up to 1 s and
  were refused, while `116` waits were granted.
- Eight sessions that did not share the row's hold would commit about one
  transaction per 1.2 s each, about `133` in the cell. Only a scheme under
  which the pausing holder does not hold the row reaches that - a debit
  written after the pause, or a reservation.

## What was cut, and what was not measured

- M4' is one cell, one repetition, interleaved with nothing: it shows a
  mechanism, not a rate.
- `app.lock_wait`'s share of DB time (`sys.ash`) and the exact wait count
  (`lock_waits_recorded` in `sys.meta`) were not read; the wait breakdown
  is the client's per-statement latency and the ring's sample.
- Client CPU was not recorded (see "What bounds a cell").
- `k` above 8, more than 8 sessions and `relaxed` were not run; the order
  asked for none of them.
- PostgreSQL's floor for this shape was not measured.
- **No delta against a previous number**: the shape is new. The nearest
  earlier cell is CP-S0's M4
  (`results-cp-s0-premise-v3.4.7-1-gcc54a296.md`): autocommit, an absolute
  `SET`, `relaxed`, `7.9 %` of hot statements refused decided-newer. This
  file's `8.5 %` at `k = 0` under `group`, in explicit transactions, is
  the same refusal at a like rate on a different shape.

## What it says about the engine

1. **A row written early in an explicit transaction is a real cost here**:
   `1.7x` at two following statements and `2.8x` at eight under `group`,
   `4.7x` under `strict`, and refusals at the fault net once a holder
   passes 1 s. CP-S0 could not see it: its cells were autocommit, or wrote
   the hot row under a short hold.
2. **Under `group` the cost is the client's statement order, and the
   client can remove all of it that this run can measure.** Writing the
   row last is level with writing rows nobody shares. A commit-time apply
   is that same order done by the engine, so on throughput it can add
   nothing to a client that reorders; its case is the client that cannot -
   a debit whose verdict the transaction needs before its other work, or
   a transaction that pauses after it.
3. **Under `strict` neither helps much**: the row is held across the sync
   and the last-writer arms reach a third of disjoint. CLA's inference,
   not a measurement: what would close that is in the commit protocol, not
   in when the row is written.
4. **The decided-newer refusal is the cost no ordering removes, and
   ordering makes it worse**: under `group` `1.4-12.8 %` of transactions
   on the hot row fail and are re-run whole by the client (under `strict`,
   `0-0.4 %`). A write applied at latest state would not be refused;
   neither would a re-run inside the engine (the order's A1), which needs
   no new structure.
