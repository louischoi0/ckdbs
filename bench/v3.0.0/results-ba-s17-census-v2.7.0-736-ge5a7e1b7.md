# BA-S17 census re-run (`v2.7.0-736-ge5a7e1b7`)

**Thesis.** The census BA-S4 ran at `c390a624` (`v2.7.0-725-gc390a624`) found synced commits
answered by a reactor that waits, so that a second core made the synced write curve fall.
Re-run on the same 675-run KDS matrix at `e5a7e1b7` (`v2.7.0-736-ge5a7e1b7`), **a second core
no longer costs the synced write shapes anything and `strict` shares its syncs**: at 16
sessions and `cores = 2`, `group` reads 6,102-6,606 stmt/s on the write shapes (BA-S4:
1,656-3,558) and `strict` 6,214-6,383 stmt/s (BA-S4: 703-752), against a PostgreSQL floor of
5,035-5,325 stmt/s read a day earlier and re-read at +1.3 to +7.4 % today. The WAL append's
latch wait fell from 10.01 % to 0.38 % of `cores` x span, the sync gate from 55.35 % to
9.60 %, and no refusal reaches a client. **Two things BA carries are not met**: the carve's
longest stall at `relaxed` is a median 226-258 ms against BA-S13's 10 ms exit (BA-S4: 20-46 ms,
so it is worse), and the checkpoint half (BI-Q1) is unbuilt and its longest stall at `relaxed`
is also longer than BA-S4's. Cells BA did not change read +12 to +28 % over BA-S4 on the synced
write shapes at `cores = 1`, a shift the same-day A/B shows is not BA's and the PostgreSQL control
does not reproduce; **ratios against BA-S4 on synced cells carry it.**

- **B** = `e5a7e1b7`, `v2.7.0-736-ge5a7e1b7`; the binary is the same copy as the A/B file's B,
  sha256 `d2a6977f220a32147b6a2df985aec10b60ee7411fe3fca3381b78e46e525223e`. The worktree
  `ba-open-marks` branch `worktree-ba-open-marks` has since moved to `1a5176ae`
  (`v2.7.0-746-g1a5176ae`) with engine changes in 15 files (BJ-S1 to BJ-S3); **the engine
  measured is `e5a7e1b7`**.
- **Compared with** `results-ba-s4-census-v2.7.0-725-gc390a624.md` and its archive. The engine
  difference between the two is exactly BA-S7 (parts 1 and 2), S12, S13, S14 and S15; the
  driver `tools/ba_census.py` is unchanged between them (`git log c390a624..e5a7e1b7 -- tools`
  is empty), and `e5a7e1b7` itself merged documents only.
- This file was written from the records of a run that the operator stopped before it wrote its
  files; **no new measurement was made for it.**

## 1. Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-10. KDS first pass 05:01-07:53 UTC; contamination screen 07:53; re-run of the 32 flagged cells 07:53-08:19; same-day PostgreSQL control 08:49-08:54 |
| Branch / worktree | `worktree-ba-open-marks`, worktree `ba-open-marks` |
| Engine | `e5a7e1b7`, `v2.7.0-736-ge5a7e1b7`, `build-release` rebuilt at that commit (binary mtime 2026-10-10 02:09:03 UTC, after the commit's 02:06:37); tree recorded clean by the earlier run |
| Measured copy | one copy for all 675 runs, sha256 `d2a6977f...e5223e` (`binary.sha256`: recorded four times, one per `census.sh` start, all equal; `binary-measured.sha256` re-hashed at the time of writing) |
| Device | data files under `~/bench-runs/ba-s17/work`, `/dev/root` ext4 (`data_device` in every run JSON), not tmpfs |
| Host | 8 logical CPUs, AMD EPYC 9V74, 4 physical cores x 2 threads (siblings 0/1, 2/3, 4/5, 6/7) |
| Pinning | KDS reactors `reactor_cpus` `0` (`cores` 1), `0,2` (2), `0,2,4,6` (4); clients on CPUs 4,5,6,7 (client i on `4 + i mod 4`); the same as BA-S4 |
| Server config | `durability` relaxed / group / strict; `cores` 1, 2, 4; `buffer_pool_frames = 65536`; port 15641 (chosen) |
| Driver | `tools/ba_census.py`, unmodified: `--runs 3 --seconds 8 --warmup 2 --client-cpus 4,5,6,7 --max-load 6 --max-wait 300`. One client process per session |
| Suite | not executed |
| PostgreSQL | **not re-run.** The floor is BA-S4's (PostgreSQL 18.6, `~/pg-bench-ba`, port 15434, every tuning knob at default), read from its archive. Seven of its `on` cells were re-run after the KDS census as a control (section 5) |
| Row sweep (README rule 9) | **not applicable to this driver.** `point` reads a preloaded 10,000-row relation (`--rows 10000`); the other four shapes append to a relation that starts empty and grows by the run's statement count. The census has no row-count axis; the A/B file carries 200, 1,000 and 10,000 rows |

## 2. The matrix, and what was re-run

- **KDS: 5 shapes x 3 durabilities x `cores` {1, 2, 4} x sessions {1, 2, 4, 8, 16} x 3
  runs = 675 runs**, 8 s measured after 2 s of warm-up, a fresh server per run. The shapes
  (`ba_census.py`): `point` a pk read; `trade` scenario 0's two `INSERT`s and two pk `UPDATE`s,
  autocommit; `insert1` a monotonic-pk `INSERT` into one shared relation; `insertN` the same
  into one relation per client; `recent` an `INSERT` then a pk read of a row written up to 300
  ids earlier.
- **PostgreSQL was not re-run** (450 runs at BA-S4). Every PostgreSQL number here is BA-S4's.
  Its same-day control is 7 `on` cells x 3 runs, section 5.
- **`cores = 2` is the largest clean cell** (BA-Q2): its reactors are on two physical cores and
  the clients on the other two. `cores = 4` shares physical cores with the clients and is
  indicative only; the relaxed rows still fall from `cores = 2` to `cores = 4` at 16 sessions
  (64,908 to 53,816 stmt/s on `point`, 63,915 to 49,903 on `trade`, 72,063 to 60,538 on
  `insertN`), as at BA-S4, and this file does not explain them.
- **Client-bound cells** (any client over 80 % of one CPU; `analyze.py`): in the clean cells
  `point` at 8 and 16 sessions in every durability, `recent` `relaxed` at 8 and 16 sessions (3
  of 3 runs), and `trade` `relaxed` at 16 sessions (1 of 3). At BA-S4 only `point` was marked
  in the clean cells. **A faster engine reaches the clients' ceiling sooner**: these rows are
  floors, and the `recent` `relaxed` scaling below (1.71x) is a lower bound.

### How contaminated runs were found and handled

Other sessions ran `ctest -j8` (`kds_tests`) and builds on this host during the first hour of
the census, and an unattended upgrade and editor tools at other times. The driver's own gate
(`--max-load 6`) and `census.sh`'s gate (wait at most 30 minutes for no `ctest`, `cc1plus`
or `kds_tests`) do not see a process that starts mid-run. The rule that did:

- **`screen.py`**, run once after the first pass over the 1-second host sampler of the A/B
  file (`host-samples.jsonl`, per-CPU busy time and the busy foreign processes): a run's window
  is its JSON's mtime minus 30 s. **A run is contaminated** when a process named `kds_tests`,
  `ctest`, `cc1plus` or `cmake` is among the busy foreign processes in the window, or when
  CPUs 1 and 3 (no server and no client of the census is ever pinned there) averaged over 10 %
  busy.
- **First pass: 76 of 672 runs flagged** (`screen.err`), 32 of them by a named process and 44 by
  idle CPUs busy at 10-40 %, in **32 cells: 31 `point` cells and one `trade` cell**
  (`point` `group` 33 runs, `relaxed` 23, `strict` 18; `trade` `group` 2), at `cores` 1, 2
  and 4 alike. All were written between 05:01 and 06:04 UTC. 672, not 675: the cell `point`
  `relaxed` `cores = 1` 16 sessions had failed (client socket timeout, `census.log`) and wrote
  no run.
- **Handling: the whole cell is re-run, not the flagged run.** `rerun.sh` moved every run of a
  cell that held a flagged run (96 runs, 32 cells) to `quarantine/round1/`, deleted nothing, and
  resumed `census.sh`, which re-ran every cell with fewer than three runs: the 32 cells and the
  failed one. The re-run took 07:53-08:19.
- **Second screen: 0 of 675 runs flagged** (`c2.err`). The analysis therefore excludes
  nothing (`excluded as contaminated: 0`): the 96 quarantined runs are not in it, and
  `contaminated.txt` is a record of what left, not an input. Four gate waits of 60-210 s
  (`census.log`).
- **What the screen does not cover.** `c3.txt` lists 16 kept runs (`point` `group`
  `cores = 4`, `point` `relaxed` `cores` 2 and 4, `trade` `relaxed` `cores = 4`, `trade` `strict`
  `cores = 1`) whose window holds a heavy foreign process that is not a test or build: a
  `pgrep` at 100 % for one sample, `claude` and `node` at 29-129 %, `git`, `rg`, and an
  `unattended-upgrade` at 96 %. How `c3.txt` was produced is not recorded; its content is read
  from the sampler above. They were kept. Their throughput lies a median 1.2 % from their
  cell's median (all kept runs: 0.5 %), with the largest deviations (+16.3 %, -13.9 %, +8.9 %)
  in `cores = 4` and low-session `point` cells that no verdict below rests on.
  452 of 675 kept runs list the census driver itself among the busy foreign processes; that is
  not contamination.
- **The screen's input is not archived** (`host-samples.jsonl`, 8.5 MB, lives with the A/B
  run), so `screen.py` cannot be re-run from this archive.
- **BA-S4 was not screened**: its load was bounded by the driver (median 2.57, max 6.00 over
  1,125 runs) and by the check at a run's two ends. Its cells may carry contamination this
  file's did not.
- **Host, as the driver recorded it**: 1-min load before a run median 2.00, max 6.00; the
  longest wait for the load gate 40 s; 0 failed cells and 0 errors in the final 675 runs; every
  run's binary hash equal and data device `/dev/root ext4`.

## 3. What adding a core buys

Statements/s, KDS against PostgreSQL. Unsynced pairs KDS `relaxed` with PostgreSQL `off`, synced
pairs KDS `group` and `strict` with `on` (D1's point; PostgreSQL group-commits by itself).

### Relaxed

| shape | cores | engine | 1 session | 2 sessions | 4 sessions | 8 sessions | 16 sessions |
|---|---|---|---|---|---|---|---|
| point | 1 | KDS `relaxed`, B | 14,092 stmt/s | 23,070 stmt/s | 43,458 stmt/s | 43,158 stmt/s | 41,560 stmt/s |
| point | 1 | KDS `relaxed`, BA-S4 | 14,521 stmt/s | 23,776 stmt/s | 44,842 stmt/s | 44,671 stmt/s | 43,409 stmt/s |
| point | CPUs 0 | PostgreSQL `off`, BA-S4 | 16,974 stmt/s | 30,116 stmt/s | 28,987 stmt/s | 27,438 stmt/s | 26,960 stmt/s |
| point | 2 | KDS `relaxed`, B | 13,840 stmt/s | 21,401 stmt/s | 41,917 stmt/s | 59,073 stmt/s | 64,908 stmt/s |
| point | 2 | KDS `relaxed`, BA-S4 | 14,262 stmt/s | 22,844 stmt/s | 43,536 stmt/s | 60,016 stmt/s | 65,347 stmt/s |
| point | CPUs 0,2 | PostgreSQL `off`, BA-S4 | 16,929 stmt/s | 29,184 stmt/s | 60,665 stmt/s | 57,128 stmt/s | 54,433 stmt/s |
| trade | 1 | KDS `relaxed`, B | 16,588 stmt/s | 27,161 stmt/s | 38,757 stmt/s | 38,552 stmt/s | 38,426 stmt/s |
| trade | 1 | KDS `relaxed`, BA-S4 | 16,436 stmt/s | 27,429 stmt/s | 37,600 stmt/s | 37,323 stmt/s | 37,118 stmt/s |
| trade | CPUs 0 | PostgreSQL `off`, BA-S4 | 13,812 stmt/s | 21,473 stmt/s | 19,897 stmt/s | 18,740 stmt/s | 18,540 stmt/s |
| trade | 2 | KDS `relaxed`, B | 16,140 stmt/s | 26,695 stmt/s | 47,445 stmt/s | 64,704 stmt/s | 63,915 stmt/s |
| trade | 2 | KDS `relaxed`, BA-S4 | 16,110 stmt/s | 26,642 stmt/s | 41,920 stmt/s | 53,805 stmt/s | 60,113 stmt/s |
| trade | CPUs 0,2 | PostgreSQL `off`, BA-S4 | 13,823 stmt/s | 24,204 stmt/s | 41,634 stmt/s | 39,222 stmt/s | 37,593 stmt/s |
| insert1 | 1 | KDS `relaxed`, B | 18,169 stmt/s | 31,053 stmt/s | 43,062 stmt/s | 42,516 stmt/s | 42,151 stmt/s |
| insert1 | 1 | KDS `relaxed`, BA-S4 | 17,799 stmt/s | 28,342 stmt/s | 38,884 stmt/s | 39,022 stmt/s | 38,166 stmt/s |
| insert1 | CPUs 0 | PostgreSQL `off`, BA-S4 | 17,332 stmt/s | 31,384 stmt/s | 29,454 stmt/s | 27,775 stmt/s | 27,186 stmt/s |
| insert1 | 2 | KDS `relaxed`, B | 18,062 stmt/s | 31,404 stmt/s | 51,469 stmt/s | 64,228 stmt/s | 69,538 stmt/s |
| insert1 | 2 | KDS `relaxed`, BA-S4 | 17,693 stmt/s | 28,389 stmt/s | 47,159 stmt/s | 54,525 stmt/s | 68,341 stmt/s |
| insert1 | CPUs 0,2 | PostgreSQL `off`, BA-S4 | 17,296 stmt/s | 29,595 stmt/s | 61,344 stmt/s | 58,062 stmt/s | 54,628 stmt/s |
| insertN | 1 | KDS `relaxed`, B | 18,323 stmt/s | 30,331 stmt/s | 42,512 stmt/s | 42,344 stmt/s | 42,220 stmt/s |
| insertN | 1 | KDS `relaxed`, BA-S4 | 17,955 stmt/s | 28,463 stmt/s | 38,410 stmt/s | 38,691 stmt/s | 37,721 stmt/s |
| insertN | CPUs 0 | PostgreSQL `off`, BA-S4 | 17,299 stmt/s | 31,136 stmt/s | 29,122 stmt/s | 27,595 stmt/s | 26,933 stmt/s |
| insertN | 2 | KDS `relaxed`, B | 17,931 stmt/s | 31,264 stmt/s | 51,685 stmt/s | 69,101 stmt/s | 72,063 stmt/s |
| insertN | 2 | KDS `relaxed`, BA-S4 | 17,582 stmt/s | 28,798 stmt/s | 46,430 stmt/s | 60,343 stmt/s | 64,647 stmt/s |
| insertN | CPUs 0,2 | PostgreSQL `off`, BA-S4 | 17,431 stmt/s | 29,954 stmt/s | 61,180 stmt/s | 57,862 stmt/s | 54,282 stmt/s |
| recent | 1 | KDS `relaxed`, B | 14,470 stmt/s | 23,036 stmt/s | 38,226 stmt/s | 38,549 stmt/s | 38,836 stmt/s |
| recent | 1 | KDS `relaxed`, BA-S4 | 14,344 stmt/s | 21,792 stmt/s | 33,341 stmt/s | 33,847 stmt/s | 34,206 stmt/s |
| recent | CPUs 0 | PostgreSQL `off`, BA-S4 | 14,173 stmt/s | 23,963 stmt/s | 23,176 stmt/s | 22,280 stmt/s | 21,652 stmt/s |
| recent | 2 | KDS `relaxed`, B | 14,293 stmt/s | 21,768 stmt/s | 40,887 stmt/s | 59,680 stmt/s | 66,407 stmt/s |
| recent | 2 | KDS `relaxed`, BA-S4 | 14,044 stmt/s | 21,064 stmt/s | 37,931 stmt/s | 47,495 stmt/s | 53,233 stmt/s |
| recent | CPUs 0,2 | PostgreSQL `off`, BA-S4 | 14,222 stmt/s | 25,031 stmt/s | 46,609 stmt/s | 45,710 stmt/s | 43,880 stmt/s |

- **Relaxed scaling is unchanged within 0.15x** (16 sessions, `cores = 2` over `cores = 1`:
  1.56-1.71x against BA-S4's 1.51-1.79x; PostgreSQL 2.01-2.03x), and the engine is faster where it
  appends: at 16 sessions `cores = 1` B over BA-S4 reads -4.3 % (`point`, a read), +3.5 %
  (`trade`), +10.4 % (`insert1`), +11.9 % (`insertN`), +13.5 % (`recent`); at `cores = 2`
  -0.7 %, +6.3 %, +1.8 %, +11.5 %, +24.7 %. The write shapes gain from BA-S12 (section 4); `point`,
  which BA touches least, reads -0.7 % to -4.3 %, which is the number to subtract for the host.
- **KDS still leads PostgreSQL at 16 sessions on every shape at CPU sets 0 and 0,2**, and the
  lead is as large as at BA-S4 or larger on the append shapes (`insertN`, `cores = 2`: 72,063
  against 54,282 stmt/s, +32.8 %; BA-S4 +19.1 %). At one session PostgreSQL still leads on `point` (16,974
  against 14,092 stmt/s at one CPU). Several `cores = 2` cells are client-bound floors
  (above).

### Synced

| shape | durability | cores | engine | 1 session | 2 sessions | 4 sessions | 8 sessions | 16 sessions |
|---|---|---|---|---|---|---|---|---|
| trade | group | 1 | B | 819 stmt/s | 873 stmt/s | 1,687 stmt/s | 3,020 stmt/s | 5,569 stmt/s |
| trade | group | 1 | S4 | 676 stmt/s | 712 stmt/s | 1,368 stmt/s | 2,622 stmt/s | 4,977 stmt/s |
| trade | group | 2 | B | 792 stmt/s | 915 stmt/s | 1,781 stmt/s | 3,418 stmt/s | 6,606 stmt/s |
| trade | group | 2 | S4 | 657 stmt/s | 722 stmt/s | 811 stmt/s | 1,862 stmt/s | 1,656 stmt/s |
| trade | group | 4 | B | 793 stmt/s | 856 stmt/s | 1,747 stmt/s | 3,428 stmt/s | 6,793 stmt/s |
| trade | group | 4 | S4 | 654 stmt/s | 720 stmt/s | 1,434 stmt/s | 2,125 stmt/s | 3,905 stmt/s |
| trade | strict | 1 | B | 808 stmt/s | 860 stmt/s | 842 stmt/s | 874 stmt/s | 862 stmt/s |
| trade | strict | 1 | S4 | 680 stmt/s | 695 stmt/s | 700 stmt/s | 690 stmt/s | 684 stmt/s |
| trade | strict | 2 | B | 825 stmt/s | 926 stmt/s | 1,635 stmt/s | 3,026 stmt/s | 6,239 stmt/s |
| trade | strict | 2 | S4 | 653 stmt/s | 682 stmt/s | 717 stmt/s | 729 stmt/s | 721 stmt/s |
| trade | strict | 4 | B | 813 stmt/s | 887 stmt/s | 1,619 stmt/s | 3,156 stmt/s | 5,767 stmt/s |
| trade | strict | 4 | S4 | 655 stmt/s | 717 stmt/s | 1,000 stmt/s | 1,133 stmt/s | 857 stmt/s |
| trade | PostgreSQL on | CPUs 0 | BA-S4 | 673 stmt/s | 709 stmt/s | 1,313 stmt/s | 2,529 stmt/s | 4,902 stmt/s |
| trade | PostgreSQL on | CPUs 0,2 | BA-S4 | 678 stmt/s | 743 stmt/s | 1,416 stmt/s | 2,661 stmt/s | 5,150 stmt/s |
| insert1 | group | 1 | B | 851 stmt/s | 861 stmt/s | 1,687 stmt/s | 3,237 stmt/s | 5,921 stmt/s |
| insert1 | group | 1 | S4 | 668 stmt/s | 704 stmt/s | 1,346 stmt/s | 2,675 stmt/s | 4,927 stmt/s |
| insert1 | group | 2 | B | 842 stmt/s | 915 stmt/s | 1,467 stmt/s | 2,494 stmt/s | 6,208 stmt/s |
| insert1 | group | 2 | S4 | 671 stmt/s | 744 stmt/s | 825 stmt/s | 1,294 stmt/s | 2,922 stmt/s |
| insert1 | group | 4 | B | 819 stmt/s | 926 stmt/s | 1,808 stmt/s | 3,418 stmt/s | 6,328 stmt/s |
| insert1 | group | 4 | S4 | 682 stmt/s | 752 stmt/s | 1,104 stmt/s | 2,125 stmt/s | 3,991 stmt/s |
| insert1 | strict | 1 | B | 809 stmt/s | 844 stmt/s | 872 stmt/s | 892 stmt/s | 873 stmt/s |
| insert1 | strict | 1 | S4 | 708 stmt/s | 721 stmt/s | 733 stmt/s | 712 stmt/s | 728 stmt/s |
| insert1 | strict | 2 | B | 828 stmt/s | 946 stmt/s | 1,552 stmt/s | 3,189 stmt/s | 6,214 stmt/s |
| insert1 | strict | 2 | S4 | 702 stmt/s | 726 stmt/s | 739 stmt/s | 746 stmt/s | 703 stmt/s |
| insert1 | strict | 4 | B | 834 stmt/s | 915 stmt/s | 1,701 stmt/s | 3,085 stmt/s | 6,014 stmt/s |
| insert1 | strict | 4 | S4 | 665 stmt/s | 696 stmt/s | 942 stmt/s | 1,175 stmt/s | 1,010 stmt/s |
| insert1 | PostgreSQL on | CPUs 0 | BA-S4 | 708 stmt/s | 734 stmt/s | 1,385 stmt/s | 2,698 stmt/s | 5,120 stmt/s |
| insert1 | PostgreSQL on | CPUs 0,2 | BA-S4 | 691 stmt/s | 749 stmt/s | 1,448 stmt/s | 2,771 stmt/s | 5,325 stmt/s |
| insertN | group | 1 | B | 848 stmt/s | 862 stmt/s | 1,710 stmt/s | 3,310 stmt/s | 5,801 stmt/s |
| insertN | group | 1 | S4 | 701 stmt/s | 718 stmt/s | 1,376 stmt/s | 2,708 stmt/s | 4,918 stmt/s |
| insertN | group | 2 | B | 834 stmt/s | 934 stmt/s | 1,518 stmt/s | 3,218 stmt/s | 6,102 stmt/s |
| insertN | group | 2 | S4 | 689 stmt/s | 717 stmt/s | 1,072 stmt/s | 1,441 stmt/s | 3,558 stmt/s |
| insertN | group | 4 | B | 799 stmt/s | 908 stmt/s | 1,759 stmt/s | 3,324 stmt/s | 6,552 stmt/s |
| insertN | group | 4 | S4 | 687 stmt/s | 690 stmt/s | 1,440 stmt/s | 2,156 stmt/s | 4,226 stmt/s |
| insertN | strict | 1 | B | 854 stmt/s | 882 stmt/s | 903 stmt/s | 876 stmt/s | 861 stmt/s |
| insertN | strict | 1 | S4 | 688 stmt/s | 729 stmt/s | 744 stmt/s | 740 stmt/s | 726 stmt/s |
| insertN | strict | 2 | B | 836 stmt/s | 913 stmt/s | 1,650 stmt/s | 3,136 stmt/s | 6,383 stmt/s |
| insertN | strict | 2 | S4 | 706 stmt/s | 715 stmt/s | 767 stmt/s | 756 stmt/s | 752 stmt/s |
| insertN | strict | 4 | B | 841 stmt/s | 930 stmt/s | 1,729 stmt/s | 3,196 stmt/s | 6,077 stmt/s |
| insertN | strict | 4 | S4 | 692 stmt/s | 747 stmt/s | 1,008 stmt/s | 1,126 stmt/s | 985 stmt/s |
| insertN | PostgreSQL on | CPUs 0 | BA-S4 | 647 stmt/s | 702 stmt/s | 1,364 stmt/s | 2,622 stmt/s | 4,857 stmt/s |
| insertN | PostgreSQL on | CPUs 0,2 | BA-S4 | 686 stmt/s | 733 stmt/s | 1,403 stmt/s | 2,666 stmt/s | 5,035 stmt/s |
| recent | group | 1 | B | 1,540 stmt/s | 1,633 stmt/s | 2,196 stmt/s | 4,110 stmt/s | 7,226 stmt/s |
| recent | group | 1 | S4 | 1,274 stmt/s | 1,356 stmt/s | 1,777 stmt/s | 3,361 stmt/s | 6,033 stmt/s |
| recent | group | 2 | B | 1,521 stmt/s | 1,797 stmt/s | 3,558 stmt/s | 6,114 stmt/s | 12,102 stmt/s |
| recent | group | 2 | S4 | 1,253 stmt/s | 1,350 stmt/s | 2,094 stmt/s | 2,788 stmt/s | 6,325 stmt/s |
| recent | group | 4 | B | 1,496 stmt/s | 1,817 stmt/s | 3,525 stmt/s | 6,792 stmt/s | 12,627 stmt/s |
| recent | group | 4 | S4 | 1,250 stmt/s | 1,437 stmt/s | 2,789 stmt/s | 4,846 stmt/s | 7,376 stmt/s |
| recent | strict | 1 | B | 1,474 stmt/s | 1,573 stmt/s | 1,687 stmt/s | 1,719 stmt/s | 1,708 stmt/s |
| recent | strict | 1 | S4 | 1,248 stmt/s | 1,337 stmt/s | 1,371 stmt/s | 1,372 stmt/s | 1,336 stmt/s |
| recent | strict | 2 | B | 1,520 stmt/s | 1,685 stmt/s | 2,871 stmt/s | 5,727 stmt/s | 11,382 stmt/s |
| recent | strict | 2 | S4 | 1,248 stmt/s | 1,330 stmt/s | 1,376 stmt/s | 1,326 stmt/s | 1,472 stmt/s |
| recent | strict | 4 | B | 1,502 stmt/s | 1,672 stmt/s | 3,020 stmt/s | 5,667 stmt/s | 10,102 stmt/s |
| recent | strict | 4 | S4 | 1,252 stmt/s | 1,349 stmt/s | 1,814 stmt/s | 1,867 stmt/s | 1,867 stmt/s |
| recent | PostgreSQL on | CPUs 0 | BA-S4 | 1,232 stmt/s | 1,417 stmt/s | 2,466 stmt/s | 5,143 stmt/s | 9,733 stmt/s |
| recent | PostgreSQL on | CPUs 0,2 | BA-S4 | 1,238 stmt/s | 1,420 stmt/s | 2,702 stmt/s | 5,058 stmt/s | 9,942 stmt/s |

At 16 sessions, every engine and core count (B, BA-S4, PostgreSQL `on`):

| shape | durability | B cores=1 | B cores=2 | B cores=4 | S4 cores=1 | S4 cores=2 | S4 cores=4 | PG on, CPUs 0 | PG on, CPUs 0,2 | PG on, CPUs 0,2,4,6 |
|---|---|---|---|---|---|---|---|---|---|---|
| trade | group | 5,569 stmt/s | 6,606 stmt/s | 6,793 stmt/s | 4,977 stmt/s | 1,656 stmt/s | 3,905 stmt/s | 4,902 stmt/s | 5,150 stmt/s | 5,286 stmt/s |
| trade | strict | 862 stmt/s | 6,239 stmt/s | 5,767 stmt/s | 684 stmt/s | 721 stmt/s | 857 stmt/s | 4,902 stmt/s | 5,150 stmt/s | 5,286 stmt/s |
| insert1 | group | 5,921 stmt/s | 6,208 stmt/s | 6,328 stmt/s | 4,927 stmt/s | 2,922 stmt/s | 3,991 stmt/s | 5,120 stmt/s | 5,325 stmt/s | 5,220 stmt/s |
| insert1 | strict | 873 stmt/s | 6,214 stmt/s | 6,014 stmt/s | 728 stmt/s | 703 stmt/s | 1,010 stmt/s | 5,120 stmt/s | 5,325 stmt/s | 5,220 stmt/s |
| insertN | group | 5,801 stmt/s | 6,102 stmt/s | 6,552 stmt/s | 4,918 stmt/s | 3,558 stmt/s | 4,226 stmt/s | 4,857 stmt/s | 5,035 stmt/s | 5,173 stmt/s |
| insertN | strict | 861 stmt/s | 6,383 stmt/s | 6,077 stmt/s | 726 stmt/s | 752 stmt/s | 985 stmt/s | 4,857 stmt/s | 5,035 stmt/s | 5,173 stmt/s |
| recent | group | 7,226 stmt/s | 12,102 stmt/s | 12,627 stmt/s | 6,033 stmt/s | 6,325 stmt/s | 7,376 stmt/s | 9,733 stmt/s | 9,942 stmt/s | 10,171 stmt/s |
| recent | strict | 1,708 stmt/s | 11,382 stmt/s | 10,102 stmt/s | 1,336 stmt/s | 1,472 stmt/s | 1,867 stmt/s | 9,733 stmt/s | 9,942 stmt/s | 10,171 stmt/s |

### The second core, within this census

Same day, same binary, so no host shift enters these ratios.

| shape | durability | B | BA-S4 | PostgreSQL (BA-S4) |
|---|---|---|---|---|
| point | relaxed | 1.56x | 1.51x | 2.02x (`off`) |
| point | group | 1.55x | 1.50x | 2.02x (`on`) |
| point | strict | 1.55x | 1.50x | 2.02x (`on`) |
| trade | relaxed | 1.66x | 1.62x | 2.03x (`off`) |
| trade | group | 1.19x | 0.33x | 1.05x (`on`) |
| trade | strict | 7.24x | 1.05x | 1.05x (`on`) |
| insert1 | relaxed | 1.65x | 1.79x | 2.01x (`off`) |
| insert1 | group | 1.05x | 0.59x | 1.04x (`on`) |
| insert1 | strict | 7.11x | 0.97x | 1.04x (`on`) |
| insertN | relaxed | 1.71x | 1.71x | 2.02x (`off`) |
| insertN | group | 1.05x | 0.72x | 1.04x (`on`) |
| insertN | strict | 7.42x | 1.03x | 1.04x (`on`) |
| recent | relaxed | 1.71x | 1.56x | 2.03x (`off`) |
| recent | group | 1.67x | 1.05x | 1.02x (`on`) |
| recent | strict | 6.66x | 1.10x | 1.02x (`on`) |

- **`group`: a second core is now free or better.** `trade` 1.19x, `insert1` 1.05x, `insertN`
  1.05x and `recent` 1.67x, where BA-S4 read 0.33x, 0.59x, 0.72x and 1.05x and PostgreSQL's
  curve reads 1.02-1.05x.
- **`strict` reads 6.7-7.4x because its `cores = 1` cell does not batch, not because
  `cores = 2` scales.** At `cores = 1` `strict` is 808-903 stmt/s on the write shapes at every
  session count, one sync per commit, as at BA-S4 (680-744); BA-S7 changed nothing at `cores = 1` (its
  text: "no registry, so no deferral"). At `cores = 2` `strict` is within 6 % of `group`
  (6,214 against 6,208 stmt/s on `insert1`; 6,383 against 6,102 on `insertN`; 11,382 against
  12,102 on `recent`). PostgreSQL `on` on one CPU is 5.6-5.9x `strict` at `cores = 1`
  (4,857-5,120 against 861-873 stmt/s).
- **`cores = 4` adds 2-7 % over `cores = 2` for the synced shapes** (6,793 and 6,606 stmt/s on
  `trade` `group`); `cores = 4` is indicative only.

### Latency of one statement

Median over a cell's runs of each run's percentile (the A/B file has the per-statement shape of
the single-connection case).

| cell | census | statements per run | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| insert1 `relaxed` `cores = 2`, 16 sessions | B | 556,967 statements | 41 µs | 116 µs | 151 µs | 286 µs | 378 µs |
| insert1 `relaxed` `cores = 2`, 16 sessions | BA-S4 | 548,467 statements | 43 µs | 138 µs | 152 µs | 243 µs | 309 µs |
| insert1 `group` `cores = 2`, 1 session | B | 6,736 statements | 891 µs | 997 µs | 1,034 µs | 1,747 µs | 4,679 µs |
| insert1 `group` `cores = 2`, 1 session | BA-S4 | 5,372 statements | 1,007 µs | 1,222 µs | 1,287 µs | 2,390 µs | 5,499 µs |
| insert1 `group` `cores = 2`, 16 sessions | B | 49,850 statements | 1,006 µs | 1,910 µs | 1,981 µs | 4,502 µs | 9,387 µs |
| insert1 `group` `cores = 2`, 16 sessions | BA-S4 | 23,391 statements | 1,410 µs | 2,243 µs | 2,391 µs | 5,125 µs | 12,238 µs |
| insert1 `strict` `cores = 2`, 1 session | B | 6,626 statements | 885 µs | 1,000 µs | 1,036 µs | 1,782 µs | 4,914 µs |
| insert1 `strict` `cores = 2`, 1 session | BA-S4 | 5,618 statements | 1,019 µs | 1,205 µs | 1,256 µs | 2,022 µs | 5,342 µs |
| insert1 `strict` `cores = 2`, 16 sessions | B | 49,753 statements | 1,013 µs | 1,990 µs | 2,076 µs | 4,782 µs | 9,607 µs |
| insert1 `strict` `cores = 2`, 16 sessions | BA-S4 | 5,645 statements | 11,891 µs | 17,185 µs | 18,951 µs | 40,516 µs | 53,426 µs |
| insert1 `group` `cores = 1`, 16 sessions | B | 47,419 statements | 1,331 µs | 2,261 µs | 2,332 µs | 4,923 µs | 9,234 µs |
| insert1 `group` `cores = 1`, 16 sessions | BA-S4 | 39,470 statements | 1,818 µs | 2,694 µs | 2,855 µs | 5,836 µs | 11,098 µs |
| point `relaxed` `cores = 1`, 1 session | B | 111,232 statements | 58 µs | 65 µs | 66 µs | 81 µs | 92 µs |
| point `relaxed` `cores = 1`, 1 session | BA-S4 | 115,484 statements | 58 µs | 64 µs | 65 µs | 76 µs | 88 µs |

`strict` at 16 sessions and `cores = 2` fell from a p50 of 18,951 µs and a p99 of 53,426 µs to
2,076 and 9,607 µs; its p0 fell from 11,891 µs to 1,013 µs, about one device sync, so the
queue behind a sync is gone. `group` at one session and `cores = 2` fell from p50 1,287 to
1,034 µs, which section 5 reads as the day's shift and not BA's.

## 4. A verdict per carried item

Each row cites the stage that carried it, the stated exit, BA-S4's reading, this reading, and
a verdict. The shares are a median over a cell's three runs of the item's contended wait over
`cores` x the measured span (not over `sched_wall_us`: `ba_census.py` says why).

### The waits, by type (rule 3)

A statement's latency at saturation is a sum of waits; the engine's counters split the contended
ones by item. Shares of `cores` x span, median of a cell's runs; **clean cells are `cores` <= 2**.
Client and socket round-trip time is not a counter of the engine and cannot be split here: for a
synced commit the remainder is the device sync (a commit's p0 at one session, 885-891 µs, is
about one sync).

| item | max share, all cells | cell | max share, clean cells (cores <= 2) | clean cell |
|---|---|---|---|---|
| P1 frame table | 0.86 % | point relaxed k4 s16 | 0.49 % | recent relaxed k2 s16 |
| P2 window | 2.66 % | trade relaxed k4 s16 | 0.38 % | insertN relaxed k2 s16 |
| P3 statistics | 0.00 % | point relaxed k4 s8 | 0.00 % | point group k2 s16 |
| P4 relation borrow | 0.10 % | insert1 relaxed k4 s16 | 0.05 % | insert1 relaxed k2 s16 |
| P6 WAL append | 0.49 % | insertN relaxed k4 s16 | 0.38 % | insertN relaxed k2 s16 |
| P7 sync gate | 9.60 % | insert1 group k2 s8 | 9.60 % | insert1 group k2 s8 |
| P7 snapshot ceiling | 113.48 % | insert1 strict k2 s16 | 113.48 % | insert1 strict k2 s16 |
| P8 assertion dir | 0.00 % | insertN relaxed k4 s16 | 0.00 % | insert1 relaxed k2 s8 |
| superblock | 0.00 % | insert1 group k1 s1 | 0.00 % | insert1 group k1 s1 |

BA-S4's, same method:

| item | max share, all cells | cell | max share, clean cells (cores <= 2) | clean cell |
|---|---|---|---|---|
| P1 frame table | 0.91 % | recent relaxed k4 s16 | 0.67 % | recent relaxed k2 s16 |
| P2 window | 1.80 % | insertN relaxed k4 s16 | 0.26 % | insertN relaxed k2 s16 |
| P3 statistics | 0.00 % | point group k4 s16 | 0.00 % | point group k2 s16 |
| P4 relation borrow | 0.09 % | insert1 relaxed k4 s16 | 0.06 % | insert1 relaxed k2 s4 |
| P6 WAL append | 11.36 % | insertN relaxed k4 s16 | 10.01 % | insertN relaxed k2 s16 |
| P7 sync gate | 55.35 % | recent group k2 s8 | 55.35 % | recent group k2 s8 |
| P7 snapshot ceiling | 39.44 % | recent strict k4 s16 | 39.33 % | recent strict k2 s4 |
| P8 assertion dir | 0.00 % | trade relaxed k4 s16 | 0.00 % | insertN relaxed k2 s8 |
| superblock | 0.00 % | insert1 group k1 s1 | 0.00 % | insert1 group k1 s1 |

### BA-S7: P7, the commit durability

| item | durability | B max | B cell | S4 max | S4 cell |
|---|---|---|---|---|---|
| P7 sync gate | relaxed | 0.04 % | recent k2 s8 | 1.47 % | trade k4 s16 |
| P7 sync gate | group | 9.60 % | insert1 k2 s8 | 55.35 % | recent k2 s8 |
| P7 sync gate | strict | 0.10 % | insert1 k4 s2 | 47.96 % | insert1 k2 s16 |
| P7 snapshot ceiling | relaxed | 0.16 % | insertN k2 s16 | 0.09 % | insert1 k4 s16 |
| P7 snapshot ceiling | group | 0.00 % | insert1 k2 s1 | 0.00 % | insert1 k2 s1 |
| P7 snapshot ceiling | strict | 113.48 % | insert1 k2 s16 | 39.44 % | recent k4 s16 |
| P6 WAL append | relaxed | 0.49 % | insertN k4 s16 | 11.36 % | insertN k4 s16 |
| P6 WAL append | group | 0.01 % | insertN k4 s16 | 6.28 % | insertN k4 s16 |
| P6 WAL append | strict | 0.01 % | insertN k4 s16 | 0.30 % | insert1 k4 s8 |

| P7 reading | BA-S4 | B | verdict |
|---|---|---|---|
| sync gate share, largest cell | 55.35 % (`recent` `group` `cores = 2`, 8 sessions) | 9.60 % (`insert1` `group` `cores = 2`, 8 sessions) | down 5.8x; **still 5 % or over in two cells** (9.60 %, 7.73 % `recent`), so material by the stated 5 % rule in those cells |
| sync gate share, `strict` | 47.96 % (`insert1` `cores = 2`, 16 sessions) | 0.10 % | gone |
| sync gate at `group`, `cores = 2`, by sessions (`trade`, `insert1`, `insertN`, `recent`) | - | 1 session 0.02 / 0.02 / 0.00 / 0.02 %; 8 sessions 1.82 / 9.60 / 1.30 / 7.73 %; 16 sessions 1.89 / 1.54 / 3.73 / 2.13 % | peaks at 8 sessions |
| snapshot ceiling share | 39.44 % (`recent` `strict` `cores = 4`, 16 sessions) | **113.48 %** (`insert1` `strict` `cores = 2`, 16 sessions); 16 of 25 cells over 5 % at each of `strict` `cores = 2` and 4; 77.1 % at `cores = 4` | see below |
| drain passes with a commit staged (`group`, `cores = 2` / `4`) | 730,881,004 / 1,466,025,694 (126.9 / 300.8 per statement) | 3,495,134 / 4,843,422 (0.55 / 0.90 per statement) | down 200-300x; **not 0** |
| drain passes, `strict`, `cores = 2` / `4` | 0 / 0 | 3,023,620 / 3,999,676 (0.48 / 0.77 per statement) | new, one per wake |
| syncs, `group` `cores = 2` (inline / writer) | 54,796 / 303,178 | 440 / 410,906 | core 0's sync goes to the writer |
| `group` at 16 sessions, `cores = 2`, `trade` / `insert1` / `insertN` / `recent` | 1,656 / 2,922 / 3,558 / 6,325 stmt/s | 6,606 / 6,208 / 6,102 / 12,102 stmt/s | 1.7-4.0x, ahead of PostgreSQL's 5,150 / 5,325 / 5,035 / 9,942 stmt/s |
| `strict` at 16 sessions, `cores = 2`, same shapes | 721 / 703 / 752 / 1,472 stmt/s | 6,239 / 6,214 / 6,383 / 11,382 stmt/s | 7.7-8.8x |

- **What a share over 100 % means.** `analyze.py` defines a share as an item's contended wait
  summed over every core, over `cores` x the measured span. The snapshot ceiling's tally is a
  *statement* wait (BA-S4: "one of its two sites parks the statement's coroutine and the other
  spins the reactor, and the tally does not split them"). The run that reads 113.48 % is
  `insert1` `strict` `cores = 2` 16 sessions: `contention_ceiling_waits` 16,613 and
  `contention_ceiling_wait_us` 18,169,350 over 8 s x 2 cores = 16 s, a mean of 1,094 µs a wait,
  about one device sync. 16 sessions on 2 reactors can have up to 16 statements parked at once,
  so the sum of their waits can reach 8 times the cores x span; a reactor cannot be busy for
  more than 100 % of its own time. **A share over 100 % is therefore at least in part statement
  wait time of coroutines parked on the ceiling, not reactor time**, and it shows how long
  other sessions' snapshots wait for an in-flight `strict` commit to publish: with BA-S7 part 2
  parking the commit, that wait is now served by the writer's shared sync and costs the
  reactor nothing. The same cell's throughput rose 8.8x, from 703 to 6,214 stmt/s. **The
  number is no longer a measure of lost core time and should not be compared with BA-S4's
  39 %**, which included reactor spin; whether any of the 113 % is still spin the tally cannot
  say.
- **Exit "spin counter at 0": met as the stage read it, not literally.** `contention_drain_
  passes_pending` still counts each drain taken with a commit staged; BA-S7 part 1 reads the
  exit as "no pass reported as work", a blocked reactor taking one pass per wake. 0.55-0.90 passes
  per statement at `cores` >= 2 is of the order of one per commit; BA-S4's 127-301 was a busy
  wait. At `cores = 1`, where S7 changed nothing, 0.07 per statement (385,080 passes), as at
  BA-S4 (0.06).
- **Verdict: P7 is closed in effect.** The synced write curve no longer falls, `strict` shares
  its syncs, and core 0 is no longer starved (BA-R4, below). Remaining: the sync gate is 5 % or
  over in two `group` cells at 8 sessions; `strict` at `cores = 1` does not batch (862 against
  5,569 stmt/s `group`) and S7 does not claim it does.

### BA-S12: P6, the WAL append

| P6 reading | BA-S4 | B | verdict |
|---|---|---|---|
| stream latch wait, largest clean cell | 10.01 % (`insertN` `relaxed` `cores = 2`, 16 sessions) | 0.38 % (same cell) | **met** (exit: the wait in the census shape) |
| largest over all cells | 11.36 % (`insertN` `relaxed` `cores = 4`, 16) | 0.49 % (same cell) | below 5 % everywhere |
| by durability, `cores` >= 2 | `relaxed` 11.36 %, `group` 6.28 %, `strict` 0.30 % | 0.49 %, 0.01 %, 0.01 % | |
| `insertN` `relaxed` `cores = 2`, 16 sessions | 64,647 stmt/s | 72,063 stmt/s | +11.5 % |

### BA-S13: the carve, and the checkpoint (BI-Q1)

Exit: "the stall counters under 10 ms in the census shape." `*_longest_us` fields are maxima
since the server started and include setup and warm-up (reported as read, never as a delta).

| durability | cores | S4 carve median / max | B carve median / max | S4 carve runs over 10 ms | B carve runs over 10 ms | S4 checkpoint median / max | B checkpoint median / max | S4 checkpoint runs over 10 ms | B checkpoint runs over 10 ms |
|---|---|---|---|---|---|---|---|---|---|
| relaxed | 1 | 20.5 ms / 107.6 ms | 225.5 ms / 296.2 ms | 60 of 75 runs | 62 of 75 runs | 19.3 ms / 160.6 ms | 122.8 ms / 379.1 ms | 63 of 75 runs | 75 of 75 runs |
| relaxed | 2 | 30.5 ms / 452.4 ms | 257.9 ms / 333.7 ms | 62 of 75 runs | 59 of 75 runs | 55.9 ms / 203.1 ms | 187.9 ms / 510.5 ms | 75 of 75 runs | 75 of 75 runs |
| relaxed | 4 | 45.8 ms / 481.6 ms | 251.5 ms / 352.4 ms | 60 of 75 runs | 59 of 75 runs | 121.0 ms / 596.1 ms | 209.1 ms / 647.4 ms | 75 of 75 runs | 75 of 75 runs |
| group | 1 | 5.9 ms / 56.0 ms | 5.4 ms / 19.2 ms | 23 of 75 runs | 6 of 75 runs | 13.1 ms / 172.7 ms | 21.3 ms / 199.1 ms | 42 of 75 runs | 63 of 75 runs |
| group | 2 | 10.5 ms / 2,242.1 ms | 5.0 ms / 30.7 ms | 38 of 75 runs | 11 of 75 runs | 159.5 ms / 3,093.7 ms | 200.5 ms / 1,555.3 ms | 75 of 75 runs | 75 of 75 runs |
| group | 4 | 16.7 ms / 1,616.1 ms | 5.2 ms / 230.2 ms | 45 of 75 runs | 18 of 75 runs | 212.2 ms / 3,088.8 ms | 67.5 ms / 348.4 ms | 75 of 75 runs | 75 of 75 runs |
| strict | 1 | 3.0 ms / 22.5 ms | 3.6 ms / 17.4 ms | 11 of 75 runs | 6 of 75 runs | 12.6 ms / 161.0 ms | 15.8 ms / 210.2 ms | 41 of 75 runs | 60 of 75 runs |
| strict | 2 | 5.3 ms / 81.2 ms | 4.7 ms / 16.4 ms | 10 of 75 runs | 10 of 75 runs | 31.8 ms / 305.5 ms | 28.7 ms / 247.6 ms | 75 of 75 runs | 75 of 75 runs |
| strict | 4 | 8.7 ms / 73.4 ms | 5.9 ms / 48.1 ms | 31 of 75 runs | 12 of 75 runs | 40.4 ms / 204.8 ms | 34.6 ms / 441.5 ms | 75 of 75 runs | 75 of 75 runs |

- **The carve's target of 10 ms is met for `group` and `strict` by the median and not by the
  tail, and is not met at `relaxed`.**
  - `group` and `strict`: a median of 3.6-5.9 ms (BA-S4 3.0-16.7 ms); the maximum fell from
    2,242 ms to 31 ms at `group` `cores = 2`, and from 1,616 ms to 230 ms at `cores = 4`. Runs
    over 10 ms: 63 of 450 against 158 of 450 at BA-S4 across the six rows.
  - **`relaxed`: a median of 225.5, 257.9 and 251.5 ms at `cores` 1, 2 and 4, against BA-S4's
    20.5, 30.5 and 45.8 ms: 5.5-11 times worse**, 180 of 225 runs over 10 ms, maximum 352.4 ms.
    The whole-matrix maximum fell from 2,242 ms to 352 ms and 243 of 675 runs are over 10 ms
    against 340, but all of the remainder's weight sits in `relaxed`.
  - **Not explained by this run.** BA-S13's text says its carve now syncs the data file
    unconditionally and that "the data file's `fdatasync` flushes everything else unsynced in the
    file too"; at `relaxed` nothing else syncs the data file, so the carve may pay for the whole
    backlog of dirty pages. That is a reading consistent with the direction and with the
    A/B file's probe (carve 1.9-3.9 ms on a small fresh server), and it is untested.
- **The checkpoint half is unbuilt** (BA-R10's third paragraph waits for the operator's mark on
  BI-Q1) and its longest stall is not better: median 92.1 ms and maximum 1,555 ms against
  BA-S4's 46.5 ms and 3,094 ms; runs over 10 ms 648 of 675 against 596. **At `relaxed` it is
  1.7-6.4 times longer** (median 122.8, 187.9 and 209.1 ms against 19.3, 55.9 and 121.0 ms);
  at `group` and `strict` it ranges from 0.3x to 1.6x of BA-S4's. The A/B file reads the same
  stall from one connection: a 62-226 ms statement in nearly every 10,000-row run.
- **Whether the checkpoint got longer or the carve's old whole-pool sync had been doing part of
  its work is not separable here**: both readings predict this table.
- Carves in the measured span: 16,345 (BA-S4: 13,786). Checkpoint runs: 2,393 (2,356).
- **Verdict: BA-S13 is not met.** The carve's stall is gone at `group` and `strict` and larger
  at `relaxed`; the checkpoint's is unchanged or larger and is BI-Q1's.

### BA-S14: P9, the rightmost leaf

| refusal reading | BA-S4 | B |
|---|---|---|
| `contention_refused_btree_descend` | 20 | 29 |
| the other six sites (lookup, parent, secure; index descend, parent, secure) | 0 | 0 |
| `contention_structural_reruns` | not built | 29 |
| client-visible `TXN_CONFLICT` retried by the driver | 20 | 0 |
| errors | 0 | 0 |

Where B's 29 fell (all `relaxed`): `trade` 5 at `cores = 2` and 10 at `cores = 4`; `insert1` 2 and 7;
`recent` 2 and 3.

- **29 `btree_descend` refusals in 675 runs (BA-S4: 20), every one re-run inside the engine
  (`contention_structural_reruns` 29), none visible to a client** (0 driver-retried
  `TXN_CONFLICT`, 0 errors). All 29 came at `relaxed` (BA-S4's: trade at `cores` 2 and 4,
  `insert1` at `cores = 4`, `recent` at `cores = 2`). None at the other six sites, none at `group`
  or `strict`.
- **The refusals were not removed; they became invisible.** By the 5 % rule's "or refused
  anything" P9 is still material in the letter. Whether a re-run is ever refused again is not
  counted separately; the client-visible 0 is what the data supports.
- **Scenario 0's `c8-s` cell with no refusal**: three passes, 0 errors, 0 torn
  (A/B file, section 10). The census does not repeat that cell.
- Not built in BA-S14 (its own text): the cell "a forced root grown over is re-run and
  succeeds" has no test seam; not measurable here.
- **Verdict: P9 met for a client** (no refusal reaches one); the engine still takes about 1 per
  23 runs.

### BA-R4's premise: what the writer hand-off costs

| census | shape | durability | core 0 p50 | core 0 p99 | peer p50 | peer p99 | clients core 0 / peers | statements per client core 0 / peers |
|---|---|---|---|---|---|---|---|---|
| B | insert1 | group | 1,967 µs | 9,663 µs | 1,965 µs | 9,554 µs | 48 / 45 | 2933.5 / 3173 |
| B | insert1 | strict | 2,065 µs | 9,739 µs | 2,047 µs | 9,262 µs | 45 / 48 | 3127 / 3128.0 |
| B | insertN | group | 1,996 µs | 9,631 µs | 2,000 µs | 9,984 µs | 48 / 45 | 2961.0 / 3298 |
| B | insertN | strict | 2,027 µs | 9,178 µs | 2,030 µs | 9,197 µs | 42 / 51 | 3193.5 / 3195 |
| B | trade | group | 1,985 µs | 8,511 µs | 1,934 µs | 8,469 µs | 47 / 46 | 3252 / 3504.0 |
| B | trade | strict | 2,055 µs | 9,650 µs | 2,061 µs | 9,847 µs | 30 / 63 | 3124.0 / 3124 |
| S4 | insert1 | group | 12,906 µs | 1,395,464 µs | 2,388 µs | 9,507 µs | 47 / 46 | 44 / 2899.5 |
| S4 | insert1 | strict | 17,891 µs | 36,927 µs | 17,750 µs | 35,903 µs | 41 / 52 | 412 / 411.0 |
| S4 | insertN | group | 11,451 µs | 1,300,255 µs | 2,409 µs | 9,897 µs | 39 / 54 | 28 / 2861.0 |
| S4 | insertN | strict | 14,162 µs | 33,692 µs | 14,485 µs | 34,022 µs | 43 / 50 | 510 / 499.0 |
| S4 | trade | group | 42,667 µs | 1,638,382 µs | 2,533 µs | 9,901 µs | 49 / 44 | 16 / 2764.0 |
| S4 | trade | strict | 15,067 µs | 33,122 µs | 17,794 µs | 35,675 µs | 43 / 50 | 480 / 412.0 |

- **The premise does not hold in the form it was stated, and the starvation is gone.** BA-S4:
  a core-0 session's `group` p50 was 11-43 ms and its p99 1.3-1.6 s, with 16-44 statements in
  8 s. Now core 0's sessions read p50 1,967-1,996 µs and p99 8,511-9,663 µs, within 4 % of the
  peers' (p50 1,934-2,000 µs, p99 8,469-9,984 µs), and complete 2,934-3,252 statements against
  the peers' 3,173-3,504. `strict`: 2,027-2,065 µs against 2,030-2,061 µs (BA-S4 14-18 ms).
- **BA-R4's claim that a hand-off "doubled `group`'s p99"** compares one hand-off arm with one
  inline arm. Both arms now hand off (inline syncs at `cores = 2`: 440 of 411,346), so the
  comparison cannot be remade here; what this shows is that with every core handing off, core 0
  pays no more than a peer. The single-connection cost of the hand-off is the A/B file's
  +13 to +32 µs a commit (1.0-3.1 %).
- **Verdict: met** (the premise's symptom, core 0 starving behind a writer, no longer occurs).

### P11: session placement

The proxy: within a cell, runs whose sessions landed on every core against the rest (`analyze.py`).

| census | cells with both kinds of run | every-core runs faster by over 10 % | slower by over 10 % | median gain | `group` / `relaxed` / `strict` median |
|---|---|---|---|---|---|
| B | 36 cells | 8 cells | 5 cells | -0.7 % | -0.2 % / +0.2 % / -1.8 % |
| S4 | 30 cells | 8 cells | 3 cells | +2.2 % | -1.0 % / +0.8 % / +6.0 % |


The within-cell regression (`p11.py`; slope of relative throughput on imbalance, 0 = an even
split, 1 = all sessions on one core):

| census | cores | durability | runs | slope | sd of relative throughput | lopsided runs, median | even runs, median |
|---|---|---|---|---|---|---|---|
| B | 2 | relaxed | 60 runs | -0.031 | 4.7 % | -0.1 % over 19 runs | +0.2 % over 41 runs |
| B | 2 | group | 60 runs | +0.026 | 4.4 % | -0.4 % over 21 runs | 0.0 % over 39 runs |
| B | 2 | strict | 60 runs | -0.016 | 3.4 % | +0.5 % over 29 runs | +0.1 % over 31 runs |
| S4 | 2 | relaxed | 60 runs | -0.039 | 5.0 % | -0.3 % over 23 runs | +0.3 % over 37 runs |
| S4 | 2 | group | 60 runs | +0.074 | 19.1 % | +0.3 % over 29 runs | -0.3 % over 31 runs |
| S4 | 2 | strict | 60 runs | -0.004 | 3.1 % | +0.9 % over 20 runs | +0.1 % over 40 runs |

- **Verdict: no placement effect larger than the between-run spread is seen at `cores = 2`**
  (slopes within +/-0.03 against an sd of 3-5 %; lopsided and even runs within 0.5 % of each
  other), and the `group` confound BA-S4 named (a session on core 0 paying the inline sync) is
  gone. **This is not BA-R0's test**: the driver opens no balanced arm. P11 stays not shown
  material and not shown immaterial, and BA-S16 stays unopened. The `cores = 4` regression is
  noisier (relaxed slope -0.103, sd 11.5 %, 10 lopsided runs at -2.6 %) and indicative only.
- Between-run spread of a cell (max / min - 1 of three runs): median 3.8 % (BA-S4 3.5 %), 90th
  percentile 19.3 % (BA-S4 39.8 %).

### The items that stay not material or not measurable

P1 (frame table) 0.49 % clean / 0.86 % all cells, P2 (window) 0.38 % / 2.66 %, P3 0.00 %, P4
0.05 % / 0.10 %, P8 0.00 %: as at BA-S4, none reaches 5 % in a clean cell. **P5 (the pk mark) is
still not measurable** by this census (page 7 is a page latch and the counters do not say which
page); `insert1` against `insertN` at `relaxed` `cores = 2`, 16 sessions reads 69,538 against
72,063 stmt/s, `insert1` 3.5 % behind (BA-S4 had it 5.7 % ahead), at `group` 6,208 against
6,102 stmt/s, `insert1` 1.7 % ahead. Material is not shown.

## 5. The cross-day baseline: how much of "B over BA-S4" is the host

BA-S4 ran the day before. A cell BA did not change should read about the same on both days;
these do not.

| durability | sessions | shapes | B over BA-S4, per shape | median |
|---|---|---|---|---|
| relaxed | 1 session | 5 shapes | point -3.0 %, trade +0.9 %, insert1 +2.1 %, insertN +2.0 %, recent +0.9 % | +0.9 % |
| relaxed | 2 sessions | 5 shapes | point -3.0 %, trade -1.0 %, insert1 +9.6 %, insertN +6.6 %, recent +5.7 % | +5.7 % |
| relaxed | 4 sessions | 5 shapes | point -3.1 %, trade +3.1 %, insert1 +10.7 %, insertN +10.7 %, recent +14.7 % | +10.7 % |
| relaxed | 8 sessions | 5 shapes | point -3.4 %, trade +3.3 %, insert1 +9.0 %, insertN +9.4 %, recent +13.9 % | +9.0 % |
| relaxed | 16 sessions | 5 shapes | point -4.3 %, trade +3.5 %, insert1 +10.4 %, insertN +11.9 %, recent +13.5 % | +10.4 % |
| group | 1 session | 5 shapes | point -1.9 %, trade +21.2 %, insert1 +27.4 %, insertN +21.1 %, recent +20.9 % | +21.1 % |
| group | 2 sessions | 5 shapes | point -1.8 %, trade +22.6 %, insert1 +22.3 %, insertN +20.0 %, recent +20.4 % | +20.4 % |
| group | 4 sessions | 5 shapes | point -1.5 %, trade +23.4 %, insert1 +25.3 %, insertN +24.2 %, recent +23.6 % | +23.6 % |
| group | 8 sessions | 5 shapes | point -5.4 %, trade +15.2 %, insert1 +21.0 %, insertN +22.2 %, recent +22.3 % | +21.0 % |
| group | 16 sessions | 5 shapes | point -3.4 %, trade +11.9 %, insert1 +20.2 %, insertN +18.0 %, recent +19.8 % | +18.0 % |
| strict | 1 session | 5 shapes | point -2.8 %, trade +18.8 %, insert1 +14.3 %, insertN +24.2 %, recent +18.1 % | +18.1 % |
| strict | 2 sessions | 5 shapes | point -1.2 %, trade +23.7 %, insert1 +17.1 %, insertN +21.0 %, recent +17.7 % | +17.7 % |
| strict | 4 sessions | 5 shapes | point -2.7 %, trade +20.4 %, insert1 +19.0 %, insertN +21.4 %, recent +23.0 % | +20.4 % |
| strict | 8 sessions | 5 shapes | point -3.2 %, trade +26.8 %, insert1 +25.1 %, insertN +18.4 %, recent +25.3 % | +25.1 % |
| strict | 16 sessions | 5 shapes | point -3.8 %, trade +26.1 %, insert1 +20.0 %, insertN +18.5 %, recent +27.8 % | +20.0 % |

- **Relaxed: the baseline holds at one session and drifts up with sessions** (+0.9 % median at
  one, +5.7 % at two, +9-11 % at 4-16 sessions), the drift being the append shapes (`insert1`
  `insertN` `recent` +9 to +15 %) where BA-S12 acts; `trade` -1.0 to +3.5 %; `point`, a read,
  -3.0 to -4.3 %. **The host shift on a CPU-bound cell is bounded by the `point` reads at -1.2 to
  -5.4 % and the one-session writes at +0.9 to +2.1 %.**
- **Synced `cores = 1`: B reads +12 % to +28 % over BA-S4 on every write shape and session
  count** (median +20.9 % `group`, +19.0 % `strict`; `point` -1.2 to -5.4 %). BA changed nothing
  on this path (BA-S7: "at `cores = 1` nothing changed"; `cores = 1` syncs are 385,170 inline
  and 11,700 by the writer, BA-S4 317,859 and 11,767). It reads as a shift of the synced commit
  itself: p50 1,297 µs at BA-S4, 1,009 µs here, for `insert1` `group` one session.
- **Evidence that it is the host or the day and not BA**: the A/B file's A, the commit BA opened
  from, which has none of BA's stages, reads `group` `cores = 1` commit p50s of 1,005-1,033 µs
  the same day, and B is within 5 µs of it in all three sizes. So today's 1.0 ms commit is not
  something BA made.
- **Evidence against a plain device shift**: the same-day PostgreSQL control, seven of BA-S4's
  `on` cells re-run at 08:49-08:54, reads -0.0 % to +7.4 % (median +2.5 %; one session `insert1`
  CPU 0 -0.0 %, PostgreSQL's commit p50 1,241 µs at BA-S4 and 1,244 µs today).

| shape | CPUs | sessions | today (3 runs) | BA-S4 median | today over BA-S4 |
|---|---|---|---|---|---|
| insert1 | 0 | 1 | 705 stmt/s, 710 stmt/s, 708 stmt/s | 708 stmt/s | -0.0 % |
| insert1 | 0 | 16 | 5,335 stmt/s, 5,249 stmt/s, 5,167 stmt/s | 5,120 stmt/s | +2.5 % |
| insert1 | 0,2 | 1 | 711 stmt/s, 716 stmt/s, 736 stmt/s | 691 stmt/s | +3.6 % |
| insert1 | 0,2 | 16 | 5,350 stmt/s, 5,403 stmt/s, 5,393 stmt/s | 5,325 stmt/s | +1.3 % |
| insertN | 0,2 | 16 | 5,338 stmt/s, 5,464 stmt/s, 5,407 stmt/s | 5,035 stmt/s | +7.4 % |
| recent | 0,2 | 16 | 10,473 stmt/s, 10,492 stmt/s, 10,474 stmt/s | 9,942 stmt/s | +5.4 % |
| trade | 0,2 | 16 | 5,318 stmt/s, 5,242 stmt/s, 5,223 stmt/s | 5,150 stmt/s | +1.8 % |

- **The shift is therefore KDS-specific and its cause is not isolated.** Candidates: the
  device's behaviour under KDS's write pattern but not PostgreSQL's; or the engine at
  `c390a624` itself (BA-S4's binary), which was not measured on this day. The experiment that
  separates them (BA-S4's binary, `group` `cores = 1`, one session, today, beside B) was not run.
- **How to read the tables above.** Within-census ratios (the second core, section 3) are
  same-day and carry no shift. A ratio against BA-S4 on a synced cell at `cores = 1` carries up
  to +20 %; at `cores = 2` the gains of 1.7-8.8x are far larger than that shift, so they stand,
  but a number like "`group` `trade` `cores = 4`: 3,905 to 6,793 stmt/s (+74 %)" includes
  about a fifth of host. Relaxed ratios carry about +/-5 %, and the `point` read is the
  reference for it.

## 6. What this census teaches about the engine

- **The engine's synced scaling was one problem, and BA-S7 solved it.** Taking the sync off
  every reactor and parking `strict`'s commit turns the second reactor from a second waiter into a
  second worker: `group` at 16 sessions reads 1.05-1.19x of `cores = 1` on the write shapes, where
  PostgreSQL is at 1.04-1.05x, and `strict` at `cores = 2` is within 6 % of `group`. Both exceed PostgreSQL's floor at
  `cores = 2` by 17-28 % on the write shapes (BA-S4's PostgreSQL numbers; the control puts the
  seven cells it re-ran +1.3 to +7.4 % higher today).
- **The latch items were the right ones to leave alone and the right one to fix.** P1-P4 and P8
  stay under 1 % in the clean cells; P6, the only latch item over the line, fell from 10 % to
  0.4 %, and the append shapes read +1.8 to +11.9 % over BA-S4 at 16 sessions and `cores = 2`,
  which is what removing a latch wait predicts and is not attributed to S12 alone.
- **The remaining bound is the stall, not a latch.** At `relaxed` the carve and the checkpoint
  hold a reactor for a median 226-258 ms and 123-209 ms, which BA-S4 saw at 20-46 ms and 19-121
  ms. The one BA stage aimed at the carve made it worse at `relaxed` and better at the synced
  durabilities, and the checkpoint is BI's. If BA-S13's text is right that a carve's data-file
  sync flushes the dirty backlog, the stall's size is the backlog's, and the cure is the
  checkpoint's, not the carve's.
- **`strict` at `cores = 1` is the one synced cell that still does not batch** (862 stmt/s on
  16 sessions against 5,569 for `group`), as designed by BA-S7 and not as PostgreSQL does it.

## 7. What BA carries

1. **The relaxed carve** (median 226-258 ms against a 10 ms exit; BA-S4 20-46 ms): BA-S13 is
   not met at `relaxed`.
2. **The checkpoint half** (BI-Q1, unmarked): unbuilt, and longer than at BA-S4 at `relaxed`.
3. **P7's residue**: the sync gate over 5 % in two `group` cells at 8 sessions (9.60 %, 7.73 %);
   the snapshot ceiling's share is a wait on parked statements and is not comparable to BA-S4's;
   `strict` at `cores = 1` does not batch.
4. **P9 in the engine**: 29 refusals in 675 runs, all re-run, none seen by a client; "a forced
   root grown over is re-run" has no seam.
5. **P5 and P11 stay unshown**: P5 is not measurable with page-latch counters, P11 has no balanced
   arm. BA-S11 and BA-S16 stay unopened.
6. **`cores = 4`** is indicative and the relaxed fall from 2 to 4 cores at 16 sessions is not
   explained.
7. **The synced `cores = 1` shift of +12 to +28 % over BA-S4**, whose cause is not isolated
   (section 5).
8. **Client-bound floors**: `point` and `recent` `relaxed` at `cores = 2` are floors on the
   engine.

## 8. Where the data could not support a claim

- The carve's relaxed regression has a candidate cause (BA-S13's own caveat) and no test.
- The checkpoint comparison cannot separate "longer" from "took over the carve's work".
- `c3.txt`'s generator is not recorded; its meaning is read from the sampler.
- The screen's input is not archived; BA-S4 was not screened.
- The synced `cores = 1` +20 % has no isolated cause; the experiment to run is named above.
- The snapshot ceiling's 113 % mixes parked-statement wait with any reactor spin the tally
  cannot split.
- The census has no row-count axis, no balanced arm (P11) and no page-level latch counter (P5).
- PostgreSQL was read from BA-S4 (a day earlier, same host); the control covers seven synced
  cells, not the unsynced `off` rows.
- The same-day control's PostgreSQL commit (1,244 µs at one session) and KDS's (1,009 µs) are
  different write patterns, so "PostgreSQL did not move" does not exclude a device shift that
  only KDS's pattern sees.

## 9. Counters (sums over every run)

- B: `contention_carves` 16,345; `contention_checkpoint_runs` 2,393; `contention_drain_passes_pending` 15,746,932; `contention_page_latch_spin_turns` 90,213,311; `contention_refused_btree_descend` 29; `contention_refused_btree_lookup` 0; `contention_refused_btree_parent` 0; `contention_refused_btree_secure` 0; `contention_refused_index_descend` 0; `contention_refused_index_parent` 0; `contention_refused_index_secure` 0; `contention_structural_reruns` 29; `contention_syncs_inline` 796,261; `contention_syncs_writer` 1,852,504
  - wal sync counters: {'wal_interval_syncs': 174552.0, 'wal_sync_failures': 0.0, 'wal_syncs': 1689954.0}
- S4: `contention_carves` 13,786; `contention_checkpoint_runs` 2,356; `contention_drain_passes_pending` 2,197,224,334; `contention_page_latch_spin_turns` 98,466,594; `contention_refused_btree_descend` 20; `contention_refused_btree_lookup` 0; `contention_refused_btree_parent` 0; `contention_refused_btree_secure` 0; `contention_refused_index_descend` 0; `contention_refused_index_parent` 0; `contention_refused_index_secure` 0; `contention_syncs_inline` 991,774; `contention_syncs_writer` 1,334,172
  - wal sync counters: {'wal_interval_syncs': 152214.0, 'wal_sync_failures': 0.0, 'wal_syncs': 1324493.0}
