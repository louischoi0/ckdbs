# DX-S2's dirty-pool cell: the client's refusals fall from 2.0-2.2 % of point updates to 2 in 173,915, because a refused write drains the dirty queue and runs again

**Thesis.** Measured on worktree `dx-pool-under-pressure`, DX-S2 at
`5e25f1d26` (`v3.4.17-205-g5e25f1d26`) against DX's base at `481d101aa`
(`v3.4.17-202-g481d101aa`), interleaved. The cell is DX-S0's
(`results-dx-s0-census-v3.4.17-202-g481d101aa.md` §2), unchanged.

- **The client's refusals: 746-2,050 per 6 s lifetime on the base, 0-1 on
  DX-S2.** As a share of statements, that is 2.00-2.24 % against 0-0.004 %.
  The base arm reproduces DX-S0's 2.26-2.49 % of the same binary, a little
  lower.
- **The pool refuses less, not only the client** (`pool_refused`, which
  counts every refusal, re-run ones included): 1,120-2,902 a lifetime on
  the base, 6-27 on DX-S2. Inline reclaim batches fall from
  40,195-105,354 to 1,276-5,905.
  - The mechanism, read from the counters: a refused statement's drain
    writes the queue its walk filled before the next reservation asks, so
    the next walk meets clean frames. On the base the queue waits for core
    0's 50 ms tick, and every walk in between is refused or re-walks.
  - What still reaches the client is a window's open refused where the
    drain wrote nothing or the deadline had passed (`known-gaps.md`, "A
    full pool still refuses").
- **No answer moved**: 0 statements affected other than one row, in every
  lifetime of both arms.
- **Throughput is not compared.** Updates per 6 s range 35,773-89,489 on the
  base and 18,280-74,153 on DX-S2. The cell's lifetime-to-lifetime
  variance is 3.1 times on one binary (DX-S0 §2) and is not explained, so
  two lifetimes an arm say nothing about cost. DX's overhead is the
  milestone close's interleaved A/B (DX-S3), not this cell's.

| field | value |
|---|---|
| date executed | 2026-11-27, 14:33:34-14:34:26 UTC |
| branch / commit | `worktree-dx-pool-under-pressure` at `5e25f1d26` (`v3.4.17-205-g5e25f1d26`); base `481d101aa` (`v3.4.17-202-g481d101aa`) |
| work order | `instructions/v3.2.0/workorder-dx-pool-under-pressure.md` §5, DX-S2 |
| builds | Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, each from `git archive` of its commit (rule 1). DX-S2's: `build-dx-s2/rel`, 32 s. The base's: DX-S0's, reused |
| binaries | copies (rule 3): base `build-dx-base/run/bin/kds_server`, sha256 `17cda4082ac1714e5ba146bf72b212f50593de24aead4a4a87b61fb736cce0c7` (DX-S0's); DX-S2 `build-dx-s2/run/bin/kds_server`, sha256 `926719225d557275ae56ddbfa83385eb5dd5b7a6634388cfa71f3a9bb35ec25e` |
| device | `/dev/mapper/fedora-root`, xfs (rule 2); the data file opened without `O_DIRECT` |
| cell | `tools/dm_s1_check.py`, unmodified, on a fresh copy of DX-S0's cold base (`build-dx-base/run/cold_base`, 1,000,000 rows) per invocation: `buffer_pool_frames = 2048`, `cores = 1`, 4 writer sessions, random-key `UPDATE ... WHERE id = k`, 6 s a lifetime, `--arms 0,0` (two lifetimes an invocation; `DM_MEMO_ENTRIES` is read by nothing at either commit) |
| order | base, DX-S2, base, DX-S2 (`archive/.../ab.sh.as-run`) |
| port | 15781 (rule 5) |
| host | load 2.43-2.99 (1 min) before each lifetime, from `/proc/loadavg` in each JSON (rule 4); no build or suite process. Two other `kds_server` processes were up: firestrike's instance, and one under `/sts/kds.conf` at 2.3 % CPU over its life |
| watchdog | a 60 s poll of `progress.txt`; it saw no stall |
| raw | `bench/v3.5.0/archive/dx-s2-dirty-pool-v3.4.17-205-g5e25f1d26/` |
| time | the Release build 32 s, the cell 52 s; with the suite and its flake census, about 15 min in all, inside the 20 min stage budget |

## 1. The cell, per lifetime

`refused, client` is the refusals the sessions were answered with;
`window` and `reservation` split it by message ("cannot hold another
mutation's" for the window's open, "N resident" for `ReserveFrame`).

| invocation | arm | lifetime | updates (rows affected = 1) | rows affected ≠ 1 | refused, client (window / reservation) | share of statements | `pool_refused` | inline reclaim batches | dirty frames queued | dirty frames drained |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | base | L1 | 84,170 updates | 0 updates | 1,850 refusals (1,546 / 304) | 2.15 % | 2,458 refusals | 90,433 batches | 75,058 frames | 75,058 frames |
| 1 | base | L2 | 89,489 updates | 0 updates | 2,050 refusals (1,624 / 426) | 2.24 % | 2,902 refusals | 105,354 batches | 79,745 frames | 78,996 frames |
| 2 | DX-S2 | L1 | 55,385 updates | 0 updates | 0 refusals | 0 % | 20 refusals | 4,343 batches | 48,976 frames | 48,976 frames |
| 2 | DX-S2 | L2 | 26,095 updates | 0 updates | 1 refusal (1 / 0) | 0.004 % | 6 refusals | 1,570 batches | 21,939 frames | 21,939 frames |
| 3 | base | L1 | 35,773 updates | 0 updates | 746 refusals (559 / 187) | 2.04 % | 1,120 refusals | 40,195 batches | 32,034 frames | 32,034 frames |
| 3 | base | L2 | 50,209 updates | 0 updates | 1,023 refusals (873 / 150) | 2.00 % | 1,323 refusals | 49,175 batches | 45,043 frames | 45,043 frames |
| 4 | DX-S2 | L1 | 18,280 updates | 0 updates | 0 refusals | 0 % | 6 refusals | 1,276 batches | 16,212 frames | 16,212 frames |
| 4 | DX-S2 | L2 | 74,153 updates | 0 updates | 1 refusal (1 / 0) | 0.001 % | 27 refusals | 5,905 batches | 64,677 frames | 64,677 frames |

- **Against this engine's previous number for the same cell** (DX-S0 §2, the
  same base binary): 2.26-2.49 % then, 2.00-2.24 % now on the base arm,
  and 0-0.004 % on DX-S2.
- **Against DM-S1's base arm** (`results-dm-s1-ceiling-v3.5.0-89-gc003e7fe.md`
  §3, another host and commit): 0.37-0.55 %. Recorded, not attributed.
- `pool_refused` exceeds the client's count on DX-S2 by 6-26 a lifetime:
  refusals a re-run absorbed. On the base it exceeds it by 300-852, which
  DX-S0 §2 left unattributed.

## 2. What this does not measure

- **Cost.** No interleaved CPU A/B, and two lifetimes an arm. The
  milestone's close (DX-S3) measures DX-R5, that a pool which is not full
  pays nothing.
- **More than one core.** The cell runs at `cores = 1`, where the base's
  drain waits on core 0's tick and no other core drains.
- **A pool of pinned or resident-class frames**, where the drain writes
  nothing and the refusal stands.
