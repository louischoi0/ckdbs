# DJ-S5 - the close: the opening commit against DJ's closing code, with DJ's keys unset and set

Measured 2026-11-20 at `v3.4.17-6-g5333fbb2` (`5333fbb2`, worktree
`dj-connections-sessions-cursors`, the code DJ closes with) against
`v3.4.17-1-g054dbc5e` (`054dbc5e`, the commit DJ opened from), for
`instructions/v3.2.0/workorder-dj-connections-sessions-cursors.md` §5
(DJ-S5), DJ-R1, DJ-R6 and DJ-R7. **No delta of DJ's is resolved on this
host: no arm's difference from A keeps its sign across the two runs, and
every one sits inside the A-to-A2 spread of one run or the other.**

## Setup

- Binaries, each built in `build-release` at its commit in this worktree,
  copied and hashed: **A** `054dbc5e`, sha256 `3df8801d61b6cbd8...`;
  **B** `5333fbb2`, sha256 `785e57e3d0e67824...` (in full in the archive).
- Arms: **A**; **B**, DJ's keys unset (DJ-R1: the engine as it was);
  **Bk**, B with `max_connections = 100000` and
  `idle_session_timeout_s = 3600` - every session seated and counted and
  the sweep armed, neither ever reached; **A2**, A again, the floor.
- Data file on `/dev/nvme0n1p1`, ext4; port 15697. `cores = 1`, the
  reactor pinned to CPU 4 (`reactor_cpus`), the server's threads to CPUs
  4-7, the driver to CPU 0. `durability = group`,
  `buffer_pool_frames = 65536`, `auth = off`. A 100,000-row relation
  `c (id int64, a int64, b int64, s varchar(32)) BTREE`, preloaded per
  server.
- Driver `tools/dj_close.py`. Per arm a fresh server; three cells of 3 s:
  a tuple key point `SELECT`, an autocommit single-row `INSERT`, and a
  connect, one point read and close loop. Four server lifetimes, the arm
  order rotated and reversed per lifetime. Each cell waits for a host with
  no compiler, linker or suite of another session, and is run again if one
  appeared during it: every cell below ran on its first attempt; the gate
  held 59.6 s and 0.4 s. Load average 1.1 to 6.7 at the cells' ends.
- **Two runs.** In the first, both of Bk's dear point cells were the first
  arm of their lifetime, and A read its dearest when it opened lifetime 1:
  a lifetime's first cell reads dearer here. The second run (`--lead`)
  opens every lifetime with a discarded A cell, so no measured arm runs
  first. Both are reported; neither is preferred.
- Cut to fit the budget: `cores = 1` only; no TLS; no SCRAM (the reserve
  and the auth-time seat are not on any path measured); no idle sessions
  beside the cells (DJ-S0 found 10,000 cost nothing readable on A). About
  12 minutes of arms and 12 minutes of release build.

## Server CPU per statement or connection (median of 4 lifetimes, range in brackets)

| run | cell | A | A2 | B | Bk |
|---|---|---|---|---|---|
| 1 | point `SELECT` | 30.66 µs (30.42-31.74 µs) | 30.78 µs (30.35-31.48 µs) | 30.38 µs (30.07-31.56 µs) | 32.48 µs (30.78-34.90 µs) |
| 1 | autocommit `INSERT` | 67.54 µs (64.40-68.19 µs) | 66.95 µs (64.56-71.06 µs) | 69.50 µs (64.56-72.57 µs) | 67.44 µs (65.13-74.07 µs) |
| 1 | connect, read, close | 88.44 µs (87.68-100.40 µs) | 92.98 µs (87.20-99.74 µs) | 89.89 µs (87.11-92.41 µs) | 92.93 µs (86.47-97.39 µs) |
| 2 (led) | point `SELECT` | 32.97 µs (32.08-33.59 µs) | 33.72 µs (32.86-36.74 µs) | 34.66 µs (32.61-36.56 µs) | 32.41 µs (31.25-32.90 µs) |
| 2 (led) | autocommit `INSERT` | 69.98 µs (62.43-70.75 µs) | 67.31 µs (59.35-72.26 µs) | 60.55 µs (59.14-73.05 µs) | 68.10 µs (61.20-71.05 µs) |
| 2 (led) | connect, read, close | 97.36 µs (86.96-111.88 µs) | 90.96 µs (88.35-99.95 µs) | 96.34 µs (87.80-103.15 µs) | 102.97 µs (100.72-106.75 µs) |

## Latency (mean of 4 lifetimes)

| run | cell | arm | p0 | p25 | p50 | p75 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|
| 1 | point | A | 65.0 µs | 70.3 µs | 71.2 µs | 73.3 µs | 80.3 µs | 114.2 µs |
| 1 | point | A2 | 64.6 µs | 69.8 µs | 70.8 µs | 72.7 µs | 81.7 µs | 114.2 µs |
| 1 | point | B | 65.4 µs | 69.6 µs | 70.5 µs | 72.2 µs | 88.8 µs | 102.3 µs |
| 1 | point | Bk | 64.6 µs | 70.0 µs | 73.7 µs | 82.2 µs | 108.4 µs | 136.6 µs |
| 2 | point | A | 64.5 µs | 70.4 µs | 73.5 µs | 80.4 µs | 135.3 µs | 159.1 µs |
| 2 | point | A2 | 63.8 µs | 73.5 µs | 79.9 µs | 90.1 µs | 140.3 µs | 159.7 µs |
| 2 | point | B | 61.3 µs | 73.1 µs | 78.9 µs | 99.8 µs | 139.3 µs | 157.3 µs |
| 2 | point | Bk | 63.9 µs | 70.3 µs | 71.7 µs | 77.7 µs | 114.2 µs | 154.6 µs |
| 1 | insert | A | 283.7 µs | 396.9 µs | 448.6 µs | 515.0 µs | 634.5 µs | 763.3 µs |
| 1 | insert | A2 | 280.3 µs | 391.0 µs | 441.7 µs | 509.3 µs | 640.6 µs | 1,424.2 µs |
| 1 | insert | B | 291.1 µs | 400.4 µs | 454.4 µs | 522.0 µs | 658.0 µs | 830.8 µs |
| 1 | insert | Bk | 298.8 µs | 407.2 µs | 458.6 µs | 524.5 µs | 658.0 µs | 826.8 µs |
| 2 | insert | A | 296.5 µs | 406.1 µs | 461.4 µs | 534.4 µs | 685.1 µs | 1,047.3 µs |
| 2 | insert | A2 | 290.1 µs | 400.8 µs | 451.4 µs | 516.0 µs | 650.0 µs | 807.7 µs |
| 2 | insert | B | 286.1 µs | 397.0 µs | 442.9 µs | 505.1 µs | 637.5 µs | 1,126.6 µs |
| 2 | insert | Bk | 292.5 µs | 396.8 µs | 446.1 µs | 510.6 µs | 637.3 µs | 824.7 µs |

The waits behind the latency are the group commit's for the `INSERT`
(`durability = group`) and none for the point read; no wait-event
breakdown was taken.

## What it says

- **The point read (DJ-R6).** B against A: -0.28 µs in run 1, +1.69 µs in
  run 2; A2 against A: +0.12 µs and +0.75 µs. Bk against A: +1.82 µs in
  run 1, where both of its dear lifetimes were a lifetime's first cell,
  and -0.56 µs in run 2, where none was. What DJ puts on a statement's
  path is a byte written in `SubmitStatement` while the timeout is set and
  a bool cleared in `StageStatement`; a 1 µs cost is not resolved from
  that, and was not found.
- **The write**: per-lifetime spreads of 6 to 14 µs, a median that moves
  9 µs between B's runs; nothing resolved. Its percentiles are level.
- **The connect loop (DJ-R7)**: B within A's and A2's ranges in both runs.
  Bk, which takes and gives back a seat per connection, read 4.5 µs above
  A in run 1 (level with A2) and 5.6 µs above A in run 2, where A2 sat
  6.4 µs below A: inside the floor of each run, its lean not excluded.
- **DJ-R1 holds in what was measured**: with the keys unset no cell moved
  outside the floor.

## Archive

`bench/v3.4.17/archive/dj-close-v3.4.17-6-g5333fbb2/`: `close.json` (run
1), `close-lead.json` (run 2), each run's driver log and status.
