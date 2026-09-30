# AZ-S7 — what AZ costs, `273416c` against `a59da9c`

`instructions/v3.0.0/raft-marks-2026-09-30.md` §8: the interleaved A/B overhead
measurement over a milestone's whole code change, at its close
(`instructions/v3.0.0/workorder-az-ay-carry-forward.md` §5: once, from
`a59da9c` to the commit that closes AZ). Run 2026-09-30 10:03–10:15 UTC on
`worktrees/az-s7-close` (branch `worktree-az-s7-close`), HEAD `273416c`
(`v2.7.0-566-g273416c`) when the run started.

**The short answer, cell by cell.** Every number is B (`273416c`) minus A
(`a59da9c`), `cores = 1`, `relaxed`, BTREE, p50 at the client unless stated,
with the noise floor beside it. B is about 0.5 to 1 µs faster than A on
*every* arm, controls included (below); a cost of AZ has to show above that
shift, and only one does.

| cell | what | B − A | noise floor / control | reading |
|---|---|---|---|---|
| 1 | assertion admission, one-row autocommit `INSERT`, key finds its group, 200 / 1,000 / 10,000 rows preloaded | −0.83 / −0.88 / −0.52 µs of ~53 µs | same statement repeated on one server 0.20–0.66 µs (abs. median); the no-assertion insert reads −0.51 / −0.21 / −0.20 µs, the point read −0.71 / −0.53 / −0.63 µs | **no cost**: the assertion premium (assert − no-assertion insert) is +3.2 µs on B and +3.2 µs on A at 200 rows, +2.7 / +3.3 at 1,000, +3.2 / +4.0 at 10,000; B − A of the premium is +0.24 / −0.50 / −0.70 µs |
| 1 | the same, every key opens a new group | −0.65 / −0.75 / −0.67 µs of ~53 µs | as above | **no cost**: B − A of the premium is −0.04 / −0.62 / −1.00 µs |
| 2 | D9(a) forward check, child `INSERT`, one row per transaction (autocommit) | −0.82 µs (IQR −1.10 to −0.28) of ~51 µs; engine −0.33 µs of ~14 µs | 0.15 µs (B), 0.26 µs (A); controls `parent-update` −0.70, `child-update` −0.76, `select` −1.06, `ping` −0.46, fk-less insert −0.01 µs | **no cost**: inside the controls' spread |
| 2 | the same in `BEGIN` / `INSERT` / `COMMIT` | −1.16 µs (IQR −2.21 to +0.01) | as above | **no cost** |
| 3 | K child rows, each a distinct parent, in one transaction, K = 64 / 1,024 | −0.84 / +0.29 µs a row (IQR −1.8..−0.0 / −1.4..+9.4) | fk-less control −0.74 / −0.82 µs | **not resolvable** |
| 3 | the same, K = 4,096 | **+2.84 µs a row** (IQR −1.5..+4.2); inserts alone +2.61 µs | fk-less control −0.99 µs | **resolved, small**: +12 ms on a 4,096-row transaction (+5 %) |
| 3 | the same, K = 16,384 | **+11.29 µs a row** (IQR +9.5..+13.1), 70.9 µs against 59.7 µs: 14,104 against 16,750 rows/s | fk-less control −0.48 µs (IQR −1.5..+1.0) | **resolved, +19 %**: +184 ms on a 16,384-row transaction; all of it in the inserts (+11.17 µs a row), none at the decide (+0.07 µs a row) |
| 3 | K = 1 (one 3-round-trip transaction, per transaction) | −0.17 µs (IQR −7.2..+8.8) | fk-less −1.09 µs | **not resolvable** |
| controls | point read, plain insert, `SHOW META` | see cells 1 and 2 | | B is 0.3–1.1 µs faster on all three; no AZ path in them |

**What AZ costs, in one paragraph.** On a one-row write at one core AZ costs
nothing this run can resolve: neither the assertion admission AZ-S3 added to
every admission (cell 1, three row-set sizes, both sub-shapes) nor the
foreign-key forward check's new ledger test (cell 2) moves a statement by more
than the ~0.7 µs by which B is faster than A on every arm, and the premium of
an assertion over no assertion is the same 3–4 µs on both engines. What AZ
does cost is in a **long transaction of distinct parents**: AZ-S5's
`HoldsRow` asks the transaction's borrow ledger before every parent `S`, the
ledger is a linear vector (`LockHoldings::Holds`, `lock_table.hpp`), and a
distinct parent is never found, so each insert walks the whole ledger. The
excess on a child insert grows with the insert's position in the transaction
(+1.4 µs in the first eighth of a 16,384-row transaction, +18.4 µs in the
last), which makes the per-row cost linear in K — about **0.7 ns × K a row**,
or a transaction quadratic in its distinct parents: invisible at K ≤ 1,024
(+0.3 µs a row, inside the floor), +2.8 µs a row at 4,096, +11.3 µs a row
(+19 %) at 16,384. Measured at one core, `relaxed`, one session; nothing was
measured above K = 16,384.

---

## Rules (`bench/README.md`)

1. **Release, rebuilt at the measured commit.** Both arms built `Release`
   (`CMakeCache.txt` `CMAKE_BUILD_TYPE:STRING=Release`) from `git archive` of
   the commit into a scratch tree (`/home/cdkbs/bench-runs/az-s7/src-A`,
   `src-B`), target `kds_server` only, `-DKDS_BUILD_TESTS=OFF
   -DKDS_BUILD_SIM=OFF` (`build.sh` in the archive). The worktree's own
   build directories were not used. The two builds ran concurrently with
   each other, before any measurement; nothing built during the run.
2. **Block device.** Data files under `/home/cdkbs/bench-runs/az-s7/` on
   `/dev/root`, ext4 (`df -T`).
3. **A copy of the binary, hashed.** Every server started from the copy:

   | arm | commit | describe | binary | sha256 | source binary mtime |
   |---|---|---|---|---|---|
   | A | `a59da9c` (AZ's base, committed 2026-09-30 06:39:03 UTC) | `v2.7.0-533-ga59da9c` | `kds_server-A` | `1d07d33386541ec3a13090d37918e229957e29c2c777ff15f2a63db6957f8746` | 2026-09-30 10:01:28 UTC |
   | B | `273416c` (AZ's last code commit, the merge of AZ-S2..S5 and `origin/main`, committed 2026-09-30 09:54:53 UTC) | `v2.7.0-566-g273416c` | `kds_server-B` | `4b2cddaf7e46a11a74df18d43b66c0a933f89d6680d062ae9ddf148e31dec54d` | 2026-09-30 10:01:29 UTC |

   Both binaries were built after both commits, from exact `git archive`
   trees, so neither is older than the commit it names. The worktree was
   clean at `273416c` when the run started; the files added since —
   `tools/assertion_overhead_benchmark.py` (below) and this file with its
   archive — enter neither binary. A's sha256 is the one the AY-S11 file
   records for its B arm, `0552d55`: `a59da9c`'s engine is that build,
   byte for byte (the commits between are documentation). Each arm ran on its
   own fresh volume in every run.
4. **Host load, per cell.** `/proc/loadavg` and the
   `pgrep -a -f "cc1plus|cmake --build|ctest"` result are in every `host.txt`
   and every JSON under `archive/az-s7-overhead-v2.7.0-566-g273416c/`,
   reproduced in the raw tables. **No competing build in any of the 23
   runs**; one-minute load before each run 0.61–0.99 (the driver's own
   threads; the run script waited, bounded, for it to fall under 1.0). The
   five- and fifteen-minute loads were still decaying from the two builds
   in the first runs (3.7 / 4.7 before run 1, 1.8 / 3.4 before `c3-run3`).
5. **Ports chosen.** 15600 (B) and 15601 (A) for every pair. Not 15432.

Host: 8 CPUs. Every server `cores = 1`, `durability = relaxed` (`group`'s
commit is 82–85 % of an update and would hide what is measured), BTREE,
`log_level = warn`, otherwise defaults; **a fresh server and data file for
every (cell, run)**: 23 pairs. The two servers of a pair run side by side and
the driver alternates which goes first per block, so only one is active at a
time. `strict` was not measured. Behaviour: no code changed, so the test
suite was **not executed**; correctness of the measured statements is
witnessed only by zero driver errors in every arm of every run, and by
`SHOW ASSERTIONS` reporting `enforcing=1` on both servers in every cell-1
run (`show_assertions` in each `c1-*` JSON).

## Drivers

- **Cell 1**: a **new driver, `tools/assertion_overhead_benchmark.py`**, since
  no existing driver prices one engine against another on the assertion path
  (`assertion_benchmark.py` prices an assertion against a twin on one
  engine). It imports the helpers of `fk_overhead_benchmark.py` and has the
  same shape. Relations `as_a` (`CREATE ASSERTION cap ON as_a GROUP BY (g)
  CHECK COUNT(*) <= 2000000000`, declared after the preload) and `as_n` (none),
  each `(id int64, g int64, v int64)` preloaded with `--preload` rows in
  `--preload` distinct groups. **Row-count mapping: `--preload 200` / `1000` /
  `10000` is 200 / 1,000 / 10,000 rows and as many groups present when the
  measured inserts start.** Arms: `insert-assert-existing` (g from the
  existing groups), `insert-assert-newgroup` (every row a never-seen g),
  `insert-none-existing` / `insert-none-newgroup` (the same into `as_n`),
  `insert-none-again` (the floor: `insert-none-existing` repeated), `select`
  (pk read of `as_n`), `ping` (`SHOW META`). `--ops 8000 --blocks 16`, five
  runs per size (40,000 statements per arm per server per size). The ceiling is
  never reached, so no write is refused.
- **Cells 2 and 3**: `tools/fk_overhead_benchmark.py`, **unmodified** since
  AY-S11 (last commit touching it `80af40a`; `kwp.py`, `bench_common.py` and
  `ckdbs_cli.py` likewise unchanged). `--cell price --ops 4000 --blocks 8`,
  five runs; `--cell ledger --ks 1,64,1024,4096,16384 --rows-per-k 32768`,
  three runs (K child rows, each a distinct parent, parents 1..K; reps per K
  = rows / K, at least 5). Cell 3's row-count mapping: K is the rows in one
  transaction; 1,024 / 4,096 / 16,384 stand for the 1K / 10K axis, 64 for the
  small end (a 200-row transaction was not run).
- **AY-S11's cell 1 (the statement mix, `lock_contention_benchmark.py`)
  was not re-run**: AZ changed no path that mix takes beyond the controls here.
- **Not run**: the behavioural fence check (`chk.sh`'s parent `DELETE` against
  an open child transaction), `CREATE ASSERTION` and checkpoint cost as a
  measurement. `CREATE ASSERTION` over the preload was timed once per server
  per run as a by-product (table below); it has no floor of its own and is not
  a finding. Checkpoint cost was not measured.

Reproduction: `all.sh` and `pair.sh` (archive) drive every run; `t1.py`,
`t1b.py` (cell 1), `t2b.py` (cell 2), `t3.py`, `t3b.py` (cell 3) and `gen.py`
(the raw tables below) read `R`, the run directory
`/home/cdkbs/bench-runs/az-s7`, which a reader points at their own. The
`c3-run*` JSON is gzipped (`gunzip -k` before `t3.py`).

## B is uniformly 0.5–1 µs faster, and it is not AZ's

Read this before any row of cells 1 and 2. On every arm the three cells
share — the point read, `SHOW META`, the no-assertion and fk-less inserts,
and the updates — B's p50 is lower than A's by 0.2 to 1.1 µs (cell 1 at 10,000
rows: `select` −0.63, `ping` −0.33, `insert-none-existing` −0.20 µs; cell 2:
`select` −1.06, `ping` −0.46, `parent-update` −0.70, `child-update` −0.76 µs,
and `child-update-again`, the same statement repeated, −0.78 µs), with the
engine's own time per statement lower by 0.1 to 0.4 µs in step. That is 1–2 %
of a ~50 µs statement and larger than the same-server floor (0.15–0.66 µs),
so it is a real difference between the two binaries on paths AZ does not
touch (a read, `SHOW META`, a statement with neither an assertion nor an fk).
Its cause is not identified here: B also carries the `origin/main` merge in
`273416c`, and a different code layout alone can move a path of this length.
**It is why the fk and assertion arms read negative**, and why a cell's B − A
is read against the controls beside it and not against zero. The one control
that does not shift is cell 2's fk-less `child-insert-plain` (−0.01 µs, 22
clean pairs, 4 / 14 slow blocks B / A); cell 1's equivalent (`insert-none-*`)
reads −0.20 to −0.63 µs.

## Cell 1 — assertion admission

**AZ-S3's admission costs a one-row insert nothing that can be told from
the shift above, at any of the three sizes and for both sub-shapes.** B's
admission takes one more hash `Find` of the group key, and for a key opening a
new group a scan of the in-flight holds, plus a per-row unenforceable-set
check; A takes neither. One session per server, serial, autocommit, five runs
of 8,000 statements per arm per server per size, in 16 alternating blocks.

The table uses only block pairs in which both servers ran in their fast mode
(a block is slow when its p50 exceeds 1.12 × that server's own minimum block
for the arm, `t1.py`; the host's slow mode is the one AY-S11 documents: a
server sits at ~51 µs or, for consecutive blocks, ~75 µs, on either engine),
median of per-pair B − A. Nothing is dropped from the raw tables. Derived
qps is 1,000,000 / p50.

| preload (rows and groups) | arm | clean pairs of 80 | B − A p50 (IQR) | B − A engine per op | B p50 | A p50 | derived qps B / A |
|---|---|---|---|---|---|---|---|
| 200 | `insert-assert-existing` | 49 | −0.83 µs (−1.28 to −0.50) | −0.33 µs | 52.4 µs | 53.3 µs | 19,084 / 18,762 qps |
| 200 | `insert-assert-newgroup` | 76 | −0.65 µs (−1.04 to −0.32) | −0.36 µs | 52.9 µs | 53.5 µs | 18,904 / 18,692 qps |
| 200 | control `insert-none-existing` | 68 | −0.51 µs (−0.72 to −0.25) | −0.25 µs | 49.7 µs | 50.1 µs | 20,121 / 19,960 qps |
| 200 | control `insert-none-newgroup` | 66 | −0.63 µs (−0.89 to −0.35) | −0.44 µs | 49.5 µs | 50.2 µs | 20,202 / 19,920 qps |
| 200 | floor `insert-none-again` | 70 | −0.38 µs (−0.69 to −0.16) | −0.25 µs | 49.7 µs | 50.2 µs | 20,121 / 19,920 qps |
| 200 | control `select` | 63 | −0.71 µs (−1.17 to −0.21) | −0.33 µs | 64.3 µs | 65.0 µs | 15,552 / 15,385 qps |
| 200 | control `ping` | 66 | −0.31 µs (−0.57 to −0.05) | −0.17 µs | 37.8 µs | 38.1 µs | 26,455 / 26,247 qps |
| 1,000 | `insert-assert-existing` | 62 | −0.88 µs (−1.14 to −0.51) | −0.38 µs | 52.5 µs | 53.3 µs | 19,048 / 18,762 qps |
| 1,000 | `insert-assert-newgroup` | 57 | −0.75 µs (−1.23 to −0.54) | −0.40 µs | 52.9 µs | 53.7 µs | 18,904 / 18,622 qps |
| 1,000 | control `insert-none-existing` | 55 | −0.21 µs (−0.67 to +0.11) | +0.05 µs | 49.6 µs | 50.0 µs | 20,161 / 20,000 qps |
| 1,000 | control `insert-none-newgroup` | 64 | −0.43 µs (−0.72 to −0.03) | −0.13 µs | 49.6 µs | 49.9 µs | 20,161 / 20,040 qps |
| 1,000 | floor `insert-none-again` | 58 | −0.37 µs (−0.62 to −0.07) | −0.13 µs | 49.7 µs | 50.2 µs | 20,121 / 19,920 qps |
| 1,000 | control `select` | 54 | −0.53 µs (−1.06 to −0.17) | −0.13 µs | 64.8 µs | 65.3 µs | 15,432 / 15,314 qps |
| 1,000 | control `ping` | 75 | −0.42 µs (−0.96 to −0.03) | −0.24 µs | 38.0 µs | 38.4 µs | 26,316 / 26,042 qps |
| 10,000 | `insert-assert-existing` | 56 | −0.52 µs (−0.84 to −0.16) | −0.50 µs | 53.2 µs | 53.6 µs | 18,797 / 18,657 qps |
| 10,000 | `insert-assert-newgroup` | 30 | −0.67 µs (−1.35 to −0.19) | −0.33 µs | 52.7 µs | 53.4 µs | 18,975 / 18,727 qps |
| 10,000 | control `insert-none-existing` | 48 | −0.20 µs (−0.44 to +0.14) | −0.12 µs | 49.7 µs | 49.7 µs | 20,121 / 20,121 qps |
| 10,000 | control `insert-none-newgroup` | 69 | −0.24 µs (−0.62 to +0.05) | −0.23 µs | 49.6 µs | 49.9 µs | 20,161 / 20,040 qps |
| 10,000 | floor `insert-none-again` | 63 | −0.26 µs (−0.45 to +0.11) | −0.16 µs | 49.5 µs | 49.8 µs | 20,202 / 20,080 qps |
| 10,000 | control `select` | 76 | −0.63 µs (−1.08 to −0.09) | −0.30 µs | 63.4 µs | 64.0 µs | 15,773 / 15,625 qps |
| 10,000 | control `ping` | 71 | −0.33 µs (−0.57 to −0.05) | −0.09 µs | 38.1 µs | 38.4 µs | 26,247 / 26,042 qps |

Noise floor from inside the run: the same statement repeated on one server
(`insert-none-existing` against `insert-none-again`, same block position, all
80 blocks) differs by an absolute median of **0.20 µs (B) / 0.26 µs (A) at 200
rows, 0.66 / 0.51 µs at 1,000, 0.42 / 0.31 µs at 10,000**.

The cleaner reading is inside each engine: the premium of an assertion is
the assertion arm less the no-assertion arm of the same shape, same block, on
one server, and AZ's cost is B's premium less A's (`t1b.py`; the IQRs are wide
because a slow-mode block on either engine moves a difference by ~22 µs, the
medians are not):

| preload | shape | premium B (assert − none) | premium A | B − A of the premium (IQR) | engine, B − A of the premium (IQR) |
|---|---|---|---|---|---|
| 200 | key finds its group | +3.20 µs | +3.22 µs | +0.24 µs (−0.85 to +22.28) | +0.59 µs (−0.42 to +10.97) |
| 200 | key opens a group | +3.26 µs | +3.33 µs | −0.04 µs (−0.51 to +0.71) | +0.18 µs (−0.35 to +5.85) |
| 1,000 | key finds its group | +2.74 µs | +3.32 µs | −0.50 µs (−22.81 to +0.32) | −0.53 µs (−10.02 to +0.46) |
| 1,000 | key opens a group | +3.26 µs | +3.95 µs | −0.62 µs (−1.28 to +0.19) | −0.37 µs (−6.39 to +0.13) |
| 10,000 | key finds its group | +3.23 µs | +3.97 µs | −0.70 µs (−22.90 to +0.15) | −0.76 µs (−11.63 to +0.09) |
| 10,000 | key opens a group | +3.48 µs | +4.24 µs | −1.00 µs (−23.53 to +0.46) | −0.43 µs (−10.05 to +3.27) |

- **Nothing positive and resolved.** At 200 rows the two premiums agree to
  0.24 and 0.04 µs, inside the floor; at 1,000 and 10,000 B's premium is
  smaller than A's by 0.5–1.0 µs, which is the direction and size of the
  engine-wide shift, not an assertion effect. Whatever AZ-S3 added to the
  admission is below what a serial 50 µs statement can show.
- **The assertion itself costs ~3–4 µs an insert on both engines** (~2–3 µs
  of it in the engine): 6–8 % of a statement, present in A, and the only
  assertion-shaped number this cell resolves. Neither sub-shape nor the row-set
  size changes it by more than the floor: a key that opens a group costs
  ~0.1–0.6 µs more than one that finds its group, on both engines, and 10,000
  present groups cost no more than 200 (hash `Find`, no scan that grows with
  groups).
- `CREATE ASSERTION` over the preload (one shot a run, by-product, B / A):
  0.2–0.3 ms / 0.2–0.3 ms at 200 rows, 0.8 ms / 0.8–1.0 ms at 1,000,
  7.7–8.5 ms / 7.8–9.5 ms at 10,000: equal on both engines, ~0.8 µs a row.
  It has no floor beside it and is not a measurement of AZ-S2/S3's build-side
  change.

**Latency decomposition (ck-tester's documentation rule 3).** The unit is one serial
statement: ~50 µs (no assertion) = ~14 µs engine (`SHOW META`) + ~36 µs
client, socket and reactor wake (not separable here); the assertion adds ~3–4
µs, ~2–3 µs of it in the engine. No fsync (`relaxed`), no lock or conflict
wait (one session; the assertion directory's latch is taken, never waited on),
no read wait of note.

**Row-count sweep (rule 9).** 200 / 1,000 / 10,000 preloaded rows and groups
are columns of the tables above; the admission does not scale with them (the
premium is 2.7–3.5 µs on B at all three).

## Cell 2 — the FK forward check

**A child insert costs nothing this run can resolve over A.** B asks
`HoldsRow` before the parent `S`; A has no such test. Serial, one session,
five runs of 4,000 statements per arm per server, 8 alternating blocks. The
table uses the same clean-pair filter (1.12 × each server's own minimum block
for the arm) and the median of per-pair B − A (`t2b.py`).

| arm | clean pairs of 40 | B p50 | A p50 | derived qps B / A (1e6 / p50) | **B − A p50** (IQR) | B − A engine per op |
|---|---|---|---|---|---|---|
| `child-insert-fk` (autocommit, one txn) | 26 | 51.2 µs | 51.8 µs | 19,531 / 19,305 qps | **−0.82 µs** (−1.10 to −0.28) | −0.33 µs |
| `child-insert-fk-txn` (BEGIN, INSERT, COMMIT; the unit) | 27 | 110.9 µs | 112.0 µs | 9,017 / 8,929 txn/s | −1.16 µs (−2.21 to +0.01) | −0.32 µs |
| control `child-insert-plain` (no fk declared) | 22 | 49.8 µs | 49.8 µs | 20,080 / 20,080 qps | −0.01 µs (−0.17 to +0.27) | +0.02 µs |
| control `parent-update` | 24 | 50.5 µs | 51.1 µs | 19,802 / 19,569 qps | −0.70 µs (−1.03 to −0.24) | −0.33 µs |
| control `child-update` (fk column untouched) | 20 | 50.7 µs | 51.4 µs | 19,724 / 19,455 qps | −0.76 µs (−1.14 to −0.37) | −0.23 µs |
| noise floor `child-update-again` | 24 | 50.6 µs | 51.2 µs | 19,763 / 19,531 qps | −0.78 µs (−1.21 to −0.22) | −0.17 µs |
| control `select` (pk read) | 23 | 63.8 µs | 64.8 µs | 15,674 / 15,432 qps | −1.06 µs (−1.61 to −0.20) | −0.43 µs |
| control `ping` (`SHOW META`) | 23 | 37.9 µs | 38.4 µs | 26,385 / 26,042 qps | −0.46 µs (−0.74 to −0.28) | −0.41 µs |

- Same-server floor (`child-update` against `child-update-again`, same block,
  clean): B median −0.06 µs, absolute median 0.15 µs (24 blocks); A +0.18 µs,
  0.26 µs (29 blocks). `child-update` and `child-update-again`, the same
  statement twice, read −0.76 and −0.78 µs B − A: the cell reproduces its own
  shift to 0.02 µs.
- **`child-insert-fk` −0.82 µs sits among the controls** (−0.70, −0.76, −0.78,
  −1.06, −0.46 µs) and its engine delta (−0.33 µs) sits among theirs (−0.17 to
  −0.43 µs). The one control that reads zero, `child-insert-plain`, is the
  same insert with no fk: if `HoldsRow` on an empty-ledger insert cost
  anything, `child-insert-fk` would read above the updates, and it reads at
  them. **Bound: `child-insert-fk` is within 0.12 µs of
  the nearest shifted control and 0.36 µs below the highest (`ping`, −0.46
  µs); it is 0.81 µs below the unshifted `child-insert-plain`, in the direction
  of B being faster, not slower.** The ledger of a one-row statement is a
  handful of entries (not counted), so `HoldsRow` scans almost nothing here;
  cell 3 is where it grows.
- The explicit-transaction unit (three round trips) reads −1.16 µs, IQR up to
  +0.01 µs: not resolvable.
- Slow-mode blocks (B / A of 40): `child-insert-fk` 11 / 5, `child-update`
  11 / 9, `select` 8 / 12; arms flip between ~51 and ~75 µs on either engine.
  Filtered pairs only are in the table; raw p50s are in the raw tables.

**Latency decomposition (rule 3).** ~51 µs = ~14 µs engine + ~37 µs client,
socket and reactor wake. No fsync wait (`relaxed`), no lock or conflict wait
(one session: the parent `S` is taken, never waited on), no read wait of
note.

**Row-count sweep (rule 9).** A pk point statement does not scale with rows;
this cell ran at one cardinality (256 parents, 256 children). Cell 3 is the
row-count axis, at the transaction level.

## Cell 3 — a transaction of K distinct parents (the ledger scan)

**The scan AZ-S5 added is a per-insert term that grows with K: invisible at
K ≤ 1,024, +2.8 µs a row at 4,096, +11.3 µs a row at 16,384.** One explicit
transaction inserts K child rows, each referencing a distinct parent (parents
1..K of 16,384), `BEGIN` to `COMMIT` timed whole and divided by K. `plain` is
the same K inserts into a child with no fk declared. K = 1 is a transaction
of three round trips and is per transaction, not per row. Three runs, reps
pooled: K = 1: 98,304 transactions; 64: 1,536; 1,024: 96; 4,096: 24; 16,384:
15. No errors at any K in either arm (`max_locks_per_txn` is 65,536; a
16,384-row transaction takes two entries a row and does not reach it).

Median per-row wall time, derived rows per second (1e6 / µs):

| K | `fk` B | `fk` A | derived rows/s B / A | `plain` B | `plain` A | `fk` − `plain` B / A | paired B − A per row, `fk` (IQR) | paired B − A per row, `plain` (IQR) |
|---|---|---|---|---|---|---|---|---|
| 1 (per txn) | 118.9 µs | 119.2 µs | 8,410 / 8,389 txn/s | 115.7 µs | 116.7 µs | +3.2 / +2.4 µs | −0.17 µs (−7.2 to +8.8) | −1.09 µs (−7.9 to +5.5) |
| 64 | 53.6 µs | 54.3 µs | 18,657 / 18,416 rows/s | 52.6 µs | 53.3 µs | +1.0 / +1.0 µs | −0.84 µs (−1.8 to −0.0) | −0.74 µs (−8.5 to +0.1) |
| 1,024 | 54.2 µs | 53.9 µs | 18,450 / 18,553 rows/s | 51.0 µs | 51.7 µs | +3.2 / +2.2 µs | +0.29 µs (−1.4 to +9.4) | −0.82 µs (−1.4 to +0.1) |
| 4,096 | 57.5 µs | 54.9 µs | 17,391 / 18,215 rows/s | 50.7 µs | 51.6 µs | +6.8 / +3.4 µs | **+2.84 µs** (−1.5 to +4.2) | −0.99 µs (−1.5 to −0.7) |
| 16,384 | 70.9 µs | 59.7 µs | 14,104 / 16,750 rows/s | 51.3 µs | 51.8 µs | +19.7 / +7.9 µs | **+11.29 µs** (+9.5 to +13.1) | −0.48 µs (−1.5 to +1.0) |

Paired B − A is per repetition (both engines ran the same rep back to back,
order alternating). The `plain` column is the floor: it stays within −1.1 to
−0.5 µs at every K (B's general shift again), while `fk` leaves it from
K = 4,096. At K = 16,384 the IQR of `fk` (+9.5 to +13.1 µs) does not reach the
control's; at 4,096 the `fk` IQR spans zero, and the median is resolved only
together with the inserts' own difference (+2.61 µs below) and the octile
slope.

**Where the excess is: in the inserts, none at the decide.** The transaction
splits into its inserts and the rest (`BEGIN`, `COMMIT`, the driver's loop;
`t3b.py`). The rest, per transaction, per run:

| K | `fk` B | `fk` A | `plain` B | `plain` A | `fk` B − A of the rest | inserts B − A (median, per row) |
|---|---|---|---|---|---|---|
| 1,024 | 0.80 / 0.75 / 0.72 ms | 0.75 / 0.78 / 0.71 ms | 0.44 / 0.47 / 0.47 ms | 0.44 / 0.45 / 0.44 ms | +0.01 ms (+0.01 µs a row) | +0.28 µs |
| 4,096 | 4.46 / 4.49 / 4.40 ms | 4.57 / 4.54 / 4.43 ms | 1.91 / 1.92 / 2.00 ms | 1.93 / 1.89 / 2.01 ms | −0.07 ms (−0.02 µs a row) | +2.61 µs |
| 16,384 | 49.77 / 50.63 / 50.67 ms | 49.15 / 49.14 / 49.33 ms | 13.63 / 13.60 / 14.26 ms | 13.65 / 13.65 / 13.41 ms | +1.15 ms (+0.07 µs a row) | +11.17 µs |

The decide's cost is the same on both engines (and on both, `fk`'s rest at
16,384 is 49 ms against `plain`'s 14 ms: the lock table's release of twice
the entries, present in A). What differs is the inserts, and their excess
depends on position. Per-statement latency by eighth of the transaction
(pooled µs, `stmt_us_by_pos`; the 7th eighth carries the stall below on both
engines):

| K | engine | 1st | 2nd | 3rd | 4th | 5th | 6th | 7th | 8th |
|---|---|---|---|---|---|---|---|---|---|
| 16,384 | `fk` B | 56.1 µs | 60.9 µs | 65.5 µs | 70.1 µs | 74.3 µs | 74.6 µs | 114.2 µs | 79.1 µs |
| 16,384 | `fk` A | 54.7 µs | 53.6 µs | 59.0 µs | 56.0 µs | 55.5 µs | 58.4 µs | 97.1 µs | 60.7 µs |
| 16,384 | B − A | +1.4 µs | +7.3 µs | +6.5 µs | +14.1 µs | +18.8 µs | +16.2 µs | +17.1 µs | +18.4 µs |
| 4,096 | `fk` B | 52.9 µs | 55.1 µs | 57.3 µs | 58.1 µs | 62.3 µs | 59.6 µs | 59.8 µs | 61.2 µs |
| 4,096 | `fk` A | 55.6 µs | 57.1 µs | 54.7 µs | 55.6 µs | 56.0 µs | 56.5 µs | 58.0 µs | 56.3 µs |
| 4,096 | B − A | −2.7 µs | −2.0 µs | +2.6 µs | +2.5 µs | +6.3 µs | +3.1 µs | +1.8 µs | +4.9 µs |
| 16,384 | `plain` B / A | 53.5 / 50.8 µs | 53.1 / 50.7 µs | 52.8 / 50.5 µs | 50.5 / 50.6 µs | 50.0 / 57.1 µs | 50.3 / 51.7 µs | 53.2 / 50.6 µs | 50.9 / 50.9 µs |

**Reading.** B's insert latency climbs with its position in the transaction,
A's does not, and the `plain` control, which takes no parent `S`, is flat on
both. A linear scan whose length is the ledger's size reads exactly this way:
the excess at the last eighth of 16,384 (+18.4 µs at row ~15,400) is ~3.8
times the excess at the last eighth of 4,096 (+4.9 µs at row ~3,600) for ~4.3
times the position, and the per-row medians rise ×4 from K = 4,096 to 16,384
(+2.8 to +11.3 µs; +0.3 µs at 1,024, inside the floor), **about 0.7 ns × K a
row** (0.7 ns × 4,096 = 2.9 µs, × 16,384 = 11.5 µs). Source read, not profiled, at `273416c`: `HoldsRow`
(`src/server/command_dispatcher.cpp`) asks
`LockHoldings::Holds` (`include/kds/txn/lock_table.hpp`), which iterates the
whole `held_` vector, before every parent `S`; a distinct parent is never
held, so the scan never returns early, and the ledger gains the child's tuple
borrow and the parent's `S` per row. Indexing the ledger's lookup would
remove what was measured; by source read only. The fit is a three-point one
with a floor of about ±1 µs beside it, so it fixes the order of the cost and
not its exponent.

**The ~0.4 s stall is on both engines, again.** In every run the first
K = 1,024 `fk` transaction (6th eighth, 162 µs on B, 156 µs on A) and the
second K = 16,384 `fk` transaction (7th eighth, 114 / 97 µs) each carry ~0.4 s
more. It never falls in `plain`, which runs after `fk` at each K. It lifts
both engines' engine time per row at K = 16,384 (B 35.8 µs, A 23.5 µs; `plain`
13.6 / 13.3 µs), and is not a K-dependent term; its cause is not identified
here.

**Latency decomposition (rule 3).** `relaxed`: no fsync wait inside the
transaction (the decide writes the log once), no lock wait (one session). At
K ≥ 64 the unit is ~51–59 µs a row on A, of which the engine is ~13 µs
(`plain`); on B at K = 16,384 the scan adds ~11 µs a row, all client-visible
inside the insert statement.

## What this decides, and what it does not

- **AZ's overhead on the general path at one core is not resolvable**
  (cells 1 and 2): assertion admission and a one-row child insert read
  inside the controls, and B is ~0.5–1 µs faster than A on the paths AZ did
  not touch. Nothing here is a regression for a one-row write. The overhead
  measurement `CLAUDE.md`'s step 3 asks at a milestone's close is, for AZ,
  **measured**.
- **AZ-S5's `HoldsRow` has its first data point, and it is the one
  `docs/inflight/known-gaps.md` (Foreign keys) names**: a per-insert linear
  scan of the borrow ledger, ~0.7 ns × K a row, no cost to a transaction of
  1,000 distinct parents, +5 % at 4,096 and +19 % at 16,384. Whether it stands
  as the price, or the lookup is indexed, is the operator's to decide;
  measured at one core, one session only.
- **Not measured, and not implied**: `cores > 1`, `group` or `strict`
  durability, contended parent or assertion locks, the assertion's
  in-flight-hold scan under a transaction holding many groups (cell 1 is
  autocommit, so the scan sees one hold), the behavioural fence check, `CREATE
  ASSERTION` and checkpoint as a priced cost, PostgreSQL's floor for these
  shapes (not measured for this shape), and any client faster than this
  Python driver (~36 µs of a ~50 µs statement lies outside the engine, so a
  faster client would see a sub-µs cost as a larger share).
- **Qualifications.** (1) The 0.5–1 µs B-faster shift is unexplained (above);
  every cell-1 and cell-2 B − A is read against it, which costs resolution:
  a B-side cost below ~0.3 µs cannot be excluded. (2) The clean-pair filter
  and its 1.12× threshold are this file's choice, inherited from AY-S11; the
  unfiltered raw tables carry every block. The bimodal slow mode is on both
  engines (slow blocks B / A of 80 in cell 1: 22 / 10 for
  `insert-assert-existing` at 200 rows, 2 / 23 at 10,000). (3) Cell 3's
  K = 4,096 and 16,384 rest on 24 and 15 paired repetitions, and the stall
  above sits in both arms; the 4,096 `fk` IQR spans zero. (4) Cell 1 is
  autocommit, on a driver written in this stage; both arms ran the same
  driver, and `bench/README.md` asks for unmodified drivers so that a driver
  change is not measured as an engine change, which holds here only because
  of that. (5) No `SHOW META` control runs inside cell 3's transactions; its
  control is the fk-less `plain` insert. (6) A's binary is byte-identical to
  the one AY-S11 measured as its B arm, so cell 3's A arm is a repeat of that
  engine on a different day: the rest at K = 16,384 (49 ms on A) and the
  ~0.4 s stall are reproduced. (7) First runs after the two builds saw
  five-minute loads of 3.7–4.7 (decaying); the one-minute load was under 1.0
  and no build ran.

---

## Raw tables

Every arm, both engines, p0 / p25 / p50 / p95 / p99 with throughput, per run.
The JSON under `archive/az-s7-overhead-v2.7.0-566-g273416c/` (`c1-n{200,1000,
10000}-run{1..5}`, `c2-run{1..5}`, `c3-run{1..3}`, each with `result.json`
(gzipped in `c3-run*`), `driver.txt`, `host.txt` and both servers' configs;
servers' warn logs are `*.log`, which `.gitignore` keeps out of the commit)
carries the per-block records and the per-core `SHOW META` before and after.
Cell 3's table here gives each run's median per-row wall time and engine time
per row; the per-position and per-rep data are in the JSON.

### Cell 1 raw (assertion admission)

**`c1-n200-run1/result.json`** - precell loadavg: 0.61 3.74 4.73 1/318 295179; in-driver load before `0.61 3.74 4.73 1/313 295207`, after `0.64 3.70 4.71 2/328 295225`; competing builds: none

preload 200, create assertion B / A: 0.2 / 0.2 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 43.3 / 51.1 / 52.1 / 67.5 / 76.3 µs, 17726 qps | 43.4 / 52.1 / 53.0 / 65.3 / 76.6 µs, 17567 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.6 / 52.0 / 52.9 / 67.3 / 77.6 µs, 17292 qps | 45.5 / 52.5 / 53.2 / 67.8 / 78.2 µs, 17394 qps | 0 / 0 errors |
| `insert-none-existing` | 41.3 / 48.6 / 49.8 / 63.9 / 74.2 µs, 18887 qps | 40.4 / 48.8 / 50.0 / 64.8 / 74.4 µs, 18676 qps | 0 / 0 errors |
| `insert-none-newgroup` | 42.1 / 48.6 / 49.9 / 62.3 / 72.7 µs, 18556 qps | 43.5 / 48.8 / 50.0 / 62.4 / 73.2 µs, 18342 qps | 0 / 0 errors |
| `insert-none-again` | 40.3 / 48.7 / 49.8 / 62.2 / 73.8 µs, 18668 qps | 40.3 / 48.9 / 50.1 / 65.0 / 75.4 µs, 17776 qps | 0 / 0 errors |
| `select` | 60.3 / 64.1 / 64.9 / 80.4 / 90.4 µs, 14725 qps | 58.8 / 64.2 / 64.9 / 81.7 / 93.0 µs, 14663 qps | 0 / 0 errors |
| `ping` | 36.6 / 37.4 / 37.8 / 55.5 / 68.9 µs, 24552 qps | 37.0 / 37.8 / 38.4 / 63.2 / 74.5 µs, 22336 qps | 0 / 0 errors |

**`c1-n200-run2/result.json`** - precell loadavg: 0.93 2.75 4.19 1/315 295830; in-driver load before `0.93 2.75 4.19 1/324 295861`, after `1.09 2.75 4.19 1/317 295869`; competing builds: none

preload 200, create assertion B / A: 0.3 / 0.3 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 43.1 / 52.3 / 53.7 / 79.0 / 95.2 µs, 15708 qps | 45.9 / 52.7 / 54.1 / 78.5 / 93.9 µs, 16031 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.6 / 52.5 / 53.3 / 65.1 / 76.4 µs, 16776 qps | 46.2 / 52.8 / 53.7 / 68.4 / 79.1 µs, 17264 qps | 0 / 0 errors |
| `insert-none-existing` | 39.5 / 48.6 / 49.6 / 60.0 / 70.9 µs, 19095 qps | 40.4 / 49.2 / 50.2 / 62.0 / 73.9 µs, 18792 qps | 0 / 0 errors |
| `insert-none-newgroup` | 40.7 / 48.5 / 49.4 / 60.2 / 70.1 µs, 18747 qps | 41.2 / 49.1 / 50.2 / 66.2 / 75.7 µs, 18150 qps | 0 / 0 errors |
| `insert-none-again` | 41.7 / 48.7 / 49.7 / 72.0 / 79.8 µs, 18328 qps | 41.7 / 49.3 / 50.6 / 74.1 / 87.6 µs, 16903 qps | 0 / 0 errors |
| `select` | 57.3 / 63.3 / 64.1 / 85.2 / 97.6 µs, 14726 qps | 59.1 / 64.4 / 65.1 / 80.7 / 91.2 µs, 14690 qps | 0 / 0 errors |
| `ping` | 36.6 / 37.5 / 37.8 / 47.2 / 56.4 µs, 25253 qps | 37.5 / 38.7 / 39.5 / 49.1 / 58.8 µs, 23347 qps | 0 / 0 errors |

**`c1-n200-run3/result.json`** - precell loadavg: 0.95 2.04 3.66 1/322 296597; in-driver load before `0.95 2.04 3.66 1/326 296621`, after `1.04 2.02 3.64 1/314 296626`; competing builds: none

preload 200, create assertion B / A: 0.2 / 0.2 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 45.2 / 51.4 / 53.7 / 82.3 / 97.3 µs, 14934 qps | 45.8 / 52.2 / 53.4 / 79.2 / 94.8 µs, 15866 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.3 / 51.5 / 52.7 / 67.0 / 77.7 µs, 17602 qps | 47.2 / 52.6 / 53.5 / 64.8 / 75.0 µs, 17424 qps | 0 / 0 errors |
| `insert-none-existing` | 42.4 / 49.1 / 50.4 / 74.4 / 89.1 µs, 17575 qps | 41.7 / 50.3 / 71.5 / 84.0 / 94.0 µs, 15144 qps | 0 / 0 errors |
| `insert-none-newgroup` | 41.9 / 48.8 / 50.0 / 73.3 / 87.1 µs, 17824 qps | 43.3 / 51.0 / 72.0 / 86.1 / 95.3 µs, 14061 qps | 0 / 0 errors |
| `insert-none-again` | 41.3 / 48.9 / 49.9 / 62.7 / 72.7 µs, 18704 qps | 43.1 / 49.2 / 50.2 / 73.3 / 82.6 µs, 17939 qps | 0 / 0 errors |
| `select` | 60.0 / 65.2 / 66.5 / 99.8 / 116.7 µs, 13298 qps | 59.6 / 66.1 / 67.3 / 101.8 / 116.2 µs, 12946 qps | 0 / 0 errors |
| `ping` | 35.0 / 37.7 / 38.0 / 63.1 / 72.6 µs, 23022 qps | 37.1 / 37.9 / 38.3 / 64.8 / 68.9 µs, 23716 qps | 0 / 0 errors |

**`c1-n200-run4/result.json`** - precell loadavg: 0.93 1.57 3.17 1/314 297418; in-driver load before `0.93 1.57 3.17 1/318 297442`, after `0.94 1.56 3.15 1/312 297450`; competing builds: none

preload 200, create assertion B / A: 0.3 / 0.2 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 44.9 / 53.5 / 74.8 / 89.2 / 98.4 µs, 13676 qps | 45.8 / 52.7 / 53.4 / 77.2 / 91.6 µs, 16676 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.5 / 52.2 / 53.1 / 76.0 / 85.6 µs, 16750 qps | 45.8 / 53.0 / 53.6 / 65.8 / 76.7 µs, 17160 qps | 0 / 0 errors |
| `insert-none-existing` | 39.6 / 48.8 / 49.7 / 59.6 / 69.8 µs, 19082 qps | 41.2 / 49.2 / 50.4 / 73.8 / 87.1 µs, 17553 qps | 0 / 0 errors |
| `insert-none-newgroup` | 41.3 / 48.6 / 49.7 / 59.5 / 69.2 µs, 18818 qps | 41.1 / 49.0 / 50.0 / 61.1 / 72.7 µs, 17536 qps | 0 / 0 errors |
| `insert-none-again` | 42.7 / 48.6 / 49.6 / 59.1 / 68.7 µs, 18754 qps | 42.2 / 49.0 / 50.0 / 61.4 / 72.3 µs, 18746 qps | 0 / 0 errors |
| `select` | 56.5 / 63.4 / 64.7 / 97.3 / 114.1 µs, 13757 qps | 61.4 / 64.4 / 65.4 / 97.2 / 113.3 µs, 13656 qps | 0 / 0 errors |
| `ping` | 36.8 / 37.8 / 38.2 / 63.4 / 74.3 µs, 22563 qps | 36.9 / 37.7 / 38.0 / 47.1 / 56.0 µs, 25155 qps | 0 / 0 errors |

**`c1-n200-run5/result.json`** - precell loadavg: 0.94 1.47 3.04 1/313 297743; in-driver load before `0.94 1.47 3.04 1/316 297776`, after `1.02 1.48 3.03 1/320 297787`; competing builds: none

preload 200, create assertion B / A: 0.3 / 0.2 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 45.4 / 51.2 / 52.5 / 78.1 / 95.1 µs, 16109 qps | 45.9 / 52.8 / 53.8 / 77.8 / 91.5 µs, 16723 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.0 / 51.7 / 52.8 / 78.6 / 94.1 µs, 16043 qps | 47.4 / 53.1 / 53.9 / 68.4 / 79.5 µs, 16991 qps | 0 / 0 errors |
| `insert-none-existing` | 41.2 / 48.5 / 49.5 / 59.5 / 69.7 µs, 19119 qps | 40.3 / 49.2 / 50.3 / 65.6 / 75.8 µs, 18652 qps | 0 / 0 errors |
| `insert-none-newgroup` | 41.2 / 48.4 / 49.3 / 58.2 / 69.5 µs, 19077 qps | 41.2 / 49.1 / 50.3 / 64.5 / 73.7 µs, 18489 qps | 0 / 0 errors |
| `insert-none-again` | 41.6 / 48.9 / 50.4 / 75.7 / 91.0 µs, 16799 qps | 40.2 / 49.2 / 50.4 / 64.5 / 74.6 µs, 17212 qps | 0 / 0 errors |
| `select` | 61.3 / 63.5 / 64.3 / 100.8 / 115.1 µs, 13845 qps | 59.7 / 64.2 / 64.9 / 80.1 / 91.0 µs, 14713 qps | 0 / 0 errors |
| `ping` | 36.8 / 37.5 / 37.8 / 51.0 / 61.4 µs, 25052 qps | 37.1 / 37.7 / 38.0 / 48.9 / 58.2 µs, 25027 qps | 0 / 0 errors |

**`c1-n1000-run1/result.json`** - precell loadavg: 0.64 3.70 4.71 1/324 295244; in-driver load before `0.75 3.67 4.69 1/328 295286`, after `0.85 3.64 4.68 1/321 295295`; competing builds: none

preload 1000, create assertion B / A: 0.8 / 0.8 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 45.5 / 51.2 / 52.1 / 75.3 / 86.1 µs, 17016 qps | 45.6 / 52.5 / 53.3 / 65.5 / 76.0 µs, 17486 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.6 / 52.3 / 58.1 / 84.9 / 99.5 µs, 14846 qps | 46.0 / 53.7 / 74.4 / 87.6 / 100.9 µs, 14193 qps | 0 / 0 errors |
| `insert-none-existing` | 41.0 / 48.8 / 49.8 / 71.2 / 79.1 µs, 18315 qps | 40.0 / 50.4 / 55.0 / 80.1 / 94.2 µs, 15592 qps | 0 / 0 errors |
| `insert-none-newgroup` | 40.5 / 49.4 / 51.7 / 78.9 / 92.2 µs, 15588 qps | 41.0 / 49.1 / 50.3 / 67.5 / 75.9 µs, 18144 qps | 0 / 0 errors |
| `insert-none-again` | 42.3 / 50.0 / 70.5 / 82.6 / 93.6 µs, 15030 qps | 41.4 / 49.2 / 50.4 / 70.5 / 77.9 µs, 18292 qps | 0 / 0 errors |
| `select` | 57.5 / 64.9 / 71.9 / 103.1 / 116.8 µs, 12484 qps | 59.9 / 64.4 / 65.3 / 81.1 / 92.2 µs, 14556 qps | 0 / 0 errors |
| `ping` | 36.8 / 37.7 / 38.0 / 55.0 / 63.7 µs, 24600 qps | 37.0 / 37.8 / 38.0 / 48.4 / 58.1 µs, 25078 qps | 0 / 0 errors |

**`c1-n1000-run2/result.json`** - precell loadavg: 0.92 2.60 4.11 1/321 295990; in-driver load before `0.92 2.60 4.11 1/325 296017`, after `0.94 2.55 4.07 1/319 296033`; competing builds: none

preload 1000, create assertion B / A: 0.8 / 0.8 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 44.9 / 52.1 / 55.6 / 84.5 / 97.7 µs, 14979 qps | 45.6 / 53.1 / 54.5 / 81.3 / 97.6 µs, 15260 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.8 / 52.2 / 53.2 / 68.1 / 78.5 µs, 17437 qps | 47.6 / 53.4 / 54.1 / 66.7 / 77.3 µs, 17151 qps | 0 / 0 errors |
| `insert-none-existing` | 40.5 / 49.6 / 52.9 / 78.9 / 93.5 µs, 15606 qps | 40.2 / 48.9 / 50.0 / 72.4 / 79.0 µs, 18337 qps | 0 / 0 errors |
| `insert-none-newgroup` | 42.3 / 49.2 / 50.5 / 74.0 / 89.4 µs, 15580 qps | 42.2 / 49.1 / 50.2 / 73.2 / 86.9 µs, 17722 qps | 0 / 0 errors |
| `insert-none-again` | 40.1 / 49.0 / 50.0 / 64.0 / 74.3 µs, 18513 qps | 40.8 / 49.3 / 50.7 / 75.3 / 89.2 µs, 17163 qps | 0 / 0 errors |
| `select` | 58.2 / 64.9 / 66.5 / 97.3 / 114.6 µs, 13578 qps | 60.8 / 64.9 / 78.9 / 111.0 / 124.1 µs, 11989 qps | 0 / 0 errors |
| `ping` | 37.4 / 38.2 / 38.5 / 49.6 / 59.8 µs, 24523 qps | 37.3 / 38.1 / 38.5 / 50.9 / 63.6 µs, 24414 qps | 0 / 0 errors |

**`c1-n1000-run3/result.json`** - precell loadavg: 0.95 1.98 3.62 1/313 296675; in-driver load before `0.95 1.98 3.62 1/318 296702`, after `1.04 1.99 3.61 2/314 296714`; competing builds: none

preload 1000, create assertion B / A: 0.8 / 1.0 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 43.8 / 51.8 / 52.9 / 67.0 / 77.3 µs, 17568 qps | 46.0 / 53.0 / 53.9 / 79.7 / 95.5 µs, 15774 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 44.8 / 52.0 / 53.3 / 81.1 / 97.3 µs, 15889 qps | 45.6 / 53.4 / 54.5 / 80.9 / 96.3 µs, 15042 qps | 0 / 0 errors |
| `insert-none-existing` | 42.0 / 49.2 / 60.5 / 81.3 / 94.1 µs, 15532 qps | 38.7 / 48.8 / 49.8 / 61.4 / 73.0 µs, 18677 qps | 0 / 0 errors |
| `insert-none-newgroup` | 40.0 / 48.0 / 49.1 / 72.4 / 84.6 µs, 16590 qps | 42.2 / 48.5 / 49.6 / 60.9 / 71.0 µs, 18960 qps | 0 / 0 errors |
| `insert-none-again` | 42.2 / 48.4 / 49.5 / 64.1 / 74.3 µs, 18745 qps | 40.0 / 48.9 / 49.9 / 62.2 / 72.7 µs, 18588 qps | 0 / 0 errors |
| `select` | 58.4 / 63.9 / 64.9 / 81.9 / 94.4 µs, 14614 qps | 59.7 / 64.5 / 65.6 / 82.2 / 97.5 µs, 14440 qps | 0 / 0 errors |
| `ping` | 36.9 / 37.7 / 38.1 / 62.3 / 67.9 µs, 23876 qps | 37.0 / 38.0 / 38.4 / 60.6 / 65.5 µs, 23931 qps | 0 / 0 errors |

**`c1-n1000-run4/result.json`** - precell loadavg: 0.86 1.54 3.14 1/315 297476; in-driver load before `0.86 1.54 3.14 1/317 297500`, after `0.95 1.54 3.13 1/325 297514`; competing builds: none

preload 1000, create assertion B / A: 0.8 / 0.8 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 43.7 / 51.3 / 52.3 / 67.5 / 76.8 µs, 17474 qps | 45.6 / 52.5 / 53.2 / 65.7 / 76.3 µs, 17291 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 46.8 / 51.6 / 52.7 / 68.9 / 80.5 µs, 17490 qps | 45.7 / 52.9 / 53.7 / 67.7 / 77.6 µs, 17128 qps | 0 / 0 errors |
| `insert-none-existing` | 41.1 / 48.7 / 49.8 / 71.9 / 80.3 µs, 17860 qps | 39.3 / 49.2 / 50.3 / 60.1 / 70.9 µs, 18722 qps | 0 / 0 errors |
| `insert-none-newgroup` | 38.2 / 48.6 / 49.6 / 63.9 / 74.1 µs, 18056 qps | 41.2 / 49.2 / 50.3 / 60.8 / 71.1 µs, 18554 qps | 0 / 0 errors |
| `insert-none-again` | 42.6 / 48.7 / 49.7 / 60.2 / 70.8 µs, 18873 qps | 40.5 / 50.1 / 52.5 / 80.0 / 93.4 µs, 15540 qps | 0 / 0 errors |
| `select` | 58.3 / 64.1 / 65.3 / 88.8 / 101.5 µs, 14384 qps | 59.3 / 64.6 / 65.7 / 83.7 / 95.3 µs, 14449 qps | 0 / 0 errors |
| `ping` | 36.7 / 37.6 / 37.9 / 51.2 / 60.9 µs, 24969 qps | 37.3 / 38.2 / 38.6 / 52.1 / 61.4 µs, 24171 qps | 0 / 0 errors |

**`c1-n1000-run5/result.json`** - precell loadavg: 0.94 1.46 3.02 1/316 297806; in-driver load before `0.94 1.46 3.02 1/314 297833`, after `1.02 1.47 3.01 1/319 297847`; competing builds: none

preload 1000, create assertion B / A: 0.8 / 0.8 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 45.4 / 51.8 / 52.8 / 68.5 / 78.7 µs, 17550 qps | 44.9 / 52.7 / 53.6 / 79.5 / 96.0 µs, 16174 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.2 / 52.3 / 53.2 / 69.0 / 78.8 µs, 17272 qps | 46.5 / 52.9 / 53.6 / 67.5 / 80.4 µs, 17038 qps | 0 / 0 errors |
| `insert-none-existing` | 40.8 / 48.6 / 49.6 / 64.0 / 73.6 µs, 18587 qps | 40.6 / 48.6 / 49.6 / 73.2 / 82.1 µs, 17890 qps | 0 / 0 errors |
| `insert-none-newgroup` | 41.3 / 48.6 / 49.7 / 72.2 / 78.9 µs, 17597 qps | 39.9 / 48.7 / 49.9 / 73.5 / 87.1 µs, 17805 qps | 0 / 0 errors |
| `insert-none-again` | 41.8 / 48.7 / 50.0 / 73.2 / 86.5 µs, 17574 qps | 40.8 / 48.6 / 49.8 / 73.3 / 82.6 µs, 18154 qps | 0 / 0 errors |
| `select` | 58.0 / 63.2 / 64.5 / 103.8 / 117.8 µs, 14003 qps | 62.3 / 64.6 / 65.5 / 82.3 / 93.1 µs, 14482 qps | 0 / 0 errors |
| `ping` | 34.4 / 37.1 / 37.4 / 62.6 / 70.0 µs, 23602 qps | 37.3 / 38.1 / 38.5 / 51.2 / 62.0 µs, 24208 qps | 0 / 0 errors |

**`c1-n10000-run1/result.json`** - precell loadavg: 0.78 3.58 4.65 1/317 295314; in-driver load before `0.78 3.58 4.65 1/321 295349`, after `0.90 3.51 4.62 1/328 295362`; competing builds: none

preload 10000, create assertion B / A: 7.8 / 7.8 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 46.0 / 52.3 / 53.2 / 66.5 / 76.5 µs, 17443 qps | 44.3 / 53.5 / 54.6 / 81.2 / 97.5 µs, 15367 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.0 / 51.9 / 53.1 / 77.2 / 91.9 µs, 16784 qps | 46.7 / 54.4 / 76.6 / 91.5 / 102.0 µs, 13248 qps | 0 / 0 errors |
| `insert-none-existing` | 42.4 / 48.8 / 50.0 / 72.6 / 85.0 µs, 17904 qps | 41.6 / 50.0 / 64.9 / 81.9 / 94.9 µs, 15236 qps | 0 / 0 errors |
| `insert-none-newgroup` | 40.5 / 48.8 / 49.8 / 71.6 / 80.5 µs, 17859 qps | 41.6 / 49.1 / 50.3 / 66.4 / 79.7 µs, 17896 qps | 0 / 0 errors |
| `insert-none-again` | 42.5 / 48.6 / 49.5 / 61.6 / 70.8 µs, 18864 qps | 41.5 / 48.9 / 49.9 / 60.3 / 69.2 µs, 18520 qps | 0 / 0 errors |
| `select` | 59.2 / 62.6 / 63.3 / 75.3 / 84.9 µs, 15221 qps | 61.6 / 63.6 / 64.2 / 74.9 / 81.5 µs, 15100 qps | 0 / 0 errors |
| `ping` | 33.1 / 37.9 / 38.1 / 49.8 / 58.7 µs, 24888 qps | 37.1 / 38.1 / 38.5 / 63.2 / 68.2 µs, 23403 qps | 0 / 0 errors |

**`c1-n10000-run2/result.json`** - precell loadavg: 0.94 2.55 4.07 1/314 296052; in-driver load before `0.94 2.55 4.07 1/316 296077`, after `1.18 2.55 4.06 1/316 296093`; competing builds: none

preload 10000, create assertion B / A: 8.5 / 8.1 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 46.7 / 51.7 / 52.7 / 64.2 / 74.0 µs, 17653 qps | 47.4 / 52.7 / 53.4 / 67.8 / 78.2 µs, 17082 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.8 / 51.6 / 53.0 / 80.5 / 97.2 µs, 15348 qps | 45.0 / 52.6 / 53.5 / 66.9 / 78.2 µs, 17379 qps | 0 / 0 errors |
| `insert-none-existing` | 40.5 / 48.6 / 49.7 / 61.5 / 72.0 µs, 18728 qps | 42.9 / 48.6 / 49.5 / 60.7 / 70.7 µs, 18711 qps | 0 / 0 errors |
| `insert-none-newgroup` | 41.2 / 48.7 / 49.7 / 59.7 / 69.5 µs, 18926 qps | 41.3 / 48.6 / 49.5 / 59.8 / 68.8 µs, 18915 qps | 0 / 0 errors |
| `insert-none-again` | 40.2 / 48.6 / 49.6 / 64.2 / 74.5 µs, 18758 qps | 41.9 / 48.6 / 49.7 / 73.3 / 85.9 µs, 18020 qps | 0 / 0 errors |
| `select` | 58.1 / 62.0 / 62.8 / 76.5 / 88.0 µs, 15286 qps | 61.3 / 63.5 / 64.1 / 77.3 / 96.4 µs, 14949 qps | 0 / 0 errors |
| `ping` | 36.7 / 37.4 / 37.8 / 64.2 / 76.7 µs, 22170 qps | 37.2 / 38.3 / 38.9 / 64.1 / 68.1 µs, 22776 qps | 0 / 0 errors |

**`c1-n10000-run3/result.json`** - precell loadavg: 0.95 1.95 3.59 1/316 296740; in-driver load before `0.95 1.95 3.59 1/321 296767`, after `1.20 1.97 3.58 1/318 296782`; competing builds: none

preload 10000, create assertion B / A: 8.1 / 8.2 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 45.0 / 52.9 / 53.7 / 76.5 / 84.7 µs, 16860 qps | 46.0 / 76.4 / 77.6 / 93.5 / 102.3 µs, 12482 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.5 / 75.5 / 77.0 / 93.0 / 102.2 µs, 12835 qps | 46.6 / 53.4 / 54.1 / 76.9 / 85.8 µs, 15800 qps | 0 / 0 errors |
| `insert-none-existing` | 42.0 / 50.1 / 71.4 / 83.7 / 94.1 µs, 14903 qps | 42.5 / 49.1 / 50.2 / 62.7 / 74.0 µs, 18541 qps | 0 / 0 errors |
| `insert-none-newgroup` | 42.1 / 49.3 / 50.7 / 77.3 / 91.1 µs, 16488 qps | 41.4 / 49.1 / 50.1 / 62.1 / 73.0 µs, 18238 qps | 0 / 0 errors |
| `insert-none-again` | 41.5 / 51.1 / 71.8 / 85.4 / 94.6 µs, 14450 qps | 43.9 / 49.4 / 50.7 / 74.9 / 89.8 µs, 17229 qps | 0 / 0 errors |
| `select` | 56.1 / 63.4 / 64.1 / 79.0 / 87.5 µs, 14947 qps | 58.2 / 64.0 / 65.1 / 97.2 / 113.9 µs, 13467 qps | 0 / 0 errors |
| `ping` | 37.0 / 37.9 / 38.2 / 50.5 / 62.4 µs, 24687 qps | 37.1 / 38.1 / 38.4 / 48.9 / 58.1 µs, 23387 qps | 0 / 0 errors |

**`c1-n10000-run4/result.json`** - precell loadavg: 0.95 1.54 3.13 1/314 297533; in-driver load before `1.04 1.55 3.12 1/318 297561`, after `1.10 1.55 3.11 1/318 297574`; competing builds: none

preload 10000, create assertion B / A: 8.1 / 7.9 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 45.1 / 52.4 / 53.1 / 67.0 / 77.7 µs, 17222 qps | 46.7 / 53.2 / 54.4 / 81.3 / 97.6 µs, 15402 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 46.2 / 51.9 / 52.8 / 67.1 / 77.5 µs, 16530 qps | 46.4 / 53.0 / 62.3 / 86.9 / 100.1 µs, 14434 qps | 0 / 0 errors |
| `insert-none-existing` | 40.0 / 48.7 / 49.8 / 63.0 / 72.0 µs, 18234 qps | 41.1 / 49.3 / 51.1 / 78.5 / 92.6 µs, 16113 qps | 0 / 0 errors |
| `insert-none-newgroup` | 41.5 / 48.6 / 49.7 / 71.1 / 76.5 µs, 18523 qps | 42.6 / 48.8 / 50.1 / 73.7 / 85.5 µs, 17962 qps | 0 / 0 errors |
| `insert-none-again` | 41.6 / 48.7 / 49.8 / 72.0 / 81.3 µs, 18314 qps | 42.3 / 48.9 / 50.0 / 73.6 / 86.5 µs, 17963 qps | 0 / 0 errors |
| `select` | 59.1 / 62.6 / 63.2 / 74.4 / 82.8 µs, 15273 qps | 60.9 / 63.2 / 63.9 / 79.1 / 89.2 µs, 14992 qps | 0 / 0 errors |
| `ping` | 36.9 / 37.6 / 37.9 / 46.7 / 55.0 µs, 25308 qps | 37.2 / 37.9 / 38.3 / 52.0 / 62.7 µs, 24286 qps | 0 / 0 errors |

**`c1-n10000-run5/result.json`** - precell loadavg: 0.94 1.44 2.99 2/315 297866; in-driver load before `0.94 1.44 2.99 1/324 297895`, after `1.11 1.46 2.98 1/317 297902`; competing builds: none

preload 10000, create assertion B / A: 7.7 / 9.5 ms

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 45.7 / 52.6 / 53.4 / 65.0 / 74.9 µs, 17383 qps | 45.9 / 52.9 / 53.6 / 68.2 / 78.2 µs, 17354 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.3 / 52.0 / 52.9 / 65.8 / 76.5 µs, 16580 qps | 46.9 / 60.2 / 76.9 / 92.7 / 101.4 µs, 12994 qps | 0 / 0 errors |
| `insert-none-existing` | 40.9 / 48.9 / 51.9 / 78.5 / 91.7 µs, 15813 qps | 40.9 / 48.6 / 49.7 / 60.6 / 71.9 µs, 18694 qps | 0 / 0 errors |
| `insert-none-newgroup` | 42.1 / 48.4 / 49.5 / 63.8 / 74.5 µs, 18740 qps | 41.2 / 48.9 / 50.3 / 74.8 / 89.8 µs, 17393 qps | 0 / 0 errors |
| `insert-none-again` | 42.1 / 48.5 / 49.4 / 60.5 / 72.4 µs, 18934 qps | 41.8 / 48.7 / 49.7 / 63.2 / 76.3 µs, 18720 qps | 0 / 0 errors |
| `select` | 59.5 / 63.4 / 64.0 / 77.7 / 85.8 µs, 15072 qps | 57.6 / 62.4 / 63.1 / 77.2 / 86.1 µs, 15252 qps | 0 / 0 errors |
| `ping` | 37.1 / 38.0 / 38.4 / 48.9 / 57.6 µs, 24388 qps | 37.2 / 38.1 / 38.5 / 53.0 / 62.2 µs, 24128 qps | 0 / 0 errors |

### Cell 2 raw (child insert, fk)

**`c2-run1/result.json`** - precell loadavg: 0.90 3.51 4.62 1/324 295381; in-driver load before `0.90 3.51 4.62 1/322 295406`, after `0.90 3.47 4.60 3/329 295416`; competing builds: none

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `child-insert-fk` | 46.6 / 51.7 / 54.8 / 80.7 / 96.1 µs, 15458 qps | 45.1 / 50.7 / 51.7 / 67.2 / 80.8 µs, 17462 qps | 0 / 0 errors |
| `child-insert-fk-txn` | 105.3 / 111.2 / 113.2 / 150.9 / 168.2 µs, 8086 qps | 104.3 / 111.6 / 125.0 / 165.8 / 176.6 µs, 7489 qps | 0 / 0 errors |
| `child-insert-fk-txn:stmt` | 46.0 / 51.8 / 53.1 / 75.1 / 88.2 µs, 0 qps | 45.6 / 52.4 / 58.2 / 81.7 / 96.6 µs, 0 qps | 0 / 0 errors |
| `child-insert-plain` | 40.7 / 49.1 / 50.6 / 75.6 / 92.2 µs, 16547 qps | 43.6 / 48.8 / 49.7 / 68.2 / 77.5 µs, 18639 qps | 0 / 0 errors |
| `parent-update` | 43.0 / 51.1 / 52.8 / 78.6 / 93.5 µs, 15784 qps | 41.3 / 51.3 / 52.0 / 63.3 / 72.5 µs, 18099 qps | 0 / 0 errors |
| `child-update` | 43.6 / 73.7 / 74.5 / 89.7 / 97.7 µs, 13117 qps | 42.9 / 51.3 / 51.9 / 63.0 / 72.5 µs, 18233 qps | 0 / 0 errors |
| `child-update-again` | 43.6 / 73.6 / 74.5 / 90.1 / 98.2 µs, 13429 qps | 44.9 / 51.5 / 52.2 / 67.6 / 76.3 µs, 18182 qps | 0 / 0 errors |
| `select` | 59.7 / 65.6 / 93.1 / 110.8 / 121.1 µs, 11650 qps | 61.6 / 66.3 / 91.1 / 107.7 / 117.7 µs, 10953 qps | 0 / 0 errors |
| `ping` | 37.2 / 38.7 / 46.0 / 67.1 / 79.0 µs, 19597 qps | 37.2 / 43.0 / 61.2 / 68.9 / 81.1 µs, 17727 qps | 0 / 0 errors |

**`c2-run2/result.json`** - precell loadavg: 0.92 2.42 3.99 1/314 296181; in-driver load before `0.92 2.42 3.99 1/324 296214`, after `1.00 2.42 3.98 1/317 296218`; competing builds: none

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `child-insert-fk` | 45.5 / 51.0 / 72.6 / 83.6 / 96.6 µs, 14890 qps | 43.0 / 51.0 / 52.1 / 76.8 / 92.5 µs, 16391 qps | 0 / 0 errors |
| `child-insert-fk-txn` | 104.3 / 109.8 / 113.3 / 154.9 / 171.9 µs, 7848 qps | 104.6 / 109.9 / 111.4 / 133.8 / 145.6 µs, 8519 qps | 0 / 0 errors |
| `child-insert-fk-txn:stmt` | 45.3 / 50.9 / 52.5 / 78.1 / 92.2 µs, 0 qps | 46.1 / 51.0 / 51.9 / 65.4 / 75.8 µs, 0 qps | 0 / 0 errors |
| `child-insert-plain` | 42.3 / 48.7 / 49.8 / 63.5 / 72.6 µs, 18695 qps | 42.4 / 48.7 / 49.7 / 60.9 / 71.4 µs, 18636 qps | 0 / 0 errors |
| `parent-update` | 43.9 / 49.9 / 50.6 / 64.0 / 75.2 µs, 18497 qps | 41.8 / 50.0 / 50.7 / 63.8 / 73.2 µs, 18499 qps | 0 / 0 errors |
| `child-update` | 43.2 / 50.3 / 51.0 / 62.0 / 73.8 µs, 17869 qps | 42.0 / 51.0 / 57.9 / 82.0 / 96.2 µs, 15282 qps | 0 / 0 errors |
| `child-update-again` | 41.2 / 50.4 / 51.0 / 61.8 / 72.4 µs, 18862 qps | 42.7 / 50.7 / 52.1 / 79.3 / 95.0 µs, 16282 qps | 0 / 0 errors |
| `select` | 60.5 / 64.0 / 64.6 / 77.3 / 88.1 µs, 13519 qps | 60.8 / 64.0 / 64.7 / 78.4 / 89.7 µs, 14762 qps | 0 / 0 errors |
| `ping` | 37.0 / 37.6 / 38.0 / 50.5 / 61.3 µs, 24793 qps | 37.0 / 38.0 / 38.4 / 48.8 / 56.2 µs, 24302 qps | 0 / 0 errors |

**`c2-run3/result.json`** - precell loadavg: 0.99 1.87 3.51 2/312 296903; in-driver load before `0.99 1.87 3.51 1/316 296929`, after `1.07 1.88 3.50 1/323 296940`; competing builds: none

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `child-insert-fk` | 43.7 / 50.9 / 51.9 / 77.0 / 92.2 µs, 16723 qps | 45.1 / 51.6 / 53.3 / 79.7 / 94.7 µs, 15802 qps | 0 / 0 errors |
| `child-insert-fk-txn` | 103.6 / 109.3 / 111.0 / 131.3 / 141.1 µs, 8643 qps | 106.8 / 113.3 / 147.1 / 167.8 / 176.7 µs, 7231 qps | 0 / 0 errors |
| `child-insert-fk-txn:stmt` | 45.1 / 50.5 / 51.5 / 63.7 / 73.6 µs, 0 qps | 47.1 / 53.1 / 73.4 / 86.0 / 97.1 µs, 0 qps | 0 / 0 errors |
| `child-insert-plain` | 41.3 / 48.7 / 49.7 / 61.8 / 71.9 µs, 18590 qps | 70.1 / 72.5 / 73.4 / 89.2 / 97.7 µs, 12827 qps | 0 / 0 errors |
| `parent-update` | 40.7 / 49.4 / 50.5 / 63.6 / 73.0 µs, 18586 qps | 44.6 / 74.0 / 74.9 / 90.0 / 98.4 µs, 13285 qps | 0 / 0 errors |
| `child-update` | 43.1 / 49.7 / 50.6 / 62.8 / 73.4 µs, 18610 qps | 41.0 / 51.5 / 73.8 / 84.6 / 97.3 µs, 14727 qps | 0 / 0 errors |
| `child-update-again` | 42.1 / 50.5 / 52.4 / 79.7 / 95.0 µs, 16052 qps | 41.4 / 51.5 / 53.8 / 80.8 / 95.4 µs, 15800 qps | 0 / 0 errors |
| `select` | 59.5 / 63.8 / 64.6 / 80.5 / 96.7 µs, 14710 qps | 60.8 / 67.3 / 95.0 / 112.3 / 121.1 µs, 10329 qps | 0 / 0 errors |
| `ping` | 36.9 / 37.8 / 38.7 / 66.5 / 78.2 µs, 21308 qps | 37.0 / 38.0 / 38.4 / 49.4 / 63.3 µs, 24417 qps | 0 / 0 errors |

**`c2-run4/result.json`** - precell loadavg: 0.93 1.49 3.06 2/322 297683; in-driver load before `0.93 1.49 3.06 1/326 297709`, after `1.02 1.50 3.06 1/318 297713`; competing builds: none

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `child-insert-fk` | 43.4 / 50.0 / 50.8 / 75.0 / 89.4 µs, 17492 qps | 45.1 / 51.2 / 52.0 / 63.4 / 75.1 µs, 18078 qps | 0 / 0 errors |
| `child-insert-fk-txn` | 102.8 / 108.8 / 110.7 / 150.4 / 169.7 µs, 8264 qps | 105.4 / 110.6 / 112.1 / 134.3 / 146.7 µs, 8536 qps | 0 / 0 errors |
| `child-insert-fk-txn:stmt` | 45.9 / 50.2 / 51.3 / 75.3 / 88.8 µs, 0 qps | 46.4 / 51.2 / 52.3 / 66.2 / 78.0 µs, 0 qps | 0 / 0 errors |
| `child-insert-plain` | 40.6 / 48.8 / 49.9 / 61.5 / 73.4 µs, 18754 qps | 41.7 / 49.7 / 56.3 / 80.8 / 94.5 µs, 15552 qps | 0 / 0 errors |
| `parent-update` | 41.8 / 49.5 / 50.4 / 60.4 / 73.6 µs, 18482 qps | 41.0 / 50.9 / 51.7 / 76.8 / 91.9 µs, 16864 qps | 0 / 0 errors |
| `child-update` | 39.9 / 49.9 / 50.7 / 63.5 / 74.5 µs, 18356 qps | 40.3 / 51.0 / 51.5 / 60.9 / 68.0 µs, 18384 qps | 0 / 0 errors |
| `child-update-again` | 38.6 / 49.9 / 50.7 / 63.3 / 74.1 µs, 18909 qps | 40.6 / 51.0 / 51.5 / 60.8 / 67.7 µs, 18775 qps | 0 / 0 errors |
| `select` | 58.1 / 62.4 / 63.2 / 77.8 / 87.6 µs, 13145 qps | 59.5 / 64.0 / 64.7 / 77.1 / 86.9 µs, 14860 qps | 0 / 0 errors |
| `ping` | 36.9 / 37.8 / 38.2 / 53.5 / 63.2 µs, 23856 qps | 37.3 / 38.0 / 38.4 / 48.2 / 56.8 µs, 24684 qps | 0 / 0 errors |

**`c2-run5/result.json`** - precell loadavg: 0.93 1.41 2.95 1/313 297968; in-driver load before `0.93 1.41 2.95 1/317 297994`, after `1.18 1.45 2.96 1/318 298007`; competing builds: none

| arm | B (`273416c`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `child-insert-fk` | 43.5 / 50.3 / 51.3 / 76.6 / 93.0 µs, 17124 qps | 42.8 / 50.9 / 51.7 / 64.4 / 73.7 µs, 17989 qps | 0 / 0 errors |
| `child-insert-fk-txn` | 103.9 / 109.6 / 111.4 / 134.8 / 143.3 µs, 8541 qps | 104.8 / 110.5 / 112.1 / 132.3 / 142.8 µs, 8522 qps | 0 / 0 errors |
| `child-insert-fk-txn:stmt` | 44.9 / 50.6 / 51.6 / 66.4 / 76.6 µs, 0 qps | 45.7 / 51.4 / 52.4 / 64.8 / 75.2 µs, 0 qps | 0 / 0 errors |
| `child-insert-plain` | 41.3 / 49.3 / 50.8 / 74.3 / 88.3 µs, 16619 qps | 39.7 / 49.2 / 50.5 / 74.6 / 89.2 µs, 17100 qps | 0 / 0 errors |
| `parent-update` | 41.2 / 49.1 / 54.1 / 81.5 / 95.3 µs, 15651 qps | 40.5 / 50.1 / 50.9 / 63.2 / 74.6 µs, 18373 qps | 0 / 0 errors |
| `child-update` | 42.7 / 49.4 / 54.7 / 81.2 / 94.3 µs, 15560 qps | 39.9 / 50.6 / 51.3 / 63.1 / 72.3 µs, 18354 qps | 0 / 0 errors |
| `child-update-again` | 40.2 / 49.0 / 50.1 / 75.0 / 90.7 µs, 17964 qps | 40.9 / 50.5 / 51.0 / 60.5 / 69.4 µs, 18908 qps | 0 / 0 errors |
| `select` | 57.3 / 62.6 / 63.6 / 80.5 / 91.5 µs, 14857 qps | 61.2 / 64.5 / 65.4 / 100.4 / 116.0 µs, 13523 qps | 0 / 0 errors |
| `ping` | 36.6 / 37.4 / 37.8 / 51.7 / 60.3 µs, 24807 qps | 37.6 / 38.9 / 42.8 / 51.8 / 61.4 µs, 19799 qps | 0 / 0 errors |

### Cell 3 raw (per-K, per-run medians of per-row wall time, engine time per row)

**`c3-run1/result.json`** - precell loadavg: 0.83 3.41 4.57 1/325 295445; in-driver load before `0.83 3.41 4.57 1/329 295469`, after `1.87 3.18 4.40 1/318 295584`; competing builds: none

| K | kind | reps | B median per row | A median per row | B engine µs/row | A engine µs/row | errors B / A |
|---|---|---|---|---|---|---|---|
| 1 | `fk` | 32768 | 117.3 µs | 118.8 µs | 52.5 µs | 52.2 µs | 0 / 0 |
| 1 | `plain` | 32768 | 115.2 µs | 115.8 µs | 49.8 µs | 49.4 µs | 0 / 0 |
| 64 | `fk` | 512 | 53.8 µs | 54.7 µs | 14.4 µs | 16.1 µs | 0 / 0 |
| 64 | `plain` | 512 | 52.8 µs | 53.7 µs | 13.8 µs | 16.6 µs | 0 / 0 |
| 1024 | `fk` | 32 | 54.5 µs | 54.0 µs | 31.8 µs | 29.5 µs | 0 / 0 |
| 1024 | `plain` | 32 | 51.0 µs | 52.0 µs | 12.7 µs | 12.9 µs | 0 / 0 |
| 4096 | `fk` | 8 | 57.1 µs | 55.2 µs | 18.4 µs | 16.5 µs | 0 / 0 |
| 4096 | `plain` | 8 | 50.8 µs | 51.7 µs | 12.7 µs | 12.9 µs | 0 / 0 |
| 16384 | `fk` | 5 | 70.5 µs | 59.1 µs | 34.4 µs | 23.5 µs | 0 / 0 |
| 16384 | `plain` | 5 | 51.3 µs | 51.9 µs | 13.1 µs | 13.4 µs | 0 / 0 |

**`c3-run2/result.json`** - precell loadavg: 0.92 2.38 3.96 2/314 296249; in-driver load before `0.92 2.38 3.96 1/311 296273`, after `1.62 2.27 3.80 1/319 296403`; competing builds: none

| K | kind | reps | B median per row | A median per row | B engine µs/row | A engine µs/row | errors B / A |
|---|---|---|---|---|---|---|---|
| 1 | `fk` | 32768 | 119.3 µs | 119.7 µs | 54.1 µs | 53.8 µs | 0 / 0 |
| 1 | `plain` | 32768 | 115.8 µs | 118.5 µs | 49.7 µs | 53.3 µs | 0 / 0 |
| 64 | `fk` | 512 | 53.9 µs | 54.5 µs | 15.0 µs | 18.1 µs | 0 / 0 |
| 64 | `plain` | 512 | 52.2 µs | 53.3 µs | 13.1 µs | 17.2 µs | 0 / 0 |
| 1024 | `fk` | 32 | 54.7 µs | 54.4 µs | 29.2 µs | 29.7 µs | 0 / 0 |
| 1024 | `plain` | 32 | 51.2 µs | 51.6 µs | 14.0 µs | 13.3 µs | 0 / 0 |
| 4096 | `fk` | 8 | 58.2 µs | 53.7 µs | 19.5 µs | 16.6 µs | 0 / 0 |
| 4096 | `plain` | 8 | 50.4 µs | 51.6 µs | 12.9 µs | 12.8 µs | 0 / 0 |
| 16384 | `fk` | 5 | 70.9 µs | 59.6 µs | 36.7 µs | 23.0 µs | 0 / 0 |
| 16384 | `plain` | 5 | 51.1 µs | 51.6 µs | 13.2 µs | 13.3 µs | 0 / 0 |

**`c3-run3/result.json`** - precell loadavg: 0.97 1.80 3.44 1/319 297051; in-driver load before `0.97 1.80 3.44 1/316 297079`, after `1.30 1.72 3.27 1/313 297203`; competing builds: none

| K | kind | reps | B median per row | A median per row | B engine µs/row | A engine µs/row | errors B / A |
|---|---|---|---|---|---|---|---|
| 1 | `fk` | 32768 | 121.2 µs | 119.3 µs | 58.2 µs | 52.9 µs | 0 / 0 |
| 1 | `plain` | 32768 | 116.1 µs | 116.4 µs | 50.6 µs | 50.6 µs | 0 / 0 |
| 64 | `fk` | 512 | 53.2 µs | 53.9 µs | 14.7 µs | 14.3 µs | 0 / 0 |
| 64 | `plain` | 512 | 52.7 µs | 52.9 µs | 13.9 µs | 14.6 µs | 0 / 0 |
| 1024 | `fk` | 32 | 53.7 µs | 53.7 µs | 29.0 µs | 27.6 µs | 0 / 0 |
| 1024 | `plain` | 32 | 50.8 µs | 51.6 µs | 13.7 µs | 13.1 µs | 0 / 0 |
| 4096 | `fk` | 8 | 57.3 µs | 54.8 µs | 17.4 µs | 15.5 µs | 0 / 0 |
| 4096 | `plain` | 8 | 50.6 µs | 51.6 µs | 13.1 µs | 13.7 µs | 0 / 0 |
| 16384 | `fk` | 5 | 73.8 µs | 64.1 µs | 36.2 µs | 24.1 µs | 0 / 0 |
| 16384 | `plain` | 5 | 52.3 µs | 51.3 µs | 14.4 µs | 13.1 µs | 0 / 0 |
