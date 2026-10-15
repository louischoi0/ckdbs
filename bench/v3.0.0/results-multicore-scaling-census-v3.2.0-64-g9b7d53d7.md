# Multi-core scaling census, KDS cores 1/2/4 against a same-session PostgreSQL 18 (`v3.2.0-64-g9b7d53d7`)

**Thesis.** At `9b7d53d7` (`v3.2.0-64-g9b7d53d7`), with `relaxed` durability, **four reactor cores still
deliver less than two on both shapes measured, at both session counts**: `cores = 4` reads 0.72-0.79 of
`cores = 2`, well outside the 0.8-7.1 % spread between repeats of one cell. BA-S17's finding
(`results-ba-s17-census-v2.7.0-736-ge5a7e1b7.md`) survives BL-S3 session placement, BP-S6's lock partitions and
BP/BQ, and the gap is wider than it was (BA-S17 at 16 sessions: 0.83 on `point`, 0.87 on `insert1`), because
`cores = 2` rose 11-29 % while `cores = 4` rose 0-11 %. The single-core and two-core cells are 10-44 % faster
than BA-S17's; this file does not attribute that to any change (different session, different commit, shorter
span). Against PostgreSQL 18 on the same CPU sets, KDS leads by 2.1-2.3x on one CPU, by 1.2-1.7x on two, and
**trails by 9-40 % on four**, because PostgreSQL still scales to four CPUs and KDS does not. PostgreSQL is the
floor (bench/README rule 4), so the four-CPU row is a place where the engine falls under it, not a verdict.

**This is a cut run.** The brief gave 20 minutes including the build. Two shapes (`point`, `insert1`), two
session counts (16, 8), `relaxed` only, 4 s measured spans. `trade`, `insertN` and `recent` were **not measured**
(section 5).

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-15, KDS and PostgreSQL cells interleaved by shape, 08:36-08:50 UTC (`archive/census-multicore-v3.2.0-64-g9b7d53d7/census.log`, per-run JSON) |
| Engine | `origin/main` = `9b7d53d7`, `git describe --tags` = `v3.2.0-64-g9b7d53d7`, commit time 2026-10-15 07:33:09 UTC. Built from `git archive 9b7d53d7` extracted into a scratch directory, not from a worktree, so the tree is the commit exactly (clean by construction) |
| Build | plain Release (`-DCMAKE_BUILD_TYPE=Release`, target `kds_server`), binary mtime 2026-10-15 08:33:05 UTC, after the commit |
| Measured copy | `kds_server.release`, sha256 `ed1e4fd476a9cb17d32204861f697b97483b9eb9f12aca7504f9215577d8b804` (`binary.sha256`, equal to the build output's hash; every run JSON carries the same `bin_sha256`) |
| Device | data files under `~/bench-runs/mc-v320/work`, `/dev/root` ext4 (`data_device` in every run JSON), not tmpfs |
| Host | 8 logical CPUs, 4 physical cores x 2 threads (siblings 0/1, 2/3, 4/5, 6/7), shared with other sessions |
| Pinning | as BA-S17: KDS `reactor_cpus` `0` / `0,2` / `0,2,4,6` for `cores` 1 / 2 / 4; PostgreSQL postmaster and children re-pinned to the same lists; clients on CPUs 4,5,6,7, one process per session |
| KDS config | `durability = relaxed`, `buffer_pool_frames = 65536` (driver default), every other key at its default |
| PostgreSQL | 18.6, `/home/cdkbs/pg-bench-ba`, port 15434, started in this session, tuning at defaults (`shared_buffers` 128 MB); the driver sets `synchronous_commit = off` per session (`--durability off`) |
| Driver | `tools/ba_census.py` at `9b7d53d7`'s tree (unmodified), `--runs 3 --seconds 4 --warmup 1 --client-cpus 4,5,6,7 --max-load 6 --max-wait 120`; invocation script `archive/.../census.sh`; documented in `bench/docs/` per rule 7 (not re-explained here) |
| Suite | not executed (no code change; measurement only) |

**Where this differs from BA-S17.** Span 4 s with 1 s warm-up (BA-S17: 8 s, 2 s) and 3 runs per cell (same);
fixed cell order `cores` 1, 2, 4 within a session count (BA-S17 same loop order; no rotation, so a position
effect is not separated from the core count); only 16 and 8 sessions; no contamination screen or re-run
round. Everything else (flags, pinning, shapes, preloaded rows) is BA-S17's. BP-BQ's cell (`cores = 4`, 8 sessions)
used a 20 s span.

**Row sweep (rule 9): not applicable to this driver.** `point` reads a preloaded 10,000-row relation
(`--rows 10000`) and a pk point read does not scale with rows; `insert1` appends to a relation that starts empty
and grows by the run's statement count. 200 and 1K rows were not run.

## 2. Throughput

Median of 3 runs, `relaxed` (KDS) / `off` (PostgreSQL), stmt/s. KDS columns are `cores`; PostgreSQL columns are
the CPU set. `k4/k2` is KDS `cores = 4` over `cores = 2`.

| shape | sessions | KDS k1 | KDS k2 | KDS k4 | k4/k2 | PG CPU 0 | PG CPUs 0,2 | PG CPUs 0,2,4,6 |
|---|---|---|---|---|---|---|---|---|
| point | 16 | 55,671 | 71,728 | 56,491 | 0.788 | 26,622 | 52,588 | 77,531 |
| point | 8 | 57,314 | 66,703 | 49,749 | 0.746 | 27,162 | 55,424 | 82,360 |
| insert1 | 16 | 58,953 | 89,463 | 67,294 | 0.752 | 26,313 | 51,282 | 74,034 |
| insert1 | 8 | 61,201 | 80,850 | 58,133 | 0.719 | 27,022 | 54,654 | 66,042 |

KDS over PostgreSQL at the same CPU count (a floor, not a reference):

| shape | sessions | 1 CPU | 2 CPUs | 4 CPUs |
|---|---|---|---|---|
| point | 16 | 2.09x | 1.36x | 0.73x |
| point | 8 | 2.11x | 1.20x | 0.60x |
| insert1 | 16 | 2.24x | 1.74x | 0.91x |
| insert1 | 8 | 2.26x | 1.48x | 0.88x |

**The answer to the operator's question.** Yes: `cores = 4` is below `cores = 2` in all four cells. At `cores = 4`
`point` at 16 sessions is level with `cores = 1` (56,491 against 55,671, +1.5 %, inside noise); at 8 sessions
and on `insert1` at 8 sessions it is below `cores = 1` (-13 % and -5 %). Only `insert1` at 16 sessions still
gains over `cores = 1` (+14 %). Nothing in these cells was client-bound (0 of 3 runs in every cell had a client
above 80 % of one CPU), no run recorded a refusal or an error, so the ceiling is the engine's or the CPU
sharing's (section 4), not the driver's.

### Noise floor

The between-run spread (max/min - 1 of 3 runs) is 1.4-2.5 % at `cores` 1 and 2 on `point`, 0.8-5.2 % on
`insert1`, and 1.7-7.1 % at `cores = 4`; PostgreSQL 0.2-3.1 %. Every k4/k2 gap above (21-28 %) is 3x the largest
spread. The deltas against earlier files in section 3 smaller than about 7 % are not findings.

### Latency of one statement

Median over the cell's 3 runs of each run's percentile, microseconds, three runs x the cell's statements
(point 16 sessions: 225,000 statements per run at `cores = 1`; counts per run are in the JSON `statements`).
`tables.md` in the archive carries the full table with the PostgreSQL rows and the per-run load; the KDS rows:

| shape | sessions | cores | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| point | 16 | 1 | 76.0 | 194.8 | 230.8 | 448.7 | 520.8 |
| point | 16 | 2 | 59.2 | 209.1 | 217.7 | 321.7 | 401.1 |
| point | 16 | 4 | 54.7 | 119.9 | 189.1 | 748.6 | 965.4 |
| point | 8 | 1 | 56.4 | 125.5 | 129.7 | 187.8 | 224.8 |
| point | 8 | 2 | 51.5 | 89.4 | 102.1 | 182.7 | 201.6 |
| point | 8 | 4 | 56.7 | 103.0 | 153.4 | 283.2 | 370.7 |
| insert1 | 16 | 1 | 64.2 | 170.3 | 185.2 | 383.3 | 455.5 |
| insert1 | 16 | 2 | 36.4 | 99.1 | 131.2 | 217.4 | 275.9 |
| insert1 | 16 | 4 | 31.4 | 81.3 | 120.8 | 544.7 | 919.9 |
| insert1 | 8 | 1 | 41.9 | 71.5 | 125.0 | 166.7 | 207.6 |
| insert1 | 8 | 2 | 35.5 | 58.3 | 65.9 | 118.6 | 142.3 |
| insert1 | 8 | 4 | 33.0 | 64.5 | 82.4 | 209.2 | 327.6 |

`cores = 4` has the best p0 and, at 16 sessions, the best p25 (119.9 and 81.3 us, against 209.1 and 99.1 at
`cores = 2`), yet the worst p95/p99 (748.6 / 965.4 and 544.7 / 919.9 us, 2-4x `cores = 2`'s). A fast body with a
long tail is the signature of work that is quick when it runs and waits when it is displaced; it is not the
signature of a path that is slow per statement. This is the shape CPU sharing would produce (section 4), but
the run did not test that.

## 3. Delta against earlier cells (different sessions and commits)

BA-S17 is `e5a7e1b7` (`v2.7.0-736-ge5a7e1b7`), 8 s spans, 16 and 8 sessions read from its
`tables_s17.md`/text; BP-BQ close is `3e77e7d1` (`v2.7.0-924-g3e77e7d1`), `cores = 4` only, 8 sessions, 20 s
spans. **Each comparison crosses a session, a commit range and a span length; none is a controlled A/B.**
The BA-S17 `cores = 4` 8-session cell is not in the text this file had; BP-BQ's is used for it. The `v2.7.0`
and `v3.2.0` numbering is the project's tag history; the 400-plus commits between are not itemised.

| shape | sessions | cores | this run | earlier | earlier source | delta |
|---|---|---|---|---|---|---|
| point | 16 | 1 | 55,671 | 41,560 | BA-S17 | +34.0 % |
| point | 16 | 2 | 71,728 | 64,908 | BA-S17 | +10.5 % |
| point | 16 | 4 | 56,491 | 53,816 | BA-S17 | +5.0 % (inside noise) |
| point | 8 | 1 | 57,314 | 43,158 | BA-S17 | +32.8 % |
| point | 8 | 2 | 66,703 | 59,073 | BA-S17 | +12.9 % |
| point | 8 | 4 | 49,749 | 58,916 | BP-BQ close | -15.6 % |
| insert1 | 16 | 1 | 58,953 | 42,151 | BA-S17 | +39.9 % |
| insert1 | 16 | 2 | 89,463 | 69,538 | BA-S17 | +28.7 % |
| insert1 | 16 | 4 | 67,294 | 60,473 | BA-S17 | +11.3 % |
| insert1 | 8 | 1 | 61,201 | 42,516 | BA-S17 | +44.0 % |
| insert1 | 8 | 2 | 80,850 | 64,228 | BA-S17 | +25.9 % |
| insert1 | 8 | 4 | 58,133 | 70,600 | BP-BQ close | -17.7 % |

PostgreSQL in this session against BA-S4's (`results-ba-s4-census-v2.7.0-725-gc390a624.md`, the numbers in
BA-S17's table): CPU 0, `point` 16 sessions 26,622 against 26,960 (-1.3 %), `insert1` 26,313 against 27,186
(-3.2 %); CPUs 0,2, `point` 52,588 against 54,433 (-3.4 %), `insert1` 51,282 against 54,628 (-6.1 %). The
floor read 1-6 % low today, so this session's host was not faster than the earlier one, and KDS's single-core
gain over BA-S17 is not a host effect that PostgreSQL shares.

The two `cores = 4` 8-session cells fall 16-18 % below BP-BQ's, which BP-BQ's own text called "within noise"
against its own A and Q arms. They differ in span (4 s against 20 s), session, and the other sessions running on
the host. This file cannot tell which of these carries the gap. The 16-session `cores = 4` cells are +5 and +11 %
over BA-S17's, so the two comparison points disagree in sign for `cores = 4`: **the `cores = 4` absolute is
the least settled number here, and the k4/k2 ratio is better supported than either k4 level.**

## 4. What the run says about the engine

- **A fourth and third reactor do not add throughput on these shapes, and they lengthen the tail.**
  `cores = 4` places reactors on CPUs 4 and 6, which are where the clients run (4,5,6,7) and the SMT
  siblings of the other clients' CPUs. At `cores = 2` the reactors (CPUs 0, 2) share no physical core with a
  client; BA-S4/BA-S17 and the driver's own docstring name `cores = 2` the largest clean cell. The tail
  inflation at `cores = 4` is consistent with reactor-versus-client CPU contention, **but PostgreSQL's
  postmaster is pinned to the same CPUs 0,2,4,6 with the same clients and rises to 74-83 k stmt/s**, so
  contention on those CPUs alone does not explain KDS's drop. Either KDS pays more for a displaced reactor than
  a PostgreSQL backend does (a reactor serves many sessions, so a preempted reactor stalls all of them, where a
  preempted backend stalls one), or `cores = 4` costs something cross-core that `cores = 2` does not. The run
  cannot separate the two: a clean test needs a host where four reactors and the clients fit on disjoint physical
  cores, or clients pinned away from CPUs 4 and 6.
- **The wait breakdown (rule 3) was not measured.** At `relaxed` there is no sync wait to account for; the
  server-side contention deltas (`meta_delta`, `contention_*`) are recorded in every run JSON and were not
  analysed in the time allowed. Rule 3 is therefore unmet for this file; the first thing to read in the archive
  is `contention_*` at `cores = 4` against `cores = 2`.
- **KDS's single core is at least 2.1x one PostgreSQL CPU on both shapes** and two cores are 1.2-1.7x two
  PostgreSQL CPUs; the engine is in the right league, as the floor asks, and loses it at four CPUs.
- **Contradiction with a stated expectation:** none checked; no document in this run's scope states a
  `cores = 4` target.

## 5. What was cut

| Cut | Why |
|---|---|
| `trade`, `insertN`, `recent` (all cores, both session counts, KDS and PostgreSQL) | 20-minute budget. `recent` k1 16 sessions had started when the budget ended; it was stopped and its partial files deleted. Nothing in this file speaks to those shapes |
| 1, 2, 4 sessions | not asked; the scaling curve below 8 sessions is not shown |
| `group` and `strict` durability | not asked; every KDS number here is `relaxed` |
| Span 8 s / warm-up 2 s (BA-S17's) | cut to 4 s / 1 s to fit; 3 runs kept per cell |
| Cell-order rotation, contamination screen and re-run round (BA-S17's) | no time; `cores` 1, 2, 4 ran in that order and the first cell of a shape ran at the lowest host load, so a position effect is not excluded |
| Rule 3 wait breakdown, rule 9 row sweep | see sections 4 and 1 |

## 6. Host-load caveats

- Host load average at each run's start is in `tables.md`. The first cells ran at 0.26-5; from the second
  `point` cell on every start read 4.5-6.0. That load is the census's own: 16 client processes plus the
  reactors are busy runnable tasks, and the driver's `host_before.competing` list (other benchmark and build
  processes) was **empty in all 144 run records**. `pgrep cc1plus` found nothing before the run and the gate
  (ctest/cc1plus) never waited.
- Other sessions' servers (`autotrade`, `build-sandbox`) were resident and idle. A test process of another
  session (`kds_tests`, 17 minutes old, 0.6 % CPU, last seen on CPU 6) was left running; the first launch of
  the census waited on it in the gate until it was removed from the gate's list, losing about 2.5 minutes of the
  budget. It was not stopped, and its effect on CPU 6 in the `cores = 4` cells cannot be excluded though its CPU
  share was under 1 %.
- The build finished at 08:33 and no build ran during measurement. The census's watchdog was a poll every
  60 s of the run's progress file (`/home/cdkbs/bench-runs/mc-v320/progress`); it found no orphan, only the
  gate stall above, which was resolved by restarting the run.
- The recorded `census.log` shows one `CELL FAILED` count of 0.

## 7. Reproduce

    git archive 9b7d53d7 | tar -x -C <dir>; cmake -S <dir> -B <dir>/b -DCMAKE_BUILD_TYPE=Release; cmake --build <dir>/b --target kds_server
    bench/v3.0.0/archive/census-multicore-v3.2.0-64-g9b7d53d7/census.sh point insert1   # needs ~/bench-runs/mc-v320 and a PostgreSQL 18 on port 15434

Raw JSON per run, the log and the table generator output are under
`bench/v3.0.0/archive/census-multicore-v3.2.0-64-g9b7d53d7/`.
