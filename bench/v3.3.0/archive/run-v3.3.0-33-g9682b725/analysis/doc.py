import subprocess, sys

DESC = "v3.3.0-33-g9682b725"
SHA = "7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a"
WT = "/home/cdkbs/ckdbs/.claude/worktrees/bench-scenarios-1-4"
OUT = WT + "/bench/v3.3.0"
BR = "/home/cdkbs/bench-run"


def T(script, *args):
    """stdout of one of the table scripts"""
    r = subprocess.run(["python3", "-I", f"{BR}/{script}", *args], capture_output=True, text=True, cwd=WT)
    if r.returncode:
        raise SystemExit(f"{script} {args}: {r.stderr}")
    return r.stdout.strip() + "\n"


COMMON = f"""| Worktree / branch | `bench-scenarios-1-4` / `bench-scenarios-1-4` (the main checkout is not used) |
| HEAD | `9682b725`, `git describe --tags` = `{DESC}` ("Merge fix-fk-self-reference-refused: a self-referencing foreign key is refused NotImplemented at both doors"), committed 2026-10-20 03:33:40 UTC; equal to `origin/main` when the run began |
| Tree cleanliness | Clean (`git status` empty) at the start and at the end of the measurement; nothing under `src/`, `include/`, `tools/` or `CMakeLists.txt` was edited, and the drivers are the unmodified `tools/` scripts. `build-release/` is ignored by git. The results files under `bench/v3.3.0/` are uncommitted additions until the commit that carries them |
| Binary provenance | `build-release/kds_server`, built in this worktree at HEAD with `cmake -DCMAKE_BUILD_TYPE=Release` (`CMakeCache.txt`: `Release`), target `kds_server`, `-j4`, finished 2026-10-20 03:36:57 UTC, 3 min 17 s **after** the HEAD commit, so the binary is not older than the engine. Copied to `/home/cdkbs/bench-runs/{DESC}/kds_server` before the first cell; **copy sha256 `{SHA}`** (equal to the source binary's, checked again after the run); source binary mtime 2026-10-20 03:36:57.38 UTC. Every server started from the copy; no build ran during any reported cell (the only build of the session finished before the first cell) |
| Device | data files under `/home/cdkbs/bench-runs/{DESC}/data/`, `/dev/root`, `ext4` (`df -T`); never tmpfs. A fresh data file per cell (per server in scenario 4), deleted when the cell finished. The PostgreSQL cluster (`/home/cdkbs/pg-bench-ba`) is on the same device |
| Build type | Release |
| Host | 8 logical CPUs, AMD EPYC 9V74 (1 socket x 4 cores x 2 threads), Linux 7.0.0-1014-azure, a shared development VM; `/` was 95 % full (14 GB free) |
| Time budget | **Budget lifted by the operator for this run** (order of 2026-10-20: "lift the budget, re-measure PG too"). Every cell of the v3.1.0 rebaseline's plan ran, with its repeat counts, and PostgreSQL was re-measured fresh in the same window. Nothing was cut |
| Prior run of this shape | `bench/v3.1.0/results-scenario{{N}}-*-v3.1.0-30-gcd7c7c4e.md` (KDS binary `cd7c7c4e`, sha256 `4752f562...`, measured 2026-10-13 23:38 UTC to 2026-10-14 01:39 UTC). The two commits are 240 commits and 101 first-parent merges apart (`git rev-list --count cd7c7c4e..HEAD`), with 178 files changed under `src/`, `include/` and `CMakeLists.txt` (milestones BV to CD among them), so **every KDS delta below is cross-version and attributable to no single change**. The host and its device state are also a week-old comparison: v3.1.0's cells ran on the same VM, 6.5 days earlier |
| Server config | `log_level = warn`, `buffer_pool_frames = 65536` (required since BE), `cores` and `durability` per cell, all else default. Ports 15604 to 15607, 15610, 15620, 15651 to 15653, never 15432 (held by an unrelated `kds_server`) |
| Quiet host | before each cell a gate waited for no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process and at most 8 % busy CPU over 3 s (`lib.sh` in the archive); `/proc/loadavg` and the process list are in each cell's precheck. **A 1-second monitor ran through the whole run** (`monitor.log`: 1,500+ samples of foreign processes above 10 % CPU) and every cell was checked against it after it ended (`cellvalid.py`): a build/test process or a foreign process at 50 % CPU or more inside the window discards and re-runs the cell. **No cell was discarded**: the monitor saw only a `claude` process at 11 to 23 % of one CPU in 23 samples of the 1,518, all outside the 50 % line. One batch of PostgreSQL cells failed on a connection error and is listed under contamination below |
| PostgreSQL | PostgreSQL 18.6 (`/home/cdkbs/pg-bench-ba`, port 15434), defaults (`shared_buffers = 128MB`, `synchronous_commit = on`, `fsync = on`, `max_wal_size = 1GB`), not started or stopped by this run. **Re-measured fresh in this window** (the twin runs follow the KDS cells, never concurrent with them) |
"""

NOISE_NOTE = """**What counts as a finding.** Each KDS cell was run three times; the spread (max minus min over the median, shown beside every median) is this run's own noise floor. A delta between two medians that is smaller than the larger of the two spreads is not a finding, and the cross-version columns are held to the same rule with v3.1.0's spread. Cells whose spread is above 20 % (the `point-join` and `bar-lookup` rows, which flip between a fast and a slow mode) are reported but carry no delta claim."""


def write(name, text):
    p = f"{OUT}/{name}-{DESC}.md"
    open(p, "w").write(text)
    print("wrote", p, len(text))
