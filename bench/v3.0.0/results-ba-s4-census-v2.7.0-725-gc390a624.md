# BA-S4 census (v2.7.0-725-gc390a624)

BA's census (`instructions/v3.0.0/workorder-ba-parallelism.md` BA-R0, BA-S4):
where one core waits for another on this engine, and what adding cores buys
against PostgreSQL 18.6 on the same host. It decides which of BA's fix
stages open (BA-Q1).

- **Engine:** `c390a624` (`v2.7.0-725-gc390a624`), Release, built at that
  commit. Every server ran from one copy of the binary, sha256
  `ed5927dc9b4e3702e2e4c758511b386ab896ba004559e3060fee2f8f3e1326b6`.
- **PostgreSQL 18.6.** BA's own cluster at `~/pg-bench-ba`, port 15434,
  made by `tools/pg_setup.sh init` with every tuning knob at its default
  (`fsync = on`). The older `~/pg-bench` cluster does not start (its
  control file reads as corrupt) and was not used.
- **Driver:** `tools/ba_census.py` (BA-S3), unmodified during the run. One
  client process per session, clients pinned to CPUs 4-7. KDS reactors
  were pinned by `reactor_cpus` (`0`, `0,2`, `0,2,4,6`). PostgreSQL's
  postmaster and its children were pinned to the same three sets. KDS
  listened on port 15641, chosen; PostgreSQL on 15434.
- **Matrix:**
  - KDS: 5 shapes x 3 durability classes x `cores` {1, 2, 4} x sessions {1,
    2, 4, 8, 16}, 3 runs each. 675 runs.
  - PostgreSQL: 5 shapes x `synchronous_commit` {off, on} x the three CPU
    sets x the same sessions, 3 runs each. 450 runs.
  - 8 s measured after 2 s of warm-up per run.
  - 0 failed cells, and 0 errors in any run.
- **Raw data** in `archive/ba-s4-census-v2.7.0-725-gc390a624/`:
  - one JSON per run;
  - `census.sh`, the loop that ran them;
  - `analyze.py` and its output `analysis.md`: every cell's median
    statements/s, p50, p99, refusals and client-bound runs, each item's
    share over all cells and over the clean cells, and the P11 proxy;
  - `comparison.md`, the KDS-against-PostgreSQL tables below.

## The host, and what bounds every number here

- 8 logical CPUs on 4 physical cores, with SMT siblings 0/1, 2/3, 4/5 and
  6/7. Data on `/dev/root`, ext4.
- **`cores = 2` is the largest clean cell** (BA-Q2). Its reactors are on
  CPUs 0 and 2, on two physical cores, and the clients are on 4-7.
- **`cores = 4` is indicative only.** Its reactors are on 0, 2, 4 and 6, so
  two of them share physical cores with the clients. Every KDS `relaxed`
  row falls from `cores = 2` to `cores = 4` at 16 sessions, and those falls
  are not read as the engine's. PostgreSQL on the same CPUs 0,2,4,6 rises
  by about 1.5x over CPUs 0,2, so the sharing alone does not explain
  KDS's fall. That is unresolved here.
- **Load.** The 1-min load before a run had a median of 2.57 and a maximum
  of 6.00 over all 1,125 runs (KDS runs alone: 2.05 and 5.99). The driver
  waits for it to fall to 6 or below, for at most 300 s; no run waited
  longer than 95 s.
  - No build or `ctest` was seen before or after any run. The driver
    checks at a run's two ends, not during it.
  - `pgrep` matched another session's long-lived wait loop, whose command
    line contains `cmake --build`, in every run, and in two runs that
    loop's own `pgrep` as well. `analyze.py` counts a process as a build
    only when its own program is `cc1plus`, `cmake` or `ctest`.
  - The census's first 54 runs (18 cells, through `point` `group`
    `cores = 1` 4 sessions) ran under a 1.5 load bound. The restart under
    the 6 bound skipped complete cells, so those runs were kept, not
    re-run; the log records the restart.
- **The client-bound mark is recomputed** in `analyze.py`. A run is
  client-bound when any one client CPU was over 80 % busy with the
  clients pinned to it (client i on CPU 4 + i mod 4). The driver tested
  each client alone, which 16 sessions at about 25 % each never meet. A
  cell is marked when any of its runs is. Marked:
  - in the clean cells, `point` at 8 and 16 sessions at `cores = 2`, in
    every durability class;
  - at `cores = 4`, `point` at 8 and 16 sessions in every class, and
    `recent` `relaxed` at 16 sessions;
  - for PostgreSQL, eight cells, all on CPUs 0,2,4,6: `point` at 8 and 16
    sessions (`off` and `on`), `insertN` `off` at 8 and 16, and `insert1`
    and `recent` `off` at 16.

  Those rows are floors, not ceilings.

## Which items are material (BA-R0, BA-Q1)

An item's share is its latches' contended wait, summed over every core,
over `cores` times the measured span. The WAL writer thread, which has no
core, counts into core 0's slot, so its waits are in the sum too: a share
is of `cores` x span, and is reactor time only where no writer waits are
in it. It is material at 5 % in any cell, or on any refusal. P12 is
material when a carve or a checkpoint ran over 10 ms. Shares are quoted
from the clean cells (`cores` <= 2). Every verdict below is the same over
all cells, `cores = 4` included.

| item | largest share (clean cells) | where | material | fix stage |
|---|---|---|---|---|
| P1 frame table | 0.67 % | recent, relaxed, `cores = 2`, 16 sessions | no | BA-S8 not opened |
| P2 visibility window | 0.26 % | insertN, relaxed, `cores = 2`, 16 sessions | no | BA-S9 not opened |
| P3 statistics | 0.00 % | - | no | BA-S5 not opened |
| P4 relation borrow | 0.06 % | insert1, relaxed, `cores = 2`, 4 sessions | no | BA-S10 not opened |
| P5 pk mark | not measurable (below) | - | not shown | BA-S11 not opened |
| P6 WAL append | **10.01 %** | insertN, relaxed, `cores = 2`, 16 sessions | **yes** | **BA-S12** |
| P7 commit durability | **55.35 %** (sync gate); **39.33 %** (snapshot ceiling, defect B) | sync gate: recent, group, `cores = 2`, 8 sessions; ceiling: recent, strict, `cores = 2`, 4 sessions | **yes** | **BA-S7** |
| P8 assertion directory | 0.00 % | - | no | BA-S6 not opened |
| P9 rightmost leaf | **20 refusals** | trade, insert1, recent at `cores` >= 2 | **yes** | **BA-S14** |
| P10 no yield | not measured | - | exempt (BA-Q1) | **BA-S15** |
| P11 session placement | not tested (below) | - | not shown | BA-S16 not opened |
| P12 periodic stalls | **carve 2,242 ms, checkpoint 3,094 ms** at the longest | over 10 ms in 340 and 596 of 675 runs, over each server's life | **yes** | **BA-S13** |

- **P5 cannot be read from this census.** Page 7 is a page latch, and the
  page-latch counters do not say which page. `insert1` (one relation) and
  `insertN` (one per client) both pay page 7 and differ in the rightmost
  leaf, so their gap prices the leaf, not page 7. At `relaxed`,
  `cores = 2`, 16 sessions, they run at 68,341 and 64,647 statements/s,
  so `insert1` is 5.7 % ahead and their runs do not overlap. At `group`
  `insert1` runs 18 % below `insertN` (2,922 against 3,558 statements/s). Material is not shown, so under BA-Q1 BA-S11 does not open
  on the census. Immaterial is not shown either.
- **P11 is not tested.** BA-R0's test needs a balanced arm and the cell's
  spread, and the driver opens no balanced arm. The proxy is the kernel's
  own placement varying run to run. Within a cell, runs whose sessions
  landed on every core are set against the runs that did not
  (`analysis.md`). Each arm has one or two runs, so there is no spread to
  test against. Of 30 cells with both kinds of run, the every-core runs
  were faster by over 10 % in 8 and slower by over 10 % in 3. `group`
  cells are also confounded by P7, because a session that lands on core 0
  pays core 0's inline sync. Material is not shown, so BA-S16 does not
  open. BA-S17's re-run, after BA-S7 removes the confound, reads it again.
- **P7's two shares.**
  - The sync gate's waits include any the writer thread took (above).
  - The snapshot ceiling is BA-S1c's `CeilingWait`. One of its two sites
    parks the statement's coroutine and the other spins the reactor, and
    the tally does not split them. Its share is a statement wait over
    `cores` x span, not wholly reactor time.

  P7 is material on either share, and on the throughput below.
- **P12's longest values are maxima over each server's life.** They
  include setup and warm-up. In 13 of the 340 runs whose carve exceeded
  10 ms, no carve ran in the measured span at all. The verdict does not
  depend on when a stall ran.

## What adding a core buys

KDS against PostgreSQL, statements/s at 16 sessions, with 1 session in
parentheses. Full tables: `archive/.../comparison.md` and `analysis.md`.

**Unsynced: KDS `relaxed` against PostgreSQL `synchronous_commit = off`.**

| shape | KDS `cores = 1` | KDS `cores = 2` | PostgreSQL CPU 0 | PostgreSQL CPUs 0,2 |
|---|---|---|---|---|
| point | 43,409 stmt/s (14,521) | 65,347 stmt/s, client-bound (14,262) | 26,960 stmt/s (16,974) | 54,433 stmt/s (16,929) |
| trade | 37,118 stmt/s (16,436) | 60,113 stmt/s (16,110) | 18,540 stmt/s (13,812) | 37,593 stmt/s (13,823) |
| insert1 | 38,166 stmt/s (17,799) | 68,341 stmt/s (17,693) | 27,186 stmt/s (17,332) | 54,628 stmt/s (17,296) |
| insertN | 37,721 stmt/s (17,955) | 64,647 stmt/s (17,582) | 26,933 stmt/s (17,299) | 54,282 stmt/s (17,431) |
| recent | 34,206 stmt/s (14,344) | 53,233 stmt/s (14,044) | 21,652 stmt/s (14,173) | 43,880 stmt/s (14,222) |

**Synced: KDS `group` and `strict` against PostgreSQL `on`.**

| shape | KDS `group`, `cores = 1` | KDS `group`, `cores = 2` | KDS `strict`, `cores = 2` | PostgreSQL `on`, CPUs 0,2 |
|---|---|---|---|---|
| trade | 4,977 stmt/s (676) | **1,656 stmt/s** (657) | 721 stmt/s (653) | 5,150 stmt/s (678) |
| insert1 | 4,927 stmt/s (668) | **2,922 stmt/s** (671) | 703 stmt/s (702) | 5,325 stmt/s (691) |
| insertN | 4,918 stmt/s (701) | 3,558 stmt/s (689) | 752 stmt/s (706) | 5,035 stmt/s (686) |
| recent | 6,033 stmt/s (1,274) | 6,325 stmt/s (1,253) | 1,472 stmt/s (1,248) | 9,942 stmt/s (1,238) |

- **Unsynced, KDS scales from one core to two** by 1.5-1.8x, where
  PostgreSQL scales by 2.0x on every shape. `point`'s 1.5x is a floor,
  because its `cores = 2` cell is client-bound.
- **Unsynced, KDS leads PostgreSQL at 16 sessions on every shape at the
  clean CPU sets. Below 16 sessions, and on four CPUs, it does not always
  lead.**
  - At one session PostgreSQL leads on `point` at every CPU set
    (16,974 against 14,521 statements/s at one CPU).
  - At `cores = 1` it leads at 2 sessions on `point`, `insert1`, `insertN`
    and `recent`.
  - At `cores = 2` it leads on the same four shapes at 2 and 4 sessions,
    and on `insert1` at 8 (`insert1` at 4 sessions: 47,159 against 61,344
    statements/s).
  - On CPUs 0,2,4,6 PostgreSQL leads on every shape at 16 sessions
    (`point` 55,404 against 80,327 statements/s). Both engines are
    indicative there, and several of PostgreSQL's cells are client-bound
    floors.
- **Synced, a second core makes KDS slower on the write shapes.** At 16
  sessions `group` trade falls from 4,977 to 1,656 statements/s, insert1
  from 4,927 to 2,922, and insertN from 4,918 to 3,558. `recent` rises
  from 6,033 to 6,325. PostgreSQL's synced curve does not fall: about
  5,000 on the write shapes and about 10,000 on `recent`, at every CPU set.
- **`strict` does not batch at all.** On the write shapes it stays at
  650-770 statements/s, whatever the session count, at `cores` 1 and 2:
  one sync per commit. `recent`, two statements per commit, stays at
  1,250-1,470. PostgreSQL's `on`, which is D1's durability point,
  group-commits by itself to 4,857-5,325 statements/s on the write shapes.

## BA-R4's premise: what the writer hand-off costs

`manager.cpp`'s `Sync()` keeps core 0's waited-on syncs inline, on the
argument that a hand-off to the writer "doubled `group`'s p99" on a 2-core
host. The census reads each session's latency by the core it landed on,
at `cores = 2`. Core 0 syncs inline; a peer hands its sync to the writer.
Each cell is the median over clients of each client's own p50 or p99,
over every run of the cell.

| shape | durability | core 0 p50 | core 0 p99 | peer p50 | peer p99 | clients on core 0 / peers | statements per client, core 0 / peers (median) |
|---|---|---|---|---|---|---|---|
| insert1 | group | 12,905.8 µs | 1,395,463.6 µs | 2,387.8 µs | 9,507.4 µs | 47 / 46 clients | 44 / 2,900 statements |
| insertN | group | 11,450.9 µs | 1,300,254.6 µs | 2,409.4 µs | 9,896.8 µs | 39 / 54 clients | 28 / 2,861 statements |
| trade | group | 42,666.6 µs | 1,638,382.4 µs | 2,532.7 µs | 9,900.9 µs | 49 / 44 clients | 16 / 2,764 statements |
| insert1 | strict | 17,891.3 µs | 36,927.3 µs | 17,749.8 µs | 35,902.9 µs | 41 / 52 clients | 412 / 411 statements |
| insertN | strict | 14,162.3 µs | 33,691.6 µs | 14,484.9 µs | 34,022.5 µs | 43 / 50 clients | 510 / 499 statements |
| trade | strict | 15,067.3 µs | 33,121.7 µs | 17,793.8 µs | 35,675.1 µs | 43 / 50 clients | 480 / 412 statements |

**The premise does not hold in this configuration, and the census cannot
say why.**
- **`group`.** A peer's session sees a p50 of about 2.4 ms and a p99 of
  about 10 ms. A session on core 0 sees a p50 of 11-43 ms and a p99 of
  1.3-1.6 s, and completes 16-44 statements in 8 s against a peer's
  2,800-2,900. With that few statements, a core-0 client's p99 is close
  to its maximum.
- **What the core-0 arm prices is not the inline sync alone.** Three
  things are in it, and the counters do not split them:
  - the inline `fdatasync`;
  - waits on the sync gate behind the writer's syncs for the peers. At
    `cores = 1`, where every sync is inline and no writer syncs, the gate
    shows 0.00 % and `group` matches PostgreSQL (4,977 against 4,902
    statements/s on `trade`);
  - every carve and checkpoint the reactor runs. `*_longest_us` is an
    instance-wide maximum, so which core ran the seconds-long ones is not
    recorded.
- **The arm the premise compares was not run.** The premise compares a
  hand-off from every core with inline syncs. The census compares two
  arms of one mixed configuration. What it shows is that core 0's
  sessions starve while a writer syncs for the peers. It does not show
  that a hand-off from core 0 would cost less.
- **`strict`.** The two arms cost about the same: 14-18 ms at the p50
  against 1.2-1.3 ms at one session. A peer's `strict` blocks its reactor
  on the writer's condition variable as surely as core 0's blocks on the
  device. Every session on a core then queues behind one sync at a time.

## Other counters (sums over every KDS run)

- **Syncs.** 991,774 inline and 1,334,172 by the writer.
- **Drain passes with a commit staged: 2,197,224,334.** All of them came
  at `group`. 317,636 were at `cores = 1`, about one per commit, and the
  rest at `cores` >= 2. That is §1.7's spin on a parked `group` commit
  (P7). The counter does not say which core took a pass.
- **Snapshot-ceiling waits (defect B, P7): 151,270 waits, for 306 s in
  all.** 306.0 s of that came at `strict` with `cores` >= 2, and none at
  `cores = 1`. P7's row above carries the share.
- **Page-latch spin turns: 98,466,594.** Not attributed to a page or to an
  item.
- **Refusals.** 20 at `btree_descend`, and none at the other six sites.
  They came from trade at `cores` 2 and 4, insert1 at `cores = 4`, and
  recent at `cores = 2`. All were retried and none reached a client as an
  error.

## Insight

The census found one problem large enough to explain most of the rest.
**On this engine a synced commit is answered by a reactor that waits for
it, whichever core it is on.** Core 0 waits inline on the device, and on
the sync gate behind the writer. A peer blocks on the writer under
`strict`, and under `group` the drain spins. `strict` also caps other
sessions' snapshots across its sync (defect B, up to 39 % of `cores` x
span). So adding a core adds a second reactor that mostly waits. The
synced write curve falls where PostgreSQL's stays flat, and `strict`
never shares a sync. The latch items the survey led with (P1-P4, P8) are
each below 1 % in every clean cell. P6, the WAL append latch, is the only
latch item over the line besides P7's sync gate, at 10 %. P5 and P11 were
not measurable with this census, so they are not shown material, which is
not the same as immaterial. The fix order this gives is BA-S7 first:
commits leave every reactor, and `strict` parks and shares the writer's
syncs. Then BA-S12 (the append), BA-S13 (the periodic stalls, up to
seconds long, on a core the counters do not name), BA-S14 (the refusals)
and BA-S15 (exempt from the census).
