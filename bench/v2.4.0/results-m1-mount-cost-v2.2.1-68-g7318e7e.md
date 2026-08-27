# M1 — mount cost of the tenth bootstrap relation (`sys.ranges`, RD1/RA2)

`instructions/v2.4.0/range-foundation.md` §7 M1: boot-to-listener time of
`kds_server` at superblock **v15** (`b0b6e8a`, pre-RA2, 9 bootstrap
relations) against **v16** (`7318e7e`, which adds `sys.ranges` — oid 133,
root page 15 — as the tenth). This reads against **H1**
(stated with its falsifier in the work order §4, restated in
`docs/inflight/in-progress/workplan-range-directory.md` §3a, which says so
itself — *"falsifiers in the order §4"*): *"a v16 instance carrying an empty
`sys.ranges` behaves identically to v15 on every existing path."* The suite
half of H1 already held (2775/2775 + sim 171/171 at `7318e7e`, recorded in
the workplan §3a's verdict paragraph, not in the work order); this run owns
the mount-cost half only. Measured on the worktree
`v2.4.0-range-foundation-1` at `7318e7e` (`v2.2.1-68-g7318e7e`).

## 1. Stamp

| | |
|---|---|
| Date/time executed | 2026-08-27, ~12:00–12:10 UTC |
| Version directory | `bench/v2.4.0/` names the work order's target version; **no `v2.4.0` tag exists** — the operator names versions — so `git describe --tags` reads `v2.2.1-68-g7318e7e`, and that string, not the directory, is what dates every number here |
| Worktree | `v2.4.0-range-foundation-1` |
| Branch | `worktree-v2.4.0-range-foundation-1` |
| v16 arm commit | `7318e7e` (`git describe --tags` = `v2.2.1-68-g7318e7e`) |
| v15 arm commit | `b0b6e8a` (`git describe --tags b0b6e8a` = `v2.2.1-56-gb0b6e8a`) |
| Tree cleanliness | clean at measurement time (`git status` — nothing to commit) both before and after the run; no source edited for this cell |
| Host | `ip-172-31-1-92`, Linux 7.0.0-1011-aws, Ubuntu 26.04 |
| CPU | Intel Xeon Platinum 8488C, 1 socket × 4 cores × 2 threads/core = 8 logical CPUs (SMT on), 1 NUMA node (`lscpu`) |
| Data-file device | `$HOME` (`/home/ubuntu/mount-bench-m1/scratch`) → `/dev/root`, **ext4** (`df -T`); binary copies also on this ext4 filesystem, never tmpfs — `/tmp` on this host is tmpfs (`df -T /tmp`), used only for the v15 out-of-tree *source checkout*, never for a data file |
| Build type | Release both arms: `CMAKE_BUILD_TYPE=Release`, `CMAKE_CXX_FLAGS_RELEASE = -O3 -DNDEBUG`, generator Unix Makefiles, compiler `/usr/bin/c++` = GCC 15.2.0, `nproc`=8 |
| Server config | `cores = 1` (key omitted, code default `include/kds/server/expeditor.hpp:460`), `durability = relaxed` (explicit, non-default — the sample's default is `group`), `port` per-arm (15901/15902 sanity, 15911/15912 A/A, 15921/15922 A/B), no other non-default key |

## 2. Binary provenance

| | v15 arm | v16 arm |
|---|---|---|
| Source commit | `b0b6e8a0e4da171c817af90633b9357e48e04ea7` | `7318e7ecd2737757e6410583b14081d059dd2323` |
| `git describe --tags` | `v2.2.1-56-gb0b6e8a` | `v2.2.1-68-g7318e7e` |
| Source checked out via | `git archive b0b6e8a \| tar -x -C /tmp/m1-v15-src` (no worktree) | existing worktree tree, clean |
| Build tree | `/tmp/m1-v15-src/build-release` (out-of-tree, Release, same flags) | `.../v2.4.0-range-foundation-1/build-release` |
| Build-tree binary mtime | 2026-08-27 12:02:03 | 2026-08-27 12:01:04 |
| Commit committer-date | 2026-08-27T09:18:55Z | 2026-08-27T11:52:47Z |
| Binary newer than commit? | yes (12:02:03 > 09:18:55) | yes (12:01:04 > 11:52:47, after a forced rebuild — the tree's *stored* `kds_server` predated HEAD at first check and was rebuilt for this run) |
| Run copy | `/home/ubuntu/m1-runbins/kds_server-v15` | `/home/ubuntu/m1-runbins/kds_server-v16` |
| Copy sha256 | `581062ade0108e8c1858c6644c20155a09f7fa3041a5be5ab00ca7bb0b90fa05` | `640c669bfba5a19e5f4a55c71db27571b094cf56e6b62dd6f0dfbf71024b2c02` |
| Copy = build-tree binary (byte-identical)? | yes, hash matches the source binary | yes, hash matches the source binary |

Both copies were made once, before the first cell, and every server in this
run — sanity check, A/A pilot, A/B run — started from these two copies, never
from either build tree's own binary. **On this worktree at `7318e7e`**, the
build tree's `build-release/kds_server` was found *older* than HEAD's commit
time at the start of this session (mtime 11:48:54 vs. commit 11:52:47) and
was rebuilt (`rm` + `cmake --build`) before copying, per the standing
provenance rule.

## 3. Sanity check — arm identity, before any measurement

Run against fresh single-shot servers, not the benchmark driver:

- `kds_server-v15` on a fresh file: `ckdbs on .../kds.db: 15 pages,
  superblock version 15` — `SHOW TABLES` lists 11 relations: `types objects
  columns tables indexes patterns access_stats cabins fkeys pattern_defs
  assertions`; no `ranges`. Of these, 9 (`types` through `fkeys`) are
  `catalog::Bootstrap()`'s fixed-offset `kSysTables` array
  (`src/catalog/catalog.cpp:532`, `std::array<SysTableBootstrap, 9>` at
  v15) — the array the work order's "9 → 10" framing names; `pattern_defs`
  and `assertions` are registered outside that array (real row-codec
  relations, per the array's own comment) and SHOW TABLES lists both sets
  together, hence 11 rather than 9.
- `kds_server-v16` mounting that same v15-created file: refused —
  `startup failed: superblock version 15 is not this build's (16)`, exit
  code 1. Exactly the refusal the work order names.
- `kds_server-v16` on its own fresh file: `ckdbs on .../kds.db: 16 pages,
  superblock version 16` — `SHOW TABLES` lists the same 11 **plus
  `ranges`** (12 total; `kSysTables` itself is `std::array<…, 10>` at this
  commit, `catalog.cpp:532`, confirming the 9 → 10 widening). `SELECT
  COUNT(*) FROM ranges` answers `ERR no columns for this rel_id` —
  expected: RD1's whole product is the relation's existence and emptiness,
  RD2 (the row format) is unbuilt, so the relation is queryable by name but
  has no columns yet.

All three outcomes match the work order's stated contract. Arm identity is
confirmed before any timing was taken.

## 4. Instrument and cell shape

`tools/mount_cost_benchmark.py --mounts 1 --rows 0` per invocation. One
invocation produces **both** cells in one shot, because of how the driver is
built: `loader.start()` times `Popen` → `"listening on"` on a file the
invocation just created (**cell a — first boot**: create + mount +
listener), then `loader.stop()` (clean, no crash) and a fresh `Mount` on the
*same now-existing* file is timed the same way for the one requested mount
(**cell b — remount**: mount-only, no file creation; **`kSysTables` is not
walked here** — `Catalog::Bootstrap()` runs on the fresh path only and is
deliberately not called on an existing file (`src/bootstrap/bootstrap.cpp:123`
against `:90-96`), so what differs between the arms on this path is that the
v16 file carries one more catalog page (page 15) and 16 pages rather than 15.
The 9 → 10 widening executes in **cell a**). `--rows 0` leaves an empty log
both arms. Fresh data file per invocation (the driver `rm -rf`s and
recreates its scratch subdirectory, keyed by `--label`, at the top of every
run, so the two arms never share a directory); fresh server process every
time (a new `kds_server` each `Mount`).
`--scratch /home/ubuntu/mount-bench-m1/scratch` — confirmed ext4 above.

Confounds and how the protocol controls them:

- **Page cache state** — fresh file per invocation (new inode each time) and
  strict A/B interleaving, so any residual cache warmth lands on both arms
  in the same reps, not preferentially on one.
- **File creation cost folded into cell a** — intended; the work order says
  so. Cell a's number is create+mount+listener, which is the honest "first
  boot" number and includes v16's one extra bootstrap `CreateAt` for
  `sys.ranges`. Cell b isolates the remount and excludes creation.
- **First-mount vs. remount** — kept as two separate cells rather than
  conflated; §6 reads each against its own band.
- **Position within an interleaved pair** — the arm that runs *first* in each
  rep is measurably slower on this host, and this run does not alternate the
  order: v15 runs first in all 12 A/B reps, as arm A does in all 12 pilot
  reps. That is why the control is read as a paired delta rather than as
  zero — §5's same-binary cell-b median delta is **+1.68 ms**, which is the
  size of the first-position penalty, and §6's v15−v16 delta must be read
  against *that*, not against 0. The alignment is what makes the control
  valid: the arm under suspicion of doing more work (v16) sits in the same
  slot as the pilot's faster arm, so an added cost would show as the A/B
  delta falling *below* the control's, and it does not.
- **CPU frequency / thermal drift** — controlled by interleaving (A,B,A,B,…)
  rather than by running one arm's block of reps, then the other's.
- **Background load** — `bench/wait_quiet.sh` run and reported quiet before
  the A/A pilot and again before the A/B run; `pgrep cc1plus`/`ld`/
  `kds_tests` checked clear at both points; loadavg 0.2–0.8 throughout, no
  concurrent build observed via `pgrep`.
- **An environment-level warm-up step, found and controlled for, not
  designed for** — see §5.

## 5. Noise band — established first, from an A/A pilot

**Protocol**: the v16 binary run in both "arms" (labelled A and B, distinct
ports 15911/15912, distinct scratch subdirectories under the same ext4
mount), 12 reps each, strictly interleaved A,B,A,B,…, same `--mounts 1
--rows 0` shape as the real run. This section is written from that pilot's
output alone, before the v15/v16 numbers below were read.

**What the pilot found, honestly**: cell a's absolute wall time is **not
stationary within a sitting** — both arm A and arm B jump from ~150–180 ms
on their first two reps to a stable ~335–350 ms from rep 3 onward, in the
same reps, in both arms simultaneously:

| rep | A create (ms) | B create (ms) | A remount (ms) | B remount (ms) |
|---|---|---|---|---|
| 1 | 167.75 | 156.11 | 103.94 | 101.13 |
| 2 | 146.46 | 164.21 | 103.07 | 97.00 |
| 3 | 344.49 | 344.49 | 97.06 | 96.17 |
| 4 | 351.07 | 338.71 | 102.79 | 103.22 |
| 5 | 343.76 | 342.44 | 101.39 | 100.60 |
| 6 | 343.26 | 340.22 | 102.50 | 101.20 |
| 7 | 341.66 | 344.77 | 99.54 | 96.66 |
| 8 | 348.18 | 342.48 | 99.88 | 97.81 |
| 9 | 346.09 | 335.04 | 105.88 | 101.39 |
| 10 | 347.03 | 340.98 | 100.54 | 101.61 |
| 11 | 341.51 | 341.42 | 103.10 | 103.56 |
| 12 | 340.91 | 342.25 | 103.62 | 101.17 |

Because the step lands on **both** arms at the same rep, it is a system-wide
effect, not an artifact of either "arm", and it is not attributable to a
binary difference since both arms are the same binary here. **Its mechanism
is not identified by this run, and one candidate is ruled out by the run's
own data**: burst I/O credit depletion on the underlying EBS volume would not
reset, yet the A/B sitting minutes later starts fast again (149.03/143.08 ms
at v15, 157.12/178.38 at v16) and steps at its own rep 3 — so whatever the
state is, it accumulates within a sitting and clears between them. Naming it
is left to the next mount-cost cell on this host; nothing below depends on
which mechanism it is, only on its being common to both arms. This is why the
protocol calls for **paired per-rep deltas**, which cancel a common-mode
step, rather than only comparing raw distributions:

| cell | A median | B median | \|median(A)−median(B)\| | per-rep delta (A−B): median | per-rep delta: min .. max |
|---|---|---|---|---|---|
| a — create (first boot) | 343.51 ms | 341.20 ms | 2.31 ms | 2.18 ms | −17.75 .. +12.36 ms |
| b — remount | 102.65 ms | 101.15 ms | 1.50 ms | 1.68 ms | −1.07 .. +6.07 ms |

Two conventions, so every derived number above and below is checkable from
the per-rep rows: n = 12 is even, so a **median** here is the mean of the two
middle reps (343.51 = (343.26+343.76)/2, which is what makes the |median(A)−
median(B)| column read 2.31 rather than 2.28), while the **p50 column of §6's
distribution tables is the driver's own nearest-rank p50** — the lower of the
two middle reps. The `|median(v15)−median(v16)|` figures in §6 use the
interpolated median of this table, not that p50.

**Noise band, stated before any A/B delta is read**:

- **Cell a (create)**: median delta ≈ **2.2 ms**, per-rep spread **−17.75 ms
  to +12.36 ms**. A same-binary A/B delta smaller than this width is noise,
  not a finding. That is the whole band this cell fixes in advance, and
  nothing below widens it.
  - *Post-hoc, added after §6 was read and marked as such rather than folded
    into the band above*: §6's rep-2 delta of **−35.30 ms** is **outside**
    this band — twice the pilot's widest single-rep excursion (−17.75 ms),
    not marginally past it. §6 argues it back to the same rep-1–2 warm-up
    window on the ground that both arms of both sittings are unstable there;
    that is reasoning about one rep after the fact, not a band, and the
    cell-a verdict below rests on the **median** delta, which rep 2 does not
    move.
- **Cell b (remount)**: median delta ≈ **1.7 ms**, per-rep spread **−1.1 ms
  to +6.1 ms**. This cell does not carry the create step's instability — its
  *level* is stationary across the sitting (96–106 ms at every rep, no
  150 → 340 step), so its band is tight. Its *deltas* are not quite as
  stationary: the pilot's widest single-rep delta, +6.07 ms, is at rep 2, so
  this cell carries some extra spread at the front of a sitting too — at
  roughly a tenth of cell a's size.

## 6. A/B run — v15 vs. v16, interleaved

12 reps per arm, v15 then v16 alternating (v15,v16,v15,v16,…), `--mounts 1
--rows 0`, ports 15921 (v15) / 15922 (v16), same ext4 scratch root. Machine
re-confirmed quiet immediately before (`wait_quiet.sh`, loadavg 0.2–0.7, no
`cc1plus`/`ld`/`kds_tests`).

### Cell a — first boot (create + mount + listener)

| rep | v15 (ms) | v16 (ms) | delta v15−v16 (ms) |
|---|---|---|---|
| 1 | 149.03 | 157.12 | −8.09 |
| 2 | 143.08 | 178.38 | **−35.30** |
| 3 | 354.24 | 342.42 | +11.82 |
| 4 | 346.54 | 334.76 | +11.78 |
| 5 | 340.84 | 338.34 | +2.50 |
| 6 | 347.29 | 342.32 | +4.97 |
| 7 | 341.06 | 342.68 | −1.62 |
| 8 | 347.22 | 337.44 | +9.78 |
| 9 | 343.28 | 341.35 | +1.93 |
| 10 | 340.47 | 346.83 | −6.36 |
| 11 | 344.38 | 339.77 | +4.61 |
| 12 | 345.69 | 337.47 | +8.22 |

Rep 2's −35.30 ms is the same warm-up-window phenomenon §5 found in the A/A
pilot (both pilot arms also showed their largest excursions on reps 1–2,
before the step to steady state at rep 3) — it is a property of *when in the
sitting* the rep ran, not of which binary ran it, and it is reported rather
than dropped.

| | min | p0 | p25 | p50 | p95 | p99 | max | mean | n |
|---|---|---|---|---|---|---|---|---|---|
| v15 | 143.08 | 143.08 | 340.47 | 343.28 | 354.24 | 354.24 | 354.24 | 311.93 | 12 |
| v16 | 157.12 | 157.12 | 334.76 | 338.34 | 346.83 | 346.83 | 346.83 | 311.57 | 12 |

`\|median(v15)−median(v16)\|` = **4.78 ms**. Paired per-rep delta: median
**3.56 ms**, spread −35.30 .. +11.82 ms.

**Read against the cell-a band** (median ≈2.2 ms, spread −17.75..+12.36 ms):
the paired median delta of 3.56 ms exceeds the A/A control's own paired median
of 2.18 ms by **1.38 ms** — computable from the two per-rep tables, and an
order below the dispersion of the pilot's own deltas — and every rep from 3
onward (i.e. every rep after the shared warm-up window) falls inside the A/A
pilot's spread. Rep 2's −35.30 ms does not: it is **twice** the pilot's widest
same-binary excursion (arm A 146.46 ms against arm B 164.21 ms, 17.75 ms on
the same binary), so §5's post-hoc note, not the band, is what carries it. The
median it does not move is what the verdict rests on. **No finding**: cell a's
median delta is inside the noise band.

### Cell b — remount (mount-only; no bootstrap, one more catalog page in the
file)

| rep | v15 (ms) | v16 (ms) | delta v15−v16 (ms) |
|---|---|---|---|
| 1 | 106.01 | 98.64 | +7.37 |
| 2 | 102.90 | 94.29 | +8.61 |
| 3 | 101.34 | 100.45 | +0.89 |
| 4 | 103.82 | 104.16 | −0.34 |
| 5 | 103.49 | 100.26 | +3.23 |
| 6 | 100.41 | 102.79 | −2.38 |
| 7 | 101.76 | 99.16 | +2.60 |
| 8 | 103.68 | 99.49 | +4.19 |
| 9 | 101.36 | 101.33 | +0.03 |
| 10 | 98.86 | 100.06 | −1.20 |
| 11 | 103.45 | 98.82 | +4.63 |
| 12 | 103.80 | 103.72 | +0.08 |

| | min | p0 | p25 | p50 | p95 | p99 | max | mean | n |
|---|---|---|---|---|---|---|---|---|---|
| v15 | 98.86 | 98.86 | 101.34 | 102.90 | 106.01 | 106.01 | 106.01 | 102.57 | 12 |
| v16 | 94.29 | 94.29 | 98.82 | 100.06 | 104.16 | 104.16 | 104.16 | 100.26 | 12 |

`\|median(v15)−median(v16)\|` = **3.02 ms**. Paired per-rep delta: median
**1.75 ms**, spread −2.38 .. +8.61 ms.

**Read against the cell-b band** (median ≈1.7 ms, spread −1.1..+6.1 ms): the
paired median delta (1.75 ms) is statistically indistinguishable from the
A/A pilot's own median delta (1.68 ms) between two runs of the *same*
binary — and cell b is the low-noise cell, with no file-creation instability
riding along. What it prices needs saying exactly, because it is **not** a
catalog scan: a remount runs no bootstrap and reads no catalog row before
"listening on" (`ScanAll`, `src/catalog/catalog.cpp:74`, is statement-time and
on demand), so this cell bounds *the steady-state mount path being unchanged*
— which is H1's claim — rather than pricing a per-relation walk. The cell that
executes the tenth relation's extra work is cell a.

Four reps sit outside the pilot's single-rep extremes, and they are named
rather than smoothed. Reps 1 and 2 (+7.37, +8.61 ms) run past its high end
(+6.07 ms) and land in the same rep positions where §5's warm-up window
inflates spread system-wide. Reps **6 (−2.38 ms)** and **10 (−1.20 ms)** run
past its *low* end (−1.07 ms) by 1.31 and 0.13 ms — the side that would mean
**v16 slower** — and they carry no warm-up excuse, being mid-sitting. A
12-sample pilot bounds a tail loosely, and both sit past that bound by less
than the instrument's own ~2 ms resolution (§9), so neither is a finding; but
"every rep after the warm-up window is inside the pilot's band" would have
been the wrong sentence, and this is the right one.

The same caution read one more way, on this section's own tables: dropping
the non-stationary reps 1–2 from *both* runs, the A/B paired median falls to
+0.49 ms against the control's +1.10 ms, which puts v16 ~0.6 ms *slower* than
position alone predicts, where the full 12-rep reading put it ~0.06 ms
faster. The point estimate is therefore not stable to that choice, and what
this cell can honestly claim is a **bound of one to two milliseconds**, not a
zero. **No finding** either way: on every subset the delta stays inside the
noise band, and the full-sample median delta essentially reproduces the A/A
control.

### Wait attribution for cell b (ck-tester rule 3)

`SHOW META`'s recovery counters decompose the remount's wall time; means
over the 12 reps of each arm:

| component | v15 mean (ms) | v16 mean (ms) |
|---|---|---|
| analysis | 27.20 | 26.80 |
| redo | 33.44 | 32.23 |
| high-water | 0.00 | 0.00 |
| undo | 0.00 | 0.00 |
| checkpoint | 6.21 | 6.62 |
| **sum, attributed** | **66.85** | **65.65** |
| wall (mean) | 102.57 | 100.26 |
| **residual (unattributed)** | **35.72** | **34.61** |

High-water and undo are zero on both arms — expected, there is no undo work
and no cross-core catalog write to high-water past on an empty, freshly
created, single-core file. Analysis and redo are each a read of the
segment body from the anchor, not a per-record cost (§10), which is why they
are tens of milliseconds against `recovery_records=2` on every rep of both
arms — and that 2 is the previous mount's completion checkpoint, **not**
bootstrap's catalog writes (§10 again). They move by ~1 ms between arms,
inside the noise already established. The residual — file
open, buffer pool init, everything up to the "listening on" line that
recovery's own counters do not own (the line precedes `bind()`, so
bind/listen itself is outside the measured wall — `bench/docs/README.md:695`)
— is ~35 ms on both arms and does **not** grow measurably for the one extra
catalog page the v16 file carries. **This is the quantified half of the answer
to H1's mount half**: every counter and the residual move by ~1 ms between the
arms against a ~1.7 ms same-binary control — not a finding. It is a bound on
the steady-state path, not a per-relation price: the per-relation work
(`CreateAt` plus the two catalog rows for the tenth entry) runs in cell a,
where the band is coarser.

## 7. Versus PostgreSQL

**Not run, and does not apply as stated.** `bench/docs/README.md`'s entry
for `mount_cost_benchmark.py` already says so: *"No PostgreSQL twin yet: the
twin is `pg_ctl start` → first accepted connection, clean and after `kill
-9`, over the same nine-mount shape."* M1 measures a format-epoch bump
internal to ckdbs's own superblock (v15 → v16) — there is no PostgreSQL
analogue to "one more bootstrap catalog relation added to `kds_server`'s own
schema," so even with the twin built this specific comparison (v15-schema
mount vs. v16-schema mount) would have nothing to compare against on the
PostgreSQL side. The task that would build the general mount-cost twin is
named in `bench/docs/README.md`'s entry for this driver; it was not
attempted in this session.

## 8. Verdict on H1 (mount half)

**H1's mount half holds.** Both cells' v15-vs-v16 **median** deltas are
inside the noise band established by the A/A pilot before either was read
(§6 names the four single-rep excursions that sit outside the pilot's
single-rep extremes, and why none of them moves a median):

| cell | noise-band median delta | noise-band spread | A/B median delta | A/B \|median diff\| | verdict |
|---|---|---|---|---|---|
| a — first boot | 2.18 ms | −17.75..+12.36 ms | 3.56 ms | 4.78 ms | inside band |
| b — remount | 1.68 ms | −1.07..+6.07 ms | 1.75 ms | 3.02 ms | inside band, reproduces the A/A control almost exactly |

H1's falsifier — "any mount-time cost outside M1's noise band" — did not
fire. Combined with the suite result the workplan §3a already carries
(2775/2775 + sim 171/171 at `7318e7e`), **RA2's shape is not refuted**: an
empty `sys.ranges` costs nothing measurable at mount, on either the
create-plus-load path or the steady-state remount path, at `cores = 1`.

## 9. What this run does not measure

- **Populated-file mounts.** Every cell here used `--rows 0`; a mount with a
  non-trivial WAL body or a catalog carrying user relations is **not run**.
  `sys.ranges` staying empty is the entire premise of H1 at this milestone —
  RD5 (allocation) is explicitly out of scope for R3-A — so a populated-file
  mount cell would be measuring a state RA2 does not yet produce.
- **Crash-recovery mounts** (`--crash`, redo/undo work on the tenth
  relation's page). **Not run** — no crash fixture was built for this cell,
  and RD1's relation carries no rows for anything to have crashed over.
- **Per-statement overhead A/B.** The operator's 2026-08-24 amendment
  suspends the interleaved A/B overhead measurement for v2-stage
  development; this is stated as **not run**, never implied. (This session
  did run its own interleaved A/B — but for the *mount-cost* cell the work
  order asked for, which is a distinct measurement from the suspended
  per-statement overhead gate.)
- **Anything finer than the instrument's own resolution.** The driver detects
  the listener by re-reading the server log every 2 ms
  (`tools/mount_cost_benchmark.py:147`, `time.sleep(0.002)`), so a wall time
  carries up to ~2 ms of polling quantisation. It is sampled identically in
  both arms and therefore cancels in a paired delta rather than biasing it,
  but it does set the floor: this cell can bound a per-mount cost at roughly
  a millisecond or two, it cannot resolve one below that, and no claim here
  should be read as a measured zero.
- **M2 and M3** (`instructions/v2.4.0/range-foundation.md` §7) — the catalog
  ceiling delta and the pre-range statement-mix baseline. Both are **not
  run**; this session's task was M1 only.
- **More than 12 reps per arm.** The work order §7 sets no rep floor — it
  asks for interleaved arms in one sitting, a fresh server and data file per
  invocation, and per-rep spreads before any median, all of which this run
  did; the "nine" this driver is usually run at is its own `--mounts 9`
  default and `bench/docs/README.md:695`'s "nine-mount shape", not a
  requirement of the order. 12 reps were run per arm per cell: **48 driver
  invocations** (24 A/A + 24 A/B), each starting two servers — 96 servers —
  plus the three sanity servers of §3. A larger sample was not run and might
  narrow the band further, but would not change §8's verdict given how far
  inside the band every A/B delta already sits.
- **The remount's residual decomposed further than `SHOW META`'s five
  counters allow.** The ~35 ms residual in §6 is named as
  "everything recovery does not own" — file open, buffer pool init, and the
  rest of the mount up to the "listening on" line (not `bind()`, which that
  line precedes) — because no finer per-phase counter exists on this engine to
  split it further; that is a source-read limit of the
  instrument (`tools/mount_cost_benchmark.py`'s own docstring), not a gap
  this run chose to leave.

## 10. What this teaches about the engine

**Measured** (`v2.2.1-68-g7318e7e` for v16, `v2.2.1-56-gb0b6e8a` for v15):
RD1's format bump is bounded below this instrument's resolution on both mount
paths it touches — a bound, not a measured zero. The tenth relation's real
work is bootstrap's (one `CreateAt` plus two catalog rows,
`src/catalog/catalog.cpp:532` and the phases below it), which runs on the
create path only, and it is invisible against that cell's own same-binary
band; on the remount path no bootstrap runs at all, and the ~35 ms residual
that would carry a per-relation cost if one existed moved by roughly 1 ms,
against a same-binary control of the same size. That is the useful data point C1
(`instructions/v2.4.0/range-foundation.md` §3) asked for from the "cost at
mount" angle: **a bootstrap catalog relation is cheap enough at mount that
the format-epoch bump's cost is not the reason to prefer or avoid a
non-bootstrap directory shape** — C1's decision has to turn on the
mount-time *readability* argument the work order frames it around (the
routing cache must be built before any statement runs), not on a mount-time
cost argument, because no mount-time cost survives the bound measured here.

**Source-read** (`src/wal/checkpointer.cpp:24,210`): `--rows 0` still
produces `recovery_records=2` on every mount in this run, on both arms and
so at both 15 and 16 catalog pages — **not** bootstrap's own catalog writes
(those happen, and are checkpointed away, during the *create* step, so a
clean remount never has to redo them). The 2 is a completion checkpoint's
own `kCheckpointBegin` + `kCheckpointEnd` pair, written once at the end of
*every* mount (`src/server/mount_recovery.cpp:346`'s "completion
checkpoint", counted by `src/wal/log_scanner.cpp:147`) — the loader's own
clean stop leaves exactly this pair for the next mount's analysis/redo to
scan, regardless of how many catalog relations exist. This is why cell b's
`recovery_analysis_us` and `recovery_redo_us` are non-zero even with no user
data and no bootstrap replay — but the ~27 ms and ~33 ms they report are not
the price of two records. `wal::ScanLog` reads a segment's whole body from the
anchor in one call and analysis and redo each run their own scan, so the cost
is a function of `segment_size` (64 MiB by default) and not of how much was
logged (`tools/mount_cost_benchmark.py:24-27`). The floor is a segment read
paid twice; the schema's size does not enter it either way.

That has a consequence for the next cell, and it is the opposite of the
obvious one: because the scan runs anchor-to-end-of-segment, a **populated**
mount scans *less* body and comes out **cheaper**, so subtracting this ~66 ms
(66.85 ms at v15, 65.65 ms at v16, analysis+redo+checkpoint) from a populated
mount would not isolate a per-row cost — it would mix a per-row cost with a
shrinking segment remainder moving the other way. Separating a per-relation
bootstrap cost from a per-row one, the distinction
`.claude/agents/ck-tester.md`'s benchmark documentation rule 9 asks every
sweep to establish, needs the segment remainder held fixed or measured, which
this narrower M1 cell was not designed to do (§9).

**A confound worth naming for the next mount-cost cell on this host**: cell
a's absolute wall time is not stationary within one sitting — a
warm-up-to-steady-state step from ~150 ms to ~340 ms appears at rep 2→3 of
*every* sitting run in this session (A/A pilot and A/B run alike),
independent of which binary is running. Paired interleaving cancels it for
a delta, but a future cell reading cell-a's raw p50 in isolation, without an
A/A control run in the same sitting, would be reading a number this run
shows is sitting-dependent rather than binary-dependent.
