# AZ-S7 — what AZ costs at its last code commit, `9f170b1` against `a59da9c`

`instructions/v3.0.0/raft-marks-2026-09-30.md` §8: the interleaved A/B overhead
measurement over a milestone's whole code change, at its close
(`instructions/v3.0.0/workorder-az-ay-carry-forward.md` §5). Run 2026-09-30
10:27–10:41 UTC on `worktrees/az-s7-close` (branch `worktree-az-s7-close`).
B is `9f170b1` (`v2.7.0-568-g9f170b1`, committed 2026-09-30 10:23:03 UTC), AZ's
last code commit: the merge of `198181e`, which replaced AZ-S5's `HoldsRow`
ledger walk with an O(1) test (`TookShare` / `LockHoldings::LastIs`). A is
`a59da9c` (`v2.7.0-533-ga59da9c`), AZ's base, unchanged from the previous run.
The worktree's HEAD was `4550f63` and clean; `git diff --stat 9f170b1 4550f63`
touches only `bench/` and the driver `tools/assertion_overhead_benchmark.py`,
so no engine file differs between HEAD and the measured B.

**The short answer.** Every number is B (`9f170b1`) minus A (`a59da9c`),
`cores = 1`, `relaxed`, BTREE, p50 at the client unless stated, with the noise
floor beside it.

| cell | what | B − A | noise floor / control | reading |
|---|---|---|---|---|
| 1 | assertion admission, one-row autocommit `INSERT`, key finds its group, 200 / 1,000 / 10,000 rows preloaded | −0.97 / −0.50 / −0.01 µs of ~52 µs | same statement repeated on one server 0.39 / 0.51 / 0.38 µs (B, abs. median); no-assertion insert −0.30 / −0.13 / +0.03 µs, point read +0.12 / −0.68 / −0.41 µs | **no cost**: the assertion premium is +2.1 / +2.4 / +3.3 µs on B and +3.0 / +2.8 / +3.4 µs on A; B − A of the premium −1.11 / −0.40 / −0.30 µs |
| 1 | the same, every key opens a new group | −0.56 / −0.60 / −0.07 µs | as above | **no cost**: B − A of the premium −0.50 / −0.30 / +0.02 µs |
| 2 | D9(a) forward check, child `INSERT`, one row per transaction (autocommit) | −0.31 µs (IQR −0.66 to +0.22) of ~51 µs; engine −0.26 µs | 0.18 µs (B), 0.22 µs (A); controls fk-less insert −0.04, `parent-update` +0.02, `child-update` −0.10, `select` −0.32, `ping` +0.08 µs | **no cost**: inside the controls' spread |
| 2 | the same in `BEGIN` / `INSERT` / `COMMIT` | −0.21 µs (IQR −1.60 to +0.42) | as above | **no cost** |
| 3 | K child rows, each a distinct parent, in one transaction, K = 64 / 1,024 | −0.10 / −0.05 µs a row (IQR −1.0..+0.7 / −1.3..+0.9) | fk-less control −0.54 / −0.16 µs | **no cost resolvable** |
| 3 | the same, K = 4,096 | −0.09 µs a row (IQR −7.4..+2.8); inserts alone −0.02 µs | fk-less control +0.13 µs | **no cost resolvable** (566 file: +2.84) |
| 3 | the same, K = 16,384 | **−0.19 µs a row** (IQR −4.3..+3.0): 60.5 µs against 61.2 µs, 16,525 against 16,344 rows/s | fk-less control −0.46 µs (IQR −3.7..+1.5) | **the K-dependent excess is gone** (566 file: +11.29 µs, 14,104 against 16,750 rows/s); inserts −0.84 µs a row, decide +0.12 µs a row |
| 3 | K = 1 (one 3-round-trip transaction, per transaction) | +0.69 µs (IQR −7.0..+8.1) | fk-less +0.63 µs | **not resolvable** (same sign and size in the control) |
| controls | point read, plain insert, `SHOW META` | see cells 1 and 2 | | B − A within −0.7..+0.2 µs on all |

**What AZ costs at its last code commit, in one paragraph.** Nothing this run
can resolve, in any cell. The one cost the previous run resolved, the
per-insert scan of the transaction's borrow ledger in a long transaction of
distinct parents, does not reproduce: at K = 16,384 B's per-row wall time is
60.5 µs against A's 61.2 µs (−0.19 µs a row, IQR −4.3 to +3.0) where
`273416c` read +11.29 µs, and across the eight positions of a 16,384-row
transaction B − A reads −3.5 to +1.9 µs with no climb, where `273416c` climbed
from +1.4 to +18.4 µs. What remains in a long transaction is cost that A
carries as well: at K = 16,384 the foreign-key transaction's per-row time is
8.3 µs above the fk-less one on B and 9.1 µs on A, from the lock table's
release of twice the entries at the decide (49–52 ms a transaction on both,
against 14 ms fk-less). Assertion admission (cell 1) and the one-row child
insert (cell 2) read inside their controls. Measured at one core, `relaxed`,
one session; nothing was measured above K = 16,384, and a difference below
~1–3 µs a row at K = 16,384 cannot be excluded at this run's resolution.

---

## Delta against the 566 file, cell 3 (`273416c` to `9f170b1`)

Same driver (`tools/fk_overhead_benchmark.py`, unmodified), same shape, same A
binary byte for byte, same host, run 12 minutes after the 566 file's own run
ended and two commits apart — one fix. The 566 file's numbers come from its
archive (`c3-run{1,2,3}/result.json.gz`, `archive/az-s7-overhead-v2.7.0-566-g273416c/`),
read by `cmp566.py` (archive). Median per-row wall time, µs, old / new (old is
B = `273416c`, three runs; new is B = `9f170b1`, five runs); A is the same
engine in both.

| K | `fk` B | `fk` A | `plain` B | `plain` A | paired B − A, `fk` | paired B − A, `plain` | `fk` − `plain`, B | `fk` − `plain`, A |
|---|---|---|---|---|---|---|---|---|
| 64 | 53.6 / 54.1 | 54.3 / 54.2 | 52.6 / 53.0 | 53.3 / 53.4 | −0.84 / −0.10 | −0.74 / −0.54 | +1.0 / +1.0 | +1.0 / +0.7 |
| 1,024 | 54.2 / 53.0 | 53.9 / 53.1 | 51.0 / 51.6 | 51.7 / 51.8 | +0.29 / −0.05 | −0.82 / −0.16 | +3.2 / +1.3 | +2.2 / +1.3 |
| 4,096 | 57.5 / 54.1 | 54.9 / 54.0 | 50.7 / 51.4 | 51.6 / 51.5 | **+2.84 / −0.09** | −0.99 / +0.13 | +6.8 / +2.6 | +3.4 / +2.6 |
| 16,384 | 70.9 / 60.5 | 59.7 / 61.2 | 51.3 / 52.2 | 51.8 / 52.0 | **+11.29 / −0.19** | −0.48 / −0.46 | +19.7 / +8.3 | +7.9 / +9.1 |

Derived rows/s (1e6 / median µs), `fk` B / A, old then new: K = 4,096
17,388 / 18,208 then 18,501 / 18,506; K = 16,384 14,097 / 16,757 then
16,525 / 16,344.

- **The excess is gone, not reduced.** `fk`'s B − A went from +2.84 to −0.09 µs
  a row at 4,096 and from +11.29 to −0.19 µs at 16,384, and B − A of `fk` − `plain`
  from +3.4 and +11.8 µs to 0.0 and −0.8 µs. B itself moved by −3.4 µs a row
  at 4,096 and −10.4 µs at 16,384; A, unchanged code, moved by −0.9 and +1.5 µs, which is
  the day-to-day floor of this cell on one engine. Nothing remains
  that is K-dependent and above that floor.
- **The position profile is flat.** Pooled insert latency of B less A by
  eighth of the transaction (`stmt_us_by_pos`, µs):

  | K | run | 1st | 2nd | 3rd | 4th | 5th | 6th | 7th | 8th |
  |---|---|---|---|---|---|---|---|---|---|
  | 16,384 | `273416c` (566 file) | +1.4 | +7.3 | +6.5 | +14.0 | +18.8 | +16.1 | +17.1 | +18.4 |
  | 16,384 | `9f170b1` | −1.5 | +0.0 | +1.9 | −3.5 | −2.4 | −1.6 | +0.7 | −2.9 |
  | 4,096 | `273416c` (566 file) | −2.7 | −1.9 | +2.6 | +2.5 | +6.2 | +3.1 | +1.8 | +4.9 |
  | 4,096 | `9f170b1` | −2.4 | −1.2 | −3.8 | −3.8 | −3.6 | −3.1 | −2.6 | −2.5 |

  The fix's premise was that the old excess was a scan growing with the ledger;
  the old profile climbed with position and the new one does not. The new
  4,096 row sits 1.2–3.8 µs below zero on seven of eight positions, a level
  offset and not a slope. These are pooled means, which carry the slow
  transactions and the stall (below); the per-transaction medians of the
  same K read −0.09 µs a row (table above) and the inserts' median −0.02 µs
  (`t3b.py`), so the offset is in the tail of A's transactions, not in the
  body. It is in the direction of B being faster, and its cause is not
  identified.

## Rules (`bench/README.md`)

1. **Release, rebuilt at the measured commit.** B built `Release`
   (`CMakeCache.txt` `CMAKE_BUILD_TYPE:STRING=Release`) from `git archive
   9f170b1` into `/home/cdkbs/bench-runs/az-s7b/src-B`, target `kds_server`
   only, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF` (`build.sh` in the
   archive), alone on the host, before any measurement; nothing built during
   the run. A is the previous run's binary, reused: its copy's sha256 equals
   `binaries.sha256` of the 566 file's archive, and `a59da9c` was not rebuilt.
2. **Block device.** Data files under `/home/cdkbs/bench-runs/az-s7b/` on
   `/dev/root`, ext4 (`df -T`).
3. **A copy of the binary, hashed.** Every server started from the copy:

   | arm | commit | describe | binary | sha256 | source binary mtime |
   |---|---|---|---|---|---|
   | A | `a59da9c` (AZ's base, committed 2026-09-30 06:39:03 UTC) | `v2.7.0-533-ga59da9c` | `kds_server-A` | `1d07d33386541ec3a13090d37918e229957e29c2c777ff15f2a63db6957f8746` | 2026-09-30 10:01:28 UTC (the 566 run's build; copy re-hashed this run, identical) |
   | B | `9f170b1` (AZ's last code commit, the merge of `198181e`, committed 2026-09-30 10:23:03 UTC) | `v2.7.0-568-g9f170b1` | `kds_server-B` | `309fc46dc1eddf084c20aef86f5ad1d0bf203fcb9a31b3c80a2c3a1b9b9a888d` | 2026-09-30 10:27:15 UTC |

   B was built after its commit, from an exact `git archive` tree, so it is
   not older than the commit it names. A is older than B's commit by
   construction (it is AZ's base) and is the engine at `a59da9c` byte for
   byte. Each arm ran on its own fresh volume in every run.
4. **Host load, per cell.** `/proc/loadavg` and the `pgrep -a -f "cc1plus|cmake
   --build|ctest"` result are in every `host.txt` and every JSON under
   `archive/az-s7-overhead-v2.7.0-568-g9f170b1/`, reproduced in the raw
   tables. **No competing build in any of the 25 runs**; one-minute load
   before each run 0.87–0.98 (the run script waited, bounded, for it to fall
   under 1.0; the in-driver reading, which includes the driver's own thread,
   reached 1.06 once). The fifteen-minute load was still decaying from the
   B build and from other sessions' earlier work (2.8 before run 1, 1.9
   before the last). The host was otherwise idle at 96 % (`top`) before the
   first run. `uptime` at the start read load 4.25 from the preceding
   hour's builds by other sessions, which had ended (`pgrep cc1plus` empty).
5. **Ports chosen.** 15600 (B) and 15601 (A) for every pair. Not 15432 (an
   unrelated `kds_server` was running on 15432 and was not touched).

Host: 8 CPUs. Every server `cores = 1`, `durability = relaxed` (`group`'s
commit is 82–85 % of an update and would hide what is measured), BTREE,
`log_level = warn`, otherwise defaults; **a fresh server and data file for
every (cell, run)**: 25 pairs. The two servers of a pair run side by side and
the driver alternates which goes first per block, so only one is active at a
time. `strict` was not measured. Behaviour: no code changed in this stage, so
the test suite was **not executed**; correctness of the measured statements
is witnessed only by zero driver errors in every arm of every run, and by
`SHOW ASSERTIONS` reporting `enforcing=1` on both servers in every cell-1
run (`show_assertions` in each `c1-*` JSON).

## Drivers

Identical to the 566 file: `tools/assertion_overhead_benchmark.py` (cell 1,
`--preload 200|1000|10000 --ops 8000 --blocks 16`, five runs per size, 200 /
1,000 / 10,000 rows and as many groups present when the measured inserts
start; arms `insert-assert-existing`, `insert-assert-newgroup`,
`insert-none-existing`, `insert-none-newgroup`, `insert-none-again` (the
floor), `select`, `ping`) and `tools/fk_overhead_benchmark.py` (cells 2 and
3, unmodified), cell 2 `--cell price --ops 4000 --blocks 8`, five runs; cell 3
`--cell ledger --ks 1,64,1024,4096,16384 --rows-per-k 32768`, **five runs**
(the 566 file ran three). Cell 3's reps per K are rows / K, at least 5, so the
pool is K = 1: 163,840 transactions; 64: 2,560; 1,024: 160; 4,096: 40;
16,384: 25 (the 566 file: 98,304 / 1,536 / 96 / 24 / 15). Cell 3's row-count
mapping: K is the rows in one transaction; 1,024 / 4,096 / 16,384 stand for
the 1K / 10K axis, 64 for the small end (a 200-row transaction was not run).
Driver flags and invocation are in `bench/docs/README.md`; reproduction
scripts `all.sh`, `pair.sh`, `t1.py`, `t1b.py`, `t2b.py`, `t3.py`, `t3b.py`,
`cmp566.py`, `gen.py` are in the archive and read the run directory
`/home/cdkbs/bench-runs/az-s7b`. The `c3-run*` JSON is gzipped.

The behavioural fence check, `CREATE ASSERTION` and checkpoint cost as
priced measurements, AY-S11's statement mix (`lock_contention_benchmark.py`)
were not run. `CREATE ASSERTION` over the preload, timed once a server per
run as a by-product, was 0.2–0.3 ms at 200 rows, 0.8 ms at 1,000 and 7.6–9.5
ms at 10,000 on both engines.

## Cell 1 — assertion admission

**AZ-S3's admission costs a one-row insert nothing that can be told from
the floor, at any of the three sizes and for both sub-shapes.** B takes one
more hash `Find` of the group key, and for a key opening a new group a scan of
the in-flight holds, plus a per-row unenforceable-set check; A takes neither.
One session per server, serial, autocommit, five runs of 8,000 statements per
arm per server per size, in 16 alternating blocks. The table uses only block
pairs in which both servers ran in their fast mode (a block is slow when its
p50 exceeds 1.12 × that server's own minimum block for the arm, `t1.py`),
median of per-pair B − A. Nothing is dropped from the raw tables. Derived qps
is 1,000,000 / p50.

| preload (rows and groups) | arm | clean pairs of 80 | B − A p50 (IQR) | B − A engine per op | B p50 | A p50 | derived qps B / A |
|---|---|---|---|---|---|---|---|
| 200 | `insert-assert-existing` | 65 | −0.97 µs (−1.30 to −0.42) | −0.51 µs | 52.0 µs | 52.9 µs | 19,231 / 18,904 qps |
| 200 | `insert-assert-newgroup` | 64 | −0.56 µs (−1.16 to +0.04) | −0.33 µs | 52.8 µs | 53.5 µs | 18,939 / 18,692 qps |
| 200 | control `insert-none-existing` | 57 | −0.30 µs (−0.59 to −0.10) | −0.11 µs | 49.7 µs | 50.0 µs | 20,121 / 20,000 qps |
| 200 | control `insert-none-newgroup` | 72 | −0.26 µs (−0.54 to +0.10) | −0.12 µs | 49.8 µs | 49.8 µs | 20,080 / 20,080 qps |
| 200 | floor `insert-none-again` | 66 | −0.17 µs (−0.59 to +0.13) | −0.05 µs | 49.7 µs | 49.9 µs | 20,121 / 20,040 qps |
| 200 | control `select` | 47 | +0.12 µs (−0.82 to +0.63) | +0.00 µs | 64.7 µs | 64.4 µs | 15,456 / 15,528 qps |
| 200 | control `ping` | 60 | −0.09 µs (−0.39 to +0.27) | +0.07 µs | 38.0 µs | 38.0 µs | 26,316 / 26,316 qps |
| 1,000 | `insert-assert-existing` | 70 | −0.50 µs (−0.87 to −0.11) | −0.21 µs | 52.2 µs | 52.8 µs | 19,157 / 18,939 qps |
| 1,000 | `insert-assert-newgroup` | 61 | −0.60 µs (−0.90 to −0.29) | −0.31 µs | 52.5 µs | 53.1 µs | 19,048 / 18,832 qps |
| 1,000 | control `insert-none-existing` | 55 | −0.13 µs (−0.30 to +0.09) | −0.08 µs | 49.7 µs | 49.9 µs | 20,121 / 20,040 qps |
| 1,000 | control `insert-none-newgroup` | 68 | −0.37 µs (−0.54 to −0.07) | −0.21 µs | 49.6 µs | 50.0 µs | 20,161 / 20,000 qps |
| 1,000 | floor `insert-none-again` | 64 | −0.11 µs (−0.42 to +0.08) | −0.19 µs | 49.8 µs | 50.1 µs | 20,080 / 19,960 qps |
| 1,000 | control `select` | 58 | −0.68 µs (−1.05 to −0.35) | −0.38 µs | 64.4 µs | 65.3 µs | 15,528 / 15,314 qps |
| 1,000 | control `ping` | 62 | −0.07 µs (−0.38 to +0.30) | +0.16 µs | 38.0 µs | 38.1 µs | 26,316 / 26,247 qps |
| 10,000 | `insert-assert-existing` | 62 | −0.01 µs (−0.39 to +0.35) | +0.03 µs | 53.0 µs | 53.0 µs | 18,868 / 18,868 qps |
| 10,000 | `insert-assert-newgroup` | 69 | −0.07 µs (−0.50 to +0.34) | −0.00 µs | 52.9 µs | 53.0 µs | 18,904 / 18,868 qps |
| 10,000 | control `insert-none-existing` | 71 | +0.03 µs (−0.23 to +0.29) | +0.12 µs | 49.9 µs | 49.9 µs | 20,040 / 20,040 qps |
| 10,000 | control `insert-none-newgroup` | 67 | −0.09 µs (−0.28 to +0.12) | +0.05 µs | 49.8 µs | 49.9 µs | 20,080 / 20,040 qps |
| 10,000 | floor `insert-none-again` | 64 | −0.05 µs (−0.30 to +0.27) | −0.08 µs | 49.8 µs | 49.9 µs | 20,080 / 20,040 qps |
| 10,000 | control `select` | 62 | −0.41 µs (−1.02 to +0.03) | −0.25 µs | 63.2 µs | 63.6 µs | 15,823 / 15,723 qps |
| 10,000 | control `ping` | 74 | +0.20 µs (−0.12 to +0.37) | +0.38 µs | 38.3 µs | 38.2 µs | 26,110 / 26,178 qps |

Noise floor from inside the run: the same statement repeated on one server
(`insert-none-existing` against `insert-none-again`, same block position, all
80 blocks) differs by an absolute median of **0.39 µs (B) / 0.37 µs (A) at 200
rows, 0.51 / 0.31 µs at 1,000, 0.38 / 0.33 µs at 10,000**. Every B − A above,
assertion arms included, is smaller in magnitude than 1 µs; the two
largest (−0.97 and −0.50 µs at 200 and 1,000 rows, `insert-assert-existing`)
are negative, B faster.

The cleaner reading is inside each engine: the premium of an assertion is the
assertion arm less the no-assertion arm of the same shape, same block, on one
server, and AZ's cost is B's premium less A's (`t1b.py`; the IQRs are wide
because a slow-mode block on either engine moves a difference by ~22 µs, the
medians are not):

| preload | shape | premium B (assert − none) | premium A | B − A of the premium (IQR) | engine, B − A of the premium (IQR) |
|---|---|---|---|---|---|
| 200 | key finds its group | +2.09 µs | +3.02 µs | −1.11 µs (−22.78 to −0.24) | −0.94 µs (−8.39 to −0.07) |
| 200 | key opens a group | +2.95 µs | +3.43 µs | −0.50 µs (−1.36 to +0.25) | −0.40 µs (−3.40 to +0.31) |
| 1,000 | key finds its group | +2.36 µs | +2.76 µs | −0.40 µs (−6.61 to +0.39) | −0.40 µs (−7.58 to +0.60) |
| 1,000 | key opens a group | +2.92 µs | +3.27 µs | −0.30 µs (−0.79 to +0.88) | −0.09 µs (−0.80 to +4.89) |
| 10,000 | key finds its group | +3.28 µs | +3.42 µs | −0.30 µs (−1.35 to +0.69) | −0.24 µs (−6.35 to +0.34) |
| 10,000 | key opens a group | +3.21 µs | +3.17 µs | +0.02 µs (−1.06 to +0.67) | +0.13 µs (−1.38 to +1.15) |

- **Nothing positive and resolved.** B's premium is at or below A's in five
  of six rows and equal in the sixth (+0.02 µs, inside a 0.3–0.4 µs floor).
  Whatever AZ-S3 added to the admission is below what a serial 50 µs
  statement can show.
- **The assertion itself costs ~2–3.5 µs an insert on both engines** (~1.5–2.6
  µs of it in the engine): 4–7 % of a statement, present in A, and the only
  assertion-shaped number this cell resolves. Its dependence on the preload
  is small and differs by engine: on the existing-group shape B's premium
  rises +2.1 to +3.3 µs from 200 to 10,000 groups where A's rises +3.0 to
  +3.4 µs, and on the new-group shape B's is flat (+2.95 to +3.21 µs) where
  A's is flat (+3.43 to +3.17 µs). That is why B − A of the premium climbs
  from −1.1 / −0.5 µs toward zero with size; it reaches +0.02 µs at most and
  never becomes a resolved cost, but a size-dependent term of up to ~1 µs
  on B's existing-group admission is not excluded by these medians.

**Latency decomposition (documentation rule 3).** The unit is one serial
statement: ~50 µs (no assertion) = ~14 µs engine (`SHOW META`) + ~36 µs
client, socket and reactor wake (not separable here); the assertion adds ~2–3.5
µs, ~1.5–2.6 µs of it in the engine. No fsync (`relaxed`), no lock or conflict
wait (one session; the assertion directory's latch is taken, never waited on),
no read wait of note.

**Row-count sweep (rule 9).** 200 / 1,000 / 10,000 preloaded rows and groups
are the rows of the tables above; the admission does not scale with them
beyond the fraction of a microsecond stated.

## Cell 2 — the FK forward check

**A child insert costs nothing this run can resolve over A.** B now reads the
ledger's newest record (`TookShare`) after the parent `S` where `273416c` asked
`HoldsRow` before it; A has neither. Serial, one session, five runs of 4,000
statements per arm per server, 8 alternating blocks, the same clean-pair
filter (`t2b.py`).

| arm | clean pairs of 40 | B p50 | A p50 | derived qps B / A (1e6 / p50) | **B − A p50** (IQR) | B − A engine per op |
|---|---|---|---|---|---|---|
| `child-insert-fk` (autocommit, one txn) | 25 | 51.0 µs | 51.3 µs | 19,608 / 19,493 qps | **−0.31 µs** (−0.66 to +0.22) | −0.26 µs |
| `child-insert-fk-txn` (BEGIN, INSERT, COMMIT; the unit) | 21 | 110.8 µs | 110.8 µs | 9,025 / 9,025 txn/s | −0.21 µs (−1.60 to +0.42) | −0.81 µs |
| control `child-insert-plain` (no fk declared) | 31 | 49.7 µs | 49.7 µs | 20,121 / 20,121 qps | −0.04 µs (−0.31 to +0.17) | −0.17 µs |
| control `parent-update` | 19 | 50.7 µs | 50.4 µs | 19,724 / 19,841 qps | +0.02 µs (−0.39 to +0.26) | −0.15 µs |
| control `child-update` (fk column untouched) | 21 | 51.0 µs | 50.6 µs | 19,608 / 19,763 qps | −0.10 µs (−0.45 to +0.51) | −0.17 µs |
| noise floor `child-update-again` | 32 | 50.6 µs | 50.9 µs | 19,763 / 19,646 qps | −0.44 µs (−0.77 to +0.39) | −0.44 µs |
| control `select` (pk read) | 27 | 64.2 µs | 64.4 µs | 15,576 / 15,528 qps | −0.32 µs (−0.60 to +0.08) | −0.28 µs |
| control `ping` (`SHOW META`) | 33 | 38.3 µs | 38.2 µs | 26,110 / 26,178 qps | +0.08 µs (−0.16 to +0.36) | +0.03 µs |

- Same-server floor (`child-update` against `child-update-again`, same block,
  clean): B median +0.15 µs, absolute median 0.18 µs (29 blocks); A +0.02 µs,
  0.22 µs (27 blocks). `child-update` and `child-update-again`, the same
  statement twice, read −0.10 and −0.44 µs B − A: the cell reproduces its own
  B − A to 0.34 µs.
- **`child-insert-fk` −0.31 µs sits among the controls** (−0.44 to +0.08 µs)
  and its engine delta (−0.26 µs) among theirs (−0.44 to +0.03). The fk-less
  `child-insert-plain` reads −0.04 µs. The one-row transaction's ledger is a
  handful of entries (not counted), so the ledger test reads almost
  nothing here; cell 3 is where it grows.
- The explicit-transaction unit (three round trips) reads −0.21 µs, IQR
  −1.60 to +0.42: not resolvable.
- Slow-mode blocks (B / A of 40): `child-insert-fk` 1 / 14, `child-update`
  11 / 9, `select` 5 / 8; arms flip between ~51 and ~75 µs on either engine.
  Filtered pairs only are in the table; raw p50s are in the raw tables.

**Latency decomposition (rule 3).** ~51 µs = ~14 µs engine + ~37 µs client,
socket and reactor wake. No fsync wait (`relaxed`), no lock or conflict wait
(one session: the parent `S` is taken, never waited on), no read wait of note.

**Row-count sweep (rule 9).** A pk point statement does not scale with rows;
this cell ran at one cardinality (256 parents, 256 children). Cell 3 is the
row-count axis, at the transaction level.

## Cell 3 — a transaction of K distinct parents

**No K-dependent cost of B over A remains at K up to 16,384.** One explicit
transaction inserts K child rows, each referencing a distinct parent (parents
1..K of 16,384), `BEGIN` to `COMMIT` timed whole and divided by K. `plain` is
the same K inserts into a child with no fk declared. K = 1 is a transaction of
three round trips and is per transaction, not per row. Five runs, reps
pooled. No errors at any K in either arm (`max_locks_per_txn` is 65,536; a
16,384-row transaction takes two entries a row and does not reach it).

Median per-row wall time, derived rows per second (1e6 / µs, from the
unrounded median):

| K | `fk` B | `fk` A | derived rows/s B / A | `plain` B | `plain` A | `fk` − `plain` B / A | paired B − A per row, `fk` (IQR) | paired B − A per row, `plain` (IQR) |
|---|---|---|---|---|---|---|---|---|
| 1 (per txn) | 119.9 µs | 119.1 µs | 8,340 / 8,396 txn/s | 117.2 µs | 116.5 µs | +2.6 / +2.6 µs | +0.69 µs (−7.0 to +8.1) | +0.63 µs (−6.2 to +7.7) |
| 64 | 54.1 µs | 54.2 µs | 18,484 / 18,450 rows/s | 53.0 µs | 53.4 µs | +1.0 / +0.7 µs | −0.10 µs (−1.0 to +0.7) | −0.54 µs (−22.4 to +0.7) |
| 1,024 | 53.0 µs | 53.1 µs | 18,868 / 18,832 rows/s | 51.6 µs | 51.8 µs | +1.3 / +1.3 µs | −0.05 µs (−1.3 to +0.9) | −0.16 µs (−2.8 to +0.4) |
| 4,096 | 54.1 µs | 54.0 µs | 18,501 / 18,506 rows/s | 51.4 µs | 51.5 µs | +2.6 / +2.6 µs | −0.09 µs (−7.4 to +2.8) | +0.13 µs (−3.2 to +0.9) |
| 16,384 | 60.5 µs | 61.2 µs | 16,525 / 16,344 rows/s | 52.2 µs | 52.0 µs | +8.3 / +9.1 µs | −0.19 µs (−4.3 to +3.0) | −0.46 µs (−3.7 to +1.5) |

(The rows/s of K = 64 and 1,024 are derived from the rounded medians.)
Paired B − A is per repetition (both engines ran the same rep back to back,
order alternating). The `plain` control stays within −0.54 to +0.13 µs at
K ≥ 64 and the `fk` row sits with it at every K. K = 1 is 0.69 µs with a
control that reads +0.63 µs and IQRs of ±7 µs: not resolvable.

**Where the rest of a long transaction goes: the decide, on both engines.**
The transaction splits into its inserts and the rest (`BEGIN`, `COMMIT`, the
driver's loop; `t3b.py`). The rest, per transaction, per run:

| K | `fk` B | `fk` A | `plain` B | `plain` A | `fk` B − A of the rest | inserts B − A (median, per row) |
|---|---|---|---|---|---|---|
| 1,024 | 0.72 / 0.76 / 0.73 / 0.72 / 0.69 ms | 0.71 / 0.72 / 0.77 / 0.68 / 0.70 ms | 0.47 / 0.50 / 0.45 / 0.44 / 0.44 ms | 0.49 / 0.51 / 0.48 / 0.53 / 0.47 ms | +0.01 ms (+0.01 µs a row) | −0.17 µs |
| 4,096 | 4.69 / 4.54 / 4.59 / 4.63 / 4.60 ms | 4.38 / 4.87 / 4.69 / 4.69 / 4.49 ms | 1.95 / 2.09 / 1.95 / 2.08 / 2.00 ms | 2.16 / 1.98 / 1.97 / 2.03 / 2.10 ms | −0.02 ms (−0.00 µs a row) | −0.02 µs |
| 16,384 | 51.5 / 52.0 / 51.7 / 51.1 / 51.8 ms | 49.5 / 49.7 / 50.4 / 49.1 / 49.4 ms | 13.8 / 13.8 / 13.7 / 13.6 / 14.3 ms | 13.9 / 14.3 / 14.0 / 14.3 / 13.6 ms | +2.01 ms (+0.12 µs a row) | −0.84 µs |

The decide's cost is the same on both engines up to ~2 ms of 50 at K =
16,384 (B +2.0 ms, +4 %, +0.12 µs a row; all five B runs sit above all five A
runs, 51.1–52.0 ms against 49.1–50.4 ms, so this difference is consistent in
sign and is reported as such, at 0.12 µs a row it is below the resolution of
the per-row medians, and its cause is not identified). On both, `fk`'s rest at 16,384 is 49–52
ms against `plain`'s 14 ms: the lock table's release of twice the entries,
present in A and the whole of the `fk` − `plain` gap (+8.3 / +9.1 µs a row)
at 16,384. The inserts themselves no longer differ (−0.84 µs a row).

**The stall is on both engines, again.** In the pooled octiles the K = 1,024
`fk` cell's 6th eighth (132.5 µs on B, 136.2 µs on A against ~54 elsewhere)
and the K = 16,384 `fk` cell's 7th eighth (89.5 / 88.8 µs against ~57) stand
out on both engines, in `fk` only, as in the previous run (which measured it
at ~0.4 s in one transaction; not re-measured here). It is not a
K-dependent term and not B's; its cause is not identified here.

**Latency decomposition (rule 3).** `relaxed`: no fsync wait inside the
transaction (the decide writes the log once), no lock wait (one session). At
K ≥ 64 the unit is ~52–54 µs a row (`plain`) of which the engine is ~14
µs; the foreign-key transaction adds +1.0 µs a row at K = 64 and +8.3 µs at
16,384 on B (+9.1 on A), the latter in the decide. Engine time per row
(polled `SHOW META`, `t3.py`): `fk` 22.8 µs B / 22.7 µs A at K = 16,384,
`plain` 13.6 / 14.1 µs.

**Row-count sweep (rule 9).** K is the axis (above); 64 / 1,024 / 4,096 /
16,384 rows per transaction; the same-K `plain` control is flat across them.

## What this decides, and what it does not

- **AZ's overhead at its last code commit is not resolvable at one core**
  (cells 1, 2 and 3): assertion admission, a one-row child insert and a
  transaction of up to 16,384 distinct parents read inside their controls.
  The regression the previous run resolved (+2.84 µs a row at K = 4,096,
  +11.29 µs at 16,384, attributed to `HoldsRow`'s linear ledger walk) is
  gone at `9f170b1`: −0.09 and −0.19 µs a row, the octile profile flat. The
  overhead measurement `CLAUDE.md`'s step 3 asks at a milestone's close is,
  for AZ, **measured at the final commit**.
- **What AZ-S5's O(1) test leaves**: the lock table's release of the
  ledger at the decide, 49–52 ms a 16,384-row transaction on both engines
  (present in A), which is not AZ's. The remaining B − A of the rest is
  +0.12 µs a row at 16,384.
- **Not measured, and not implied**: `cores > 1`, `group` or `strict`
  durability, contended parent or assertion locks, the assertion's
  in-flight-hold scan under a transaction holding many groups (cell 1 is
  autocommit, so the scan sees one hold), K above 16,384, the behavioural
  fence check, `CREATE ASSERTION` and checkpoint as priced costs,
  PostgreSQL's floor for these shapes (not measured for this shape), and any
  client faster than this Python driver (~36 µs of a ~50 µs statement lies
  outside the engine).
- **Qualifications.** (1) Resolution: a B-side cost below ~0.4 µs a
  statement (the same-server floor) cannot be excluded in cells 1 and 2, and
  below ~1–3 µs a row (the `fk` IQR at 16,384 is −4.3..+3.0) in cell 3. The
  previous run's cost was ~4 times the top of that IQR. (2) The 566 file read
  B 0.5–1 µs faster than A on every arm; this run does not reproduce that
  shift (controls −0.7 to +0.2 µs) with the same A binary, so it was a
  property of that run pair and is not characterised here. (3) The clean-pair
  filter and its 1.12× threshold are inherited from AY-S11; the unfiltered raw
  tables carry every block. The bimodal slow mode is on both engines (slow
  blocks B / A of 80 in cell 1: 2 / 13 for `insert-assert-existing` at 200
  rows, 2 / 16 at 10,000; 17 / 7 for `insert-none-existing` at 200). (4)
  Cell 3's K = 4,096 and 16,384 rest on 40 and 25 paired repetitions,
  the stall above sits in both arms, and the 4,096 and 16,384 `fk` IQRs span
  zero. (5) No `SHOW META` control runs inside cell 3's transactions; its
  control is the fk-less `plain` insert. (6) The delta against the 566 file
  compares two separate runs: its B arm ran 12 minutes before this one, with
  three runs instead of five. A, byte-identical, reads 54.9 / 59.7 µs at
  K = 4,096 / 16,384 there and 54.0 / 61.2 µs here, so run-to-run the same
  engine moves by ~1–1.5 µs a row at those K, against a change of 3.4 and
  10.4 µs in B. (7) The 4,096 octile row reads 1.2–3.8 µs below zero
  on seven of eight positions in pooled means; a level offset in A's tail,
  not a slope (medians read zero), cause not identified. (8) One-minute load
  before each run was 0.87–0.98 and no build ran, but other sessions' agents
  were open on the host (not running a build or a test) and the five- and
  fifteen-minute loads were still decaying from earlier builds (1.3–2.8).

---

## Raw tables

Every arm, both engines, p0 / p25 / p50 / p95 / p99 with throughput, per run.
The JSON under `archive/az-s7-overhead-v2.7.0-568-g9f170b1/` (`c1-n{200,1000,
10000}-run{1..5}`, `c2-run{1..5}`, `c3-run{1..5}`, each with `result.json`
(gzipped in `c3-run*`), `driver.txt`, `host.txt` and both servers' configs)
carries the per-block records and the per-core `SHOW META` before and after.
Cell 3's table here gives each run's median per-row wall time and engine time
per row; the per-position and per-rep data are in the JSON.

### Cell 1 raw (assertion admission)

**`c1-n200-run1/result.json`** - precell loadavg: 0.94 2.75 2.78 2/308 306064; in-driver load before `0.94 2.75 2.78 1/319 306096`, after `1.03 2.74 2.78 1/318 306103`; competing builds: none

preload 200, create assertion B / A: 0.2 / 0.3 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 44.9 / 51.1 / 52.5 / 78.0 / 93.0 µs, 16768 qps | 43.8 / 51.7 / 53.4 / 80.0 / 96.5 µs, 15627 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.2 / 51.9 / 52.9 / 73.0 / 83.0 µs, 17364 qps | 46.1 / 51.7 / 52.8 / 67.0 / 77.8 µs, 17448 qps | 0 / 0 errors |
| `insert-none-existing` | 39.6 / 48.6 / 49.9 / 73.8 / 87.8 µs, 17686 qps | 40.3 / 49.2 / 50.9 / 77.3 / 92.3 µs, 16497 qps | 0 / 0 errors |
| `insert-none-newgroup` | 41.8 / 48.6 / 49.9 / 74.3 / 89.6 µs, 16983 qps | 41.7 / 48.8 / 49.8 / 62.4 / 73.2 µs, 17724 qps | 0 / 0 errors |
| `insert-none-again` | 41.2 / 48.5 / 49.5 / 69.6 / 77.7 µs, 18596 qps | 41.5 / 48.9 / 49.9 / 72.0 / 78.7 µs, 18058 qps | 0 / 0 errors |
| `select` | 58.0 / 64.5 / 67.0 / 101.8 / 117.0 µs, 12854 qps | 58.7 / 63.8 / 64.8 / 99.2 / 114.3 µs, 13866 qps | 0 / 0 errors |
| `ping` | 36.6 / 37.4 / 37.9 / 64.6 / 75.2 µs, 22608 qps | 36.9 / 38.0 / 40.4 / 66.2 / 78.7 µs, 20581 qps | 0 / 0 errors |

**`c1-n200-run2/result.json`** - precell loadavg: 0.97 2.15 2.55 1/299 306624; in-driver load before `0.97 2.15 2.55 1/305 306648`, after `1.05 2.13 2.54 1/312 306660`; competing builds: none

preload 200, create assertion B / A: 0.2 / 0.2 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 45.3 / 51.2 / 52.2 / 63.4 / 73.7 µs, 17730 qps | 47.2 / 53.3 / 54.2 / 79.4 / 95.3 µs, 15715 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.6 / 51.9 / 52.8 / 65.7 / 75.3 µs, 16739 qps | 47.6 / 53.3 / 54.1 / 69.6 / 80.9 µs, 16763 qps | 0 / 0 errors |
| `insert-none-existing` | 42.5 / 49.0 / 50.4 / 75.0 / 89.9 µs, 17108 qps | 43.3 / 49.1 / 50.4 / 65.2 / 74.6 µs, 18451 qps | 0 / 0 errors |
| `insert-none-newgroup` | 40.9 / 49.0 / 50.2 / 73.4 / 86.1 µs, 17559 qps | 42.0 / 49.2 / 50.3 / 65.9 / 77.5 µs, 18276 qps | 0 / 0 errors |
| `insert-none-again` | 40.0 / 49.3 / 51.2 / 77.1 / 91.1 µs, 15839 qps | 42.4 / 49.2 / 50.4 / 73.3 / 87.4 µs, 17672 qps | 0 / 0 errors |
| `select` | 59.9 / 64.2 / 65.2 / 98.3 / 114.8 µs, 13603 qps | 59.2 / 63.6 / 64.4 / 76.7 / 87.1 µs, 14967 qps | 0 / 0 errors |
| `ping` | 34.9 / 37.9 / 38.2 / 49.9 / 59.5 µs, 24825 qps | 36.8 / 37.7 / 38.1 / 47.8 / 56.1 µs, 25015 qps | 0 / 0 errors |

**`c1-n200-run3/result.json`** - precell loadavg: 0.92 1.66 2.28 1/297 307371; in-driver load before `0.92 1.66 2.28 1/310 307403`, after `0.93 1.64 2.28 1/310 307408`; competing builds: none

preload 200, create assertion B / A: 0.2 / 0.2 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 45.1 / 50.9 / 51.9 / 67.1 / 76.9 µs, 17712 qps | 45.8 / 52.3 / 53.3 / 79.9 / 95.4 µs, 16064 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.2 / 51.8 / 53.1 / 80.5 / 96.8 µs, 15861 qps | 46.6 / 53.1 / 56.4 / 85.3 / 100.1 µs, 14861 qps | 0 / 0 errors |
| `insert-none-existing` | 41.2 / 48.8 / 50.0 / 72.4 / 84.0 µs, 18184 qps | 39.7 / 48.7 / 49.8 / 61.9 / 71.7 µs, 18964 qps | 0 / 0 errors |
| `insert-none-newgroup` | 39.0 / 48.7 / 49.8 / 62.2 / 71.8 µs, 18680 qps | 40.9 / 48.7 / 49.7 / 60.9 / 70.5 µs, 18685 qps | 0 / 0 errors |
| `insert-none-again` | 41.3 / 48.7 / 49.9 / 72.9 / 82.6 µs, 17118 qps | 41.0 / 48.7 / 50.0 / 75.0 / 89.2 µs, 17433 qps | 0 / 0 errors |
| `select` | 58.7 / 63.9 / 64.7 / 80.6 / 91.9 µs, 14734 qps | 60.4 / 93.3 / 95.2 / 112.0 / 120.1 µs, 10880 qps | 0 / 0 errors |
| `ping` | 36.7 / 37.5 / 37.8 / 48.3 / 59.2 µs, 25236 qps | 36.8 / 37.9 / 38.5 / 67.3 / 80.4 µs, 20386 qps | 0 / 0 errors |

**`c1-n200-run4/result.json`** - precell loadavg: 0.98 1.46 2.12 1/298 307947; in-driver load before `0.98 1.46 2.12 1/309 307979`, after `1.14 1.49 2.12 1/307 307988`; competing builds: none

preload 200, create assertion B / A: 0.3 / 0.2 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 43.7 / 51.0 / 51.9 / 63.6 / 74.7 µs, 17832 qps | 45.8 / 52.5 / 53.3 / 66.3 / 77.7 µs, 17435 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.7 / 51.8 / 52.9 / 70.8 / 79.8 µs, 17366 qps | 46.0 / 53.3 / 54.1 / 76.8 / 90.5 µs, 16603 qps | 0 / 0 errors |
| `insert-none-existing` | 41.5 / 49.3 / 51.1 / 77.2 / 92.2 µs, 16331 qps | 40.9 / 49.1 / 50.2 / 61.4 / 72.1 µs, 18840 qps | 0 / 0 errors |
| `insert-none-newgroup` | 41.7 / 48.7 / 49.8 / 72.5 / 83.2 µs, 17847 qps | 40.8 / 49.1 / 50.3 / 65.4 / 74.5 µs, 18130 qps | 0 / 0 errors |
| `insert-none-again` | 40.6 / 48.8 / 49.7 / 71.5 / 77.6 µs, 18014 qps | 41.0 / 49.2 / 50.3 / 63.5 / 73.1 µs, 18418 qps | 0 / 0 errors |
| `select` | 57.2 / 62.5 / 63.4 / 83.4 / 97.5 µs, 14918 qps | 58.7 / 63.8 / 64.5 / 94.6 / 104.5 µs, 14396 qps | 0 / 0 errors |
| `ping` | 33.4 / 37.4 / 37.8 / 62.8 / 73.6 µs, 22587 qps | 37.3 / 38.2 / 38.9 / 65.9 / 78.4 µs, 21415 qps | 0 / 0 errors |

**`c1-n200-run5/result.json`** - precell loadavg: 0.96 1.36 1.98 1/310 308587; in-driver load before `0.96 1.36 1.98 1/316 308612`, after `1.04 1.37 1.98 1/315 308625`; competing builds: none

preload 200, create assertion B / A: 0.2 / 0.3 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 42.2 / 51.0 / 51.9 / 67.5 / 77.4 µs, 17626 qps | 44.4 / 51.2 / 52.2 / 67.2 / 75.9 µs, 17595 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 46.0 / 51.9 / 52.9 / 78.1 / 93.8 µs, 16546 qps | 45.5 / 51.8 / 52.6 / 66.3 / 78.1 µs, 17566 qps | 0 / 0 errors |
| `insert-none-existing` | 40.7 / 48.6 / 50.0 / 72.9 / 86.3 µs, 18124 qps | 39.9 / 48.5 / 49.8 / 72.5 / 84.5 µs, 18354 qps | 0 / 0 errors |
| `insert-none-newgroup` | 41.5 / 48.7 / 49.9 / 67.7 / 76.5 µs, 18496 qps | 42.0 / 48.5 / 49.7 / 61.1 / 73.0 µs, 18765 qps | 0 / 0 errors |
| `insert-none-again` | 40.5 / 48.8 / 49.9 / 62.6 / 73.2 µs, 18528 qps | 41.4 / 48.7 / 49.9 / 62.8 / 74.3 µs, 17877 qps | 0 / 0 errors |
| `select` | 59.8 / 64.2 / 64.9 / 79.0 / 87.6 µs, 14816 qps | 59.4 / 64.4 / 92.4 / 108.2 / 117.8 µs, 11990 qps | 0 / 0 errors |
| `ping` | 36.3 / 37.9 / 38.2 / 48.3 / 59.0 µs, 24844 qps | 36.7 / 37.6 / 37.9 / 64.8 / 68.4 µs, 24191 qps | 0 / 0 errors |

**`c1-n1000-run1/result.json`** - precell loadavg: 0.94 2.70 2.77 1/314 306122; in-driver load before `0.94 2.70 2.77 1/312 306149`, after `0.95 2.67 2.76 1/318 306161`; competing builds: none

preload 1000, create assertion B / A: 0.8 / 0.8 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 45.5 / 51.4 / 52.3 / 74.6 / 82.2 µs, 17396 qps | 39.1 / 51.6 / 52.7 / 78.0 / 92.0 µs, 16821 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.1 / 51.4 / 52.4 / 66.0 / 76.4 µs, 17699 qps | 45.0 / 51.8 / 52.8 / 66.5 / 78.4 µs, 17533 qps | 0 / 0 errors |
| `insert-none-existing` | 42.3 / 48.7 / 49.6 / 61.5 / 72.4 µs, 18414 qps | 41.3 / 48.7 / 49.6 / 60.8 / 72.2 µs, 18740 qps | 0 / 0 errors |
| `insert-none-newgroup` | 41.7 / 49.7 / 70.6 / 83.2 / 94.9 µs, 14569 qps | 40.9 / 48.7 / 49.8 / 64.6 / 74.5 µs, 17698 qps | 0 / 0 errors |
| `insert-none-again` | 40.6 / 48.8 / 49.8 / 68.3 / 75.8 µs, 18129 qps | 42.2 / 48.8 / 50.0 / 75.4 / 90.8 µs, 17229 qps | 0 / 0 errors |
| `select` | 57.1 / 63.5 / 64.5 / 80.3 / 91.9 µs, 14775 qps | 59.8 / 64.6 / 72.9 / 107.5 / 118.9 µs, 12270 qps | 0 / 0 errors |
| `ping` | 36.6 / 37.6 / 37.9 / 47.0 / 56.9 µs, 25240 qps | 37.1 / 38.1 / 39.0 / 66.1 / 78.4 µs, 20582 qps | 0 / 0 errors |

**`c1-n1000-run2/result.json`** - precell loadavg: 0.97 2.10 2.53 1/315 306705; in-driver load before `0.97 2.10 2.53 1/312 306730`, after `1.05 2.09 2.53 1/318 306742`; competing builds: none

preload 1000, create assertion B / A: 0.8 / 0.8 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 44.7 / 51.2 / 52.1 / 76.7 / 85.4 µs, 16980 qps | 42.4 / 51.8 / 52.8 / 68.0 / 77.7 µs, 17384 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 43.1 / 51.6 / 53.1 / 79.0 / 94.6 µs, 16038 qps | 45.7 / 52.5 / 53.4 / 67.7 / 78.9 µs, 17261 qps | 0 / 0 errors |
| `insert-none-existing` | 36.1 / 48.7 / 50.1 / 74.3 / 89.6 µs, 16824 qps | 41.9 / 49.4 / 51.0 / 76.6 / 91.7 µs, 16097 qps | 0 / 0 errors |
| `insert-none-newgroup` | 40.4 / 48.7 / 49.8 / 63.5 / 73.2 µs, 18049 qps | 41.3 / 49.0 / 50.4 / 73.3 / 86.7 µs, 17344 qps | 0 / 0 errors |
| `insert-none-again` | 40.2 / 48.9 / 50.1 / 62.6 / 73.2 µs, 18586 qps | 40.4 / 48.9 / 50.2 / 62.7 / 74.9 µs, 18461 qps | 0 / 0 errors |
| `select` | 56.8 / 64.5 / 65.5 / 83.7 / 95.3 µs, 14484 qps | 57.9 / 64.8 / 65.8 / 81.8 / 96.5 µs, 14451 qps | 0 / 0 errors |
| `ping` | 37.0 / 37.7 / 37.9 / 51.9 / 62.5 µs, 24848 qps | 37.3 / 38.0 / 38.6 / 65.8 / 78.2 µs, 20746 qps | 0 / 0 errors |

**`c1-n1000-run3/result.json`** - precell loadavg: 0.93 1.64 2.28 1/313 307436; in-driver load before `1.02 1.65 2.27 2/317 307459`, after `1.01 1.64 2.27 1/317 307471`; competing builds: none

preload 1000, create assertion B / A: 0.8 / 0.8 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 45.4 / 51.4 / 52.4 / 63.0 / 72.5 µs, 17774 qps | 45.8 / 51.7 / 52.8 / 77.2 / 89.9 µs, 16771 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 44.9 / 52.0 / 53.2 / 79.9 / 96.3 µs, 16097 qps | 45.5 / 52.1 / 53.3 / 68.9 / 80.3 µs, 17207 qps | 0 / 0 errors |
| `insert-none-existing` | 41.2 / 49.0 / 50.4 / 74.2 / 89.2 µs, 16950 qps | 41.5 / 48.9 / 49.9 / 61.7 / 72.6 µs, 18653 qps | 0 / 0 errors |
| `insert-none-newgroup` | 40.0 / 48.9 / 49.9 / 72.0 / 79.3 µs, 17720 qps | 41.5 / 49.0 / 50.0 / 61.6 / 71.5 µs, 18489 qps | 0 / 0 errors |
| `insert-none-again` | 42.3 / 50.0 / 70.8 / 82.1 / 93.6 µs, 15054 qps | 42.7 / 48.9 / 49.9 / 63.1 / 72.7 µs, 17459 qps | 0 / 0 errors |
| `select` | 57.7 / 63.7 / 64.6 / 81.9 / 94.1 µs, 14694 qps | 56.7 / 64.0 / 65.1 / 94.2 / 102.8 µs, 14358 qps | 0 / 0 errors |
| `ping` | 36.9 / 37.9 / 38.2 / 49.8 / 62.0 µs, 24498 qps | 36.9 / 37.7 / 38.1 / 61.4 / 65.3 µs, 24122 qps | 0 / 0 errors |

**`c1-n1000-run4/result.json`** - precell loadavg: 0.96 1.44 2.10 1/313 308051; in-driver load before `0.96 1.44 2.10 1/317 308076`, after `0.97 1.43 2.09 1/308 308087`; competing builds: none

preload 1000, create assertion B / A: 0.8 / 0.8 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 43.6 / 51.7 / 52.6 / 63.8 / 74.7 µs, 17512 qps | 47.1 / 52.5 / 53.3 / 65.7 / 75.6 µs, 17460 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.8 / 56.7 / 76.0 / 92.1 / 101.8 µs, 13358 qps | 45.6 / 52.7 / 53.5 / 66.6 / 76.9 µs, 17355 qps | 0 / 0 errors |
| `insert-none-existing` | 43.6 / 50.7 / 71.6 / 85.9 / 96.0 µs, 14350 qps | 40.5 / 49.3 / 50.4 / 73.5 / 83.5 µs, 17717 qps | 0 / 0 errors |
| `insert-none-newgroup` | 41.2 / 48.5 / 49.3 / 59.7 / 72.3 µs, 18923 qps | 40.5 / 48.9 / 50.0 / 61.1 / 73.3 µs, 17646 qps | 0 / 0 errors |
| `insert-none-again` | 40.6 / 48.7 / 49.9 / 73.7 / 87.3 µs, 17170 qps | 40.9 / 49.2 / 50.4 / 65.3 / 76.0 µs, 18197 qps | 0 / 0 errors |
| `select` | 58.1 / 63.3 / 64.4 / 85.8 / 96.4 µs, 14626 qps | 61.6 / 64.6 / 65.6 / 82.4 / 93.7 µs, 14508 qps | 0 / 0 errors |
| `ping` | 33.3 / 38.0 / 38.4 / 50.1 / 60.2 µs, 24236 qps | 36.9 / 37.7 / 38.0 / 52.2 / 61.0 µs, 24696 qps | 0 / 0 errors |

**`c1-n1000-run5/result.json`** - precell loadavg: 0.95 1.33 1.95 1/303 308730; in-driver load before `0.95 1.33 1.95 1/313 308760`, after `1.03 1.34 1.95 1/313 308768`; competing builds: none

preload 1000, create assertion B / A: 0.8 / 0.8 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 43.4 / 51.7 / 53.8 / 81.4 / 97.1 µs, 15095 qps | 47.1 / 52.1 / 53.1 / 67.7 / 78.3 µs, 17333 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 44.6 / 51.5 / 52.4 / 64.7 / 75.8 µs, 17598 qps | 45.1 / 52.3 / 53.2 / 75.3 / 84.5 µs, 16978 qps | 0 / 0 errors |
| `insert-none-existing` | 39.5 / 49.1 / 50.2 / 62.7 / 72.6 µs, 18447 qps | 40.5 / 49.2 / 50.2 / 63.5 / 73.4 µs, 18409 qps | 0 / 0 errors |
| `insert-none-newgroup` | 39.5 / 48.7 / 49.7 / 60.3 / 70.4 µs, 18756 qps | 41.1 / 48.9 / 49.9 / 62.4 / 72.7 µs, 18419 qps | 0 / 0 errors |
| `insert-none-again` | 41.2 / 48.8 / 49.9 / 63.3 / 71.5 µs, 18164 qps | 40.4 / 49.1 / 50.1 / 71.9 / 79.4 µs, 18119 qps | 0 / 0 errors |
| `select` | 58.8 / 63.7 / 64.9 / 82.6 / 94.3 µs, 14612 qps | 60.1 / 64.7 / 66.2 / 106.6 / 120.4 µs, 13116 qps | 0 / 0 errors |
| `ping` | 36.7 / 37.5 / 38.0 / 64.5 / 77.2 µs, 21470 qps | 36.9 / 37.7 / 38.0 / 51.7 / 61.1 µs, 24773 qps | 0 / 0 errors |

**`c1-n10000-run1/result.json`** - precell loadavg: 0.87 2.62 2.74 1/307 306180; in-driver load before `0.87 2.62 2.74 1/312 306206`, after `0.97 2.59 2.73 1/302 306210`; competing builds: none

preload 10000, create assertion B / A: 7.6 / 8.0 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 44.4 / 51.7 / 52.6 / 67.1 / 75.7 µs, 17597 qps | 46.3 / 51.9 / 52.7 / 66.6 / 80.9 µs, 17450 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 41.2 / 51.3 / 52.4 / 67.2 / 78.3 µs, 16183 qps | 46.8 / 51.5 / 52.6 / 63.9 / 75.0 µs, 17136 qps | 0 / 0 errors |
| `insert-none-existing` | 41.9 / 48.6 / 49.6 / 63.4 / 73.9 µs, 18712 qps | 40.0 / 48.4 / 49.3 / 59.9 / 70.6 µs, 18858 qps | 0 / 0 errors |
| `insert-none-newgroup` | 40.7 / 48.6 / 49.7 / 64.6 / 75.5 µs, 18559 qps | 41.8 / 48.5 / 49.5 / 71.2 / 77.7 µs, 18165 qps | 0 / 0 errors |
| `insert-none-again` | 41.6 / 48.6 / 49.7 / 60.3 / 71.4 µs, 18777 qps | 42.7 / 48.5 / 49.4 / 61.5 / 71.8 µs, 18789 qps | 0 / 0 errors |
| `select` | 57.1 / 62.5 / 63.2 / 77.5 / 87.5 µs, 15194 qps | 58.4 / 62.6 / 63.3 / 76.7 / 88.1 µs, 15180 qps | 0 / 0 errors |
| `ping` | 37.2 / 38.0 / 38.4 / 52.4 / 63.3 µs, 23979 qps | 37.3 / 38.2 / 38.5 / 48.3 / 57.1 µs, 24519 qps | 0 / 0 errors |

**`c1-n10000-run2/result.json`** - precell loadavg: 0.96 2.06 2.51 1/309 306774; in-driver load before `0.96 2.06 2.51 1/312 306796`, after `1.05 2.04 2.50 1/315 306818`; competing builds: none

preload 10000, create assertion B / A: 7.8 / 7.9 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 45.8 / 52.2 / 53.1 / 66.9 / 78.5 µs, 17283 qps | 48.2 / 53.7 / 76.1 / 90.2 / 100.2 µs, 13663 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.4 / 51.5 / 52.5 / 66.1 / 77.3 µs, 17647 qps | 46.4 / 52.7 / 53.6 / 78.1 / 92.7 µs, 15990 qps | 0 / 0 errors |
| `insert-none-existing` | 42.3 / 48.9 / 50.3 / 72.7 / 85.1 µs, 17821 qps | 42.5 / 48.9 / 50.0 / 60.1 / 70.3 µs, 18665 qps | 0 / 0 errors |
| `insert-none-newgroup` | 40.7 / 50.4 / 70.9 / 83.6 / 94.2 µs, 14894 qps | 39.6 / 49.0 / 50.2 / 63.1 / 72.9 µs, 18540 qps | 0 / 0 errors |
| `insert-none-again` | 42.3 / 70.7 / 71.9 / 87.1 / 95.8 µs, 13767 qps | 40.0 / 49.0 / 50.2 / 63.2 / 75.0 µs, 18284 qps | 0 / 0 errors |
| `select` | 59.7 / 66.7 / 93.2 / 109.8 / 118.3 µs, 11381 qps | 58.3 / 63.7 / 64.5 / 83.9 / 100.1 µs, 14688 qps | 0 / 0 errors |
| `ping` | 37.4 / 38.2 / 38.5 / 52.5 / 61.6 µs, 24349 qps | 37.0 / 37.8 / 38.2 / 64.2 / 75.1 µs, 21818 qps | 0 / 0 errors |

**`c1-n10000-run3/result.json`** - precell loadavg: 0.93 1.61 2.26 1/305 307501; in-driver load before `0.93 1.61 2.26 1/309 307523`, after `1.02 1.61 2.25 1/309 307538`; competing builds: none

preload 10000, create assertion B / A: 7.6 / 7.9 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 46.4 / 52.5 / 53.3 / 74.9 / 85.5 µs, 16779 qps | 46.2 / 52.3 / 53.1 / 67.1 / 75.9 µs, 17472 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.0 / 52.3 / 53.1 / 75.2 / 85.5 µs, 15831 qps | 45.7 / 52.1 / 52.9 / 67.0 / 76.1 µs, 17440 qps | 0 / 0 errors |
| `insert-none-existing` | 41.7 / 48.8 / 49.9 / 71.5 / 83.2 µs, 18043 qps | 43.3 / 48.9 / 49.9 / 65.7 / 73.9 µs, 18447 qps | 0 / 0 errors |
| `insert-none-newgroup` | 41.5 / 48.9 / 50.0 / 72.1 / 84.7 µs, 17817 qps | 41.0 / 48.9 / 49.9 / 61.0 / 70.2 µs, 18734 qps | 0 / 0 errors |
| `insert-none-again` | 41.4 / 48.8 / 49.9 / 62.6 / 72.5 µs, 18583 qps | 40.1 / 49.1 / 50.2 / 73.1 / 85.3 µs, 17768 qps | 0 / 0 errors |
| `select` | 59.9 / 63.1 / 63.8 / 78.7 / 88.1 µs, 15005 qps | 59.2 / 63.1 / 63.6 / 76.8 / 85.4 µs, 15158 qps | 0 / 0 errors |
| `ping` | 37.2 / 38.1 / 38.4 / 51.5 / 60.0 µs, 24436 qps | 37.3 / 38.2 / 38.5 / 49.5 / 58.5 µs, 24390 qps | 0 / 0 errors |

**`c1-n10000-run4/result.json`** - precell loadavg: 0.89 1.41 2.08 2/305 308107; in-driver load before `0.89 1.41 2.08 1/309 308133`, after `0.98 1.41 2.07 1/309 308148`; competing builds: none

preload 10000, create assertion B / A: 7.9 / 7.9 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 45.2 / 52.5 / 53.3 / 77.2 / 85.5 µs, 16879 qps | 46.4 / 52.6 / 53.4 / 66.8 / 76.8 µs, 17373 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 45.3 / 52.4 / 53.7 / 82.8 / 99.2 µs, 15472 qps | 45.2 / 52.3 / 53.5 / 78.1 / 91.4 µs, 15205 qps | 0 / 0 errors |
| `insert-none-existing` | 41.1 / 48.6 / 49.6 / 62.7 / 72.8 µs, 18720 qps | 42.2 / 49.1 / 51.1 / 77.3 / 90.7 µs, 16261 qps | 0 / 0 errors |
| `insert-none-newgroup` | 39.0 / 48.6 / 49.5 / 59.0 / 68.0 µs, 18780 qps | 42.2 / 48.9 / 49.8 / 65.4 / 75.9 µs, 18393 qps | 0 / 0 errors |
| `insert-none-again` | 40.4 / 48.8 / 49.7 / 60.7 / 69.8 µs, 18315 qps | 40.9 / 48.9 / 49.8 / 61.6 / 73.2 µs, 18347 qps | 0 / 0 errors |
| `select` | 57.3 / 61.7 / 62.4 / 73.8 / 83.6 µs, 15471 qps | 57.7 / 62.8 / 63.5 / 88.8 / 99.2 µs, 14877 qps | 0 / 0 errors |
| `ping` | 36.9 / 37.8 / 38.1 / 64.1 / 68.2 µs, 23118 qps | 36.8 / 37.6 / 37.9 / 63.3 / 68.0 µs, 24100 qps | 0 / 0 errors |

**`c1-n10000-run5/result.json`** - precell loadavg: 0.95 1.32 1.94 1/316 308794; in-driver load before `0.95 1.32 1.94 1/314 308818`, after `1.03 1.32 1.94 1/312 308835`; competing builds: none

preload 10000, create assertion B / A: 9.5 / 8.0 ms

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `insert-assert-existing` | 47.6 / 52.9 / 53.7 / 68.6 / 77.8 µs, 17245 qps | 45.8 / 52.5 / 57.6 / 85.8 / 99.5 µs, 14744 qps | 0 / 0 errors |
| `insert-assert-newgroup` | 47.4 / 52.7 / 53.6 / 69.1 / 78.1 µs, 16011 qps | 45.3 / 51.9 / 53.0 / 67.3 / 76.5 µs, 17333 qps | 0 / 0 errors |
| `insert-none-existing` | 41.1 / 49.1 / 50.1 / 63.9 / 72.8 µs, 18141 qps | 39.4 / 48.9 / 49.9 / 60.5 / 72.0 µs, 18456 qps | 0 / 0 errors |
| `insert-none-newgroup` | 40.4 / 48.9 / 49.9 / 61.7 / 74.5 µs, 18291 qps | 42.5 / 49.0 / 50.1 / 64.1 / 75.5 µs, 14950 qps | 0 / 0 errors |
| `insert-none-again` | 40.0 / 48.9 / 50.1 / 73.4 / 150.0 µs, 17074 qps | 43.0 / 49.0 / 50.1 / 64.5 / 75.9 µs, 15765 qps | 0 / 0 errors |
| `select` | 57.5 / 62.5 / 65.3 / 112.5 / 222.5 µs, 12323 qps | 58.6 / 63.4 / 64.1 / 93.4 / 247.0 µs, 13656 qps | 0 / 0 errors |
| `ping` | 37.3 / 38.0 / 38.4 / 49.3 / 58.4 µs, 22730 qps | 36.8 / 37.7 / 38.0 / 51.5 / 59.8 µs, 24790 qps | 0 / 0 errors |

### Cell 2 raw (child insert, fk)

**`c2-run1/result.json`** - precell loadavg: 0.97 2.59 2.73 1/301 306234; in-driver load before `0.97 2.59 2.73 1/311 306263`, after `0.97 2.56 2.72 1/313 306270`; competing builds: none

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `child-insert-fk` | 43.3 / 50.0 / 50.9 / 65.3 / 75.8 µs, 18256 qps | 43.5 / 50.8 / 52.6 / 78.6 / 93.4 µs, 16078 qps | 0 / 0 errors |
| `child-insert-fk-txn` | 103.4 / 109.3 / 110.8 / 147.1 / 164.2 µs, 8503 qps | 104.4 / 110.5 / 120.5 / 164.2 / 172.8 µs, 7586 qps | 0 / 0 errors |
| `child-insert-fk-txn:stmt` | 44.7 / 50.3 / 51.3 / 73.9 / 81.1 µs, 0 qps | 46.0 / 51.3 / 55.7 / 80.4 / 93.8 µs, 0 qps | 0 / 0 errors |
| `child-insert-plain` | 40.7 / 48.8 / 49.9 / 61.9 / 73.4 µs, 18497 qps | 39.7 / 49.0 / 50.0 / 71.8 / 80.4 µs, 18116 qps | 0 / 0 errors |
| `parent-update` | 40.4 / 49.0 / 49.9 / 61.2 / 73.1 µs, 18731 qps | 38.3 / 49.5 / 50.3 / 73.2 / 78.8 µs, 18269 qps | 0 / 0 errors |
| `child-update` | 39.8 / 49.3 / 50.2 / 60.5 / 70.0 µs, 18741 qps | 39.2 / 50.0 / 50.7 / 74.5 / 86.8 µs, 17301 qps | 0 / 0 errors |
| `child-update-again` | 40.4 / 49.2 / 50.0 / 62.0 / 71.9 µs, 19164 qps | 43.2 / 49.8 / 50.5 / 59.6 / 68.4 µs, 19138 qps | 0 / 0 errors |
| `select` | 58.2 / 63.2 / 63.9 / 78.9 / 89.6 µs, 14936 qps | 58.6 / 63.7 / 64.4 / 79.2 / 90.1 µs, 14811 qps | 0 / 0 errors |
| `ping` | 34.2 / 38.3 / 44.4 / 64.7 / 77.7 µs, 20025 qps | 36.8 / 37.6 / 37.9 / 48.2 / 61.3 µs, 21521 qps | 0 / 0 errors |

**`c2-run2/result.json`** - precell loadavg: 0.95 1.94 2.45 1/305 306968; in-driver load before `0.95 1.94 2.45 1/307 306991`, after `1.04 1.94 2.45 1/315 307002`; competing builds: none

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `child-insert-fk` | 42.3 / 49.7 / 50.4 / 61.7 / 71.9 µs, 18539 qps | 45.0 / 53.1 / 73.7 / 89.3 / 98.4 µs, 13830 qps | 0 / 0 errors |
| `child-insert-fk-txn` | 102.7 / 109.0 / 113.9 / 160.5 / 172.0 µs, 7790 qps | 102.8 / 110.2 / 113.2 / 156.3 / 170.8 µs, 7938 qps | 0 / 0 errors |
| `child-insert-fk-txn:stmt` | 44.3 / 50.4 / 53.1 / 78.0 / 92.6 µs, 0 qps | 44.1 / 51.3 / 52.9 / 76.5 / 89.8 µs, 0 qps | 0 / 0 errors |
| `child-insert-plain` | 41.8 / 48.5 / 49.7 / 73.2 / 87.9 µs, 17696 qps | 44.5 / 48.5 / 49.5 / 71.4 / 79.8 µs, 18177 qps | 0 / 0 errors |
| `parent-update` | 40.8 / 50.3 / 72.8 / 85.1 / 95.7 µs, 14542 qps | 44.6 / 50.1 / 50.8 / 76.5 / 90.6 µs, 16686 qps | 0 / 0 errors |
| `child-update` | 42.2 / 72.8 / 73.6 / 88.8 / 96.5 µs, 13189 qps | 42.6 / 50.2 / 50.8 / 63.5 / 76.3 µs, 18084 qps | 0 / 0 errors |
| `child-update-again` | 38.9 / 48.9 / 50.4 / 74.6 / 85.8 µs, 18438 qps | 41.6 / 50.6 / 51.6 / 77.1 / 92.4 µs, 16908 qps | 0 / 0 errors |
| `select` | 59.4 / 63.0 / 63.9 / 86.4 / 100.6 µs, 13317 qps | 58.8 / 63.9 / 64.6 / 80.5 / 91.4 µs, 14737 qps | 0 / 0 errors |
| `ping` | 36.5 / 37.3 / 37.7 / 51.7 / 60.0 µs, 24863 qps | 36.6 / 37.4 / 37.7 / 51.2 / 62.7 µs, 24865 qps | 0 / 0 errors |

**`c2-run3/result.json`** - precell loadavg: 0.93 1.58 2.23 1/312 307584; in-driver load before `0.93 1.58 2.23 1/309 307608`, after `1.10 1.60 2.24 1/310 307624`; competing builds: none

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `child-insert-fk` | 41.9 / 49.7 / 50.6 / 62.1 / 73.2 µs, 18420 qps | 44.6 / 52.9 / 74.3 / 89.4 / 99.7 µs, 13793 qps | 0 / 0 errors |
| `child-insert-fk-txn` | 99.4 / 109.0 / 111.2 / 150.1 / 169.3 µs, 8284 qps | 104.1 / 114.6 / 146.0 / 167.1 / 175.9 µs, 7066 qps | 0 / 0 errors |
| `child-insert-fk-txn:stmt` | 45.0 / 50.4 / 51.6 / 75.1 / 89.6 µs, 0 qps | 46.2 / 53.5 / 72.7 / 86.9 / 97.2 µs, 0 qps | 0 / 0 errors |
| `child-insert-plain` | 39.3 / 48.9 / 50.7 / 76.1 / 91.7 µs, 16322 qps | 42.3 / 50.0 / 70.8 / 81.6 / 93.9 µs, 14969 qps | 0 / 0 errors |
| `parent-update` | 40.9 / 49.1 / 50.3 / 74.1 / 87.8 µs, 17545 qps | 44.4 / 73.6 / 74.5 / 89.9 / 97.8 µs, 13186 qps | 0 / 0 errors |
| `child-update` | 41.1 / 49.0 / 50.1 / 60.9 / 72.7 µs, 18524 qps | 47.2 / 74.0 / 74.8 / 90.3 / 98.7 µs, 12954 qps | 0 / 0 errors |
| `child-update-again` | 41.1 / 48.9 / 49.9 / 60.6 / 74.4 µs, 19294 qps | 43.0 / 51.5 / 53.6 / 80.1 / 96.0 µs, 15789 qps | 0 / 0 errors |
| `select` | 51.4 / 63.2 / 64.2 / 88.0 / 101.1 µs, 14557 qps | 58.9 / 91.7 / 95.0 / 112.8 / 123.9 µs, 9976 qps | 0 / 0 errors |
| `ping` | 37.0 / 37.7 / 38.1 / 55.0 / 65.2 µs, 24145 qps | 36.8 / 37.5 / 38.1 / 66.8 / 79.0 µs, 21403 qps | 0 / 0 errors |

**`c2-run4/result.json`** - precell loadavg: 0.98 1.41 2.07 1/305 308168; in-driver load before `0.98 1.41 2.07 1/309 308190`, after `0.98 1.40 2.07 1/310 308205`; competing builds: none

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `child-insert-fk` | 44.8 / 51.2 / 52.1 / 62.7 / 72.9 µs, 17616 qps | 44.0 / 50.5 / 51.2 / 60.8 / 69.0 µs, 18195 qps | 0 / 0 errors |
| `child-insert-fk-txn` | 104.7 / 110.6 / 116.9 / 165.7 / 176.5 µs, 7611 qps | 102.3 / 110.0 / 111.8 / 148.9 / 166.4 µs, 8412 qps | 0 / 0 errors |
| `child-insert-fk-txn:stmt` | 46.7 / 51.5 / 54.4 / 81.2 / 96.4 µs, 0 qps | 44.9 / 50.7 / 52.0 / 74.3 / 83.9 µs, 0 qps | 0 / 0 errors |
| `child-insert-plain` | 40.2 / 49.1 / 50.3 / 75.6 / 90.3 µs, 17115 qps | 42.3 / 49.1 / 50.0 / 64.0 / 74.1 µs, 18462 qps | 0 / 0 errors |
| `parent-update` | 39.7 / 51.1 / 73.8 / 86.6 / 98.2 µs, 14520 qps | 44.0 / 50.9 / 51.5 / 66.2 / 76.6 µs, 17465 qps | 0 / 0 errors |
| `child-update` | 42.1 / 50.8 / 53.2 / 80.8 / 95.6 µs, 13938 qps | 42.0 / 51.1 / 51.8 / 77.6 / 89.3 µs, 17462 qps | 0 / 0 errors |
| `child-update-again` | 43.3 / 50.6 / 51.3 / 75.3 / 84.6 µs, 18047 qps | 40.4 / 51.1 / 51.7 / 61.8 / 72.8 µs, 18570 qps | 0 / 0 errors |
| `select` | 59.2 / 65.9 / 93.2 / 110.5 / 121.7 µs, 10757 qps | 58.7 / 64.3 / 65.3 / 97.5 / 112.8 µs, 13831 qps | 0 / 0 errors |
| `ping` | 35.7 / 38.0 / 38.4 / 52.7 / 61.0 µs, 24340 qps | 37.6 / 38.4 / 38.7 / 48.2 / 57.0 µs, 24373 qps | 0 / 0 errors |

**`c2-run5/result.json`** - precell loadavg: 0.95 1.30 1.93 1/309 308883; in-driver load before `0.95 1.30 1.93 1/313 308907`, after `0.95 1.30 1.92 1/320 308920`; competing builds: none

| arm | B (`9f170b1`): p0 / p25 / p50 / p95 / p99, throughput | A (`a59da9c`): p0 / p25 / p50 / p95 / p99, throughput | errors B / A |
|---|---|---|---|
| `child-insert-fk` | 42.7 / 50.2 / 51.0 / 63.1 / 77.2 µs, 18209 qps | 42.5 / 50.4 / 51.2 / 64.9 / 73.6 µs, 18061 qps | 0 / 0 errors |
| `child-insert-fk-txn` | 104.1 / 109.5 / 110.8 / 133.1 / 142.0 µs, 8604 qps | 101.7 / 109.1 / 110.7 / 130.2 / 139.2 µs, 8511 qps | 0 / 0 errors |
| `child-insert-fk-txn:stmt` | 46.2 / 50.7 / 51.6 / 64.6 / 74.5 µs, 0 qps | 45.7 / 50.3 / 51.3 / 64.3 / 72.4 µs, 0 qps | 0 / 0 errors |
| `child-insert-plain` | 41.9 / 48.6 / 49.6 / 65.0 / 75.1 µs, 18633 qps | 39.6 / 48.4 / 49.5 / 62.8 / 74.3 µs, 17663 qps | 0 / 0 errors |
| `parent-update` | 42.0 / 50.3 / 50.9 / 63.5 / 74.4 µs, 18172 qps | 40.5 / 49.8 / 50.6 / 65.0 / 75.2 µs, 18441 qps | 0 / 0 errors |
| `child-update` | 41.5 / 50.5 / 51.1 / 62.7 / 71.7 µs, 18216 qps | 40.8 / 49.9 / 50.6 / 65.4 / 75.0 µs, 18203 qps | 0 / 0 errors |
| `child-update-again` | 42.6 / 50.2 / 50.9 / 63.5 / 75.9 µs, 18782 qps | 40.9 / 49.6 / 50.5 / 64.1 / 75.3 µs, 18894 qps | 0 / 0 errors |
| `select` | 54.2 / 63.6 / 64.5 / 80.1 / 92.3 µs, 14804 qps | 58.3 / 63.8 / 64.5 / 79.0 / 90.1 µs, 14831 qps | 0 / 0 errors |
| `ping` | 37.3 / 38.2 / 38.6 / 53.6 / 63.5 µs, 20109 qps | 37.3 / 38.1 / 38.5 / 49.7 / 59.8 µs, 24206 qps | 0 / 0 errors |

### Cell 3 raw (per-K, per-run medians of per-row wall time, engine time per row)

**`c3-run1/result.json`** - precell loadavg: 0.97 2.56 2.72 1/309 306289; in-driver load before `0.90 2.52 2.70 1/320 306320`, after `1.61 2.38 2.64 1/311 306451`; competing builds: none

| K | kind | reps | B median per row | A median per row | B engine µs/row | A engine µs/row | errors B / A |
|---|---|---|---|---|---|---|---|
| 1 | `fk` | 32768 | 120.0 µs | 118.5 µs | 54.2 µs | 52.7 µs | 0 / 0 |
| 1 | `plain` | 32768 | 118.4 µs | 116.6 µs | 55.6 µs | 50.4 µs | 0 / 0 |
| 64 | `fk` | 512 | 54.0 µs | 54.5 µs | 14.6 µs | 15.0 µs | 0 / 0 |
| 64 | `plain` | 512 | 52.9 µs | 53.1 µs | 14.3 µs | 16.5 µs | 0 / 0 |
| 1024 | `fk` | 32 | 52.9 µs | 52.9 µs | 25.3 µs | 25.3 µs | 0 / 0 |
| 1024 | `plain` | 32 | 51.7 µs | 52.1 µs | 13.5 µs | 14.6 µs | 0 / 0 |
| 4096 | `fk` | 8 | 55.6 µs | 53.9 µs | 16.5 µs | 14.7 µs | 0 / 0 |
| 4096 | `plain` | 8 | 51.2 µs | 51.8 µs | 13.0 µs | 15.6 µs | 0 / 0 |
| 16384 | `fk` | 5 | 59.9 µs | 59.5 µs | 22.8 µs | 21.5 µs | 0 / 0 |
| 16384 | `plain` | 5 | 52.4 µs | 53.8 µs | 13.8 µs | 14.1 µs | 0 / 0 |

**`c3-run2/result.json`** - precell loadavg: 0.95 1.91 2.44 1/305 307033; in-driver load before `0.95 1.91 2.44 1/308 307057`, after `1.41 1.83 2.36 1/302 307171`; competing builds: none

| K | kind | reps | B median per row | A median per row | B engine µs/row | A engine µs/row | errors B / A |
|---|---|---|---|---|---|---|---|
| 1 | `fk` | 32768 | 121.3 µs | 119.2 µs | 56.3 µs | 52.4 µs | 0 / 0 |
| 1 | `plain` | 32768 | 116.9 µs | 116.9 µs | 50.6 µs | 51.5 µs | 0 / 0 |
| 64 | `fk` | 512 | 54.1 µs | 53.9 µs | 15.0 µs | 14.2 µs | 0 / 0 |
| 64 | `plain` | 512 | 52.7 µs | 53.5 µs | 14.1 µs | 17.8 µs | 0 / 0 |
| 1024 | `fk` | 32 | 53.8 µs | 53.1 µs | 25.5 µs | 25.2 µs | 0 / 0 |
| 1024 | `plain` | 32 | 52.0 µs | 52.0 µs | 15.0 µs | 14.9 µs | 0 / 0 |
| 4096 | `fk` | 8 | 54.2 µs | 59.7 µs | 14.8 µs | 19.2 µs | 0 / 0 |
| 4096 | `plain` | 8 | 51.9 µs | 51.6 µs | 14.2 µs | 12.9 µs | 0 / 0 |
| 16384 | `fk` | 5 | 60.5 µs | 59.6 µs | 23.7 µs | 22.7 µs | 0 / 0 |
| 16384 | `plain` | 5 | 52.1 µs | 53.1 µs | 13.2 µs | 14.4 µs | 0 / 0 |

**`c3-run3/result.json`** - precell loadavg: 0.93 1.55 2.21 1/305 307677; in-driver load before `0.93 1.55 2.21 1/309 307702`, after `1.49 1.59 2.17 1/310 307816`; competing builds: none

| K | kind | reps | B median per row | A median per row | B engine µs/row | A engine µs/row | errors B / A |
|---|---|---|---|---|---|---|---|
| 1 | `fk` | 32768 | 120.2 µs | 119.8 µs | 53.6 µs | 52.8 µs | 0 / 0 |
| 1 | `plain` | 32768 | 116.7 µs | 116.4 µs | 49.6 µs | 49.9 µs | 0 / 0 |
| 64 | `fk` | 512 | 54.1 µs | 54.6 µs | 14.5 µs | 18.5 µs | 0 / 0 |
| 64 | `plain` | 512 | 53.8 µs | 53.6 µs | 17.4 µs | 17.9 µs | 0 / 0 |
| 1024 | `fk` | 32 | 52.7 µs | 54.3 µs | 24.2 µs | 27.0 µs | 0 / 0 |
| 1024 | `plain` | 32 | 51.5 µs | 51.7 µs | 12.8 µs | 13.8 µs | 0 / 0 |
| 4096 | `fk` | 8 | 53.1 µs | 54.5 µs | 14.8 µs | 17.6 µs | 0 / 0 |
| 4096 | `plain` | 8 | 50.9 µs | 51.1 µs | 12.5 µs | 12.8 µs | 0 / 0 |
| 16384 | `fk` | 5 | 57.4 µs | 66.1 µs | 22.5 µs | 25.3 µs | 0 / 0 |
| 16384 | `plain` | 5 | 51.7 µs | 51.0 µs | 13.1 µs | 13.4 µs | 0 / 0 |

**`c3-run4/result.json`** - precell loadavg: 0.98 1.40 2.07 1/313 308231; in-driver load before `1.06 1.41 2.07 1/318 308262`, after `1.72 1.53 2.06 1/321 308390`; competing builds: none

| K | kind | reps | B median per row | A median per row | B engine µs/row | A engine µs/row | errors B / A |
|---|---|---|---|---|---|---|---|
| 1 | `fk` | 32768 | 119.1 µs | 118.0 µs | 53.0 µs | 53.1 µs | 0 / 0 |
| 1 | `plain` | 32768 | 117.1 µs | 115.7 µs | 51.2 µs | 50.2 µs | 0 / 0 |
| 64 | `fk` | 512 | 53.9 µs | 53.9 µs | 14.3 µs | 14.3 µs | 0 / 0 |
| 64 | `plain` | 512 | 53.1 µs | 53.3 µs | 13.4 µs | 15.5 µs | 0 / 0 |
| 1024 | `fk` | 32 | 52.8 µs | 53.4 µs | 24.7 µs | 24.4 µs | 0 / 0 |
| 1024 | `plain` | 32 | 51.5 µs | 51.8 µs | 12.6 µs | 16.3 µs | 0 / 0 |
| 4096 | `fk` | 8 | 54.0 µs | 53.3 µs | 16.0 µs | 18.0 µs | 0 / 0 |
| 4096 | `plain` | 8 | 53.3 µs | 51.2 µs | 14.5 µs | 14.2 µs | 0 / 0 |
| 16384 | `fk` | 5 | 60.5 µs | 57.6 µs | 22.0 µs | 21.9 µs | 0 / 0 |
| 16384 | `plain` | 5 | 51.2 µs | 57.4 µs | 13.1 µs | 15.5 µs | 0 / 0 |

**`c3-run5/result.json`** - precell loadavg: 0.95 1.30 1.92 1/317 308941; in-driver load before `0.88 1.27 1.91 1/321 308964`, after `1.37 1.32 1.88 1/314 309094`; competing builds: none

| K | kind | reps | B median per row | A median per row | B engine µs/row | A engine µs/row | errors B / A |
|---|---|---|---|---|---|---|---|
| 1 | `fk` | 32768 | 119.1 µs | 120.3 µs | 52.2 µs | 54.8 µs | 0 / 0 |
| 1 | `plain` | 32768 | 117.2 µs | 116.7 µs | 51.5 µs | 52.2 µs | 0 / 0 |
| 64 | `fk` | 512 | 54.5 µs | 54.1 µs | 17.4 µs | 14.9 µs | 0 / 0 |
| 64 | `plain` | 512 | 53.0 µs | 74.8 µs | 13.3 µs | 19.3 µs | 0 / 0 |
| 1024 | `fk` | 32 | 52.9 µs | 52.3 µs | 23.5 µs | 24.5 µs | 0 / 0 |
| 1024 | `plain` | 32 | 51.6 µs | 51.6 µs | 12.8 µs | 13.9 µs | 0 / 0 |
| 4096 | `fk` | 8 | 54.3 µs | 53.5 µs | 15.2 µs | 15.2 µs | 0 / 0 |
| 4096 | `plain` | 8 | 51.5 µs | 56.8 µs | 13.7 µs | 14.9 µs | 0 / 0 |
| 16384 | `fk` | 5 | 62.6 µs | 59.8 µs | 23.0 µs | 22.1 µs | 0 / 0 |
| 16384 | `plain` | 5 | 55.5 µs | 51.5 µs | 14.8 µs | 13.1 µs | 0 / 0 |
