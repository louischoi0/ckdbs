# `bench/` — reopened 2026-09-03 for v3.0.0

Results files, by the version of record they were measured at, and the
rules a run is invalid without. **`bench/v3.0.0/` is a fresh series and
carries no delta against any v2.x number** (AR0 D15): every v2.x number
measured the engine AR0 replaces, and a per-core-WAL number set beside a
single-stream one reads as a regression or a win that neither ran.

## Where a result goes

`bench/<version>/<benchmark>-<git describe --tags>.md` under the version of
record, with a scenario run's raw driver output (JSON summaries and logs,
never data files) beside it under
`bench/<version>/archive/<scenario>-<describe>/`.

## The five rules a run is invalid without

Four carry over from `bench/docs/README.md` at `1769487` — each has already
invalidated a run on this box, and none is implied by "measure in release".
The fifth is new to v3.

1. **Release, rebuilt at the measured commit.** `CMakeLists.txt` defaults to
   Debug, which has reported the wrong *sign*; a stale `build-release`
   silently prices an older engine than `HEAD`.
2. **A block device, never tmpfs**, for the *data* file. Check with `df -T`
   at run time and **name the device in the results file**: the answer has
   differed between hosts this suite runs on. (On this box, 2026-09-03: `/`
   and `/tmp` are both `/dev/root`, ext4.)
3. **Measure a copy of the binary.** `cp` it into the run's own directory,
   hash the copy, start every server from the copy. The build tree is shared
   with every other session in this repository, and a `cmake --build`
   landing mid-matrix swaps the engine under a run that starts a fresh
   server per configuration — with nothing in the driver output to show it.
4. **Record the host's load, per cell.** `/proc/loadavg` and
   `pgrep -a -f "cc1plus|cmake --build|ctest"` before each cell, both
   written into the file. Three session scratchpads were live here on
   2026-09-02 and a competing build moved a colocated p99 by 12×.
5. **The port is chosen, never defaulted.** 15432 on this box is held by an
   unrelated `kds_server` under `/home/cdkbs/autotrade`; a cell that binds
   the default either fails or, worse, talks to that instance.

The drivers themselves are unmodified `tools/` scripts — a driver change
inside a measurement stage measures the driver. `bench/docs/` stays closed
until there is a v3 driver whose behaviour is not already documented in the
tool's own `--help`.

## Every cell carries its unit — since 2026-09-09

The operator's working rule of 2026-09-09: **in a results document's matrix,
the unit belongs in the cell** — `437 µs`, `15,414 tps`, `3 ns` — and not
only in the column heading.

The reason is what a table is for. A results file is read years after the
run, in fragments, and quoted one row at a time into a work order or a
spec; a cell lifted out of a table whose heading carried the unit is a bare
number, and a bare number in a durability discussion has been read as
milliseconds when it was microseconds. A heading is not carried by the
sentence that quotes the row.

Where a column is genuinely unitless — a count, a ratio, an error tally —
the cell says so the same way: `0 errors`, `1.3 %`, `4 cells`.

**It applies to every results file written after this rule was recorded,
and earlier files stay as they are** — the last one before it is
`results-ao-s7-c3-v2.7.0-304-g5e94dc8.md` (at `9a0525d`), whose run matrix carries its
units in the headings. The boundary is this rule's own commit and not a
date, because a date is ambiguous on the day it is written and this rule
was written on a day that already had a results file in it. Rewriting them would restate numbers nobody re-measured,
which is the one thing this directory's rules exist to prevent; a reader who
meets an older table reads its heading, as that table's own commit intended.

## A ratio against another engine names both clients - since 2026-11-28 (EF)

A latency timed in a Python client is the server's plus the client's, and
the two engines' clients do different work: KWP's rows arrive typed and
must be rendered by the client, PostgreSQL's arrive as text. Twice a
scenario file put KDS behind PostgreSQL on a statement where the engine
was ahead (`v3.2.0/results-pg-parity-v3.2.0-171-ge81ac7cd.md`,
`v3.5.1/results-ef-s0-premise-v3.5.1-6-g72580aa04.md`: 16.6 µs a statement
of client work on one side and 0.8 µs on the other).

- **A results file that sets KDS beside another engine says, per phase,
  what each side's client did**: counted rows (`kwp.Connection.query`,
  `ServerConnection.count`; `pg_wire`'s `query`), decoded values
  (`kwp.Connection.fetch`, `ServerConnection.rows`; `pg_wire`'s `fetch`
  and a decode) or rendered a reply (`ServerConnection.rendered`). The
  scenario drivers of scenarios 2 to 4 write it into their JSON
  (`meta.timed_client`).
- **A KDS driver times the mode its twin times**, phase by phase, and a
  driver that changes its timed path opens a new series: no delta is taken
  across the change but on an overlap cell run through both.
- **`ServerConnection.send_command` is not a timed path for a number set
  beside another engine's.** It builds four frames and decodes the row
  description on every statement; it stays the path of verification, of
  the byte comparisons and of an A/B between two KDS binaries, where both
  arms pay it alike.
- **Server CPU is the figure no client choice touches**
  (`tools/parity_bench.py`): where a claim is about the engine, it is
  quoted beside the wall clock.

## What a v3 number is measured on — BTREE only, since 2026-09-08

The operator's mark on AS-Q6 (`instructions/v3.0.0/raft-marks-2026-09-08-as-q6.md`):
**every relation a driver creates is `BTREE`, and a delta is taken only
against a BTREE baseline of the same driver**. The suspension that rule was
marked under (SUS-1) was lifted by BZ (`instructions/v3.2.0/raft-marks-2026-10-17.md`
§1): `CREATE TABLE ... HEAP` is admitted, and since DN (superblock 39) a heap
is a storage of its own (`docs/spec/heap-and-tuple.md` §3.1b).

A heap number beside a btree number is two workloads. Where one commit has
both series, the `-btree-` in the benchmark name keeps them apart, and **a
delta is read against the `-btree-` files and never against the others.**

## What was deleted, and where it is held

Each deletion was on the operator's word. The last commit holding the files
is the one to read them at:

    git ls-tree -r --name-only <commit> bench    # what was there
    git show <commit>:bench/<path>               # any one file

| deleted | what left | held at |
|---|---|---|
| 2026-09-02, ahead of the big-bang change; reopened 2026-09-03 by AL-R8 (`instructions/v3.0.0/workorder-al-m0-single-wal.md`) | everything: the five C++ microbenchmarks, the probe drivers, `bench/docs/`, the three scenario documents, every v2.x results file | `1769487` |
| 2026-09-30, the scenario rebaseline | every results file and archive under `bench/v3.0.0/` from AL-S8 (`f6ed10c`) through AY-S11's overhead run (`0552d55`), the `f6ed10c` HEAP files and their 2026-09-08 `-btree-` re-measurement among them; each scenario driver was re-run at `9a0525d` | `9a0525d`; AY-S11's overhead file, corrected on `main` afterwards, at `a59da9c` |
| 2026-10-12 | the rebaseline's own results: every `results-scenario*` file and `archive/scenario*` directory under `bench/v3.0.0/` (KDS at `9a0525d`, PostgreSQL 18 at `f2f1ee7`), `results-rebaseline-summary-v2.7.0-531-g9a0525d.md` and `results-kds-vs-pg18-summary-v2.7.0-545-gf2f1ee7.md` | `36449d28` |

**A citation to a `bench/` path that is not in the working tree** resolves
by this table:

- a path that names no `v3.` directory: `1769487`;
- a `bench/v3.0.0/` path: `9a0525d`;
- AY-S11's overhead file: `a59da9c`;
- the rebaseline's own files (the `results-scenario*` files and
  `archive/scenario*` directories measured at `9a0525d` or `f2f1ee7`, and
  the two summaries): `36449d28`. The `f6ed10c` scenario files and archives
  match the same globs and are held at `9a0525d`.

**The rebaseline's files stay the comparator** for a later delta of the
same driver and shape until the scenario is re-run. A delta against any
deleted file names the commit that holds it and the commit it measured, and
says how far apart the two runs are in commits and milestones. The
`f6ed10c` HEAP files are compared against nothing.

The three entries as they were last written in full, with the 2026-09-08
re-measurement's account:
[`bench/README.md` at `1ad75954`](https://github.com/louischoi0/ckdbs/blob/1ad759540c612efb2d96bc8c6dcc695f4001f6c8/bench/README.md),
or `git show 1ad75954:bench/README.md`.
