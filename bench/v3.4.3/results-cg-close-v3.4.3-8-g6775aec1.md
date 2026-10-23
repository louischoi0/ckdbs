# CG close: the paths CG touched move inside noise but one - an UPDATE walking 10,000 rows has a slow mode in 15 of 20 B runs - and an upsert by a unique column is 13x faster than the UPDATE it replaces

**Thesis.** Measured on `cg-on-conflict-do-update` at `6775aec1`
(`v3.4.3-8-g6775aec1`, **B**) against the commit CG opened from, `f44846ef`
(`v3.4.3`, **A** - its build inputs are `4fb01b46`'s; everything after it is
under `instructions/`), `cores = 1`, `relaxed`, ten interleaved servers per
build:

- **The paths S1 touched, A -> B, two whole rotations: inside noise but
  one cell.** Plain `INSERT`, `ON CONFLICT DO NOTHING` inserting and
  skipping, and the tuple-key point `UPDATE` move -16.2 % to +5.4 % at p50
  across the two rotations, every rep range overlapping, with no consistent
  sign between them.
- **The one cell: an `UPDATE` that walks 10,000 rows** (`UPDATE u SET v = x
  WHERE name = '...'`; no index serves an `UPDATE`'s WHERE) has a slow mode
  on B. First rotation: 780 -> 860 us (+10.3 %), all ten B reps (796-873)
  above all ten A reps (755-795). The rerun, which the outlier rule asks
  for: 777 -> 815 us (+4.9 %), overlapping - five B reps at 750-787, as A,
  and five at 843-875. **In 15 of 20 B reps the walk sits above every A rep;
  no A rep is that slow.** At 1,000 rows the cell is -5.3 % and +6.1 %. S1
  changed nothing inside that walk - `UpdateInner` was split into its parse
  and `UpdateParsed`, the write borrow's declaration gained a condition and
  the point path's entry the arm target; the walk's loop and `apply` are as
  they were - so this is no per-row cost S1 added. A placement effect of
  the split on B's binary is the likely source; it is not attributed here.
- **Each `DO UPDATE` arm against its plain counterpart, B only** (A refuses
  `DO UPDATE`): the insert arm is +7.9 % to +10.8 % over a plain `INSERT`
  (about 4-5 us), and the tuple-key update arm +9.0 % to +10.6 % over a
  point `UPDATE` (about 5 us; the first rotation's +23.9 % at 10,000 rows was
  the median of two clusters, and the rerun has one) - the arbiter's lock
  and lookup plus the arm re-running `UpdateParsed`'s setup per row
  (resolution, compile), which the S1 review named. **The unique-column upsert is 60-65 us, against
  130-860 us for the `UPDATE ... WHERE name = ...` it replaces**: the
  arbiter finds its holder through the unique index's check, where an
  `UPDATE`'s WHERE walks the relation.

- **Order**: `instructions/v3.2.0/workorder-cg-on-conflict-do-update.md` §5
  CG-S3, §7 "CG-S3 measured 2026-10-23".
- **Driver**: `tools/cg_close_benchmark.py` (new for this close; its docstring
  is its documentation), run by `cg-matrix.sh` in the archive.
- **Raw output**: `archive/cg-close-v3.4.3-8-g6775aec1/` - the driver JSON and
  console per server, `tables.py` and its output `tables.md` (the first
  rotation), `rerun/` with its own `tables.md` (the second), the build and
  matrix scripts, `progress.txt`.

| Item | Value |
|---|---|
| Executed | 2026-10-23 UTC. Builds 10:38:24-10:40:21; the first rotation 10:43:41-10:54:21 (the quiet gate held 185 s before its first cell and 535 s before rep 8); the rerun 11:22:44-11:24:46 (held 1,660 s first) |
| A | `f44846ef`, `v3.4.3` - the commit CG opened from (CG was written at `722006ec`, which §5 names as A; between that commit and CG's opening lie 30-odd commits of other milestones, so an A/B against it would price them, and the A1 close's precedent - A is the milestone's opening commit - is followed). Exported with `git archive`, built in a scratch directory |
| B | `6775aec1`, `v3.4.3-8-g6775aec1`, CG-S1 merged with `origin/main` |
| Binaries | Release, built this session after their commits, run from copies: `kds_server.f44846ef` sha256 `6e6c2ac19c5e7ecb5b2526628a02ffc3a96b1a54fef0a3b4eacf28114defbd35`; `kds_server.6775aec1` `3d300d01f53a2e88c9b110d8d08971223cdcd64646b6abdd80519c4df1135d36` |
| Host / device | 8 vCPU AMD EPYC 9V74, Linux 7.0.0-1014-azure; data and WAL on `/dev/root`, ext4, not tmpfs; unpinned. The quiet gate before every cell: no `kds_tests`, no `cc1plus`, 1-minute load under 2 (`progress.txt`) |
| Server configuration | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, `log_level = warn`; one fresh server and data file per cell |
| Cell | Two relations per size, loaded untimed: `t (id, c_int, c_text varchar(16))` and `u (id, name varchar(16), v)` with a unique index on `name`, 1,000 or 10,000 rows. 1,000 autocommit statements per phase from one connection, timed on the wire. Arm order alternating A B / B A |
| Baseline | No earlier number exists for these statements with this driver. The PostgreSQL floor was not measured: its `ON CONFLICT` reacts to every unique index, which is not the statement measured here |
| Suite | 4319/4319 at `54dbc716`, whose build inputs B shares |

## 1. What was run, what was cut

- **Run**: §5's close cells - plain `INSERT`, `DO NOTHING`, both arms of `DO
  UPDATE` - plus the point `UPDATE` and the walking `UPDATE`, which S1's split
  of `UpdateInner` touched, and the unique-column arm, which narrowing made
  S1's.
- **Rerun**: the whole rotation again for the `uq-update` outlier, 11:22:44-
  11:24:46, after the quiet gate held it 1,660 s behind other sessions'
  suites (`rerun/`).
- **Cut**: `cores = 4`; any durability but `relaxed`; a contended cell.

## 2. A -> B (ten reps)

A table entry is the median over the reps of that percentile, in
microseconds; every rep is in `tables.md`.

| rows | phase | arm | p0 | p25 | p50 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| 1000 | insert | A | 42.0 | 43.8 | 44.8 | 62.7 | 76.8 | 105.0 |
| 1000 | insert | B | 42.8 | 44.3 | 45.4 | 64.1 | 82.5 | 102.6 |
| 1000 | dn-insert | A | 43.9 | 45.9 | 46.5 | 61.5 | 77.6 | 101.2 |
| 1000 | dn-insert | B | 43.9 | 46.2 | 49.0 | 68.8 | 89.9 | 107.7 |
| 1000 | dn-skip | A | 38.5 | 42.3 | 42.9 | 57.6 | 72.0 | 101.5 |
| 1000 | dn-skip | B | 40.7 | 42.7 | 43.7 | 58.1 | 74.3 | 101.4 |
| 1000 | update | A | 44.2 | 46.5 | 50.8 | 64.7 | 85.1 | 3,294.7 |
| 1000 | update | B | 41.9 | 46.5 | 47.1 | 57.5 | 74.8 | 2,461.2 |
| 1000 | uq-update | A | 112.8 | 122.5 | 137.1 | 153.8 | 197.8 | 242.6 |
| 1000 | uq-update | B | 117.8 | 127.7 | 129.8 | 151.7 | 169.8 | 222.2 |
| 10000 | insert | A | 42.0 | 44.0 | 44.7 | 60.0 | 78.0 | 104.8 |
| 10000 | insert | B | 42.0 | 44.2 | 45.0 | 60.6 | 74.8 | 110.0 |
| 10000 | dn-insert | A | 44.1 | 45.9 | 49.3 | 66.7 | 86.6 | 110.4 |
| 10000 | dn-insert | B | 43.6 | 46.1 | 46.8 | 58.3 | 74.4 | 102.1 |
| 10000 | dn-skip | A | 39.0 | 42.5 | 43.2 | 56.8 | 71.8 | 3,123.3 |
| 10000 | dn-skip | B | 36.2 | 42.6 | 43.9 | 58.4 | 76.0 | 99.1 |
| 10000 | update | A | 40.5 | 46.5 | 47.3 | 57.2 | 74.8 | 101.9 |
| 10000 | update | B | 41.3 | 46.9 | 48.0 | 66.8 | 82.5 | 3,277.7 |
| 10000 | uq-update | A | 723.6 | 766.6 | 780.0 | 805.8 | 991.9 | 1,296.5 |
| 10000 | uq-update | B | 810.5 | 848.1 | 860.0 | 890.9 | 962.5 | 1,197.9 |

| rows | phase | A p50 | B p50 | delta | ranges overlap? | B above A, paired reps |
|---|---|---|---|---|---|---|
| 1000 | insert | 44.8 | 45.4 | +1.3 % | yes | 5/10 |
| 1000 | dn-insert | 46.5 | 49.0 | +5.4 % | yes | 7/10 |
| 1000 | dn-skip | 42.9 | 43.7 | +1.9 % | yes | 5/10 |
| 1000 | update | 50.8 | 47.1 | -7.3 % | yes | 4/10 |
| 1000 | uq-update | 137.1 | 129.8 | -5.3 % | yes | 4/10 |
| 10000 | insert | 44.7 | 45.0 | +0.6 % | yes | 5/10 |
| 10000 | dn-insert | 49.3 | 46.8 | -5.1 % | yes | 3/10 |
| 10000 | dn-skip | 43.2 | 43.9 | +1.4 % | yes | 6/10 |
| 10000 | update | 47.3 | 48.0 | +1.5 % | yes | 7/10 |
| 10000 | uq-update | 780.0 | 860.0 | **+10.3 %** | **no** | **10/10** |

## 3. The arms, B only

| rows | arm | p50 | counterpart | p50 | delta |
|---|---|---|---|---|---|
| 1000 | up-insert (DO UPDATE, absent key) | 49.0 | insert | 45.4 | +7.9 % |
| 1000 | up-update (DO UPDATE, placed key) | 52.1 | update (point) | 47.1 | +10.6 % |
| 1000 | uq-upsert (DO UPDATE on `name`) | 60.1 | uq-update (walk) | 129.8 | -53.7 % |
| 10000 | up-insert | 49.8 | insert | 45.0 | +10.8 % |
| 10000 | up-update | 59.5 | update (point) | 48.0 | +23.9 % |
| 10000 | uq-upsert | 65.0 | uq-update (walk) | 860.0 | -92.4 % |

The update arm's 10,000-row p50 is bimodal across the first rotation's reps
(51.7-52.2 in four, 56.8-72.8 in six), so its +23.9 % is the median of two
clusters. The rerun has one: 52.0 us against the point `UPDATE`'s 47.2
(+10.1 %), and the arms otherwise repeat (+8.9 % / +9.4 % for the insert
arm, -54.6 % / -92.7 % for the unique-column upsert).

**The rerun's A -> B** (`rerun/tables.md`):

| rows | phase | A p50 | B p50 | delta | ranges overlap? | B above A, paired reps |
|---|---|---|---|---|---|---|
| 1000 | insert | 44.3 | 44.6 | +0.6 % | yes | 7/10 |
| 1000 | dn-insert | 46.3 | 46.9 | +1.2 % | yes | 8/10 |
| 1000 | dn-skip | 44.5 | 43.0 | -3.5 % | yes | 7/10 |
| 1000 | update | 46.8 | 47.5 | +1.4 % | yes | 8/10 |
| 1000 | uq-update | 122.9 | 130.3 | +6.1 % | yes | 7/10 |
| 10000 | insert | 48.2 | 44.3 | -8.0 % | yes | 4/10 |
| 10000 | dn-insert | 55.2 | 46.3 | -16.2 % | yes | 4/10 |
| 10000 | dn-skip | 42.8 | 42.7 | -0.2 % | yes | 5/10 |
| 10000 | update | 46.9 | 47.2 | +0.7 % | yes | 6/10 |
| 10000 | uq-update | 777.0 | 815.3 | +4.9 % | yes | 7/10 |

## 4. What this says about the engine

- **The arm pays for being a whole `UPDATE`.** Each conflicting row runs
  `UpdateParsed` from its top: relation resolution, `InitTableAccess`,
  `CompileAssignments`, the write admission. That is 5-11 us a row over a
  point `UPDATE`, and it is the price of CG-R1's one write path. Compiling
  the assignments once per statement, with `EXCLUDED` as a per-row slot,
  is the cut if the cost matters.
- **An upsert by a business key replaces a walk with a probe.** `UPDATE ...
  WHERE name = ...` walks the relation because no index serves an UPDATE's
  WHERE; `ON CONFLICT (name) DO UPDATE` reaches the row through the unique
  check. The three clients S0 read hand-roll exactly that `UPDATE` today.
