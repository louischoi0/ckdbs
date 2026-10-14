import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *

DESC = "v3.1.0-30-gcd7c7c4e"
SHA = "4752f56285aba5e7b1adbd881facc8a7a7dadfab6269b468d30f0b29ce53befa"
OUT = "/home/cdkbs/ckdbs/.claude/worktrees/bench-v3.1.0-scenarios/bench/v3.1.0"
ARCH = f"bench/v3.1.0/archive"
OLDDIR = "/tmp/claude-1000/-home-cdkbs-ckdbs/ff2cbc3f-cf1f-478c-a618-6507b57727ba/scratchpad/old/bench/v3.0.0"

COMMON_STAMP = f"""| Worktree / branch | `bench-v3.1.0-scenarios` / `worktree-bench-v3.1.0-scenarios` |
| HEAD | `cd7c7c4e`, `git describe --tags` = `{DESC}` ("Merge sysview-strict-compare: a catalog view's WHERE takes the relation's literal rule, compared under each column's declared type"), committed 2026-10-13 12:24:08 UTC |
| Tree cleanliness | Clean at the start (`git status` empty); nothing under `src/`, `include/`, `tools/` or `CMakeLists.txt` was edited, and the drivers are the unmodified `tools/` scripts (the one exception is stated where it applies). The results files under `bench/v3.1.0/` are uncommitted additions |
| Binary provenance | `build-release/kds_server` built from this tree at HEAD (`cmake -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, `-O3 -DNDEBUG`), finished 2026-10-13 23:26:41 UTC, 14 hours after the HEAD commit, so the binary is not older than the engine. Copied to `/home/cdkbs/bench-runs/{DESC}/kds_server` before the first cell; **copy sha256 `{SHA}`** (equal to the source binary's), source binary mtime 2026-10-13 23:26:41.5 UTC. Every server started from the copy; no build ran during any reported cell |
| Device | data files under `/home/cdkbs/bench-runs/{DESC}/data/`, `/dev/root`, `ext4` (`df -T`); never tmpfs. A fresh data file per cell (per server, in scenario 4), deleted after |
| Build type | Release (`CMakeCache.txt`: `CMAKE_BUILD_TYPE=Release`) |
| Host | 8 logical CPUs, AMD EPYC 9V74 (1 socket x 4 cores x 2 threads), Linux 7.0.0-1014-azure, a shared development VM: other sessions build and run `ctest` on it |
| Time budget | **Lifted by the operator for this run** (order of 2026-10-13: "this work is an exception to the time-limit rule"); the rebaseline's arguments and repeat counts ran in full, no cell was cut |
| Prior run of this shape | the 2026-09-30 rebaseline at `9a0525d` (`v2.7.0-531-g9a0525d`, "AY closed"), file held at `36449d28` (`git show 36449d28:bench/v3.0.0/results-scenario*-v2.7.0-531-g9a0525d.md`). **495 commits and 176 first-parent merges apart** (`git rev-list --count 9a0525d..HEAD`), with 256 files changed under `src/`, `include/` and `CMakeLists.txt`, so **no delta below is attributable to one change** |"""

HOST_ROWS = """| Quiet host | before each cell a gate waited for no `cc1plus` / `cmake --build` / `ctest` / `kds_tests` process and at most 8 % busy CPU over 3 s (`/proc/loadavg` and the process list are in each cell's precheck, tables below). **A 2-second monitor ran through the whole run** (`monitor.log`) and a cell with a build/test process or a foreign process at 50 % CPU or more inside its window was **discarded and re-run** (up to 8 attempts, 8 happened, `contaminated/` in the archive). Foreign `claude` processes at 10 to 30 % of one CPU were present in many windows and are counted as minor, not excluded. The monitor's limit: it samples every 2 to 3 s, so a shorter burst is not seen |"""


def version_distance_note():
    return ("The comparator's describe string says `v2.7.0` because that was the version of record on 2026-09-30; "
            "it is the same architecture series (the AR0 single-WAL engine) and `bench/README.md` names it as the comparator for "
            "every later delta of the same driver and shape. The version label changed between the two runs, so every delta "
            "in this file is **cross-version** and is labelled so.")


def gate_tbl(tags, with_runlabel=None):
    starts, ends = timeline_windows()
    samples = monitor_samples()
    rows = []
    for t in tags:
        p = t if os.path.exists(f"{L}/{t}.precheck") else t + "-off"
        tm, la, gw, pr = precheck(p)
        n, b, h, mi, det = validity(t, samples, ts(tm), ts(ends[t][0]))
        rows.append((t, tm, ends[t][0], la, gw, pr, n, b, h, mi))
    return rows
