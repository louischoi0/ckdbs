# AN-S5, scenario2 half — blocked before the first statement, by SUS-1

**No TPS number in this document, for the same reason as its scenario0
sibling** (`results-an-s5-scenario0-v2.7.0-265-g13b6b55.md`): SUS-1
(`instructions/v3.0.0/workorder-as-sus1-heap-suspended.md`, landed on
`main` 2026-09-05) refuses `CREATE TABLE … HEAP`, and
`tools/scenario2_freight.py` declares two of its eight relations
(`freights`, `charges`) `HEAP` in its own fixed schema, with no flag to
override it. AN-S5 asks for the AL-S8 scenario matrix re-run at `13b6b55`
beside AL-S8's `results-scenario2-freight-v2.7.0-157-gf6ed10c.md`; it
could not be attempted past schema creation on any of the four cells.

## 1. Stamp

| Field | Value |
|---|---|
| Date/time attempted | 2026-09-07, 11:35–11:36 UTC |
| Worktree | `an-s3-snapshot-adoption` (branch `an-s3-snapshot-adoption`) |
| Engine commit measured | `13b6b55`, `git describe --tags` = `v2.7.0-265-g13b6b55`. `git diff --stat 13b6b55 HEAD -- src include tools CMakeLists.txt` is empty at this worktree's `HEAD` (`451022b`), verified this session |
| Tree cleanliness | Clean |
| Binary provenance | `/home/cdkbs/bench-runs/an-s5-v2.7.0-265-g13b6b55/kds_server`, `sha256 5bf18b6d0851facdf0e03463d8c0ed4c0def32020d33c4348173bf4c43088a3b`. Source `build-release/kds_server` mtime `2026-09-07 11:27:43.32 UTC`, copy mtime `11:28:26 UTC`, both after `13b6b55`'s commit time `11:14:21 UTC`. Started from the copy, never from `build-release/` directly |
| Device | `/home/cdkbs`, `/dev/root`, ext4 (`df -T`), 51% used |
| Build type | `build-release` (Release), not rebuilt this session |
| Host | 8 logical CPUs, AMD EPYC 9V74, 1 socket × 4 cores × 2 threads/core — same shape AL-S8 recorded |
| Precheck (`s2-c1-g`, the probe cell) | prior to the probe: `/proc/loadavg` 0.81 / 2.29 / 2.86 at 11:31:16 UTC; `pgrep -a -f 'cc1plus\|cmake --build\|ctest'` matched the same unrelated-worktree `ctest` run named in the scenario0 document (`AU+AV-S0S1/build`, <2% CPU) — not a build, not in this run's tree |

## 2. What was attempted, and what happened

Rather than run all four cells into the same wall (the schema is
`cores`/`durability`-independent, exactly as scenario0's is — see that
document's §2 for why one cell stands for all four), one probe cell
(`s2-c1-g`, `cores = 1`, `durability = group`, port 15594, fresh data
file) was run to confirm the failure mode against this specific driver
before writing this document. The server started clean from the copy;
`tools/scenario2_freight.py` (unmodified) was invoked exactly as
specified:

```
python3 tools/scenario2_freight.py --port 15594 --suffix s2c1g \
  --organizations 300 --ships 30 --operations 300 --cargos 4000 \
  --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync \
  --json .../s2test.json
```

It aborted on the sixth of eight relations (`organizations`, `ships`,
`operations`, `cargos`, `fees`, `recipes` are all declared `BTREE` and
were created; `freights` is next and is declared `HEAP`):

```
scenario2 aborted: could not create freights_s2c1g
  server said: ERR HEAP storage is suspended (SUS-1) and no new heap
  relation is created (byte 154); BTREE is the default and every existing
  heap relation still mounts and serves. The suspension, its rulings and
  the condition that lifts it are
  instructions/v3.0.0/workorder-as-sus1-heap-suspended.md
```

`charges` (also `HEAP`, `tools/scenario2_freight.py:117-120`) is never
reached — `freights` fails first in `CREATE_ORDER`. The server was
stopped cleanly (`SIGTERM`) and the partial data file removed. The other
three cells (`s2-c1-s`, `s2-c8-g`, `s2-c8-s`) were not run individually
for the same reason scenario0's sibling cells were not: the schema does
not vary with `cores` or `durability`, so every cell fails at the
identical `CREATE TABLE freights_<suffix>` statement.

## 3. Why this is not worked around

The same two rules as the scenario0 document, restated because they are
this document's whole reason for existing rather than a caveat on it:
`tools/` stays unmodified (a driver change measures the driver, and
`scenario2_freight.py`'s `HEAP` declarations on `freights`/`charges` are
deliberate — the driver's own schema comment at
`tools/scenario2_freight.py:79-81` says `HEAP` is for "the two append-only
ledgers"), and engine code is not edited to make a benchmark pass (SUS-1
is an operator-ordered suspension, not a defect). AS-Q6
(`instructions/v3.0.0/workorder-as-sus1-heap-suspended.md`) names
`scenario2_freight.py` implicitly as one of the "three [tools] emit
explicit `HEAP` and will simply be refused" — `scenario0_stockmarket.py`
is the other named in the sibling document — and records that `bench/`'s
rules are the operator's to resolve; still unratified at `13b6b55` and at
this worktree's `HEAD` (`451022b`).

## 4. What this means for AL-S8's numbers

AL-S8's own baseline (`results-scenario2-freight-v2.7.0-157-gf6ed10c.md`,
commit `f6ed10c` = `v2.7.0-157-gf6ed10c`, measured 2026-09-03) predates
SUS-1 and is unaffected by it — those numbers (578.4 / 312.7 / 543.8 /
284.7 TPS across the four cells, and the §6 finding that `--verify`
failed 18–33 times per 400 checks on invariant I1, independent of this
stage) stand as the record of that engine state. This document carries
no numbers to supersede them with, so `bench/README.md`'s "a re-run
deletes what it supersedes" does not trigger; AL-S8's file remains the
standing scenario2 baseline until SUS-1 resumes or the driver gains a
BTREE-schema variant (see the sibling document's §5, which applies here
unchanged).

Archive: the probe cell's aborted output is kept at
`bench/v3.0.0/archive/an-s5-v2.7.0-265-g13b6b55/` for the reproduction,
not as a results archive.

**Superseded on the comparator, 2026-09-08.** The operator's mark on AS-Q6
(`instructions/v3.0.0/raft-marks-2026-09-08-as-q6.md`): the driver's `freights`
and `charges` are `BTREE`, `f6ed10c` is to be re-measured with the changed
driver as the new baseline, and measurement is BTREE only for now. The
AL-S8 file this document names as the standing baseline is history from
that mark on and is compared against nothing (`bench/README.md`); the
driver lines cited above now say `BTREE`. Nothing above is re-measured or
edited.
