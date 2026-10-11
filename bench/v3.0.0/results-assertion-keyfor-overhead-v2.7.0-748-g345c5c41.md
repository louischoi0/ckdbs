# Assertion group-key coercion (`KeyFor`) — what it costs a write, `345c5c41` against `1a5176ae`

The interleaved A/B overhead measurement of one small change: `KeyFor` in
`src/exec/assertion_check.cpp` copies each `GROUP BY` value of a write and runs
`exec::CoerceLiteralToColumn` on it before `EncodeGroupKey`, on the admission and
reserve paths of `INSERT`, `UPDATE` (old and new row) and `DELETE`. It runs only
for statements on a relation that has an assertion. **B − A is that change
alone**: `git diff --stat 1a5176ae 345c5c41` is the `KeyFor` hunk, the
`group_columns` member it reads (header and catalog), a spec paragraph, test
edits and a bug-entry deletion.

**The short answer.** The change adds no cost resolvable above the run's noise
floor (about ±0.5 µs, 1 % of a 50 µs statement) on the asserted INSERT, UPDATE
and DELETE of an integer group column, and on a `date` group column it makes the
asserted `UPDATE` **3.8–3.9 µs faster at 200 and 1,000 rows** (7 %); the
reading is that A keyed one value two ways and paid for it (the mechanism was not
instrumented). One reading sits at the
floor's edge and is stated, not dismissed: the integer-group asserted `UPDATE`
at 200 and 1,000 rows reads **+0.5 / +0.6 µs** (+1.0 / +1.2 %; 5 and 6 of 7 runs
positive) against its own no-assertion control at +0.2 / +0.2 µs; at 10,000 rows
the same statement reads −0.4 µs. It is not a result at the noise floor this run
established, and it is the only cell that leans toward a cost.

## Stamp

| item | value |
|---|---|
| executed | 2026-10-10, 15:19 to 15:56 UTC (42 pair runs retained, 7 per cell over 6 cells, plus one failed pair that was discarded) |
| branch / worktree | `worktree-fix-assertion-literal-group-key`, worktree `fix-assertion-literal-group-key` |
| **B** | `345c5c41` (committed 2026-10-10 15:10:14 UTC), `git describe --tags` = `v2.7.0-748-g345c5c41`, tree clean when built |
| **A** | `1a5176ae` (committed 2026-10-10 04:29:54 UTC), `v2.7.0-746-g1a5176ae`, detached worktree under the session scratchpad, removed afterwards |
| build | `Release` (`CMAKE_BUILD_TYPE:STRING=Release` in both `build-release/CMakeCache.txt`), target `kds_server`, each built from its own tree by a copy of `scripts/build-release.sh` that takes the root as an argument (`scripts/build-release.sh` is absent from B's branch; the copy also tolerates a first build with no `CMakeCache.txt`, which the original's `set -e` does not). Built one after the other, alone from this session, before any measurement; nothing of this session's was built during the runs |
| binary provenance | source binaries built 2026-10-10 15:13:13 UTC (B) and 15:14:28 UTC (A), after their commits, so neither is older than the commit it names |
| measured binaries (copies) | `kds_server-A` sha256 `ee29d277d3b19939f1c93bdd57e789796c3a987dbd421deafc862afa2c7ef574`; `kds_server-B` sha256 `712e904fc811cb54be835ab1971293f3a936f8b32f441d99c2ecd3f66eb10dab`; both copied to `/home/cdkbs/bench-runs/keyfor/` before the first cell and started from there |
| device | data files under `/home/cdkbs/bench-runs/keyfor/` on `/dev/root`, ext4 (`df -T`), never tmpfs |
| server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072` (the sample's value), `log_level = warn`, BTREE relations, one session per server; ports **15600 (B) and 15601 (A)**, never 15432; each server pinned with `taskset -c 2`, the client with `taskset -c 4`; **a fresh server and data file per (group type, rows, run)** |
| tests | the `tests/` suite was **not executed** by this measurement. Correctness of the measured statements is witnessed by 0 driver errors in every arm of every run and by `SHOW ASSERTIONS` reporting `enforcing=1` on both servers in every run |
| host | 8 CPUs; see *Host load* |

## Method

The driver is a scratch driver, `assertion_keyfor_ab.py` in this run's archive,
shaped like `tools/assertion_overhead_benchmark.py` (it imports that tool's
`fk_overhead_benchmark` helpers and `bench_common.Phase`) and extending it with
`UPDATE` and `DELETE` arms and a `--gtype date|int` switch. `tools/` was not
touched. It is serial, one session per server, the two servers side by side, and
**which server goes first alternates every block** (16 blocks of 500 statements
per arm), so only one server is active at a time and a slow minute lands on both.
Each pair run is a fresh pair of servers; 7 pair runs per cell.

Relations, both BTREE, `(id int64, g <int64 | date>)`: `ev_a` carries
`CREATE ASSERTION cap ON ev_a GROUP BY (g) CHECK COUNT(*) <= 2000000000`, `ev_n`
carries none (the control). **Rows axis**: `--preload N` with N = 200, 1,000,
10,000 rows in each relation, row i in its own group i (a `date` group is
2000-01-01 + i days), present before the measured statements start; the assertion
is created over them, so its backfill is the stored side of every key. Every arm
runs 8,000 statements per server per run (16 blocks × 500), so 56,000 per arm per
server per cell over 7 runs.

| arm | statement | what is keyed |
|---|---|---|
| `insert-assert-existing` | `INSERT INTO ev_a VALUES (id, <existing group>)` | one key, finds its group |
| `insert-assert-newgroup` | the same, a group never seen | one key, opens a group |
| `update-assert` | `UPDATE ev_a SET g = <other existing group> WHERE id = <k>` | old row key and new value key |
| `delete-assert` | `DELETE FROM ev_a WHERE id = <inserted by insert-assert-existing>` | the departing row's key |
| `insert-none-*`, `update-none*`, `delete-none` | the same on `ev_n` | controls: no assertion, `KeyFor` unreachable |
| `insert-none-again`, `update-none-again` | the control repeated | **the noise floor from inside the run** |
| `select`, `ping` | pk point `SELECT` of `ev_n`; `SHOW META` | controls that cannot reach `KeyFor` |

The group column is where the change is exercised differently: for an `int64`
group `CoerceLiteralToColumn` has nothing to convert, so the integer cells price
the copy and the coercion call; for a `date` group the literal arrives as a
string and the call does real work, so the `date` cells price the path the change
exists for.

## Noise floor

The controls cannot reach the changed code, so their B − A is the floor
(B − A = median over 7 runs of the per-run difference of the client p50; 48
control cells, 8 control arms in each of the 6 group-type and row-count cells):

- integer-group controls (`insert-none-*`, `update-none*`, `delete-none`, `select`,
  `ping`) read −0.30 to +0.40 µs;
- `date`-group controls read −0.70 to +0.40 µs; **every UPDATE arm at 10,000
  rows reads −0.7 to −0.8 µs, the control ones too** (`update-none` −0.70,
  `update-none-again` −0.70), which is the engines' difference at that size and
  not assertion work, so an UPDATE delta at 10,000 rows is read against −0.7;
- the interquartile range of a control's per-run B − A is mostly ±0.2 to ±0.5 µs
  and −1.15 to +0.75 µs at its widest (the 10,000-row UPDATE controls).

**The floor used below is ±0.5 µs (1 % of a ~50 µs statement); a delta inside it
is not a finding.** The p50 unit is the client's: a socket round trip, the
engine's polled time in the server, and the client's own parsing.

## What the statement is made of

A latency is a sum. From `SHOW META` read around every block
(`sched_foreground_polled_us / sched_foreground_polls`, the engine's own time per
statement), `cores = 1`, `relaxed`, 1,000 rows, median of 7 runs:

| statement | client p50 (A / B) | engine time per statement (A / B) | the rest: client, socket, wake |
|---|---|---|---|
| INSERT, no assertion (`int`) | 49.0 / 49.0 µs | 13.0 / 13.0 µs | ≈ 36 µs |
| INSERT, asserted, existing group (`int`) | 51.8 / 52.1 µs | 15.2 / 15.2 µs | ≈ 37 µs |
| UPDATE, asserted (`int`) | 52.1 / 53.1 µs | 15.6 / 15.9 µs | ≈ 37 µs |
| DELETE, asserted (`int`) | 51.0 / 51.1 µs | 14.4 / 14.7 µs | ≈ 37 µs |
| UPDATE, asserted (`date`) | 57.3 / 53.5 µs | 19.3 / 16.0 µs | ≈ 38 µs |
| `SHOW META` (control) | 50.2 / 50.2 µs | 19.1 / 19.2 µs | ≈ 31 µs |

Two things follow. Assertion admission itself costs ≈ 1.0–2.2 µs of engine time
in B (INSERT 15.2 against 13.0, UPDATE 15.9 against 14.9, DELETE 14.7 against
12.8), and the change under test is a part of that, not a new layer on top of it.
And **the wait types that dominate a durable write are absent here by
construction**: `durability = relaxed` has no fsync wait (the commit wait,
82–85 % of an update under `group`, is not in this run), one session and
non-overlapping statements have no lock or conflict wait. The measured
statement is the engine's in-memory work plus a round trip, which is where a
per-row hash-key change would show if it showed anywhere. `strict` and `group`
were not measured, and a cost that small is invisible under them.

## Results — integer group column

Client p50 per statement (µs), medians of 7 runs. **QPS is derived**
(`1,000,000 / p50`, the driver being serial, and from the p50 rather than the mean
because a stalled block on this shared host moves the mean by tens of µs while the
p50 does not; the mean-derived figures are in the archive's `analysis.txt`). "B−A"
is the median over runs of the per-run difference; IQR is that difference's
interquartile range; "runs B>A" counts runs where B's p50 was higher.
**The full 11-arm tables are `tables.md` in the archive**; these rows are the
four asserted statements and the three controls that bound the noise.

| rows | statement | A p50 | B p50 | B−A p50 (median of 7 runs) | IQR of B−A | runs B>A | A QPS | B QPS | engine B−A per stmt |
|---|---|---|---|---|---|---|---|---|---|
| 200 | INSERT, asserted, existing group | 51.5 µs | 52.0 µs | +0.00 µs | -0.35 to +0.85 µs | 3/7 | 19,417 qps | 19,231 qps | +0.72 µs |
| 200 | INSERT, asserted, new group | 52.3 µs | 52.6 µs | +0.10 µs | -0.30 to +0.70 µs | 4/7 | 19,120 qps | 19,011 qps | +0.09 µs |
| 200 | UPDATE g, asserted | 52.2 µs | 53.1 µs | +0.50 µs | -0.00 to +1.00 µs | 5/7 | 19,157 qps | 18,832 qps | +0.44 µs |
| 200 | DELETE, asserted | 50.8 µs | 50.9 µs | -0.40 µs | -0.55 to +0.35 µs | 3/7 | 19,685 qps | 19,646 qps | -0.01 µs |
| 200 | ctl: INSERT repeated (noise floor) | 48.9 µs | 49.0 µs | +0.20 µs | +0.05 to +0.40 µs | 5/7 | 20,450 qps | 20,408 qps | -0.16 µs |
| 200 | ctl: UPDATE repeated (noise floor) | 51.4 µs | 51.7 µs | -0.20 µs | -0.45 to +0.45 µs | 3/7 | 19,455 qps | 19,342 qps | +0.16 µs |
| 200 | ctl: SHOW META | 50.0 µs | 50.4 µs | +0.40 µs | +0.00 to +0.50 µs | 4/7 | 20,000 qps | 19,841 qps | +0.14 µs |
| 1,000 | INSERT, asserted, existing group | 51.8 µs | 52.1 µs | +0.10 µs | -0.25 to +0.40 µs | 4/7 | 19,305 qps | 19,194 qps | +0.06 µs |
| 1,000 | INSERT, asserted, new group | 52.4 µs | 52.6 µs | +0.30 µs | -0.30 to +0.35 µs | 4/7 | 19,084 qps | 19,011 qps | +0.10 µs |
| 1,000 | UPDATE g, asserted | 52.1 µs | 53.1 µs | +0.60 µs | +0.50 to +0.80 µs | 6/7 | 19,194 qps | 18,832 qps | +0.44 µs |
| 1,000 | DELETE, asserted | 51.0 µs | 51.1 µs | +0.50 µs | +0.00 to +0.70 µs | 5/7 | 19,608 qps | 19,569 qps | +0.42 µs |
| 1,000 | ctl: INSERT repeated (noise floor) | 49.0 µs | 49.0 µs | -0.10 µs | -0.15 to +0.05 µs | 2/7 | 20,408 qps | 20,408 qps | -0.05 µs |
| 1,000 | ctl: UPDATE repeated (noise floor) | 51.2 µs | 51.3 µs | +0.20 µs | -0.10 to +0.35 µs | 4/7 | 19,531 qps | 19,493 qps | +0.27 µs |
| 1,000 | ctl: SHOW META | 50.2 µs | 50.2 µs | +0.00 µs | -0.15 to +0.15 µs | 3/7 | 19,920 qps | 19,920 qps | -0.01 µs |
| 10,000 | INSERT, asserted, existing group | 52.9 µs | 52.9 µs | +0.30 µs | -0.20 to +0.35 µs | 4/7 | 18,904 qps | 18,904 qps | +0.16 µs |
| 10,000 | INSERT, asserted, new group | 52.6 µs | 52.8 µs | +0.50 µs | +0.05 to +1.30 µs | 5/7 | 19,011 qps | 18,939 qps | +0.11 µs |
| 10,000 | UPDATE g, asserted | 56.0 µs | 56.1 µs | -0.40 µs | -0.65 to +0.15 µs | 2/7 | 17,857 qps | 17,825 qps | +0.18 µs |
| 10,000 | DELETE, asserted | 51.5 µs | 51.3 µs | -0.30 µs | -0.45 to +0.70 µs | 2/7 | 19,417 qps | 19,493 qps | -0.29 µs |
| 10,000 | ctl: INSERT repeated (noise floor) | 48.9 µs | 49.0 µs | +0.10 µs | -0.10 to +0.20 µs | 4/7 | 20,450 qps | 20,408 qps | +0.17 µs |
| 10,000 | ctl: UPDATE repeated (noise floor) | 51.7 µs | 51.4 µs | -0.20 µs | -0.45 to +0.70 µs | 2/7 | 19,342 qps | 19,455 qps | -0.03 µs |
| 10,000 | ctl: SHOW META | 50.1 µs | 50.2 µs | +0.10 µs | -0.10 to +0.20 µs | 4/7 | 19,960 qps | 19,920 qps | +0.04 µs |

**Reading.** INSERT: +0.0 / +0.1 / +0.3 µs (existing group) and +0.1 / +0.3 / +0.5
µs (new group) at 200 / 1,000 / 10,000 rows, each inside its IQR and inside the
control's floor; the largest, +0.5 µs at 10,000 rows (IQR +0.05 to +1.3, 5 of 7
positive), equals the floor and sits beside a +0.3 µs existing-group cell of the
same size. DELETE: −0.4 / +0.5 / −0.3 µs, no sign. UPDATE: +0.5 / +0.6 / −0.4 µs —
the one cell worth saying more about. At 200 and 1,000 rows its engine time reads
+0.44 µs per statement in both (against +0.18 / +0.13 µs for the unasserted
`update-none`), and 11 of 14 runs are positive. An UPDATE keys twice (old row, new
value), and a per-call cost of +0.2 µs would give this size of delta; but the same
statement at 10,000 rows reads −0.4 µs, which a fixed per-call cost would not do. **Read as at most +0.6 µs
(1.2 %) and not resolved**; a measurement that settles it needs a cell where
`KeyFor` runs many times per statement (a multi-row `INSERT`, an `UPDATE` that
walks), which this driver does not have.

## Results — `date` group column

| rows | statement | A p50 | B p50 | B−A p50 (median of 7 runs) | IQR of B−A | runs B>A | A QPS | B QPS | engine B−A per stmt |
|---|---|---|---|---|---|---|---|---|---|
| 200 | INSERT, asserted, existing group | 52.7 µs | 52.7 µs | -0.30 µs | -0.55 to +0.55 µs | 3/7 | 18,975 qps | 18,975 qps | +0.03 µs |
| 200 | INSERT, asserted, new group | 53.4 µs | 53.6 µs | +0.00 µs | -0.40 to +0.50 µs | 3/7 | 18,727 qps | 18,657 qps | -0.02 µs |
| 200 | UPDATE g, asserted | 57.2 µs | 53.6 µs | -3.80 µs | -4.35 to -3.60 µs | 0/7 | 17,483 qps | 18,657 qps | -3.30 µs |
| 200 | DELETE, asserted | 51.5 µs | 50.9 µs | +0.00 µs | -0.70 to +0.25 µs | 3/7 | 19,417 qps | 19,646 qps | +0.11 µs |
| 200 | ctl: INSERT repeated (noise floor) | 49.8 µs | 49.8 µs | +0.00 µs | -0.20 to +0.35 µs | 3/7 | 20,080 qps | 20,080 qps | +0.07 µs |
| 200 | ctl: UPDATE repeated (noise floor) | 52.7 µs | 52.4 µs | -0.40 µs | -0.75 to -0.15 µs | 2/7 | 18,975 qps | 19,084 qps | -0.00 µs |
| 200 | ctl: SHOW META | 50.3 µs | 50.6 µs | +0.40 µs | +0.05 to +0.40 µs | 5/7 | 19,881 qps | 19,763 qps | +0.20 µs |
| 1,000 | INSERT, asserted, existing group | 53.1 µs | 52.4 µs | -0.40 µs | -1.10 to +0.00 µs | 2/7 | 18,832 qps | 19,084 qps | -0.54 µs |
| 1,000 | INSERT, asserted, new group | 53.6 µs | 53.1 µs | -0.50 µs | -0.90 to +0.20 µs | 2/7 | 18,657 qps | 18,832 qps | -0.33 µs |
| 1,000 | UPDATE g, asserted | 57.3 µs | 53.5 µs | -3.90 µs | -4.15 to -3.35 µs | 0/7 | 17,452 qps | 18,692 qps | -3.03 µs |
| 1,000 | DELETE, asserted | 51.3 µs | 51.3 µs | +0.20 µs | -0.20 to +0.45 µs | 5/7 | 19,493 qps | 19,493 qps | +0.11 µs |
| 1,000 | ctl: INSERT repeated (noise floor) | 49.6 µs | 49.7 µs | +0.20 µs | -0.15 to +0.25 µs | 4/7 | 20,161 qps | 20,121 qps | -0.19 µs |
| 1,000 | ctl: UPDATE repeated (noise floor) | 52.1 µs | 52.0 µs | -0.20 µs | -0.65 to +0.25 µs | 3/7 | 19,194 qps | 19,231 qps | -0.12 µs |
| 1,000 | ctl: SHOW META | 50.1 µs | 50.2 µs | +0.30 µs | +0.05 to +0.50 µs | 5/7 | 19,960 qps | 19,920 qps | +0.00 µs |
| 10,000 | INSERT, asserted, existing group | 53.7 µs | 53.0 µs | -0.30 µs | -0.70 to -0.20 µs | 0/7 | 18,622 qps | 18,868 qps | -0.13 µs |
| 10,000 | INSERT, asserted, new group | 53.8 µs | 53.1 µs | -0.40 µs | -0.70 to -0.20 µs | 1/7 | 18,587 qps | 18,832 qps | -0.45 µs |
| 10,000 | UPDATE g, asserted | 57.7 µs | 56.9 µs | -0.80 µs | -1.25 to -0.55 µs | 0/7 | 17,331 qps | 17,575 qps | -0.75 µs |
| 10,000 | DELETE, asserted | 51.6 µs | 51.4 µs | -0.50 µs | -0.70 to +0.00 µs | 0/7 | 19,380 qps | 19,455 qps | -0.29 µs |
| 10,000 | ctl: INSERT repeated (noise floor) | 49.7 µs | 49.6 µs | -0.20 µs | -0.30 to +0.10 µs | 2/7 | 20,121 qps | 20,161 qps | -0.06 µs |
| 10,000 | ctl: UPDATE repeated (noise floor) | 52.6 µs | 51.8 µs | -0.70 µs | -1.05 to -0.30 µs | 0/7 | 19,011 qps | 19,305 qps | -0.49 µs |
| 10,000 | ctl: SHOW META | 49.9 µs | 50.0 µs | -0.10 µs | -0.25 to +0.20 µs | 3/7 | 20,040 qps | 20,000 qps | +0.01 µs |

**Reading.** The asserted INSERT and DELETE read −0.5 to +0.2 µs at the three row
counts, i.e. no cost and a faint sign the other way, all
inside the floor (the 10,000-row INSERT cells read −0.3 / −0.4 µs with 0 and 1 of 7
runs positive, and the control `insert-none-again` reads −0.2 µs there). **The
asserted UPDATE is 3.8 µs and 3.9 µs faster in B at 200 and 1,000 rows (0 of 7
runs positive, IQR −4.35 to −3.35 µs, ≈ 7 %), with the engine's own time down
3.3 and 3.0 µs per statement** (A 19.3 µs, B 16.0 µs at 1,000 rows), against
controls at −0.4 / −0.5 and −0.2 µs. At 10,000 rows it reads −0.8 µs, inside the −0.7 µs
that every UPDATE arm there shows. Before this change an UPDATE keyed the old row
from the decoded value and the new value from the literal string, two different
encodings of one date, and A's per-statement cost over its own unasserted control
was 4.4 µs where B's is 1.3 µs; the speed-up is the difference. That is a reading
from the shape of the numbers and the commit's description of the bug: the
mechanism in A's admission path (what it does with a key that matches no existing
group) was not instrumented in this run. It is a side effect of the correctness
fix, not an optimisation, and it is the whole of the one resolvable delta here.

## Host load

Rule 4 asks for load per cell: `/proc/loadavg` and `pgrep -a -f
"cc1plus|cmake --build|ctest"` are in every `host.txt` in the archive. The runner
waited up to 2 minutes for one-minute load below 1.2 before each pair and then
started regardless. **34 of the 42 retained pair runs started at a one-minute load of 1.02
to 1.19; 8 started higher (1.62, 2.20, 2.67, 3.27, 4.21, 5.25, 7.02, 7.84)** because
other sessions on this host were running a `ctest` or a `cmake --build kds_tests`
at those moments (visible in those runs' `host.txt`); that is outside this
session's control, and none of it was this session's. One pair (`date`, 1,000 rows,
run 4) hit a 5 s client timeout at a load of 8.41 and was discarded
(`failed-date-n1000-run4-clienttimeout/` in the archive) and re-run at load 1.02,
though that rerun began with another session's `cmake --build ... kds_tests` live.
The A/B within a pair is interleaved per block, so load lands on both arms; the
medians over 7 runs absorb the single-run outliers. **Sensitivity check**:
excluding the 10 runs that started above 1.2 or beside a live compile/test of
another session (`clean.txt`: 4–6 runs per cell remain) leaves every conclusion
unchanged: the integer UPDATE reads +0.55 / +0.55 / −0.20 µs, the `date` UPDATE
−3.70 / −3.50 / −0.85 µs, every other asserted cell within ±0.7 µs.

## What the run teaches

1. **The coercion at the group key is not a per-row cost this engine can see at one
   key per statement.** An 8-byte integer copy plus a coercion call that
   recognises the column type is below the 0.5 µs of resolution of a 50 µs
   round trip, and what is left over (+0.5 / +0.6 µs on the integer UPDATE at
   the two smaller sizes) is the upper bound, not a measured cost.
2. **The statement is roughly 70 % client and socket, 30 % engine** (≈ 36 µs against
   13–16 µs). A change in the engine's in-memory admission path of 0.2–0.5 µs is
   1–3 % of the engine's own time and 0.5–1 % of what a client sees; the `engine
   B−A per stmt` column carries the finer reading and agrees with the client's.
3. **A's behaviour on a `date` group UPDATE was costlier than B's because it was
   wrong**, not because the fix is a faster algorithm: the `UPDATE` delta is the
   price of A's two keys for one value. A `date` or `timestamp` assertion that
   sees updates is where the fix shows as time.
4. **Not measured, and not implied**: `group` or `strict` durability, `cores > 1`,
   concurrent writers on one group (the group's latch), a multi-row `INSERT` or an
   `UPDATE` that touches many rows (where `KeyFor` runs per row), a composite
   `GROUP BY`, `decimal` and `timestamp` group columns, and the `CREATE ASSERTION`
   backfill (it keys from decoded rows, the path the change does not alter; its
   time is in each `result.json` as `create_assertion_ms`, 0.3 ms at 200 rows).

## Provenance of the numbers

Everything above is from the 42 pair runs in
`bench/v3.0.0/archive/assertion-keyfor-overhead-v2.7.0-748-g345c5c41/`
(`<gtype>-n<rows>-run<k>/result.json.gz` with the driver's output, host state and
both servers' configs; `analysis.txt`, `pct.txt`, `tables.md`, `clean.txt` derive
from them; the driver, the runner and the analysis scripts are alongside, and the
two binaries' hashes are in `binaries.sha256`). To reproduce: build both commits
with `build-release-with-root-arg.sh <root> kds_server`, copy the binaries to
`/home/cdkbs/bench-runs/keyfor/kds_server-{A,B}`, and run `runall.py 7`. There is no
earlier kdbs number for this driver and these arms (`bench/v3.0.0/` holds the
`az-s7` run of the unmodified `tools/assertion_overhead_benchmark.py`, a different
commit range and an `int64` group only), so **this file is the baseline the next
run of this driver is read against**; PostgreSQL's floor was not measured for this
shape. The percentile table follows.

### Percentiles per statement and its control (client µs, the median over 7 runs of each run's percentile of 8,000 statements)


| group | rows | statement | A p0 | A p25 | A p50 | A p75 | A p95 | A p99 | B p0 | B p25 | B p50 | B p75 | B p95 | B p99 | ops per run |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| int | 200 | insert-assert-existing | 45.7 µs | 50.8 µs | 51.5 µs | 53.6 µs | 66.2 µs | 79.0 µs | 45.4 µs | 51.2 µs | 52.0 µs | 54.6 µs | 66.9 µs | 80.0 µs | 8,000 |
| int | 200 | insert-assert-newgroup | 46.1 µs | 51.5 µs | 52.3 µs | 54.3 µs | 67.0 µs | 82.3 µs | 46.3 µs | 51.9 µs | 52.6 µs | 54.3 µs | 67.6 µs | 83.0 µs | 8,000 |
| int | 200 | update-assert | 44.2 µs | 51.5 µs | 52.2 µs | 54.7 µs | 66.6 µs | 78.2 µs | 44.2 µs | 52.4 µs | 53.1 µs | 55.0 µs | 67.0 µs | 77.9 µs | 8,000 |
| int | 200 | delete-assert | 43.2 µs | 50.1 µs | 50.8 µs | 52.3 µs | 64.8 µs | 77.2 µs | 43.0 µs | 50.1 µs | 50.9 µs | 52.8 µs | 65.7 µs | 78.3 µs | 8,000 |
| int | 200 | insert-none-existing | 42.3 µs | 48.1 µs | 48.9 µs | 50.3 µs | 63.8 µs | 77.4 µs | 41.7 µs | 48.2 µs | 49.0 µs | 50.4 µs | 63.0 µs | 75.7 µs | 8,000 |
| int | 200 | update-none | 43.5 µs | 50.9 µs | 51.5 µs | 53.2 µs | 65.9 µs | 77.1 µs | 43.2 µs | 51.1 µs | 51.7 µs | 53.3 µs | 65.9 µs | 77.3 µs | 8,000 |
| int | 200 | delete-none | 40.9 µs | 47.9 µs | 48.6 µs | 50.0 µs | 62.8 µs | 74.8 µs | 40.9 µs | 47.9 µs | 48.6 µs | 49.8 µs | 61.5 µs | 73.7 µs | 8,000 |
| int | 200 | ping | 44.9 µs | 49.6 µs | 50.0 µs | 51.9 µs | 62.0 µs | 69.3 µs | 45.3 µs | 49.8 µs | 50.4 µs | 52.2 µs | 62.0 µs | 70.9 µs | 8,000 |
| int | 1,000 | insert-assert-existing | 44.2 µs | 51.0 µs | 51.8 µs | 54.8 µs | 67.9 µs | 82.0 µs | 45.7 µs | 51.2 µs | 52.1 µs | 54.5 µs | 68.9 µs | 82.7 µs | 8,000 |
| int | 1,000 | insert-assert-newgroup | 45.5 µs | 51.5 µs | 52.4 µs | 54.8 µs | 67.7 µs | 84.1 µs | 46.2 µs | 51.6 µs | 52.6 µs | 54.8 µs | 68.0 µs | 82.8 µs | 8,000 |
| int | 1,000 | update-assert | 44.0 µs | 51.2 µs | 52.1 µs | 55.6 µs | 67.2 µs | 80.6 µs | 44.1 µs | 52.2 µs | 53.1 µs | 56.3 µs | 68.8 µs | 82.0 µs | 8,000 |
| int | 1,000 | delete-assert | 43.3 µs | 50.2 µs | 51.0 µs | 53.0 µs | 65.8 µs | 77.1 µs | 43.3 µs | 50.4 µs | 51.1 µs | 53.2 µs | 67.0 µs | 79.9 µs | 8,000 |
| int | 1,000 | insert-none-existing | 41.1 µs | 48.2 µs | 49.0 µs | 50.3 µs | 63.9 µs | 76.3 µs | 40.8 µs | 48.2 µs | 49.0 µs | 50.5 µs | 64.2 µs | 77.1 µs | 8,000 |
| int | 1,000 | update-none | 43.1 µs | 50.5 µs | 51.2 µs | 53.2 µs | 66.0 µs | 77.7 µs | 42.9 µs | 50.8 µs | 51.5 µs | 53.2 µs | 66.0 µs | 77.7 µs | 8,000 |
| int | 1,000 | delete-none | 41.0 µs | 47.8 µs | 48.7 µs | 50.0 µs | 64.4 µs | 97.8 µs | 40.6 µs | 47.8 µs | 48.7 µs | 50.2 µs | 64.5 µs | 101.5 µs | 8,000 |
| int | 1,000 | ping | 45.0 µs | 49.6 µs | 50.2 µs | 52.6 µs | 63.3 µs | 72.2 µs | 45.2 µs | 49.6 µs | 50.2 µs | 52.7 µs | 63.1 µs | 69.8 µs | 8,000 |
| int | 10,000 | insert-assert-existing | 45.9 µs | 52.2 µs | 52.9 µs | 55.1 µs | 68.4 µs | 83.7 µs | 46.5 µs | 52.1 µs | 52.9 µs | 55.0 µs | 68.4 µs | 81.7 µs | 8,000 |
| int | 10,000 | insert-assert-newgroup | 45.9 µs | 51.9 µs | 52.6 µs | 54.7 µs | 67.6 µs | 83.8 µs | 46.0 µs | 51.9 µs | 52.8 µs | 55.0 µs | 69.2 µs | 83.5 µs | 8,000 |
| int | 10,000 | update-assert | 48.6 µs | 55.3 µs | 56.0 µs | 58.4 µs | 71.4 µs | 87.1 µs | 48.0 µs | 55.4 µs | 56.1 µs | 58.6 µs | 72.1 µs | 85.3 µs | 8,000 |
| int | 10,000 | delete-assert | 43.8 µs | 50.7 µs | 51.5 µs | 54.0 µs | 67.6 µs | 80.9 µs | 43.3 µs | 50.6 µs | 51.3 µs | 53.4 µs | 66.0 µs | 79.4 µs | 8,000 |
| int | 10,000 | insert-none-existing | 41.9 µs | 48.3 µs | 49.1 µs | 50.4 µs | 63.3 µs | 76.3 µs | 42.3 µs | 48.3 µs | 49.0 µs | 50.9 µs | 64.4 µs | 76.6 µs | 8,000 |
| int | 10,000 | update-none | 43.7 µs | 50.9 µs | 51.7 µs | 54.2 µs | 66.8 µs | 78.2 µs | 43.1 µs | 50.8 µs | 51.6 µs | 53.7 µs | 67.7 µs | 81.0 µs | 8,000 |
| int | 10,000 | delete-none | 41.4 µs | 47.9 µs | 48.7 µs | 50.0 µs | 62.7 µs | 73.4 µs | 40.6 µs | 48.0 µs | 48.9 µs | 50.2 µs | 63.3 µs | 75.7 µs | 8,000 |
| int | 10,000 | ping | 44.9 µs | 49.5 µs | 50.1 µs | 52.6 µs | 63.4 µs | 70.3 µs | 45.2 µs | 49.7 µs | 50.2 µs | 52.8 µs | 63.5 µs | 71.6 µs | 8,000 |
| date | 200 | insert-assert-existing | 45.6 µs | 51.9 µs | 52.7 µs | 54.8 µs | 67.3 µs | 80.9 µs | 45.5 µs | 51.9 µs | 52.7 µs | 55.3 µs | 68.0 µs | 83.0 µs | 8,000 |
| date | 200 | insert-assert-newgroup | 45.4 µs | 52.5 µs | 53.4 µs | 55.9 µs | 69.6 µs | 88.8 µs | 46.5 µs | 52.6 µs | 53.6 µs | 56.2 µs | 69.3 µs | 84.6 µs | 8,000 |
| date | 200 | update-assert | 48.4 µs | 56.5 µs | 57.2 µs | 59.9 µs | 73.1 µs | 88.7 µs | 44.3 µs | 52.9 µs | 53.6 µs | 55.7 µs | 69.0 µs | 81.6 µs | 8,000 |
| date | 200 | delete-assert | 43.2 µs | 50.7 µs | 51.5 µs | 53.7 µs | 66.7 µs | 79.5 µs | 43.8 µs | 50.3 µs | 50.9 µs | 52.5 µs | 66.2 µs | 79.6 µs | 8,000 |
| date | 200 | insert-none-existing | 42.0 µs | 48.9 µs | 49.7 µs | 51.1 µs | 64.6 µs | 77.5 µs | 42.2 µs | 48.9 µs | 49.8 µs | 51.3 µs | 65.4 µs | 79.4 µs | 8,000 |
| date | 200 | update-none | 43.3 µs | 51.9 µs | 52.6 µs | 55.1 µs | 67.0 µs | 80.3 µs | 42.4 µs | 51.5 µs | 52.2 µs | 54.4 µs | 66.5 µs | 79.6 µs | 8,000 |
| date | 200 | delete-none | 40.5 µs | 48.0 µs | 48.9 µs | 50.4 µs | 64.1 µs | 90.2 µs | 40.4 µs | 47.9 µs | 48.8 µs | 49.8 µs | 62.5 µs | 74.6 µs | 8,000 |
| date | 200 | ping | 45.1 µs | 49.6 µs | 50.3 µs | 52.8 µs | 63.3 µs | 70.3 µs | 44.4 µs | 50.0 µs | 50.6 µs | 52.9 µs | 63.2 µs | 71.8 µs | 8,000 |
| date | 1,000 | insert-assert-existing | 46.2 µs | 52.1 µs | 53.1 µs | 55.8 µs | 70.4 µs | 83.2 µs | 45.4 µs | 51.7 µs | 52.4 µs | 54.8 µs | 69.4 µs | 84.0 µs | 8,000 |
| date | 1,000 | insert-assert-newgroup | 46.3 µs | 52.6 µs | 53.6 µs | 56.1 µs | 69.6 µs | 85.3 µs | 46.4 µs | 52.2 µs | 53.1 µs | 55.1 µs | 69.3 µs | 86.8 µs | 8,000 |
| date | 1,000 | update-assert | 48.8 µs | 56.5 µs | 57.3 µs | 60.0 µs | 74.6 µs | 87.5 µs | 46.2 µs | 52.4 µs | 53.5 µs | 57.0 µs | 69.2 µs | 81.9 µs | 8,000 |
| date | 1,000 | delete-assert | 42.6 µs | 50.6 µs | 51.3 µs | 53.6 µs | 68.0 µs | 96.6 µs | 43.6 µs | 50.6 µs | 51.3 µs | 54.1 µs | 66.4 µs | 79.1 µs | 8,000 |
| date | 1,000 | insert-none-existing | 41.6 µs | 48.8 µs | 49.5 µs | 51.0 µs | 64.0 µs | 77.6 µs | 42.1 µs | 48.7 µs | 49.5 µs | 51.0 µs | 63.8 µs | 76.8 µs | 8,000 |
| date | 1,000 | update-none | 44.0 µs | 51.5 µs | 52.2 µs | 54.6 µs | 67.3 µs | 79.0 µs | 43.6 µs | 51.2 µs | 51.9 µs | 53.7 µs | 67.1 µs | 77.5 µs | 8,000 |
| date | 1,000 | delete-none | 40.8 µs | 47.9 µs | 48.7 µs | 50.2 µs | 64.1 µs | 75.1 µs | 40.5 µs | 48.0 µs | 48.8 µs | 50.6 µs | 65.1 µs | 91.7 µs | 8,000 |
| date | 1,000 | ping | 44.9 µs | 49.5 µs | 50.1 µs | 52.5 µs | 63.6 µs | 69.7 µs | 44.3 µs | 49.7 µs | 50.2 µs | 52.9 µs | 62.9 µs | 70.2 µs | 8,000 |
| date | 10,000 | insert-assert-existing | 46.5 µs | 52.8 µs | 53.7 µs | 55.7 µs | 68.8 µs | 83.1 µs | 46.1 µs | 52.2 µs | 53.0 µs | 55.2 µs | 68.8 µs | 82.8 µs | 8,000 |
| date | 10,000 | insert-assert-newgroup | 46.2 µs | 52.9 µs | 53.8 µs | 56.3 µs | 68.9 µs | 83.7 µs | 45.8 µs | 52.3 µs | 53.1 µs | 55.3 µs | 69.0 µs | 83.6 µs | 8,000 |
| date | 10,000 | update-assert | 49.1 µs | 56.9 µs | 57.7 µs | 60.9 µs | 74.9 µs | 88.2 µs | 48.7 µs | 56.2 µs | 56.9 µs | 58.9 µs | 72.7 µs | 87.4 µs | 8,000 |
| date | 10,000 | delete-assert | 43.7 µs | 50.9 µs | 51.6 µs | 53.8 µs | 66.9 µs | 82.3 µs | 43.2 µs | 50.5 µs | 51.4 µs | 53.7 µs | 66.9 µs | 80.7 µs | 8,000 |
| date | 10,000 | insert-none-existing | 42.0 µs | 48.9 µs | 49.7 µs | 51.1 µs | 65.2 µs | 77.9 µs | 42.3 µs | 48.8 µs | 49.7 µs | 50.8 µs | 63.9 µs | 77.1 µs | 8,000 |
| date | 10,000 | update-none | 44.0 µs | 51.9 µs | 52.5 µs | 54.6 µs | 68.3 µs | 80.4 µs | 43.1 µs | 51.2 µs | 51.9 µs | 54.0 µs | 67.5 µs | 79.9 µs | 8,000 |
| date | 10,000 | delete-none | 41.0 µs | 48.1 µs | 48.8 µs | 50.1 µs | 62.2 µs | 73.4 µs | 40.5 µs | 47.9 µs | 48.7 µs | 49.8 µs | 62.5 µs | 73.3 µs | 8,000 |
| date | 10,000 | ping | 45.2 µs | 49.4 µs | 49.9 µs | 52.0 µs | 61.8 µs | 68.1 µs | 44.6 µs | 49.5 µs | 50.0 µs | 51.9 µs | 61.8 µs | 70.0 µs | 8,000 |
