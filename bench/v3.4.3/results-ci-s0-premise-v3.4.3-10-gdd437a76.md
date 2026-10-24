# CI-S0 premise: a writer of a relation stalls for the whole `CREATE INDEX`, to the millisecond; a point reader does not stall

**Thesis.** `CREATE INDEX` holds its relation exclusively across the backfill. On a
10,000,000-row relation the build took 27.7-34.8 s, and the one autocommit
`UPDATE` of that relation that arrived 5 s into the run waited for the whole of it:
its latency was 27,738 / 28,404 / 34,776 ms, within 1 ms of the build statement's own
duration, in 3 of 3 builds. Every other writer statement of the run was a normal
~1 ms statement; the stall is one statement as long as the build. A point `SELECT`
by tuple key on the same relation was **not** stalled: 396k-496k of them completed
inside each build. The premise in
`instructions/v3.2.0/workorder-ci-online-index-build.md` §5 CI-S0 item 1 holds for
writers and, for point reads by tuple key, does not.

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-23, 11:05-11:09 UTC (runs A0 B0 A1) and 11:23-11:26 UTC (runs B1 A2 B2) |
| Worktree / branch | `/home/cdkbs/ckdbs/.claude/worktrees/online-index-build`, `worktree-online-index-build` |
| Commit | `dd437a76` (`v3.4.3-10-gdd437a76`), committed 2026-10-23 08:36:58 UTC |
| Tree | **dirty**: 3 `instructions/` files (before the run) and, from 10:40 UTC, engine and test sources (`include/kds/catalog/*`, `include/kds/server/superblock.hpp`, `include/kds/txn/lock_table.hpp`, `src/catalog/*`, `src/exec/*`, `src/server/command_dispatcher.cpp`, 5 tests) edited by another session in this worktree. None of that is in the measured binary (below). `tools/ci_s0_premise.py` is untracked |
| Binary | `build-release/kds_server`, mtime 2026-10-23 08:44:43 UTC (after the commit, before every source edit above), sha256 `316f0b283cdf9fb3...` (first 16 hex). Not rebuilt during the run (mtime checked unchanged). **Run in place, not from a copy** (the brief said no other build touches `build-release`) |
| Build type | Release |
| Device | data files in `build-release/ci-s0/` of the worktree on `/dev/root`, ext4 (`df -T`), not tmpfs |
| Server config | `cores = 4`, `durability = group`, `buffer_pool_frames = 65536`, every other key default; server pinned to CPUs 2-5, driver to CPUs 6-7 (host: 8 CPUs) |
| Why this config | `cores = 4` is the cell `bench/v3.4.1`'s BL close uses besides `cores = 1`, and keeps the build's reactor from being the only one the writer needs; `group` is the sample config's default, so an autocommit write pays its commit fsync as a real deployment's does |
| Driver | `tools/ci_s0_premise.py` (untracked; its docstring is the flag reference) |
| Prior number | **None for this shape on this engine.** This run is the baseline the next one is read against. PostgreSQL floor not measured for this shape |

**Host state.** Another session built and ran tests on this host throughout
(`clang++`, `cc1plus`, `kds_tests` from other worktrees), contrary to the brief. A
sampler wrote the compiler/test process count every 2 s (`host.log`) and a gate held
each start until none had run for 30 s. Two earlier attempts were discarded for it: one
at 10:49 UTC (a build began mid-run; its B run measured a 36.4 s build) and a second
at 11:05 UTC whose run-1 B (36.7 s build) and run-2 A overlapped a compile that
began at 11:08:40 UTC. The six runs reported here have no compiler or test process in
`host.log` for their whole span. The retaken run-1 B and the run-2 pair ran after a
30 s quiet gate; the runs were not one continuous rotation (the order is still
A B A B A B).

## 2. What was measured

Table `t (id int64, c1 int64, c2 int64, c3 int64) BTREE`, `id` the declared tuple key
(1..N), `c1` a scrambled value (`i * 2654435761 mod 1000003`, so the index is built
from unsorted input), `c2 = 0`. Loaded once through 500-row `INSERT`s (126.7 s for
N = 10,000,000) into a template volume, copied to a fresh data file and WAL directory
for every run; a fresh server per run, 500 warm-up updates, 1 s idle, then a 45 s window.

**N = 10,000,000.** Sized from one trial build on a 2,000,000-row table (3.90 s, too
short) and then taken at 5x: the build ran 27.7-34.8 s, a little over the 10-30 s asked
for (the cost per row grew from 1.95 us at 2M to 2.8-3.5 us at 10M).

| Arm | Sessions (each its own connection, all for 45 s) |
|---|---|
| A | writer: autocommit `UPDATE t SET c2 = c2 + 1 WHERE id = k`, k uniform random 1..N; reader: `SELECT * FROM t WHERE id = k` |
| B | the same two, plus a third session issuing `CREATE INDEX ix ON t (c1)` at t = 5 s |

The reader runs in both arms beside the writer; the writer baseline is therefore a
writer with a concurrent point reader, not a lone writer. The brief's "DROP INDEX
between runs" is met by a fresh volume copy per run. The windows are fixed in time, not in
completed statements, because the build's duration is the variable under test; the
writer's operation count is reported.

Rotation A B A B A B, 3 runs per arm, all statements' replies checked
(`UPDATED 1` / `SELECT`; no error or tag mismatch in any run). Row-set sweep (rule 9) was
**not** done: one cardinality (10M) plus the 2M trial, in which the writer's stall was
3.899 s against a 3.900 s build (one build, one writer, no reader; reported as
sizing only).

## 3. Writer latency: the stall is the build

| Arm / run | ops in 45 s | ops/s (derived: ops/45) | p0 ms | p25 ms | p50 ms | p90 ms | p99 ms | p99.9 ms | p100 ms |
|---|---|---|---|---|---|---|---|---|---|
| A r0 | 36,594 | 813 | 0.835 | 0.933 | 0.963 | 1.075 | 3.900 | 50.0 | 111.3 |
| A r1 | 35,964 | 799 | 0.832 | 0.933 | 0.961 | 1.095 | 4.508 | 49.6 | 112.3 |
| A r2 | 37,023 | 823 | 0.836 | 0.936 | 0.964 | 1.068 | 3.791 | 50.5 | 137.9 |
| **A pooled** | 109,581 | 812 (mean) | 0.832 | 0.934 | 0.963 | 1.077 | 4.034 | 49.8 | 137.9 |
| B r0 | 12,028 | 267 | 0.856 | 0.957 | 1.001 | 1.267 | 4.250 | 64.9 | **27,738.4** |
| B r1 | 10,449 | 232 | 0.863 | 1.005 | 1.201 | 1.370 | 4.149 | 96.3 | **28,403.5** |
| B r2 | 6,655 | 148 | 0.826 | 0.942 | 1.011 | 1.283 | 3.968 | 96.7 | **34,775.5** |
| **B pooled** | 29,132 | 216 (mean) | 0.826 | 0.965 | 1.049 | 1.318 | 4.209 | 94.7 | **34,775.5** |

The body of the distribution is a ~1 ms statement in both arms (A p50 0.961-0.964 ms,
B 1.00-1.20 ms), and p0 is 0.83-0.86 ms in both: the ~0.83 ms floor of an autocommit
write under `group` durability is the same with or without a build. Below p99.9 the
two arms differ by tenths of a millisecond; the p100 differs by four orders of
magnitude. Because the stall is one statement, no percentile below p99.9 can see it;
with 6,655-12,028 operations, one statement is under 0.02% of the sample, so even
p99.9 does not carry it (p99.9 is 65-97 ms in B, 50 ms in A, and is the checkpoint
tail, see below). The throughput drop (ops/s 812 to 216) is the stall's 27.7-34.8 s
dividing the window, not a slower statement.

### The stall against the build

| Run | Build start (s) | Build elapsed (s) | Writer max stall (s) | Stalled statement started at (s) | Stall / build | `sys.lock_waits` `waited_us` (s) | Wait / build | Writer statements started inside the build |
|---|---|---|---|---|---|---|---|---|
| B r0 | 5.00 | 27.738 | 27.738 | 5.00 | 1.0000 | 27.228 | 0.982 | 1 |
| B r1 | 5.00 | 28.404 | 28.404 | 5.00 | 1.0000 | 27.767 | 0.978 | 2 |
| B r2 | 5.00 | 34.776 | 34.776 | 5.00 | 1.0000 | 34.113 | 0.981 | 2 |

The writer's reply came within 1 ms of the build's (writer 27,738.423 / 28,403.522 /
34,775.532 ms; `CREATE` `total_us` in `sys.statement_stats` 27,738,022 / 28,404,018 /
34,776,264). The count of 1-2 writer statements "started inside the build" is the stalled
statement itself and, in r1 and r2, one statement that started in the build's last
milliseconds (r1: at t = 33.40 s, the build's end, taking 44 ms). The writer made no
progress between t = 5.00 s and the build's end.

The build's elapsed time ranges 27.7-34.8 s over three runs, a 25% spread on a host
the sampler shows quiet; A's ops/s spread is 3% (799-823). The cause of the 25% is not
established (run 2 was the last server of the rotation; not tested).

## 4. Wait breakdown

| Wait type | Measured here? | Value |
|---|---|---|
| Lock / conflict wait on the relation | yes: `sys.lock_waits`, 1 row per B run, `unit = relation`, `mode = IX` (what the writer asked), `outcome = granted`, `holder_trx` the build's | 27.23 / 27.77 / 34.11 s, 97.8-98.2% of the stalled statement |
| Rest of the stalled statement | no | 0.51 / 0.64 / 0.66 s outside the lock wait (statement start to the wait being registered, or grant to reply). `sys.lock_waits` and `sys.statement_stats` do not split it; a commit behind the build's log flush is a candidate, untested |
| Durability / commit (fsync) | not separable here | `sys.statement_stats` times a statement whole; no per-wait split of the group commit is exposed |
| Write statement / client and socket round trip | not separable here | A's writer p50 is 0.963 ms end to end; the view does not split it |
| Server time in the stall | `sys.statement_stats` `UPDATE`: `total_us` 44.6-45.0 s over the 45 s window, `max_us` 27.72 / 28.40 / 34.78 s | one session writing: 62-77% of the window's statement time is the one stalled statement |

The wait is one wait on one unit: a single `sys.lock_waits` row per run, no deadlock
and no fault-net outcome.

## 5. Readers: a point `SELECT` by tuple key is not stalled

| Arm / run | ops in 45 s | ops/s (derived) | p0 ms | p25 ms | p50 ms | p90 ms | p99 ms | p99.9 ms | p100 ms |
|---|---|---|---|---|---|---|---|---|---|
| A r0 | 807,658 | 17,948 | 0.039 | 0.049 | 0.050 | 0.069 | 0.087 | 0.122 | 6.60 |
| A r1 | 789,993 | 17,555 | 0.041 | 0.050 | 0.052 | 0.070 | 0.088 | 0.130 | 5.37 |
| A r2 | 807,305 | 17,940 | 0.042 | 0.049 | 0.051 | 0.067 | 0.085 | 0.118 | 6.54 |
| B r0 | 732,575 | 16,279 | 0.040 | 0.053 | 0.058 | 0.072 | 0.093 | 0.265 | 241.7 |
| B r1 | 681,864 | 15,153 | 0.038 | 0.053 | 0.057 | 0.071 | 0.096 | 0.312 | 1,217.3 |
| B r2 | 671,150 | 14,914 | 0.042 | 0.059 | 0.066 | 0.078 | 0.098 | 0.341 | 6.80 |

Inside the build window the reader completed 436,756 / 395,063 / 495,567 statements
(15,746 / 13,909 / 14,250 /s; 5 s before the build: 16,397 / 16,805 / 16,783 /s). No
reader waited on the relation: B's readers have no `sys.lock_waits` row, only the
writer's. The 4-17% throughput loss in the window is the reader sharing the server
with a backfill that burns about a core (server CPU, per run, B minus A: +22.8 / +19.0
/ +31.5 s over a 45 s window, with the writer idle in B); p50 moves from 0.051 to
0.057-0.066 ms. The B p100s (241.7 ms, 1,217 ms, 6.8 ms) are one or two statements each,
not at the build's start or end (r0 at t = 22.5 s and r1 at t = 29.7 s, inside the
5-33 s build; r2 at t = 39.1 s, after it), and are not attributed; the 1.2 s one is
not explained. **Not tested:** a reader that
scans or ranges over the relation, or a positioned/cursor reader. The order's sentence
that readers wait on the `X` is not supported by this arm for a point lookup by tuple
key; it is not refuted for the other read shapes.

## 6. After the build: a 0.3 s tail

In all three B runs the writer saw four to six statements of 0.19-0.35 s right after
the build's end (r2: 0.297, 0.346, 0.298, 0.301 and 0.206 s at t = 39.83, 40.13,
40.48, 40.78 and 41.12 s, the build having ended at 39.78 s), then ~0.1 s ones. A has
no statement over 0.14 s (p100 111-138 ms). The writer's rate over the rest of the
window after the build is 659 / 635 / 504 statements/s (r0 / r1 / r2), against A's
812. B r1 also has a 452 ms statement at t = 3.54 s, before the build began. This is
consistent with the checkpoint writing the
new index's pages (`checkpoint_last_pages` 1,068 / 1,032 / 8,193 after the build) but
the cause was not isolated: no checkpoint time series was taken.

## 7. The index CI-Q4's log-volume estimate needs

| Quantity | Value (3 builds, identical unless shown) |
|---|---|
| Rows N | 10,000,000 |
| Index | `ix` on `(c1)`, non-unique, no `COVERING` |
| `SHOW INDEXES` | `height = 3`, `entries = 10,000,000`, `key_width = 9`, `entry_width = 17` |
| `pages_allocated` (`SHOW META`) before / after | 81,694 -> 114,614 (r0, r1); 81,694 -> 114,610 (r2) |
| Pages the build allocated | 32,920 / 32,920 / 32,916 (delta of `pages_allocated`; the writer's own allocations in the window are inside it, bounded by 1-2 statements) |
| Entries per allocated page | 10,000,000 / 32,920 = 303.8 |
| Page size | 8 KiB inferred (the template file is 670,564,352 B at 81,694 pages = 8.2 KB/page); not read from source |
| Fill implied | about 63% of 481 (8,192 / 17), consistent with splits of unsorted input; the header size is not subtracted |

**Leaf count is not exposed** for a secondary index: `sys.indexes` carries `height` and
`entries`, `sys.relation_stats` `leaves` is the clustered tree's. The allocation delta
is leaves plus internal nodes for height 3; internal nodes are on the order of the
leaf count over the fan-out (~300+), i.e. under 150 pages, so leaves are ~32.8k
(arithmetic, not a measurement). At 17 B per entry the key payload is 170 MB; the
pages are 270 MB at 8 KiB. The `CREATED INDEX` reply tag printed `entries=0` in all three
10M builds (and in the 2M trial) while `SHOW INDEXES` read 10,000,000; that is an
observation for CI's reply text, not a result of this measurement.

## 8. What this teaches about the engine

1. **The `X` is held for the backfill's whole length, and a writer waits on one
   relation-unit lock for all of it.** One `sys.lock_waits` row, 97.8-98.2% of the
   stalled statement, no refusal: `lock_wait_fault_net_ms` was left at the value in
   `kds.conf.sample` (1000, default not re-read from the binary) and did not cut a
   28-35 s wait, so it is not a bound on this wait. A writer's visible cost of today's
   build is exactly the build's length, once.
2. **The backfill costs about a core and 2.8-3.5 us per row** at 10M, against 1.95 us
   per row at 2M (one trial build), so the build grew faster than N between the two
   sizes; two sizes do not fit a curve.
3. **Readers by tuple key do not use the `X`'s relation unit** in a way that blocks
   them. CI's design question for readers is therefore narrower than the order's
   sentence: it concerns the read shapes that walk the relation or the new index, not
   point reads.
4. **The floor and the noise.** A statement is 0.83 ms at best and 0.96 ms at p50 with
   `group` durability, and A's repeats agree within 3% (799-823 ops/s); the build's
   elapsed time does not (25% across three builds, unexplained). Treat a build time
   as a range of 28-35 s at this size on this host.
5. **No earlier number.** There is no prior kdbs result for this shape; this file is the
   baseline for CI-S2's online build, whose target is a writer stall near zero at the
   same N and the same arms.

## 9. Limits of this run

- One cardinality (10M), plus one 2M trial build; rule 9's 200 / 1K / 10K sweep was not
  done (a stall that equals the build is not a row-count-dependent finding, but the
  fixed/per-row split was not measured below 2M).
- Three runs per arm; A's repeats are the noise floor for the baseline. B has no
  repeat floor for the build time beyond its three values.
- The binary was run in place, not from a hashed copy; its mtime and hash are
  recorded, and the mtime did not change.
- The host was shared (see §1); the six reported runs overlapped no compiler or test
  process in the sampler's 2 s log, and two earlier attempts were discarded for it.
  Interleaving is A B A B A B but the last four runs were taken after a gap.
- Durability `relaxed`, other `cores` values, and a lone writer without the reader were
  not measured.
- The raw per-statement series (`final.json`, 206 MB) and the `host.log` are in
  `build-release/ci-s0/` of the worktree, not archived under `bench/`.

## 10. Reproduce

```
python3 tools/ci_s0_premise.py --bin build-release/kds_server \
  --workdir build-release/ci-s0 --rows 10000000 --window 45 --build-at 5 \
  --runs 3 --progress build-release/ci-s0/main.prog --out build-release/ci-s0/main.json
```

`--rows` is N (the table has N rows, tuple key 1..N). Defaults: `--cores 4`,
`--durability group`, `--pin-server 2-5`, `--pin-driver 6,7`. It loads the template
(about 2 min for 10M), then runs A B per run on fresh copies. `--reuse-template`,
`--run-offset`, `--arms` and `--keep-template` exist so an interrupted rotation can be
resumed, as it was here.
