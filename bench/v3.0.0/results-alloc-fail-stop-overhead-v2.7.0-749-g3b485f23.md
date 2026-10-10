# Allocation-failure fail-stop: interleaved A/B on the success path

**Thesis.** The change at `3b485f23` adds no work to a statement that succeeds:
`main()` installs a `std::set_new_handler` once, which `operator new` calls only when an
allocation fails, and `Coro::promise_type::unhandled_exception` becomes `[[noreturn]]
noexcept`, which runs only when an exception escapes a coroutine. The one thing that could
still cost something is code generation: with a `noreturn` handler the compiler may drop
the exception-cleanup path of every `Coro` frame. This run looks for that on the
coroutine-heavy shapes and finds nothing outside the harness's floor. Every short
statement is about 0.4 us (0.5 %) slower on binary B than on binary A, but a same-binary
control, where both servers run one binary, moves by up to +-0.5 us for no reason but the
server's slot, and `SHOW META` (no storage) shows the same 0.4 us as the pk `SELECT` and the
`INSERT`. The two coroutine-heavy statements (a correlated `EXISTS`, an equi-join) are
inside noise that is ten times larger. Verdict: **no regression within noise**. The expected
"perhaps marginally faster" did not appear either.

## Stamp

| | |
|---|---|
| Executed | 2026-10-10 23:40:24 - 23:49:29 UTC (main pass 23:40:24-23:43:18, swap pass 23:43:18-23:45:07, same-binary controls AA 23:45:52-23:47:40 and BB 23:47:40-23:49:29) |
| Branch | `worktree-fix-bad-alloc-reported-as-ok` (worktree `fix-bad-alloc-reported-as-ok`) |
| B (measured) | `3b485f23`, `git describe --tags` = `v2.7.0-749-g3b485f23`; tree clean when built (`git status` empty; the files this run adds are untracked) |
| A (baseline) | base commit `6b0d8412`, `v2.7.0-747-g6b0d8412` |
| Change under test | `git diff 6b0d8412 3b485f23`: `include/kds/sched/coro.hpp`, `src/sched/coro.cpp`, `include/kds/base/fail_stop.hpp`, `src/base/fail_stop.cpp`, `src/server/main.cpp` (+6 lines), tests and docs |
| Binary B | `build-release/kds_server` built 2026-10-10 23:40:00 UTC (after HEAD's commit time 23:38:27), `CMAKE_BUILD_TYPE=Release`, built by `nice -n 19 taskset -c 0-5` before any run; copy `sha256 23af8a1842bf450fae89ee406e5b04a546f55a85ad807f5157504d4149f4ce8e` |
| Binary A | `sha256 ee29d277d3b19939f1c93bdd57e789796c3a987dbd421deafc862afa2c7ef574`. **Provenance caveat:** reused, not rebuilt here. It is the scratch build of a base worktree at `1a5176ae` (`v2.7.0-746-g1a5176ae`), the parent of `6b0d8412`; `6b0d8412` adds bench and docs only (checked in an earlier stage with `git diff --name-only 1a5176ae 6b0d8412 -- src include CMakeLists.txt tests` listing 0 files), so its engine source is `6b0d8412`'s. The hash matched the copy kept in `/home/cdkbs/bench-runs/at/`; the original build's mtime was not recorded (that copy is dated 2026-10-10 20:34 UTC). It is older than B's commit by design: it is the base. |
| Copies | both servers started from copies in `/home/cdkbs/bench-runs/af/` (`kds_server-A`, `kds_server-B`), never from a build tree |
| Device | data files and logs under `/home/cdkbs/bench-runs/af/work/` on `/dev/root`, ext4 (`df -T`), not tmpfs. CPU: AMD EPYC 9V74, 8 CPUs visible |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, `log_level = warn`, BTREE relations, no other non-default key; fresh server and fresh data file per run |
| Pinning | both servers of a pair on CPU 6 (one idle while the other is timed), client on CPU 7 |
| Driver | scratch, in the archive's `drivers/` (`af_ab.py`, `af_run.py`, `analyze.py`, `render.py`); `tools/` untouched. Not added to `bench/docs/README.md` (rule 7): it is a one-stage scratch driver, the commands to reproduce are below. |

Reproduce, from a directory holding `kds_server-A` and `kds_server-B` (`OUT` in `af_run.py`):

```
python3 af_run.py 8 1000 res                  # main pass: slot A runs binary A, slot B binary B
AT_SWAP=1  python3 af_run.py 5 1000 res-swap  # swap pass: slot A runs binary B, slot B binary A
AT_SWAP=AA python3 af_run.py 5 1000 res-aa    # control: both slots run binary A
AT_SWAP=BB python3 af_run.py 5 1000 res-bb    # control: both slots run binary B
python3 analyze.py res; python3 render.py
```

Raw per-run JSON, gzip'd per-statement latencies, per-run host files, every pass's
`analysis.json`, the logs and the driver sources are archived in
`bench/v3.0.0/archive/alloc-fail-stop-overhead-v2.7.0-749-g3b485f23/`.

## Method

One "pair" is one run with a fresh server and data file per binary, identical data, and
every arm run in 16 blocks that alternate which server goes first, an equal count of
statements per server. 8 main pairs and 5 swap pairs; the swap pass gives the
position-free binary effect: with `d_main = slotB(binary B) - slotA(binary A)` and
`d_swap = slotB(binary A) - slotA(binary B)`, the binary effect is
`x = (d_main - d_swap) / 2` and the slot effect is `s = (d_main + d_swap) / 2`. Five more
pairs per binary with the same binary in both slots (AA, BB) measure what the pair design
reads when nothing differs.

| Cell | Statement | Rows | Statements per server per run |
|---|---|---|---|
| 1 | `SELECT * FROM t WHERE id = k` (pk point lookup) | `t`: 1,000 | 16,000 |
| 1c | the same arm repeated (in-run noise floor) | 1,000 | 16,000 |
| 2 | `INSERT INTO t VALUES (new id, g, 0)` | 1,000 loaded, grows by 16,000 | 16,000 |
| 2c | `INSERT INTO t2 ...`, same shape, repeated (noise floor) | 1,000 loaded | 16,000 |
| 3 | `SELECT id FROM a WHERE EXISTS (SELECT id FROM b WHERE b.a_id = a.id)`, correlated, returns 200 rows | `a`, `b`: 200 each | 1,600 |
| 3b | `SELECT a.id, b.v FROM a JOIN b ON b.a_id = a.id`, returns 200 rows | 200 each | 1,600 |
| 4 | `SHOW META` (control: no storage) | n/a | 16,000 |

Row counts map to the driver's flags: `--rows 1000` sets `t` and `t2`, `--sub-rows 200`
(default) sets `a` and `b`; `--ops 16000`, `--sub-ops 1600` set the statement counts. Every
statement, including `SHOW META`, is a `DispatchAsync` coroutine; cells 3 and 3b are the
ones that also walk the step VM's `co_await` chain. Rule 9 (sweep 200 / 1K / 10K) is
narrowed by the request: the storage cells run at 1,000 rows and the subquery cells at 200;
a change that adds no per-row work has no per-row term for a sweep to separate, but the
sweep was not run and that is not shown here.

Statistics. Percentiles are per run (nearest rank over that run's samples); tables give the
median over runs. A delta is B - A within a pair; "med [IQR]" is the median and interquartile
range over pairs.

### Host state

23 runs (8 main, 5 swap, 5 AA, 5 BB), every one started with no `cc1plus`, `cmake --build`,
`ctest` or `kds_tests` process running (`competing = none` in all 23) and no CPU other than
6 and 7 above 0.032 busy during the run (0.08 in the second before it; the exclusion line
was 0.5). CPU 6 ran 0.524-0.531 busy and CPU 7 0.582-0.592. Load average at start was
0.77-3.05 with CPUs 0-5 idle, i.e. load from other sessions outside the CPUs measured. The
one build (B) finished at 23:40:00, before the first run. **Excluded runs: 0 of 23.**

## Findings

### 1. No coroutine-path effect; every short statement shifts by the size of the floor

| cell | A QPS | B QPS | d p50 us (B-A), 8 pairs, med [IQR] | B>A pairs | swap d p50, 5 pairs, med [IQR] (B>A) | binary effect x (us) | x as % of A p50 |
|---|---|---|---|---|---|---|---|
| 1 pk point SELECT | 9,865 | 9,806 | +0.20 [+0.00, +0.70] | 5/8 | -0.80 [-0.90, -0.60] (0/5) | +0.50 | +0.5 % |
| 1c pk point SELECT, repeated | 9,333 | 9,834 | +0.25 [+0.00, +0.70] | 5/8 | -0.70 [-0.80, -0.60] (0/5) | +0.48 | +0.5 % |
| 2 INSERT | 13,179 | 13,071 | +0.40 [+0.30, +0.88] | 8/8 | -0.30 [-0.70, -0.10] (0/5) | +0.35 | +0.5 % |
| 2c INSERT, repeated | 13,297 | 12,875 | +0.50 [+0.48, +1.13] | 8/8 | -0.20 [-0.60, -0.10] (0/5) | +0.35 | +0.5 % |
| 3 correlated EXISTS (200 rows) | 1,262 | 1,265 | -2.60 [-3.00, +0.07] | 2/8 | +4.30 [+0.10, +4.50] (4/5) | -3.45 | -0.4 % |
| 3b equi-join (200 rows) | 1,673 | 1,648 | +0.90 [+0.20, +1.40] | 7/8 | -1.20 [-1.70, -0.50] (1/5) | +1.05 | +0.2 % |
| 4 SHOW META (control) | 13,139 | 13,077 | +0.50 [+0.05, +0.62] | 6/8 | -0.30 [-0.50, -0.20] (0/5) | +0.40 | +0.5 % |

QPS is derived (`1,000,000 / mean us`, median over the 8 main pairs); it carries the tail,
so a single slow run moves it (the 1c row's +502 QPS is a mean outlier, its p50 delta is
+0.25 us, the same as cell 1's). The effect is read from the p50 columns. The sign of B-A
in the main pass flips in the swap pass for every short cell, which is the signature of a
slot effect (`s` about -0.3 us), and `x` is what is left: +0.35 to +0.5 us on the four short
storage and control cells, in all of them the same size whether the statement is a read
(98 us), a write (72 us) or `SHOW META` (74 us). The two coroutine-heavy cells, where a
`co_await` chain would show if the exception-cleanup path mattered, read -3.45 us and
+1.05 us against per-pair spreads of 3-23 us; the swap pass for cell 3 swings from -23.0 to
+6.0 us across five pairs. Nothing there is separable from zero.

### 2. The noise floor, from inside the run

| control | statistic | value |
|---|---|---|
| same binary A in both slots (5 pairs), d p50 slotB-slotA | cells 1, 1c, 2, 2c, 4 medians | -0.50 to +0.50 us; per-run -0.7 to +0.8 us |
| same binary B in both slots (5 pairs), d p50 slotB-slotA | cells 1, 1c, 2, 2c, 4 medians | -0.50 to +0.30 us; per-run -0.7 to +0.8 us |
| same binary, cells 3 and 3b | per-run range | -4.5 to +2.7 us (3), -3.2 to +1.7 us (3b) |
| repeated arm inside the main pass | cell 1 vs 1c, cell 2 vs 2c, d p50 | +0.20 vs +0.25 us, +0.40 vs +0.50 us (the harness repeats itself to 0.1 us) |
| slot effect in the main/swap design | `s = (d_main + d_swap)/2` | about -0.3 us (cells 1-4) |

Same-binary pairs differ by up to +-0.5 us at the median and +-0.8 us run to run, with no
code difference. The binary effect `x` of +0.35 to +0.5 us on the short statements is
within that band, so it is not a finding. The 0.5 % is also the same on `SHOW META`, which
touches no relation, so it is not attributable to any statement path; if it is real it is a
fixed per-statement cost of the binary's layout (the dispatch and frame code is the same
for every statement), not a cost of the change's mechanisms, which are not executed on a
successful statement. Between the previous series' run of this same binary A
(`results-assertion-target-overhead-v2.7.0-749-g76335d6d.md`: pk `SELECT` 10,215 QPS,
`INSERT` 13,699 QPS, `SHOW META` 13,928 QPS) and this session's A (9,865; 13,179; 13,139),
the host's session-to-session drift is 3-6 %, ten times the A-to-B delta.

### 3. Where the time goes

A statement's latency is the engine's foreground poll plus the client/socket round trip;
with `durability = relaxed` and one connection there is no fsync wait at commit, no lock
or conflict wait, and no queue. The engine side is `sched_foreground_polled_us` per
statement (1.001 foreground polls per statement on every arm, so the poll is the statement;
`SHOW META`'s own meta reads supply the 0.001); the remainder is the client round trip,
including the Python client's own send/receive and decode, and is derived as p50 minus
engine.

| cell | p50 total A (us) | engine poll A (us) | round trip A, derived (us) | engine share | engine poll B (us) | engine d B-A main / swap (us) |
|---|---|---|---|---|---|---|
| 1 pk point SELECT | 97.6 | 32.7 | 64.9 | 33 % | 32.8 | +0.10 / -0.36 |
| 2 INSERT | 71.8 | 24.3 | 47.5 | 34 % | 24.6 | +0.40 / -0.14 |
| 3 correlated EXISTS (200 rows) | 792.2 | 474.9 | 317.3 | 60 % | 473.3 | -2.59 / +5.21 |
| 3b equi-join (200 rows) | 596.5 | 149.6 | 446.9 | 25 % | 150.0 | +0.13 / -2.00 |
| 4 SHOW META | 74.0 | 26.6 | 47.4 | 36 % | 26.9 | +0.29 / -0.16 |

The engine's own poll time, which is where a coroutine codegen change would land,
reproduces the same picture as the client's clock: +0.1 to +0.4 us in the main pass with the
sign reversed in the swap pass on every short cell, and -2.6 / +5.2 us on `EXISTS`. The
`EXISTS` statement is 60 % engine (475 us for a correlated subquery per outer row of 200;
the plan was not read with `ANALYZE`), the join 25 % engine (150 us; by the manual's
access-kind rule a pk probe per row, also not checked with `ANALYZE`) and 75 % the delivery
of 200 rows to a client that decodes them.

### 4. Distributions

Main pass, per-run percentile, median over 8 runs, microseconds, statements per server per
run in the `ops` column.

| cell | binary | ops/run | p0 | p25 | p50 | p75 | p99 |
|---|---|---|---|---|---|---|---|
| 1 pk point SELECT | A | 16,000 | 91.0 | 96.0 | 97.6 | 104.0 | 132.1 |
| 1 pk point SELECT | B | 16,000 | 91.8 | 96.6 | 98.1 | 104.6 | 132.2 |
| 1c pk point SELECT, repeated | A | 16,000 | 92.9 | 96.3 | 97.8 | 102.8 | 130.4 |
| 1c pk point SELECT, repeated | B | 16,000 | 92.8 | 96.8 | 98.2 | 103.1 | 130.2 |
| 2 INSERT | A | 16,000 | 68.5 | 71.0 | 71.8 | 74.7 | 101.8 |
| 2 INSERT | B | 16,000 | 67.8 | 71.7 | 72.5 | 75.4 | 102.7 |
| 2c INSERT, repeated | A | 16,000 | 68.6 | 71.0 | 71.9 | 74.8 | 101.0 |
| 2c INSERT, repeated | B | 16,000 | 69.0 | 71.8 | 72.7 | 75.4 | 102.2 |
| 3 correlated EXISTS (200 rows) | A | 1,600 | 764.4 | 785.5 | 792.2 | 797.8 | 833.7 |
| 3 correlated EXISTS (200 rows) | B | 1,600 | 761.7 | 782.2 | 789.5 | 795.8 | 841.9 |
| 3b equi-join (200 rows) | A | 1,600 | 571.0 | 589.7 | 596.5 | 604.6 | 628.9 |
| 3b equi-join (200 rows) | B | 1,600 | 572.4 | 590.6 | 597.3 | 605.3 | 638.5 |
| 4 SHOW META | A | 16,000 | 70.7 | 73.2 | 74.0 | 76.2 | 95.3 |
| 4 SHOW META | B | 16,000 | 69.8 | 73.4 | 74.4 | 76.4 | 95.7 |

B-A at each percentile (main median, swap median in brackets; us): cell 1 p0 +0.65 (-0.50),
p50 +0.20 (-0.80), p99 +0.50 (-1.40); cell 2 p0 +0.95 (-0.30), p50 +0.40 (-0.30), p99 +0.65
(+0.00); cell 3 p0 -1.85 (+5.00), p50 -2.60 (+4.30), p99 +5.40 (+13.20); cell 3b p0 +2.00
(-2.30), p50 +0.90 (-1.20), p99 +6.15 (-1.10); cell 4 p0 +0.20 (-1.00), p50 +0.50 (-0.30),
p99 +0.55 (-0.20). Full lists are in `render.out`. The tails move with the same
slot-dependent sign as the body, so the shape of each distribution is unchanged: no new
tail, no shifted floor (p0).

## What the run teaches

- The fail-stop change costs nothing measurable on a successful statement. Its two
  mechanisms (the new-handler, the `noreturn` unhandled-exception hook) are never executed
  on this path, and the one indirect cost it could have, `Coro` frame codegen, does not
  show on the two statements that walk the longest `co_await` chains.
- The expected "perhaps marginally faster" is not seen: the point estimate is 0.5 % slower
  on short statements and equal on the long ones, and both are inside a same-binary floor.
  A saving from dropping the cleanup path, if there is one, is below what this host
  resolves (about 0.5 us per statement, 0.5 % of a short one).
- The pair design's own floor is about +-0.5 us at the median, the same size as the largest
  A-B difference seen. A change smaller than 0.5 % of a 70-100 us statement needs more than
  13 pairs per cell to be read here, and a `SHOW META` with no storage on the path is as
  good a control as any for the fixed part of that floor.
- No limit was approached (rule 4b): this is a serial single-connection overhead A/B at
  `cores = 1` and `relaxed`; saturation, `strict` durability and 10,000-row sizes were not
  run. A conclusion about throughput at saturation or about the allocation-failure path
  itself (what happens when memory runs out) is not drawn here; the latter is the death
  test's job (`tests/bad_alloc_death_test.cpp`), not a benchmark's.

## Not measured

- **PostgreSQL floor:** not measured for these shapes (no twin was run; rule 4 allows
  saying so).
- **Prior kdbs number for the same shape:** the only earlier run of binary A is the
  `v2.7.0-749-g76335d6d` file quoted above, with a different host session; this file is the
  baseline the next run of these cells is read against.
- **Suite:** the correctness suite was not executed in this session (this stage measures a
  binary already built from `3b485f23`); the change's own death test is in the tree.
- **Sizes:** 200 / 10,000 for the storage cells, strict durability, more than one core.
