# DE-S0 premise: what a buffer-pool miss costs the sessions that did not miss

Measured on `v3.4.13-19-g35086008` (Release), the premise stage of
`instructions/v3.2.0/workorder-de-page-read-off-the-reactor.md` (§0, §1.4, §4
DE-Q1/DE-Q2, §5's DE-S0 row). **Finding, in one paragraph.** With the
operating system's page cache kept from answering, a quarter-size pool and
four sessions that never miss beside four that miss on 77 % of their reads,
the never-miss sessions' p95 and p99 rise to 2.1-3.0x and 2.2-3.5x of the same
cell with the page cache answering, while their p50 does not move beyond the
run-to-run spread. The reactor spends 27.0-27.5 % of one core inside
`io.page_read`, at a mean of 540.2-550.9 µs per read against a device probe
whose p50 is 462.9 µs. A scanner over the same pool is a different size of
problem: 93 % of the core in reads, a 64-page slice of about 3.0 ms (derived),
and the never-miss sessions' service p50 at 6.4 ms with their open-loop
schedule seconds behind. This file states the numbers; the order says the
ruling on them (build, narrow, nothing) is the operator's (DE-Q1 (a)).

## 1. Stamp

| | |
|---|---|
| Executed | 2026-11-19, the run that counts 08:31:59-about 08:41:17 UTC (`summary.json` `started`, `total_s` 557.6 s; the data file's last write 08:41:16 UTC). Written 08:47 UTC |
| Branch / worktree | `worktree-de-page-read-off-the-reactor`, worktree `de-page-read-off-the-reactor` |
| Commit | `35086008c6b35a1251f68256b96c67a569fd7dcd`, `git describe --tags` at the measurement: **`v3.4.13-19-g35086008`**. At the time of writing the same commit carries the tag `v3.4.14` (`git describe --tags --long` reads `v3.4.14-0-g35086008`); the file is named by the describe string the measurement used |
| Tree | tracked files clean. Untracked at the time of writing: `tools/de_premise.py` (the driver, new in this worktree, not committed) and the archive directory below |
| Binary | `build-release/kds_server`, Release; mtime 2026-11-19 08:12:46 UTC, **after** the commit's own timestamp (07:59:41 UTC), so the binary is not older than HEAD. Measured from a copy (`~/bench-runs/de/work/kds_server.de`, mtime carried over), **sha256 `316b0863623263c389047fb64d74b26eff0a3fff42d8a22d8133e80a107d63ab`** (identical in all 14 JSON files, and equal to the source binary's hash when checked at the time of writing) |
| Host | 8 logical CPUs = 4 cores x 2 threads (`thread_siblings_list`: 0/4, 1/5, 2/6, 3/7), INTEL(R) XEON(R) PLATINUM 8581C @ 2.10 GHz, 31 GiB; Linux 6.12.116+deb13-cloud-amd64 |
| Data device | `/dev/nvme0n1p1`, ext4 (`data_device` in every JSON; `df -T` of the work directory: `/dev/nvme0n1p1 ext4`, mounted `/`). A cloud persistent disk presented as NVMe (the order's §1.4: `nvme_card-pd`). **Not tmpfs.** Data file `~/bench-runs/de/work/de.db`, 1,056,440,320 B = 128,960 pages of 8 KiB |
| Port | 32750, chosen on the command line |
| Server configuration | `durability = relaxed`; `buffer_pool_frames` 32,768 (cells i, ii, iii, iv, iii4) or 160,000 (v, vscan); `cores = 1` with `reactor_cpus = 1`, except iii4: `cores = 4`, `reactor_cpus = 0,1,2,3`; `log_level = warn`; no other non-default key (from `de-<cell>-r<run>.conf`) |
| Driver | `tools/de_premise.py`, invoked `--bin build-release/kds_server --port 32750 --workdir /home/louis/bench-runs/de/work --out bench/v3.4.13/archive/de-s0-premise-v3.4.13-19-g35086008 --runs 2 --keep --cells i,ii,iii,iv,v,vscan,iii4` (`summary.json` `argv`). It borrows `tools/bl_mixed.py`'s connect and percentile helpers (through `tools/bb_overhead_benchmark.py`'s `host_state`, `meta_all`, `wait_for_load`) and is that driver's extension for DE's cells, which is the order's "by `tools/bl_mixed.py` extended"; it is a file of its own because the load is done once and shared by every cell. Its docstring is its documentation; `bench/docs/` is closed by `bench/README.md` and holds no entry for it |
| Raw output | `bench/v3.4.13/archive/de-s0-premise-v3.4.13-19-g35086008/`: `de-<cell>-r<run>.json` (14), `de-<cell>-r<run>.s.log` (14), `summary.json` |
| Baseline | none for this shape (section 9) |

Every table below is read straight from those JSON files; a number marked
*derived* is arithmetic on them and says what it was divided by.

## 2. What was measured

One relation, `de_big`, of 900,000 rows `(id int64, c0 int64, pad char(1000))`,
loaded once (58.0 s) and mounted by every cell. **No cell writes a row.**
Relation size is 128,960 data-file pages. The pool of 32,768 frames is 25.4 %
of that (derived); 160,000 frames hold all of it.

| role | shape | rate |
|---|---|---|
| hot, 4 sessions | point reads by tuple key over keys 1..700, open-loop | 1,000 reads/s in total (250/s a session); the leaves stay resident |
| cold, 4 sessions | point reads by tuple key, uniform over the relation, open-loop | 648.1 reads/s in total (162.0/s a session): 0.3 of a core over the device probe's p50 of 462.9 µs |
| scanner, 1 session | `SELECT id FROM de_big WHERE c0 = -1`, a whole-relation walk that matches nothing and yields every 64 pages, closed-loop | as fast as it goes |

Window 20 s after a 4 s warm-up, 2 runs per cell, cells interleaved by round
(i, ii, iii, iv, v, vscan, iii4, then again). Clients are pinned
(`taskset`) so placement does not move them; one control session (session 1)
walks the relation once before every cell (the pre-warm) and reads the
server's views.

| cell | hot | cold or scanner | pool | OS page cache | server limited | cores |
|---|---|---|---|---|---|---|
| i | 4 sessions | none | 32,768 frames | warm (`mincore` 1.00) | no | 1 |
| ii | 4 sessions | 4 cold | 32,768 frames | warm (`mincore` 1.00) | no | 1 |
| iii | 4 sessions | 4 cold | 32,768 frames | kept from answering (`mincore` 0.00 before the mount) | yes | 1 |
| iv | 4 sessions | 1 scanner | 32,768 frames | kept from answering (`mincore` 0.00) | yes | 1 |
| v | 4 sessions | 4 cold | 160,000 frames | warm (`mincore` 1.00) | no | 1 |
| vscan | 4 sessions | 1 scanner | 160,000 frames | warm (`mincore` 1.00) | no | 1 |
| iii4 | 4 sessions on cores 1-3 | 4 cold, `SET PRIORITY analytic` (held on core 0 by BL-S4's partition, per the driver's design) | 32,768 frames | kept from answering (`mincore` 0.00) | yes | 4 |

Two things about this table. **Cell vscan is not in the order**, which lists
(i) to (v) and iii at `cores = 4`; CLA added it as iv's ceiling (the scanner
with a pool that holds the relation), the way (v) is (iii)'s. **iii4 has no
`cores = 4` resident baseline**: there is no cell with the same hot and cold
sessions at four cores with the cache answering, so iii4 can be read against
the `cores = 1` cells only loosely (section 7).

**How the page cache was kept from answering (DE-Q2 (a))**, in the driver:
the data file's cache is dropped before the mount by
`posix_fadvise(POSIX_FADV_DONTNEED)` after a sync (`mincore` confirms 0.00);
the server runs in `systemd-run --user --scope -p MemorySwapMax=0`; a whole
walk by the control session fills the pool so its anonymous memory is at its
ceiling; then the scope's `memory.max` is written as `RssAnon` at that moment
plus 64 MiB, so the scope may keep only about 64 MiB of file pages.
Section 6 shows that this worked.

The reproduction mapping: `--rows 900000` (the default) gives 128,960 pages;
`--frames-quarter 32768` and `--frames-all 160000` are the two pools;
`--hot 4 --hot-rate 1000 --hot-keys 700`; `--cold 4`, with the cold rate
`--cold-util 0.3 / probe p50 = 648.1/s`; `--seconds 20 --warmup 4`;
`--mem-slack-mb 64`; `--client-cpus` was left at its default `2,3,6,7`
and `--server-cpu 1`, `--server-cpus4 0,1,2,3`. **The driver was changed
after the run**: it has since gained `--client-cpus4` (default `4,5,6,7`) for
the `cores = 4` cell, which ran on `--client-cpus`; to reproduce iii4 as run,
pass `--client-cpus4 2,3,6,7`.

## 3. The host while it ran

`competing` (the driver's `pgrep -a -f "cc1plus|cmake --build|ctest"`) was
**empty before and after every one of the 14 cells.** The 1-minute load
average before each cell was 1.20-1.50 (the driver waits until it is at or
below 1.5), and it includes the previous cell's own server and clients, so it
is not an idle-host figure. The 15-minute average fell through the run from
5.15 (before cell i, r0, still carrying the build) to 3.46 (after the last
cell). The driver waited for the 1-minute figure to fall under 1.5 for
`wait_s` seconds before the cells in the last column (70.0 s in all, inside
the run's 557.6 s).

| cell | run | loadavg before (1 / 5 / 15 min) | loadavg after (1 / 5 / 15 min) | `wait_s` |
|---|---|---|---|---|
| i | r0 | 1.39 / 3.51 / 5.15 | 1.46 / 3.29 / 5.02 | 0.0 s |
| i | r1 | 1.32 / 2.24 / 4.21 | 1.56 / 2.21 / 4.13 | 0.0 s |
| ii | r0 | 1.46 / 3.29 / 5.02 | 1.26 / 3.07 / 4.89 | 0.0 s |
| ii | r1 | 1.43 / 2.17 / 4.10 | 1.93 / 2.23 / 4.06 | 5.0 s |
| iii | r0 | 1.26 / 3.07 / 4.89 | 1.29 / 2.89 / 4.76 | 0.0 s |
| iii | r1 | 1.44 / 2.10 / 3.98 | 1.92 / 2.17 / 3.93 | 20.0 s |
| iv | r0 | 1.29 / 2.89 / 4.76 | 1.29 / 2.69 / 4.61 | 0.0 s |
| iv | r1 | 1.44 / 2.04 / 3.84 | 1.31 / 1.94 / 3.74 | 25.0 s |
| v | r0 | 1.29 / 2.69 / 4.61 | 1.20 / 2.51 / 4.48 | 0.0 s |
| v | r1 | 1.31 / 1.94 / 3.74 | 1.55 / 1.94 / 3.67 | 0.0 s |
| vscan | r0 | 1.20 / 2.51 / 4.48 | 1.66 / 2.48 / 4.40 | 0.0 s |
| vscan | r1 | 1.50 / 1.93 / 3.66 | 1.48 / 1.88 / 3.59 | 5.0 s |
| iii4 | r0 | 1.49 / 2.40 / 4.35 | 1.32 / 2.24 / 4.21 | 15.0 s |
| iii4 | r1 | 1.48 / 1.88 / 3.59 | 1.06 / 1.72 / 3.46 | 0.0 s |

## 4. The never-miss sessions: the finding

The hot sessions' percentiles, **both from the schedule's due instant** (what
an open-loop client's user sees, queueing included) **and from the send**
(service time). Cells i, ii, iii, v, vscan and iii4 first; iv is in section 8
because its unit is seconds. `n` is the operation count in the 20 s window.

### 4.1 Hot sessions, from the schedule's due instant

| cell | run | p0 | p25 | p50 | p95 | p99 | p99.9 | max | n |
|---|---|---|---|---|---|---|---|---|---|
| i | r0 | 131.4 µs | 218.9 µs | 251.1 µs | 353.3 µs | 415.1 µs | 2,601.4 µs | 7,155.9 µs | 20,003 ops |
| i | r1 | 112.3 µs | 220.3 µs | 247.7 µs | 359.1 µs | 888.6 µs | 3,647.3 µs | 6,541.0 µs | 20,004 ops |
| ii | r0 | 119.7 µs | 204.8 µs | 230.3 µs | 326.9 µs | 383.4 µs | 1,260.5 µs | 5,389.9 µs | 20,004 ops |
| ii | r1 | 125.9 µs | 220.8 µs | 256.3 µs | 380.8 µs | 579.6 µs | 3,309.7 µs | 6,509.8 µs | 20,004 ops |
| iii | r0 | 124.9 µs | 218.6 µs | 260.8 µs | 796.6 µs | 1,087.7 µs | 2,715.8 µs | 6,980.2 µs | 20,003 ops |
| iii | r1 | 111.6 µs | 221.0 µs | 257.5 µs | 792.5 µs | 1,263.1 µs | 3,130.2 µs | 5,245.9 µs | 20,003 ops |
| v | r0 | 121.1 µs | 207.4 µs | 233.7 µs | 334.3 µs | 387.0 µs | 1,052.9 µs | 5,414.2 µs | 20,004 ops |
| v | r1 | 110.4 µs | 208.4 µs | 233.4 µs | 336.4 µs | 545.0 µs | 3,432.5 µs | 6,093.0 µs | 20,004 ops |
| vscan | r0 | 152.6 µs | 244.6 µs | 268.0 µs | 335.9 µs | 378.4 µs | 1,175.6 µs | 5,943.1 µs | 20,004 ops |
| vscan | r1 | 154.0 µs | 250.1 µs | 276.6 µs | 347.9 µs | 391.0 µs | 1,834.6 µs | 5,470.3 µs | 20,004 ops |
| iii4 | r0 | 165.7 µs | 257.5 µs | 285.1 µs | 379.3 µs | 551.7 µs | 3,328.8 µs | 6,387.9 µs | 20,003 ops |
| iii4 | r1 | 165.9 µs | 251.8 µs | 279.9 µs | 367.6 µs | 420.6 µs | 1,623.6 µs | 4,505.4 µs | 20,004 ops |

### 4.2 Hot sessions, service time (from the send)

| cell | run | p0 | p25 | p50 | p95 | p99 | p99.9 | max | n |
|---|---|---|---|---|---|---|---|---|---|
| i | r0 | 82.7 µs | 139.2 µs | 164.3 µs | 257.3 µs | 302.8 µs | 1,478.7 µs | 7,075.8 µs | 20,003 ops |
| i | r1 | 87.5 µs | 141.9 µs | 161.4 µs | 257.3 µs | 341.2 µs | 3,439.9 µs | 6,425.9 µs | 20,004 ops |
| ii | r0 | 70.3 µs | 127.3 µs | 146.6 µs | 235.1 µs | 281.3 µs | 484.5 µs | 1,800.8 µs | 20,004 ops |
| ii | r1 | 70.1 µs | 139.2 µs | 166.4 µs | 277.5 µs | 351.9 µs | 2,359.4 µs | 6,145.0 µs | 20,004 ops |
| iii | r0 | 68.7 µs | 138.3 µs | 171.9 µs | 704.5 µs | 985.4 µs | 2,394.2 µs | 6,837.6 µs | 20,003 ops |
| iii | r1 | 72.0 µs | 142.8 µs | 169.8 µs | 700.0 µs | 1,095.1 µs | 2,656.5 µs | 5,134.1 µs | 20,003 ops |
| v | r0 | 70.3 µs | 129.0 µs | 149.3 µs | 242.0 µs | 288.5 µs | 501.2 µs | 2,153.4 µs | 20,004 ops |
| v | r1 | 73.0 µs | 131.4 µs | 149.3 µs | 239.6 µs | 299.9 µs | 2,212.5 µs | 5,980.7 µs | 20,004 ops |
| vscan | r0 | 88.0 µs | 166.2 µs | 183.6 µs | 240.6 µs | 265.6 µs | 355.2 µs | 1,581.0 µs | 20,004 ops |
| vscan | r1 | 104.2 µs | 171.4 µs | 190.5 µs | 248.9 µs | 276.8 µs | 775.3 µs | 4,047.4 µs | 20,004 ops |
| iii4 | r0 | 100.6 µs | 177.1 µs | 200.0 µs | 277.3 µs | 338.1 µs | 2,443.1 µs | 5,331.0 µs | 20,003 ops |
| iii4 | r1 | 96.6 µs | 172.1 µs | 196.2 µs | 271.1 µs | 311.4 µs | 997.3 µs | 4,434.8 µs | 20,004 ops |

### 4.3 Throughput the hot sessions achieved

Derived: `n / 20.0 s window`. Offered: 1,000 reads/s.

| cell | run | hot reads completed |
|---|---|---|
| i, ii, iii, v, vscan, iii4 | r0 and r1 | 1,000.2 reads/s (20,003 or 20,004 ops in 20 s): the offered rate, every run |
| iv | r0 | 488.9 reads/s (9,777 ops) |
| iv | r1 | 544.4 reads/s (10,887 ops) |

The six cells other than iv carried the whole offered load; no cell but iv
saturated the hot sessions.

### 4.4 DE-Q1 (a)'s test, applied literally

The test: **is the never-miss sessions' p99 in (iii) inside the spread of
(v)?** (v)'s two runs give a p99 of 387.0 and 545.0 µs from the schedule
(288.5 and 299.9 µs service). (iii)'s give 1,087.7 and 1,263.1 µs
(985.4 and 1,095.1 µs service). **Both (iii) runs are above both (v) runs, so
(iii) is outside (v)'s spread, and the order's C0 does not apply.** The
matched-run ratios of (iii) to (v) are 2.81 and 2.32 from the schedule, and
3.42 and 3.65 in service (derived). The weakest comparison is the noisiest
resident run of any cell, i r1 at 888.6 µs: (iii)'s lower run clears it by
1.22x (derived). The p95 is the sturdier witness: (iii)'s is 792.5-796.6 µs
from the schedule and 700.0-704.5 µs in service, against 326.9-380.8 µs and
235.1-277.5 µs in every resident run of cells i, ii, v, with a run-to-run
ratio of at most 1.16 inside a cell (ii r1 over r0 at p95, derived).

### 4.5 DE-Q1's three numbers

**(1) The excess of (iii) over (ii)** (same run index; `iii - ii`, absolute and
ratio, derived from the two tables above):

| hot percentile | basis | r0: (iii) - (ii) | r0 ratio | r1: (iii) - (ii) | r1 ratio |
|---|---|---|---|---|---|
| p50 | from schedule | +30.5 µs | 1.13x | +1.2 µs | 1.00x |
| p95 | from schedule | +469.7 µs | 2.44x | +411.7 µs | 2.08x |
| p99 | from schedule | +704.3 µs | 2.84x | +683.5 µs | 2.18x |
| p50 | service | +25.3 µs | 1.17x | +3.4 µs | 1.02x |
| p95 | service | +469.4 µs | 3.00x | +422.5 µs | 2.52x |
| p99 | service | +704.1 µs | 3.50x | +743.2 µs | 3.11x |

The p50's two runs disagree (1.13x and 1.00x) and (ii)'s own p50 moves 230.3
to 256.3 µs between its runs, so **no p50 delta is claimed**. The p95 and p99
deltas are present in both runs, in both bases, at +412 to +470 µs and +684 to
+743 µs.

**(2) The fraction of the core's time inside `io.page_read`**
(`read_share_of_one_core`, the window's `io.page_read` total over 20 s): in
(iii) **27.52 % and 27.01 %** (5,503,246 and 5,402,777 µs over 9,990 and
10,001 reads); in (iii4) 28.39 % and 28.65 % (section 7); in (ii), where the
page cache answered, 0.22 % and 0.24 %.

**(3) The device's own read time.** The probe: 2,000 `O_DIRECT` 8 KiB reads at
queue depth 1 at random page offsets of the data file, made once before the
cells (`summary.json`):

| p0 | p25 | p50 | p95 | p99 | p99.9 | max | n |
|---|---|---|---|---|---|---|---|
| 5.3 µs | 412.3 µs | 462.9 µs | 712.7 µs | 989.6 µs | 1,587.4 µs | 2,381.1 µs | 2,000 reads |

The server's own mean per counted read in (iii) is **550.9 µs and 540.2 µs**,
and 98.5 % and 98.2 % of its reads fall in the histogram buckets 8 and 9
([256, 1,024) µs); section 6 has the histogram. The server's read is a
buffered `pread` into a frame under a memory-limited scope, the probe an
`O_DIRECT` read with nothing else running, so the two are not the same
operation and their 77-88 µs gap between the server's mean and the probe's p50
is not attributed here.

These are the order's three numbers. The order's close adds a fourth
condition, that the resident cells be no slower outside their spread after a
change (DE-R4); this stage measures before, and the resident cells' own
spread is in 4.1 and 4.2. **No build, narrow or nothing ruling is made here.**

## 5. What the cold sessions saw

| cell | run | p0 | p25 | p50 | p95 | p99 | p99.9 | max | n |
|---|---|---|---|---|---|---|---|---|---|
| ii | r0 | 73.4 µs | 132.3 µs | 151.4 µs | 242.0 µs | 292.8 µs | 554.6 µs | 1,673.9 µs | 12,966 ops |
| ii | r1 | 79.9 µs | 142.8 µs | 170.0 µs | 285.4 µs | 359.3 µs | 2,585.6 µs | 6,961.6 µs | 12,966 ops |
| iii | r0 | 81.5 µs | 513.6 µs | 663.3 µs | 954.2 µs | 1,403.1 µs | 3,161.4 µs | 5,323.6 µs | 12,965 ops |
| iii | r1 | 82.7 µs | 506.2 µs | 649.0 µs | 955.3 µs | 1,633.9 µs | 3,156.9 µs | 5,916.4 µs | 12,965 ops |
| v | r0 | 78.2 µs | 129.5 µs | 148.8 µs | 241.8 µs | 293.3 µs | 519.3 µs | 1,427.4 µs | 12,966 ops |
| v | r1 | 75.1 µs | 131.6 µs | 150.7 µs | 241.8 µs | 311.6 µs | 2,723.9 µs | 6,321.9 µs | 12,966 ops |
| iii4 | r0 | 104.7 µs | 573.9 µs | 727.2 µs | 1,015.4 µs | 1,706.4 µs | 3,838.8 µs | 6,070.6 µs | 12,965 ops |
| iii4 | r1 | 104.2 µs | 562.0 µs | 722.6 µs | 1,012.8 µs | 1,541.9 µs | 4,270.8 µs | 8,678.0 µs | 12,965 ops |

Service time, from the send. The cold sessions' p0 (73-105 µs) is a read that
hit the pool; their p25 jumps from 129-143 µs with the cache answering to
506-514 µs with it kept off, because 77 % of their reads miss (9,990 and
10,001 misses over 12,965 reads, derived). The same percentiles from the
schedule's due instant are in the JSON (`cold_from_schedule`), e.g. (iii) p50
744.8 and 731.5 µs. The cold sessions throughput is 648.3 reads/s in (ii) and
(v) and 648.2 in (iii) and (iii4) (derived, `n / 20 s`), the offered rate.

## 6. Where the time goes: waits, and proof the cache did not answer

### 6.1 The wait events of the window

**What the decomposition is, and is not.** `sys.wait_stats` is cumulative
per event, and the driver takes its difference across the 20 s window
(`waits_delta`). It decomposes the **reactor's time**, not one hot request:
a hot request's extra wait while another session's `pread` is in flight is
not an event of its own on today's engine, so a per-request latency
breakdown into "waiting behind a read" is **not measurable here**. The only
event that rises by the amount of the stall is `io.page_read`, and it is
recorded against the session that read.
`cpu.on_cpu`'s count is 0 in every JSON (the event records time only), and
its total is summed over every session. `sched.run_queue` is the wait after
a task is runnable, summed over sessions, so it can exceed the wall time.
The `sysio.*` rows are the engine's own file I/O (checkpoint, log), eight
events or fewer in a cell at `cores = 1`.

| cell | run | `cpu.on_cpu` total | `io.page_read` count | `io.page_read` total | mean per read (derived) | share of one core | `sched.run_queue` count / total | `sysio.checkpoint` count / total | `sysio.wal_write` count / total |
|---|---|---|---|---|---|---|---|---|---|
| i | r0 | 0.559 s | 0 reads | 0.000 s | - | 0.00 % | 20,019 waits / 0.022 s | 8 / 11.3 ms | 8 / 8.7 ms |
| i | r1 | 0.537 s | 0 reads | 0.000 s | - | 0.00 % | 20,019 waits / 0.027 s | 8 / 12.0 ms | 8 / 8.9 ms |
| ii | r0 | 0.766 s | 9,972 reads | 0.044 s | 4.4 µs | 0.22 % | 32,981 waits / 0.053 s | 8 / 11.1 ms | 8 / 6.6 ms |
| ii | r1 | 0.913 s | 9,984 reads | 0.048 s | 4.8 µs | 0.24 % | 32,981 waits / 0.078 s | 8 / 13.4 ms | 8 / 8.0 ms |
| iii | r0 | 0.853 s | 9,990 reads | 5.503 s | 550.9 µs | 27.52 % | 32,981 waits / 0.208 s | 8 / 13.1 ms | 8 / 8.7 ms |
| iii | r1 | 0.858 s | 10,001 reads | 5.403 s | 540.2 µs | 27.01 % | 32,981 waits / 0.242 s | 8 / 14.0 ms | 6 / 5.1 ms |
| iv | r0 | 1.085 s | 395,144 reads | 18.564 s | 47.0 µs | 92.83 % | 18,561 waits / 40.154 s | 8 / 49.4 ms | 5 / 16.9 ms |
| iv | r1 | 1.075 s | 395,720 reads | 18.591 s | 47.0 µs | 92.96 % | 18,579 waits / 40.384 s | 8 / 44.4 ms | 4 / 12.0 ms |
| v | r0 | 0.758 s | 0 reads | 0.000 s | - | 0.00 % | 32,981 waits / 0.053 s | 8 / 10.1 ms | 8 / 7.1 ms |
| v | r1 | 0.762 s | 0 reads | 0.000 s | - | 0.00 % | 32,981 waits / 0.063 s | 8 / 9.7 ms | 8 / 7.9 ms |
| vscan | r0 | 19.266 s | 0 reads | 0.000 s | - | 0.00 % | 610,432 waits / 1.786 s | 8 / 5.4 ms | 8 / 5.0 ms |
| vscan | r1 | 19.216 s | 0 reads | 0.000 s | - | 0.00 % | 590,143 waits / 1.942 s | 8 / 8.4 ms | 8 / 4.7 ms |
| iii4 | r0 | 1.635 s | 9,994 reads | 5.678 s | 568.2 µs | 28.39 % | 33,026 waits / 0.069 s | 32 / 53.5 ms | 32 / 16.8 ms |
| iii4 | r1 | 1.600 s | 9,997 reads | 5.729 s | 573.1 µs | 28.65 % | 33,025 waits / 0.052 s | 32 / 53.2 ms | 32 / 17.2 ms |

(iii4's counts are the instance's, summed over its four cores, and the
`sysio` counts are four times (i)'s because each core carries its own.)
The mean `sched.run_queue` wait (derived, total over count) is 1.1-1.3 µs in
(i), 1.6-2.4 µs in (ii) and 1.6-1.9 µs in (v), **6.3 and 7.4 µs in (iii)**, and
2,163 and 2,174 µs in (iv). In (iii) the run-queue total rises by 0.13 to 0.19
s over (ii) while the reactor spent 5.4-5.5 s in reads: **most of the reactor's
read time is not visible as run-queue wait**, which is the sense in which the
engine's own events do not show a hot session being held up by a cold
session's read (above). I did not check in the source where a request that
arrives during a blocking call is first counted.

The reactor's own accounting agrees on the whole: `sched_foreground_polled_us`
(the time its foreground tasks ran, from `meta_delta`) is 0.79-0.84 s in (i),
1.16-1.38 s in (ii), **6.66-6.77 s in (iii)** and 19.85 s in (iv) of a
20.0 s window; `sched_idle_block_us` falls from 18.3-19.0 s in (i) and (ii)
to 12.9-13.0 s in (iii) and 0.0001 s in (iv). In (iii) the reactor is busy
for about a third of the window and 5.4-5.5 s of that is inside the read.
(For iii4 the `meta_delta` is the control session's core and is not used.)

### 6.2 The pool, and what reached the device

| cell | run | pool hits | pool misses | `/proc/<pid>/io` `read_bytes` | `device_bytes_per_read` (derived: `read_bytes / counted reads`) |
|---|---|---|---|---|---|
| i | r0 | 140,011 hits | 0 misses | 0 B | no reads counted |
| i | r1 | 140,011 hits | 0 misses | 0 B | no reads counted |
| ii | r0 | 220,773 hits | 9,972 misses | 0 B | 0.0 B |
| ii | r1 | 220,761 hits | 9,984 misses | 0 B | 0.0 B |
| iii | r0 | 220,755 hits | 9,990 misses | 82,575,360 B | 8,265.8 B |
| iii | r1 | 220,737 hits | 10,001 misses | 82,665,472 B | 8,265.7 B |
| iv | r0 | 86,810 hits | 395,144 misses | 3,241,820,160 B | 8,204.1 B |
| iv | r1 | 86,873 hits | 395,720 misses | 3,246,784,512 B | 8,204.8 B |
| v | r0 | 230,738 hits | 0 misses | 0 B | no reads counted |
| v | r1 | 230,745 hits | 0 misses | 0 B | no reads counted |
| vscan | r0 | 37,927,389 hits | 0 misses | 0 B | no reads counted |
| vscan | r1 | 36,628,735 hits | 0 misses | 0 B | no reads counted |
| iii4 | r0 | 220,742 hits | 9,994 misses | 82,518,016 B | 8,256.8 B |
| iii4 | r1 | 220,750 hits | 9,998 misses | 82,583,552 B | 8,260.8 B |

**The page cache answered (ii) and did not answer (iii), (iv), (iii4).** In
(ii) the kernel's `read_bytes` is 0 B over 9,972 and 9,984 counted reads: every
one was served from the page cache (mean 4.4 and 4.8 µs). In (iii), (iv) and
(iii4) the kernel charged the process 8,2xx bytes for every counted read, i.e.
one 8 KiB page from the device per counted read to within 0.9 %
(8,204-8,266 B). The limit's own state at the end of each limited cell:

| cell | run | `memory.max` | `RssAnon` at the end | `memory.stat` `file` at the end | file share of the data file (derived) | `memory.events` `max` | oom / oom_kill |
|---|---|---|---|---|---|---|---|
| iii | r0 | 339,910,656 B | 267,400 kB | 62,660,608 B | 5.9 % | 313 | 0 / 0 |
| iii | r1 | 340,324,352 B | 267,372 kB | 63,053,824 B | 6.0 % | 332 | 0 / 0 |
| iv | r0 | 340,398,080 B | 267,340 kB | 63,094,784 B | 6.0 % | 13,753 | 0 / 0 |
| iv | r1 | 339,906,560 B | 272,520 kB | 57,294,848 B | 5.4 % | 14,926 | 0 / 0 |
| iii4 | r0 | 341,598,208 B | 273,692 kB | 57,602,048 B | 5.5 % | 322 | 0 / 0 |
| iii4 | r1 | 345,919,488 B | 274,200 kB | 61,394,944 B | 5.8 % | 339 | 0 / 0 |

The scope sat at its ceiling (`memory.current` 339,890,176 B against
`memory.max` 339,910,656 B in iii r0), the kernel reclaimed against it
(`max` events, cumulative since the scope started, so the pre-warm walk is in
them), and nobody was killed. The scope keeps about 6 % of the data file's
pages, so a read can still be answered from the cache for about 6 % of the
file's pages; the `device_bytes_per_read` above says the number of such answers
was small enough not to show.

### 6.3 The `io.page_read` histogram

`io_hist_delta`, summed over cores: bucket k counts reads of [2^k, 2^(k+1)) µs.

| cell | run | reads by bucket k: count | total |
|---|---|---|---|
| ii | r0 | 1: 4,351; 2: 5,506; 3: 108; 4: 6; 7: 1 | 9,972 reads |
| ii | r1 | 1: 2,826; 2: 6,980; 3: 162; 4: 11; 5: 3; 8: 1; 9: 1 | 9,984 reads |
| iii | r0 | 2: 2; 8: 4,425; 9: 5,412; 10: 129; 11: 21; 12: 1 | 9,990 reads |
| iii | r1 | 1: 1; 2: 1; 8: 4,987; 9: 4,832; 10: 153; 11: 25; 12: 2 | 10,001 reads |
| iv | r0 | 0: 255,764; 1: 110,011; 2: 2,218; 3: 525; 4: 87; 5: 2,622; 6: 3,177; 7: 640; 8: 5,130; 9: 9,914; 10: 4,809; 11: 239; 12: 5; 13: 3 | 395,144 reads |
| iv | r1 | 0: 254,248; 1: 111,962; 2: 2,343; 3: 540; 4: 75; 5: 2,714; 6: 3,246; 7: 646; 8: 5,329; 9: 9,414; 10: 4,937; 11: 263; 12: 3 | 395,720 reads |
| iii4 | r0 | 1: 1; 2: 1; 8: 3,655; 9: 6,172; 10: 144; 11: 20; 12: 2 | 9,995 reads |
| iii4 | r1 | 1: 1; 2: 1; 8: 3,563; 9: 6,239; 10: 177; 11: 13; 12: 4 | 9,998 reads |

(The histogram's totals differ by up to four from the counted reads in 6.1
because the two views are read at slightly different instants.) The
page-cache-answered reads of (ii) sit in [2, 8) µs. (iii)'s sit in [256,
1,024) µs, 98.5 % and 98.2 %; the longest bucket is 12, [4,096, 8,192) µs,
with 1 to 4 reads. (v), (vscan) and (i) record no `io.page_read` in the
window.

### 6.4 Did the hot sessions miss inside the window?

`session_waits_end` is each session's **lifetime** `io.page_read` count,
read through `sys.session_waits` after the window. Session 1 is the control
session, whose pre-walk read the relation (128,573 counted reads in all 14 runs).
The hot sessions are
the four small counts; the cold sessions in (ii), (iii) and (iii4) the four
of about 3,000.

| cell | run | hot sessions' lifetime reads (four sessions) | cold sessions' lifetime reads | scanner's | window pool misses |
|---|---|---|---|---|---|
| i | r0 | 18, 24, 27, 33 | none | none | 0 |
| i | r1 | 18, 24, 27, 33 | none | none | 0 |
| ii | r0 | 18, 24, 27, 33 | 3,016, 3,035, 3,055, 3,056 | none | 9,972 |
| ii | r1 | 18, 24, 27, 32 | 3,016, 3,031, 3,063, 3,065 | none | 9,984 |
| iii | r0 | 18, 24, 27, 33 | 3,026, 3,035, 3,057, 3,061 | none | 9,990 |
| iii | r1 | 18, 24, 27, 32 | 3,025, 3,042, 3,055, 3,065 | none | 10,001 |
| iii4 | r0 | 6, 7, 8, 10 | 3,027, 3,038, 3,051, 3,066 | none | 9,994 |
| iii4 | r1 | 2, 12, 14, 16 | 3,030, 3,042, 3,056, 3,063 | none | 9,998 |
| iv | r0 | 0, 0, 0, 3 | none | 441,991 | 395,144 |
| iv | r1 | 0, 0, 0, 0 | none | 477,834 | 395,720 |
| v | r0 | 0, 0, 0, 0 and 46, 47, 48, 48 (eight sessions, roles not recoverable, below) | | | 0 |
| v | r1 | 0, 0, 0, 0 and 46, 47, 48, 48 (as above) | | | 0 |
| vscan | r0, r1 | 0, 0, 0, 0 (and the scanner at 0) | none | 0 | 0 |

**What it proves.** In cell i, where the window's pool misses are 0, the
four hot sessions' counts (18, 24, 27, 33, i.e. 102 reads in all) all
happened before the window, in the warm-up when the quarter pool's leaves for
keys 1..700 were first touched; the sum is of the order of the roughly 100
leaves that 700 rows of 6.98 rows a page occupy (derived). Cells ii and iii
show the same four counts in r0, and 18, 24, 27, 32 (one fewer than i r1's
33) in r1. **No hot session in ii or iii read more pages in 20 s of window
than it had in cell i's warm-up**: a read in the window would have raised a
count, and none did. So the hot sessions' p95/p99 in (iii) is not their own
misses.

**What it does not prove.** It is a lifetime count, so by itself it cannot
separate the window from the warm-up: it is the equality with cell i, whose
window misses are 0 by the pool counters, that carries the claim. It says
nothing about a hot request's wait behind another session's read (that wait
is not an event). In iii4 the counts are not equal to cell i's (2 to 16 a
session, a different set of sessions on different cores touching leaves at
warm-up), so only the comparison is lost there, not the fact that the counts
are tiny against 20,000 reads each; I did not investigate why they differ. In
(iv) the hot sessions' counts are 0 or 3 (against 18-33 in i); I did not
investigate that either (the scanner, started in the same second, walks from
page 1, where keys 1..700 are, and may have faulted those leaves in first; this
is a hypothesis, not tested). In (v) the JSON does not record a session's
role, and the sessions' counts split 0, 0, 0, 0 and 46-48 (x4) with window
misses 0; which are the hot ones cannot be read from the JSON and the
window's pool misses of 0 are what carries the claim there.

## 7. The `cores = 4` cell, iii4

iii4 is (iii) at `cores = 4` with the cold sessions declared
`SET PRIORITY analytic` (BL-S4's partition holds them on core 0, per the
driver's design; the JSON does not record the cold sessions' own core) and
the hot sessions on cores 1, 2, 3 (`hot_by_core`: sessions on core 1 are two,
n = 10,001 and 10,002; on cores 2 and 3 one each, n = 5,001).

| iii4 | core | p0 | p25 | p50 | p95 | p99 | p99.9 | max | n |
|---|---|---|---|---|---|---|---|---|---|
| r0 | 1 (2 sessions) | 165.7 µs | 250.1 µs | 283.0 µs | 382.2 µs | 735.8 µs | 4,272.0 µs | 6,387.9 µs | 10,001 ops |
| r0 | 2 | 184.3 µs | 259.2 µs | 281.8 µs | 368.4 µs | 435.4 µs | 1,484.6 µs | 3,354.1 µs | 5,001 ops |
| r0 | 3 | 207.4 µs | 265.8 µs | 292.1 µs | 383.4 µs | 462.5 µs | 2,456.9 µs | 6,352.2 µs | 5,001 ops |
| r1 | 1 (2 sessions) | 165.9 µs | 237.7 µs | 272.3 µs | 364.1 µs | 408.9 µs | 914.6 µs | 3,829.2 µs | 10,002 ops |
| r1 | 2 | 201.9 µs | 259.2 µs | 282.3 µs | 357.9 µs | 402.9 µs | 1,033.3 µs | 4,182.6 µs | 5,001 ops |
| r1 | 3 | 193.6 µs | 264.6 µs | 290.4 µs | 381.7 µs | 478.0 µs | 1,915.5 µs | 4,505.4 µs | 5,001 ops |

(From the schedule's due instant.) The hot sessions' p95 of 364.1-383.4 µs
is inside the resident cells' range (326.9-380.8 µs for i, ii, v; 383.4 is 2.6
µs above it, derived), and their p99 of 402.9-735.8 µs overlaps (v)'s
387.0-545.0 µs except r0's core 1 (735.8 µs, 1.35x (v)'s 545.0, derived), the one iii4 figure that reaches outside. **At
`cores = 4` with the cold sessions on another core, the hot sessions' p95
and p99 do not show the stall that (iii) shows**, so the stall is a
same-core effect, not a same-instance one. Three things limit that reading:

1. There is no `cores = 4` resident baseline. The floor of iii4's hot
   sessions is higher than at `cores = 1`: p0 is 165.7-207.4 µs against
   110.4-131.4 µs in i, ii, v, and p50 272.3-292.1 µs against 230.3-260.8 µs.
   A resident `cores = 4` cell would be the comparison, and it was not run.
2. The clients did not sit on siblings of the reactors' CPUs. `--client-cpus`
   was left at its default `2,3,6,7` and the `cores = 4` reactors run on
   CPUs 0-3, so the clients assigned to CPUs 2 and 3 share those CPUs with the
   reactors of cores 2 and 3, and CPUs 6 and 7 are their SMT siblings. This
   is a limit of the cell and a plausible part of its higher floor; it was
   not tested.
3. Two runs, and a single hot-session p99 moves by as much as 1.4x between runs
   of one cell here (up to 2.14x, section 10).

The cold sessions' reads cost more in iii4 (mean 568.2 and 573.1 µs, p50
727.2 and 722.6 µs service) than in iii (550.9 and 540.2 µs, 663.3 and 649.0
µs); the difference is 3 to 6 % in the mean and about 10 % in the p50, which I
do not attribute (clients on shared CPUs may be a part of it).

## 8. The scan cells, iv against vscan

### 8.1 Hot sessions in iv

From the schedule's due instant, in milliseconds because the unit is
seconds; service in the second table.

| cell | run | p0 | p25 | p50 | p95 | p99 | p99.9 | max | n |
|---|---|---|---|---|---|---|---|---|---|
| iv, from schedule | r0 | 4,206.31 ms | 5,720.84 ms | 7,215.78 ms | 9,928.12 ms | 10,167.42 ms | 10,224.27 ms | 10,240.60 ms | 9,777 ops |
| iv, from schedule | r1 | 2,398.59 ms | 4,078.30 ms | 5,758.08 ms | 8,783.74 ms | 9,051.76 ms | 9,111.96 ms | 9,125.05 ms | 10,887 ops |
| iv, service | r0 | 1.60 ms | 6.12 ms | 6.45 ms | 7.50 ms | 7.84 ms | 9.36 ms | 14.82 ms | 9,777 ops |
| iv, service | r1 | 0.14 ms | 6.10 ms | 6.44 ms | 7.54 ms | 8.14 ms | 12.15 ms | 16.27 ms | 10,887 ops |

**The hot sessions are 2.4-4.2 s behind their schedule at the best case and 5.8-7.2 s at
the median** - of the requests that were sent: the client stops at the
window's end, so the 46-51 % of due requests never sent are not in these
percentiles, which are therefore a lower bound and grow with the window's
length rather than describe a latency - they complete 9,777 and 10,887 of the 20,004 requests due
(48.9 % and 54.4 %, derived), and their service p50 is 6.45 and 6.44 ms. The
service time stays flat (p25 6.10-6.12 ms, p95 7.50-7.54 ms) while the
schedule runs away: the signature of a server whose per-request service time
has risen past the open-loop arrival interval. Four serial sessions at 6.44 ms
carry about 621 reads/s (derived, 4 / 6.44 ms) against the 1,000 offered, which
is why the backlog grows through the window. The service best case is 0.14
and 1.60 ms; the typical request was nowhere near it.

### 8.2 The scanner

The scanner's own client-side samples are not recorded (the driver kills it at
the end of the window; the four `WARN ... cancelled by request` lines in the
server logs (a scanner's walk ended by its connection being killed, consistent
with that kill) are not engine errors, and
every client's `errors` is 0 in every JSON). Its figures come from the
server's wait events.

| cell | run | counted reads | mean per read | pages/s (derived, over 20 s) | rate at 8,192 B a page (derived) | share of one core in `io.page_read` | 64-page slice (derived, 64 x mean) | walks of the relation (derived, over 128,960 pages) |
|---|---|---|---|---|---|---|---|---|
| iv | r0 | 395,144 reads | 47.0 µs | 19,757 pages/s | 161.9 MB/s | 92.83 % | 3.01 ms | 3.06 |
| iv | r1 | 395,720 reads | 47.0 µs | 19,786 pages/s | 162.1 MB/s | 92.96 % | 3.01 ms | 3.07 |

The kernel's `read_bytes` over the window is 162.1 MB/s and 162.3 MB/s
(derived, `read_bytes / 20 s`), the same rate to within 0.2 %.

The histogram of the scanner's reads (6.3) is bimodal:

| iv run | reads under 2 µs (bucket 0) | reads under 4 µs (buckets 0-1) | reads of 32 µs or more (buckets 5 and up) | time in those slow reads, lower bound (bucket floors) |
|---|---|---|---|---|
| r0 | 255,764 reads = 64.7 % | 365,775 reads = 92.6 % | 26,539 reads = 6.7 % | 12.22 s of 18.56 s = at least 65.8 % |
| r1 | 254,248 reads = 64.2 % | 366,210 reads = 92.5 % | 26,552 reads = 6.7 % | 12.17 s of 18.59 s = at least 65.4 % |

Two-thirds of the counted reads return in under 2 µs and are charged one
device page each (`read_bytes` per read, 8,204 B), so most of them were
already in the kernel's cache when the `pread` was issued, which is
consistent with the kernel's read-ahead having fetched them (an inference
from the rate and the byte count, not observed). The 6.7 % that block carry
at least 65.4-65.8 % of the read time, so the core is held in a handful of
long reads, not spread over many short ones.

**The size of the problem is the slice.** The scan yields every 64 pages
(`docs/spec/sched.md` §3, "A `SELECT`'s outermost walk yields every 64
pages"); in (iv) that is 3.01 ms of mostly blocking time. In (vscan), with the
relation resident, 592,616 and 572,324 slices of 64 pages are counted by the
pool's hits (`pool_hits / 64`, derived; consistent with the 590,413 and
570,124 run-queue waits that are not the hot sessions', derived), at 19.2-19.3 s of
`cpu.on_cpu` for the window, i.e. about 32-34 µs a slice (derived, and
assuming one run-queue wait per slice). **A cold 64-page slice is therefore
about 90 times a resident one (3.01 ms over about 33 µs, derived).** The hot
sessions' service p50 in (iv) of 6.44 ms is about twice the 3.01 ms slice
(derived), which is consistent with a hot statement waiting for one slice in
flight and one more before it is polled again; the mechanism was not
instrumented.

### 8.3 vscan, the ceiling

With the relation resident the same scanner leaves the hot sessions'
p95/p99 where cells i and v have them: p95 335.9 and 347.9 µs from the
schedule (v: 334.3, 336.4; i: 353.3, 359.1), p99 378.4 and 391.0 µs (v:
387.0, 545.0; i: 415.1, 888.6), service p99 265.6 and 276.8 µs (v: 288.5,
299.9). **The p50 and the p0 are not where cells i and v have them**: hot p50
from the schedule is 268.0 and 276.6 µs against 233.7 and 233.4 µs in (v)
(14.7 % and 18.5 % higher) and 251.1 and 247.7 µs in (i) (6.7 % and 11.7 %
higher); in service 183.6 and 190.5 µs against 149.3 and 149.3 µs in (v), and
164.3 and 161.4 µs in (i); p0 152.6 and 154.0 µs against 121.1 and 110.4 µs
in (v). A resident slice of about 33 µs (derived above) is of the order of
that shift, but the cause was not isolated, and with two runs a cell a 7-12 %
difference against (i) is near the spread of (ii)'s own p50 (230.3 to 256.3
µs, 11 %). The hot sessions completed the offered 1,000.2 reads/s in (vscan)
and 488.9-544.4 in (iv).

## 9. Baseline

**The baseline is this engine's own last run of the same shape, and there
is none.** I searched `bench/` (the whole tree: `grep` for `io.page_read` and
`page_read`; for `O_DIRECT`, `fadvise`, `DONTNEED`, `memory.max`, `MemoryMax`;
and for the relation name `de_big`) and read the files the order's §1.4
names and two the search found:

- `bench/v3.0.0/results-be-close-v2.7.0-665-geef442cf.md`: relations 2x and
  4x the pool through a warm page cache, as the order's §1.4 describes it (the shape of cell ii, not iii; I did not re-read its tables);
- `bench/v3.2.0/results-cc-close-prior-v3.2.0-186-g1a9168d3.md`: "cold" there means "a
  cold engine pool, not a cold device" (the order's §1.4's quotation);
- `bench/v3.4.6/results-cj-close-v3.4.6-16-gf17bdf81.md`: carries
  `io.page_read` only as a row of zeros in an overhead table;
- `bench/v3.0.0/results-br-close-backup-restore-v2.7.0-913-ga8110b05.md`: the
  disk's write rate and no read cell.

None runs a working set larger than the pool with the page cache kept from
answering and none prices a hot session beside a cold one, so **this file
becomes the baseline the next measurement of the shape is read against**,
cited as `v3.4.13-19-g35086008`. No number from another shape is set beside
these. **PostgreSQL, the floor, was not measured for this shape**: no twin was
written for this stage.

## 10. What was cut, the noise, and the budget

**Noise floor, from inside the run.** There are two runs a cell and so
"spread" below is a two-point range, no confidence interval is claimed. The
hot sessions' p99 moves between the two runs of **one cell** by up to 2.14x
from the schedule (cell i: 415.1 against 888.6 µs), 1.51x in ii, 1.41x in v,
1.31x in iii4, 1.16x in iii, 1.03x in vscan, and in service by up to 1.25x
(ii: 281.3 against 351.9 µs). The p99.9 and the max move more: the max of the
hot sessions from the schedule is 5.2-7.2 ms in every cell at `cores = 1` but iv,
including cell i, which has no cold session at all, and the p99.9 is
484.5-3,439.9 µs in service (1,052.9-3,647.3 µs from the schedule) across the
resident cells, so iii's 2,394.2-2,656.5 µs at p99.9 in service (2,715.8-3,130.2
from the schedule) and its max are **not
distinguishable from the resident cells'**. Which claims survive:

| claim | survives a p99 spread of 2x? |
|---|---|
| (iii)'s hot p95 is 2.1-3.0x (ii)'s | yes: p95 moves at most 1.16x inside a cell; both runs, both bases |
| (iii)'s hot p99 is above every resident run's | yes, narrowly against i r1 (1.22x), clearly against ii, v, vscan (at least 1.88x against ii r1) |
| (iii)'s hot p50 is unchanged | not resolved either way: +1.2 to +30.5 µs against a 26 µs spread |
| (iii)'s hot p99.9 and max | **no**: inside the resident cells' range |
| (iii4)'s hot p99 matches (v)'s | **no**: r0's core 1 is 735.8 µs; no `cores = 4` baseline exists |
| vscan's hot p95/p99 match i's and v's | yes at p95 and p99; the p50 is higher and not resolved (8.3) |
| iv: hot service p50 of 6.4 ms and a seconds-deep backlog | yes: two orders of magnitude over every other cell |
| the reactor's share inside `io.page_read` | yes: 27.0 % and 27.5 %, 92.8 % and 93.0 %: within 0.6 percentage points inside a cell |

**One relation size, one pool ratio, one device, one cold rate.** Rule 9
(sweep 200, 1K and 10K rows) was **not applied**: the stage's variable is the
relation against the pool, and it ran at 128,960 pages against 32,768 frames
(25.4 %) and against 160,000 (all of it), at 900,000 rows; the 20-minute
budget did not hold a sweep. A fixed cost and a per-row cost are therefore not
separated here, and the share inside `io.page_read` follows the cold rate,
which is a function of the miss ratio (here 77 % of the cold reads missed) and
the device. The cold rate (648.1 reads/s) was derived from **this run's** probe
(p50 462.9 µs x 0.3); **an earlier, aborted attempt's probe differed (682.3
reads/s from a p50 of 439.7 µs)**, so the rate is not a fixed constant across
invocations, and a re-run on a differently-behaving disk will offer a
different load.

**The page cache was not wholly off.** The 64 MiB slack lets the scope keep
about 6 % of the data file's pages (5.4-6.0 %, 6.2).

**Discarded.** A first attempt at 08:28-08:31 UTC was aborted after cells i
and ii by a 5 s client timeout on the cold pre-warm walk (the driver now
raises `KDS_CLIENT_TIMEOUT`); its two cells were discarded, not merged. The
load (58.0 s) was done in that attempt and reused; the data file is the same.

**iii4**: no baseline, clients on CPUs 2,3,6,7 (7, item 2).

**Time spent**, against the stage's 20-minute budget (CLAUDE.md "A
measurement has a time budget"; the budget counts building):

| step | time |
|---|---|
| Release build | 8 min 15 s (08:11:25-08:19:40 UTC) |
| driver smoke tests on a 70 MiB relation | about 2.5 min |
| the aborted attempt | 2 min 50 s |
| the run that counts, including its probe | 9 min 18 s (`total_s` 557.6 s) |
| **total** | **about 23 min, about 3 min over the 20-minute stage budget** |

The build waited about 4 minutes for another session's measurement to finish
and the run waited about 5 minutes for the load average to fall under 1.5
(this figure is from the session's hand-off, not from the JSON, whose
`wait_s` accounts for 70 s inside the run). By the 2026-11-18 rule a quiet
gate's hold is outside the budget; those holds are stated here and not
subtracted from the 23 minutes above. The budget was exceeded by about three
minutes and is not rounded away.

**Tests.** No engine code was changed and none was run for this file; the
stage built and ran no suite.

## 11. What the run says about the engine

**A miss on this engine holds the core, and the sessions that did not miss
pay at their tail.** At `v3.4.13-19-g35086008`, with the page cache not
answering, the reactor sits in `io.page_read` for 27.0-27.5 % of the window
at 540.2-550.9 µs a read (cell iii), and the hot sessions' p95 rises by
412-470 µs and their p99 by 684-743 µs over the same cell with the page cache
answering (ii) (4.5). That excess is of the order of the read itself (the
probe's p50, 462.9 µs; its p95 712.7 µs, p99 989.6 µs), which is what a
blocking call on a shared thread predicts: a hot request that arrives while the
reactor is inside a `pread` waits for the rest of that read. I state the
resemblance, not a model; the order's mechanism is a `pread` on the reactor
thread (§1.1) and this run supports its being the visible cost, with the
reservation that the engine has no event that records the wait itself.

**The p50 does not move, because three requests in four do not meet a read.**
With the reactor in a read 27 % of the time, about a quarter of the hot
requests arrive during one (an expectation from the share, not a measured
fraction), so a p95 and a p99 move and a p50 does not. This is why a single
number, p50 or mean, would have called this a null result, and why DE-Q1
asks for the tail.

**A warm page cache hides the whole effect.** In (ii) the same cold sessions
miss the pool at the same rate (9,972 and 9,984 misses against 9,990 and
10,001) and the hot sessions are no slower, beyond the spread, than in (v), a
pool that holds the relation; `read_bytes` is 0 and a read costs 4.4-4.8 µs,
0.22-0.24 % of a core. The read-side structures' benefits and every
standard cell of this suite that runs resident or through a warm page cache
(the order's §1.4) measure the (ii) world. This is also why the miss costs
what it does here: it is the device's latency, on a cloud disk whose 8 KiB
read at queue depth 1 is 463 µs at the median, and not the engine's own
work: the mean `io.page_read` falls from 540-551 µs to 4.4-4.8 µs when the
kernel answers, a factor of about 118 (derived, 545 over 4.6), and everything
the engine adds to a miss (a frame, an eviction, a checksum) is inside the 4.4
µs; eviction was off the reactor, `pool_reclaimed_inline` is 0 and
`pool_reclaimed_background` 8,236-8,253 in every cell with cold sessions.

**The scan is a different size of problem because the unit that yields is a
page count, not a time.** The reactor yields a walk every 64 pages
(`docs/spec/sched.md` §3), which bounds a hold at about 33 µs when the pages
are resident (derived) and at 3.01 ms when they are not (derived): the same
code, 90 times the hold, and the hot sessions' service p50 goes from 183.6 and
190.5 µs (vscan) to 6.45 and 6.44 ms (iv), their schedule falling 2.4 to 10.2
seconds behind. A point-read miss holds the core for one read of about 0.5 ms;
a scan holds it for 64 reads, 93 % of the core in total, and it does so
whether or not another session is asking for the core. The design's yield
point is stated in pages and was set against resident pages; the first data on
a cold one is this cell. Two features of the scan are specific to it: the
kernel's read-ahead (inferred) makes two-thirds of the scan's reads cost under
2 µs, so the mean (47 µs) understates the blocking read (a read of 256-1,024
µs, 15,044 and 14,743 of them, derived from buckets 8 and 9 in r0 and r1),
and the stall left to remove after read-ahead is the 6.7 % that block, 65 % or
more of the read time.

**What the run does not say.** It does not say a build is worth it: the
order reserves that for the operator and the numbers above are the three DE-Q1
asks for. It does not say the stall is the same at another cold rate, pool
ratio, relation size or device (one of each was run); it does not say what
the hot sessions' tail would be at `cores = 4` with the cache answering
(never run); and its p99.9 and max do not resolve the stall (10). The hot
sessions' warm p99 itself differs by up to 2x between two runs of one cell, so
the finding is the p95 and the p99 of cell iii against ii, v and vscan, and
the scan cell's collapse, and nothing finer.
