# Pending: four reactor cores deliver less throughput than two

Status: **PENDING** - the operator deferred it after the re-measurement.
Decides nothing, opens no stage.
Raised: 2026-10-15, in conversation with the operator.
Read at: `9b7d53d7` (`v3.2.0-64-g9b7d53d7`), which is also the measured
commit.
Related: `bench/v3.0.0/results-multicore-scaling-census-v3.2.0-64-g9b7d53d7.md`
(the re-measurement, with its cuts and host caveats);
`bench/v3.0.0/results-ba-s17-census-v2.7.0-736-ge5a7e1b7.md` (the first
reading); `bench/v3.0.0/results-bp-bq-close-v2.7.0-924-g3e77e7d1.md`
(`cores = 4` only); `instructions/v3.0.0/workorder-ba-parallelism.md`;
`instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`
(BL, paused); `docs/spec/crosscore.md`; `docs/spec/sched.md`.

## 1. What prompted it

`[operator]` While reviewing which benchmark cells trail PostgreSQL, the
operator asked whether the `cores = 4` below `cores = 2` reading came from
the latest code and from after the parallelism work: *"Is the case where
core 4 is lower than core 2 something you confirmed on the latest code?
Which benchmark is it? Is it after the parallelism improvements?"* It was
not: the reading was BA-S17's, at `e5a7e1b7` (`v2.7.0-736`), before BL-S3's
session placement, BP-S6's lock partitions and BP/BQ. The operator asked
for a re-measurement on the latest code (*"re-measure the core 2/4 scaling
on the latest code"*) and, on its result, deferred the question: *"leave
that issue in a pending state"*.

## 2. What the re-measurement shows

`ba_census`, `relaxed`, median stmt/s of 3 runs, PostgreSQL 18.6 with
`synchronous_commit = off` re-measured in the same session, at `9b7d53d7`:

| shape | sessions | KDS k1 | KDS k2 | KDS k4 | k4 / k2 | PG 1 CPU | PG 2 CPUs | PG 4 CPUs |
|---|---|---|---|---|---|---|---|---|
| point | 16 | 55,671 | 71,728 | 56,491 | 0.79 | 26,622 | 52,588 | 77,531 |
| point | 8 | 57,314 | 66,703 | 49,749 | 0.75 | 27,162 | 55,424 | 82,360 |
| insert1 | 16 | 58,953 | 89,463 | 67,294 | 0.75 | 26,313 | 51,282 | 74,034 |
| insert1 | 8 | 61,201 | 80,850 | 58,133 | 0.72 | 27,022 | 54,654 | 66,042 |

- `cores = 4` is 21-28 % under `cores = 2` in every cell, against a 0.8-7.1 %
  spread between repeats of one cell; at 8 sessions `point` falls under
  `cores = 1` (-13 %).
- At `cores = 4` p0 and p25 are the best of the three core counts and p95 and
  p99 are 2-4 times `cores = 2`'s: the throughput is lost in the tail.
- Against BA-S17 (different session and commit), `cores = 2` rose 10-29 %
  and `cores = 4` 0-11 %, so the k4 / k2 ratio is worse than BA-S17's
  (0.83 and 0.87 at 16 sessions).
- PostgreSQL scales to four CPUs on the same host and pinning; KDS trails it
  there by 9-40 % while leading it by 2.1-2.3x on one CPU and 1.2-1.7x on two.

## 3. What is not known

- **Whether it is the engine or the harness.** The host is 8 vCPUs on 4
  physical cores (siblings 0-1, 2-3, 4-5, 6-7). At `cores = 4` the reactors
  sit on CPUs 0, 2, 4, 6 and the clients on 4, 5, 6, 7, so two reactors share
  a CPU with a client and the other two share a physical core with one; at
  `cores = 2` no reactor shares a physical core with a client. PostgreSQL's
  backends were pinned to the same lists and still scale, which argues
  against the overlap being the whole story, but a backend blocks on its
  socket where a reactor polls, so the two do not react to a shared CPU
  alike. `[design]`
- **Where the tail comes from.** The run JSONs carry every `contention_*`
  counter per run; none was analysed, and no wait breakdown was produced.
- **The other shapes.** `trade`, `insertN` and `recent` were cut to the time
  budget, as were `group` and `strict`.

## 4. Options when it is taken up

1. **Separate the harness from the engine first** - a `cores = 4` cell with
   the clients on another host or on CPUs no reactor or sibling uses (on this
   8-vCPU host, `cores = 3` on 0, 2, 4 with clients on 6-7 is the largest
   clean cell), so the k4 / k2 question is asked of the engine alone. Cheapest,
   and it decides whether anything below is needed.
2. **Attribute the tail** - read the existing `contention_*` deltas per cell
   (page-latch spin turns, drain passes, carves, checkpoint runs, syncs
   inline) and the wake registry's skipped kicks, k2 against k4, and sample
   server CPU with `tools/kds_sampler.c` at `cores = 4`.
3. **Measure the remaining shapes** at the same commit, so the finding is not
   two shapes wide.

## 5. Raw data

`bench/v3.0.0/archive/census-multicore-v3.2.0-64-g9b7d53d7/` - the run
JSONs, `census.sh`, `census.log`, `tables.md` (p0-p99 per cell, load per
run) and the binary's sha256.
