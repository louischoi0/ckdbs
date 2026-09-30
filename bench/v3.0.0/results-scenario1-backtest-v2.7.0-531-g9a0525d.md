# Scenario 1 (backtest) at `v2.7.0-531-g9a0525d`: not executed

**One line.** `tools/scenario1_backtest.py` cannot run on this engine: it
creates `daily_stats` (and `model_results` and the write sweep's
`write_probe`) as `HEAP` with no flag to change it, and the server refuses
every `CREATE TABLE ... HEAP` since SUS-1. **No throughput, latency or
QPS number exists for this scenario at this commit, and none is given.**
There is no v3 predecessor for this driver either (nothing under
`bench/v3.0.0/` at `9a0525d` measured it), so this file is neither a
baseline nor a delta.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-09-30, 06:52:10 to 06:52:13 UTC |
| Worktree / branch | `bench-rerun-scenarios` / `worktree-bench-rerun-scenarios` |
| HEAD | `fc2d343`, which over `9a0525d` only deletes `bench/v3.0.0/` and edits `bench/README.md`; `src/`, `include/`, `tools/` and `CMakeLists.txt` are identical |
| Engine commit | `9a0525d`, `git describe --tags` = `v2.7.0-531-g9a0525d` ("AY-S11: AY closed") |
| Tree cleanliness | Clean |
| Binary provenance | `/home/cdkbs/bench-runs/rebaseline-9a0525d/kds_server`, copy of `build-release/kds_server`, **sha256 `1d07d33386541ec3a13090d37918e229957e29c2c777ff15f2a63db6957f8746`**, source binary mtime 2026-09-30 06:30:43 UTC |
| Device | `/dev/root`, `ext4` (`df -T`), data files under `/home/cdkbs/bench-runs/rebaseline-9a0525d/data/` |
| Build type | Release |
| Server config | `log_level = warn`; `cores` 1 / 8 and `durability` group / group / strict for the three cells; port 15610 |
| Host stamp | precheck loadavg 1.69 / 1.65 / 1.54 at all three cells (the tail of the preceding scenario3 pass); no `cc1plus`, `cmake --build` or `ctest` process |

## 2. What was attempted

Three cells, one server each from the hashed copy on a fresh data file:

    python3 tools/scenario1_backtest.py --port 15610 --suffix s1_<cell> --seed 1 \
        --bars-clustered btree --verify --sync --analyze --json <cell>.json

with the driver's defaults otherwise (30 years, 8 symbols, 60,480 bars,
all sweeps). `--bars-clustered btree` is the default and the only value the
engine admits. Cells: `s1-c1-g` (`cores = 1`, `group`), `s1-c8-g`
(`cores = 8`, `group`), `s1-c1-s` (`cores = 1`, `strict`).

## 3. What happened

| Cell | Result | Exit | Elapsed |
|---|---|---|---|
| `s1-c1-g` | refused at the fifth `CREATE TABLE` | exit 1 | 1 s |
| `s1-c8-g` | refused at the fifth `CREATE TABLE` | exit 1 | 1 s |
| `s1-c1-s` | refused at the fifth `CREATE TABLE` | exit 1 | 1 s |

The driver's output, identical in all three but for the suffix:

    loading: 30 years x 252 sessions x 8 symbols = 60,480 bars, 60,480 feature rows  (tables suffixed _s1_c1_g)  [daily_bars BTREE]  [200-row transactions]
    scenario1 aborted: could not create daily_stats_s1_c1_g
      server said: ERR HEAP storage is suspended (SUS-1) and no new heap relation is created (byte 219); BTREE is the default and every existing heap relation still mounts and serves. The suspension, its rulings and the condition that lifts it are instructions/v3.0.0/workorder-as-sus1-heap-suspended.md

The refusal is the engine's documented one (`docs/spec/heap-and-tuple.md`,
the SUS-1 note under §3.1b: "`CREATE TABLE … HEAP` is refused `Unsupported`
naming SUS-1"). The driver prints the reply without its code; the server
log carries it whole, `ERR UNSUPPORTED retryable=0 HEAP storage is
suspended (SUS-1) ...`. In the driver's `SCHEMA` table
(`tools/scenario1_backtest.py` lines 219 to 265 at `9a0525d`) `daily_stats`
and `model_results` are `HEAP`, and the write sweep's `write_probe`
relation is created `... HEAP` at line 1663; `--bars-clustered` moves only
`daily_bars`. No option of this driver reaches a BTREE `daily_stats`, and
the driver was not edited, as this measurement stage does not change
drivers (`bench/README.md`). `bench/README.md` names AS-S3 as the stage that
changes it.

## 4. What this file does not say

- No QPS matrix, no backtest phase, no write or connection sweep, no
  verify verdict: nothing past the fifth `CREATE TABLE` ran.
- The row-set sweep of rule 9, the wait decomposition and the percentile
  tables do not apply: there is no measured unit.
- PostgreSQL was not installed on this host when this file was written and no floor exists here; a standalone PostgreSQL 18.6 run of the driver's defaults was made afterwards, at `v2.7.0-545-gf2f1ee7`, in `results-scenario1-backtest-pg18-v2.7.0-545-gf2f1ee7.md` (KDS still has no counterpart).
- The one thing this run establishes is that the driver, unmodified, is
  unrunnable at `9a0525d`, in one second and with a clear message; the
  scenario has no number to defend until its schema is BTREE-only.

Archive: `bench/v3.0.0/archive/scenario1-v2.7.0-531-g9a0525d/`: the three
driver outputs, prechecks, server logs, configs, `run_s1.sh` and the
timeline.
