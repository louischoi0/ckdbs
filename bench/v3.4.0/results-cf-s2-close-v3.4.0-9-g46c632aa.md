# CF-S2: the close of Branch O — Waystone records only opted patterns

Work order: `instructions/v3.2.0/workorder-cf-waystone-premise.md` (§8.3 expected result, §8.5 CF-S2).
Harness: `bench/v3.3.0/archive/cf-harness/` (`cf_s2.py` on `cf_s0.py`'s cells, `run.sh`, `compare.py`).
Raw files: `/home/cdkbs/bench-runs/cf/s2/` (18 `<arm>-r<rep>.json`; the set-aside first r2 under `r2-contaminated/`).

**Finding.** Branch O does what §8.3 expected of it.

- **With nothing opted** (`close` against `base`, where `base` is the engine before O, recording and
  replaying every shape), recording's cost leaves the statements that cannot use it:
  - `chain4`: +3.3 % throughput, -3.6 % server CPU, both outside the spread.
  - `chain3`: -2.6 % server CPU, outside the spread; its +1.2 % throughput is inside.
  - `pk`: +3.2 % throughput, outside the spread; its -5.1 % CPU is inside a 5.1 % spread.

  CF-S0 priced this cost at 2.7-3.9 % of server CPU (arm A against arm C). Against CF-S0's arm C
  itself (a separate run), `close` is within 1.1 % in throughput and server CPU on all three cells,
  inside the spread, which is §8.3's expectation as written.
- **The one shape where replay pays, opted** (`scanprobe` on `close-opt`), keeps it. Against `close`,
  which does not opt it, `close-opt` is +4.3 % throughput and -6.0 % server CPU. CF-S0's net was
  +5.0 % and -6.4 %. Against `base` it is level (-0.2 %, -0.3 %, both inside the spread), with
  `replays=200`.
- **Opting one shape costs the others nothing measurable.** `close-opt` against `close` on `pk`,
  `chain3` and `chain4` is inside the spread: +0.0 % to +1.4 % CPU, where the opted map's miss is the
  only added work.
- **The default changes for `scanprobe`.** A `scanprobe` nobody opts in now gives up the 4.2 % /
  5.4 % that `base` got for free. That is the trade Branch O makes on purpose.
- **Correctness checks hold.** Every cell's reply digest is identical across all 18 runs
  (invariant 8). `replays=` is exactly where it should be: `base` everywhere recordable, `close`
  nowhere, and `close-opt` on `scanprobe` alone.

## Stamp

| Item | Value |
|---|---|
| Date and time | 2026-10-21. Launched 12:20:39 UTC; the 18 runs finished 12:22:47-12:52:31 UTC, with the quiet gate holding runs for another session's builds and suites (waits up to 570 s per kept run, 710 s in the set-aside r2). Rotation r2 was run again at 12:51-12:52 UTC (below) |
| Worktree / branch | `cf-waystone-premise` / `worktree-cf-waystone-premise` |
| Close | `v3.4.0-9-g46c632aa` (CF-S1b, S1c and the instance-words fold). The tree at `v3.4.0-10-g46ca0729` (CF-S1d) changes comments only in C++ |
| Base | `v3.3.0-104-g722006ec`, the order's §8.5 base (Branch O's amendment, before any of its code). The engine source is exported by `git archive` into `/home/cdkbs/bench-runs/cf/base-722006ec/src` |
| Binaries | Release (`-DCMAKE_BUILD_TYPE=Release`), both built 12:19-12:20 UTC, copied to `/home/cdkbs/bench-runs/cf/kds_server.base` (sha256 `94e87e97b6795e253f4c2f73a2aba5cce4251eb7ac1c23d817f74876deb6a9cd`) and `kds_server.close` (sha256 `e9244f23119e74d72fd26989e1937e89dd61483e5e03e94027df8f67ffca6c0e`) |
| Device | `/dev/root`, ext4; a fresh data file per run under `/home/cdkbs/bench-runs/cf/data`, deleted after |
| Host | AMD EPYC 9V74, 8 CPUs. The quiet gate (`pgrep cc1plus\|cmake --build\|ctest \|kds_tests`, widened at this stage to catch a bare `kds_tests`) was empty at every run's start. **It samples only at a run's start**, and another session built and tested throughout. loadavg at run starts was 1.7-8.8 |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `waystone_recording = true`, `waystone_replay = true` on every arm. Server pinned to CPU 2, driver to CPU 3 |
| Arms | **base**: base binary, every shape records and replays (the engine before O). **close**: close binary, nothing opted. **close-opt**: close binary, `scanprobe` opted by `WAYSTONE ON <its example>` after the load |
| Order | (base, close, close-opt), (close, close-opt, base), (close-opt, base, close), repeated: 6 reps per arm |
| Watchdog | `run.sh`'s detached 60 s watchdog plus a session relay. No relaunch was needed |

## Method and every cut

- **Cells.** These are CF-S0's cells, unchanged (`results-cf-s0-premise-v3.3.0-97-g3d628df4.md`,
  Method):
  - `chain3`, `chain4`, `pk`: tuple-key lookups and probes.
  - `scanprobe`: a FilterScan, then two tuple-key probes, 100 rows and 200 trail entries per
    statement.
  - `control`: a FilterScan alone.
- **Sizes.** Working sets of 1,000 keys and 50 sessions, 22 passes, of which passes 1 and 2 are
  warm-up.
- **Metrics.** Throughput, server CPU per statement (every server thread's schedstat), a digest of
  the last pass's replies, and one `ANALYZE` for `replays=`.
- **Rotation r2 was run a second time.**
  - In the first r2 (12:23-12:38 UTC), `close-r2` ran `chain3` at 5,516 statements/s and `chain4`
    at 2,665/s, against 9,400-10,400/s in every other rep. Another session's compile started
    during it, which the start-only gate cannot see: the gate found one at the next run's start
    (`close-opt-r2` waited 160 s).
  - The whole rotation (all three arms) was set aside to `r2-contaminated/` and run again at
    12:51-12:52 UTC. The tables use the re-run.
  - In the re-run, `close-opt-r2`'s `control` read 2,178/s against 2,330-2,450/s elsewhere, which
    is why `control`'s close-opt spread is 10.8 %. `control` cannot replay and is inside its spread
    in every comparison, so it was left as measured.
  - `close-r6`'s `chain3` read 9,446/s at a normal 55.14 us of server CPU (wall time lost outside
    the server), which is why `chain3`'s close throughput spread is 9.5 %. It was left as measured;
    it is what puts `chain3`'s base/close throughput inside the spread.
- **The order's CF-S2 list** (§8.5: "if the budget fits; a cut is stated"):
  - **`scanprobe` at `cores = 2`: run.** See the section below.
  - **The refill cell** (rows opted, the word moved by root growth): **cut.**
  - **The mount's added cost** (a mount with rows opted against one with none): **cut.**

  The harness has no cell for either of the two cuts. The refill needs root growth driven while a
  read cell runs, and the mount cost needs many opted rows and a timed restart. Building and
  validating such cells did not fit what was left of the close's hour on a host shared with
  another session's builds and suites; the build and the runs had used about 47 minutes. Both stay
  in `known-gaps.md` as unmeasured.
- Latency is reported as p50 only (p99 is in the raw files). This is a cost-and-gain measurement,
  not a latency study.

## Per arm (median of 6 reps)

| cell | arm | reps | qps | spread | server CPU us/stmt | spread | p50 us | replays (ANALYZE) |
|---|---|---|---|---|---|---|---|---|
| chain3 | base | 6 | 10,074 | 2.1% | 56.72 | 2.0% | 95.6 | 3 |
| chain3 | close | 6 | 10,191 | 9.5% | 55.27 | 2.2% | 93.6 | 0 |
| chain3 | close-opt | 6 | 10,246 | 1.5% | 55.54 | 1.8% | 94.1 | 0 |
| chain4 | base | 6 | 9,451 | 2.5% | 60.80 | 2.7% | 101.5 | 4 |
| chain4 | close | 6 | 9,774 | 1.7% | 58.69 | 1.1% | 98.8 | 0 |
| chain4 | close-opt | 6 | 9,763 | 2.6% | 58.71 | 3.1% | 98.9 | 0 |
| pk | base | 6 | 12,214 | 3.1% | 42.52 | 5.1% | 78.8 | 1 |
| pk | close | 6 | 12,615 | 2.2% | 40.47 | 3.1% | 76.5 | 0 |
| pk | close-opt | 6 | 12,471 | 2.3% | 41.06 | 3.3% | 76.9 | 0 |
| scanprobe | base | 6 | 1,841 | 1.7% | 410.49 | 1.4% | 539.0 | 200 |
| scanprobe | close | 6 | 1,767 | 1.2% | 434.01 | 1.0% | 561.8 | 0 |
| scanprobe | close-opt | 6 | 1,845 | 3.2% | 409.27 | 2.8% | 536.0 | 200 |
| control | base | 6 | 2,412 | 1.7% | 321.37 | 2.3% | 406.4 | 0 |
| control | close | 6 | 2,428 | 1.0% | 317.66 | 0.8% | 402.0 | 0 |
| control | close-opt | 6 | 2,420 | 10.8% | 319.76 | 13.0% | 403.0 | 0 |

## The comparisons

A gain is positive when the first arm is better (more qps, less CPU). `~` means inside the larger
spread.

| cell | base/close qps | base/close CPU | base/close-opt qps | base/close-opt CPU | close/close-opt qps | close/close-opt CPU |
|---|---|---|---|---|---|---|
| chain3 | -1.2%~ | -2.6% | -1.7%~ | -2.1% | -0.5%~ | +0.5%~ |
| chain4 | -3.3% | -3.6% | -3.2% | -3.6% | +0.1%~ | +0.0%~ |
| pk | -3.2% | -5.1%~ | -2.1%~ | -3.5%~ | +1.2%~ | +1.4%~ |
| scanprobe | +4.2% | +5.4% | -0.2%~ | -0.3%~ | -4.3% | -6.0% |
| control | -0.7%~ | -1.2%~ | -0.3%~ | -0.5%~ | +0.4%~ | +0.7%~ |

Read as a gain of the second arm against the first: `close` beats `base` by 1.2-3.3 % throughput and
2.6-5.1 % server CPU on the three lookup cells (`chain3`'s throughput and `pk`'s CPU inside the
spread), and `close-opt` beats `close` on `scanprobe` by 4.3 %
throughput and 6.0 % server CPU.

Reply digests are identical across all 18 runs in every cell.

## `scanprobe` at `cores = 2`

Raw files: `/home/cdkbs/bench-runs/cf/s2-cores2/`.

**How it ran.**
- The same binaries, at `cores = 2`, with the server pinned to CPUs 2 and 4. Only `scanprobe` was
  measured, on two arms: `close` and `close-opt`.
- 6 reps each, in the rotation's order. The runs took 13:01-13:05 UTC, and the gate waited up to
  270 s.
- There is one connection, so the session runs on one core. The second core holds no session, but
  it shares the instance's opted count, the schema word and `sys.patterns`.

| cell | arm | reps | qps | spread | server CPU us/stmt | spread | p50 us | replays |
|---|---|---|---|---|---|---|---|---|
| scanprobe | close | 6 | 1,763 | 7.3% | 456.54 | 3.7% | 553.0 | 0 |
| scanprobe | close-opt | 6 | 1,834 | 7.2% | 426.82 | 5.5% | 538.8 | 200 |

**Result.**
- Opted against not opted: +3.9 % throughput, inside the 7.3 % spread, and -7.0 % server CPU,
  outside the spread.
- Reply digests are identical across the 12 runs.
- Server CPU per statement is higher than at `cores = 1` (434 and 409 us). The sum counts every
  server thread, the second core's included. The difference is not attributed.

The opted gain carries to an instance with more than one core. It reaches only the core the session
runs on, so this cell does not test a peer core replaying a trail another core recorded.
`tests/waystone_across_cores_test.cpp` covers that for correctness, not for cost.

## What the numbers say about the engine

- **Recording was the whole cost of the old default.** On the lookup cells, `close` (nothing opted)
  lands where CF-S0's arm C (both keys off) did against arm A: about 1.5-2.1 us of server CPU per
  statement saved (CF-S0: 1.6-1.7 us). That is the sighting count, the directory walk and the trail-page read every
  re-recorded execution paid. The gate now costs a not-opted statement one atomic load while nothing
  is opted.
- **An opted map that is not empty is cheap for everyone else.** With `scanprobe` opted, the lookup
  cells pay a hash miss in a one-entry per-core map, plus a fill once per cache generation. That
  stays inside the spread (+0.0 to +1.4 % CPU). The refill under a moving schema word was not
  isolated (cut above).
- **Replay's gain is kept exactly where it lives.** `scanprobe` opted is level with the old engine,
  which recorded it anyway. The gain, about 0.12 us of net server CPU per replayed probe (24.7 us
  over 200), is the same shape CF-S0 found (0.14 us, arm A against arm C).
- **The trade is visible and deliberate.** A shape that would pay (many tuple-key probes per
  statement) now pays only once an operator opts it in. ANALYZE's `fetch_id` and `replays=` are the
  means to find it (manual, `WAYSTONE`).
