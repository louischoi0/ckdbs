# Recovery at mount, and what the var-heap now pays for it

Recovery runs at every mount as of the RC series, and two costs came with it:
the log grew a `PAGE_INIT` plus an 8 KiB `FULL_PAGE_IMAGE` every time a
var-heap chain grows, and a server no longer starts without reading its log
back. This run prices both, plus the completion checkpoint each mount ends
with, against the commit immediately before the series.

The short version. **The write path's added latency is at or below the noise
floor at every size measured, but its added WAL volume is not: an
INSERT-with-spill writes 1.77-1.96× the log bytes it used to, and an
UPDATE-with-spill writes 10-46× — because it previously wrote none of its
value at all, which was the defect.** That volume is not free: WAL bytes
decide how often a client waits **487 ms** for a new 64 MiB segment to be
created, and at the sizes measured here the change moves that from two stalls
to three. **The mount cost is real and large — 140 ms against 50 ms before —
and it is proportional to the number of segment bytes between the recovery
anchor and the end of the segment, not to how much was logged.** An empty log
is therefore the *worst* case, which is the opposite of the intuition, and
`kDefaultSegmentSize` has quietly become a startup-latency knob.

---

## How this was measured

| | |
|---|---|
| Date executed | 2026-08-12, 05:00-06:15 UTC |
| Branch | `main` (worktree `works-known-gaps`) |
| Commit measured | **`0af3b32`** ("Merge remote-tracking branch 'origin/main' into recovery-at-mount", committed 04:42:14 UTC) |
| Baseline commit | **`ff86bfd`**, the commit before the series (committed 03:42:40 UTC) |
| Tree cleanliness | **Clean at `0af3b32` when both binaries were built and when the suite ran.** It did **not** stay clean, and it did not stay at `0af3b32`: from 05:22 onwards a concurrent session in this same worktree modified 15 engine files — `src/server/command_dispatcher.cpp` among them, extracting a `LogFullPageImage` helper out of the very path measured here — and landed them as **`5ae7ebf`** ("fix: the review's findings on the recovery series") before this file was finished. So `git rev-parse HEAD` in this worktree no longer answers `0af3b32`. Every number below describes `0af3b32`; nothing here has been measured on `5ae7ebf` |
| Binary provenance, `0af3b32` | `build-release/kds_server`, linked 05:00:55 UTC — 18 minutes after the commit and 21 minutes before the first concurrent edit. `libkds.a` at 04:39:37 was verified current: no `src/`, `include/` or `sim/` file was newer than the archive |
| Binary provenance, `ff86bfd` | `/home/cdkbs/base-ff86bfd/build-release/kds_server`, linked 05:03:06 UTC from a `git archive` export of `ff86bfd` — a separate tree, so the worktree's checkout was never disturbed |
| Build type | **Release** (`-O3 -DNDEBUG`) on both sides |
| Device | `/dev/nvme0n1p1`, ext4, non-rotational (MSFT NVMe Accelerator). `$HOME` and `/tmp` are both on it — **this box has no tmpfs**, checked with `findmnt`; data files were still put under `$HOME` per convention. One 64 MiB segment creation costs 487 ms here (measured; see Part I) |
| Host | 2 cores, 15 GiB. **Shared with other agent sessions for the whole run** — see "What the host was doing" below |
| Server configuration | `cores = 1`, `placement = creating`, `inline_cell_width = 64`, `checkpoint_interval_ms = 5000`, `wal_drain_interval_us = 1000`, `relaxed_flush_interval_us = 10000`, `log_level = warn`, WAL segment size 64 MiB (`wal::kDefaultSegmentSize`, not configurable). `durability` is the varied knob and is named per table |
| Data files | Fresh file **and** fresh server per configuration, under `$HOME/varheap-bench` and `$HOME/mount-bench` |
| Correctness suite | `scripts/test.sh` at `0af3b32`, clean tree, finished 05:00 UTC — 22 minutes before the first concurrent edit: **2253 passed, 0 failed**, 117.31 s (Debug, which is what that script builds; nothing in `tests/` was skipped or filtered) |
| Drivers | `tools/varheap_spill_benchmark.py` and `tools/mount_cost_benchmark.py`, both new here and both documented in `bench/docs/README.md`. `--verify` was on for every var-heap cell and passed in every one |

**On the two commits in one file.** This document describes the engine at
`0af3b32`. `ff86bfd` appears only as a *control binary*, run inside the same
window, on the same box, against the same fresh-file shapes, alternating cell
by cell with `0af3b32` — the same role `--base` plays in
`tools/order_by_benchmark.py`. There is no comparison here against a number
from an earlier document or an earlier day; every figure below was measured in
this run, and each row names the binary that produced it.

### What the host was doing

This matters more than usual, so it is stated up front rather than buried.
The box is shared with other agent sessions, and during this run they
variously compiled, ran the full `ctest` suite in this worktree's Debug tree,
and drove a `kds_server` of their own on port 15432. The 1-minute load average
therefore rose from 0.24 to 2.4 across the measurement window, and each cell
records the load it actually saw in its JSON.

Three things were done about it rather than hoped away:

1. **Cells were run in immediately-adjacent A/B pairs** — `0af3b32` then
   `ff86bfd`, back to back on the same data-file shape — so slow drift lands
   on both sides equally. The driver's own load guard was overridden with
   `--force` for exactly this reason: pausing between cells to let the average
   decay would have given the first cell of each pair a quieter box than the
   second, which is the one asymmetry an A/B must not have.
2. **Every configuration was run twice** (`run 1`, `run 2`), which is the
   in-run noise floor. It is large: the *control* phase, which cannot be
   affected by any change here, moves by up to 9 µs of p50 between two runs of
   the same binary.
3. **The mount cells were taken in a verified quiet window** — no `ctest`, no
   compiler, for 90 consecutive seconds beforehand — which is why their spread
   is 0.3% on the baseline side.

The consequence for reading this document: a var-heap latency delta smaller
than ~9 µs is not a result here, and is reported as "inside the floor" rather
than as a number. WAL byte counts and mount times are unaffected by this — the
first is deterministic, the second was measured quiet.

---

## Part I — the var-heap write path

### What the change does per statement

`CommandDispatcher::LogSpills` is now called by both INSERT and UPDATE, and
per spilled value it emits:

| condition | records emitted | added bytes |
|---|---|---|
| the tail var-heap page has room | `VARHEAP_APPEND` only | value + ~40 |
| the tail is full (chain grows) | `PAGE_INIT` (kVarHeap) + `FULL_PAGE_IMAGE` of the linked tail + `VARHEAP_APPEND` | value + 8192 + ~80 |
| UPDATE, either case | the same — where `ff86bfd` emitted **nothing** | the whole of it |

A page holds 8144 bytes of values and a value costs `len + 4` of it, so the
value length sets the growth rate. Two lengths were measured: **1600 bytes**
(growth every 5 rows, the realistic case) and **8100 bytes** (growth every
row, the maximum FPI rate the engine can be made to pay).

### The WAL volume is where the cost landed, and it is exact

These are byte counts, not timings: the log's write position was read before
and after each phase. They are identical across repeat runs to the byte.

| value length | phase | `0af3b32` B/statement | `ff86bfd` B/statement | ratio |
|---|---|---|---|---|
| 1600 | `insert-spill` | 3764 | 2122 | **1.77×** |
| 1600 | `insert-inline` (control) | 478 | 478 | 1.00× |
| 1600 | `update-spill` | 3655 | 361 | **10.13×** |
| 1600 | `update-inline` (control) | 361 | 361 | 1.00× |
| 8100 | `insert-spill` | 16886 | 8626 | **1.96×** |
| 8100 | `insert-inline` (control) | 478 | 478 | 1.00× |
| 8100 | `update-spill` | 16777 | 361 | **46.51×** |
| 8100 | `update-inline` (control) | 361 | 361 | 1.00× |

Measured at 1000 rows, `relaxed`, one connection; the two controls are
identical on both builds to the byte, which is what says nothing outside the
spill path moved.

Read the UPDATE rows carefully, because "46×" is not a regression in the
ordinary sense. At `ff86bfd` an `update-spill` statement wrote **361 bytes** —
exactly what `update-inline` writes, which is the undo and heap records and
nothing else. Its 8100-byte value went to the var-heap and into no log record
at all. The ratio is the size of the hole that was there, not the size of the
new cost: the correct comparison is against the value the statement is
obliged to make durable, and 16777 bytes for an 8100-byte value plus the
8192-byte image of the page it filled is close to the floor for that
obligation.

The INSERT rows are the honest overhead question, and the answer is
`+1642 B/row` at 1600 bytes and `+8260 B/row` at 8100 bytes — in both cases
`8192/rows_per_page` plus a `PAGE_INIT`, exactly as designed. There is no
waste in it and no accidental repetition; the FPI is simply large.

### The latency, at the realistic value length

`relaxed` durability, so no commit fsync is in the way and any added CPU shows.
`insert-inline` is the control: it never touches a var-heap page, so the change
cannot reach it.

| rows | phase | build | ops | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|---|
| 200 | `insert-spill` | `0af3b32` run 1 | 200 | 35.8 | 42.8 | 44.3 | 64.2 | 97.1 | 6276 |
| 200 | `insert-spill` | `0af3b32` run 2 | 200 | 37.1 | 42.1 | 43.8 | 63.4 | 85.3 | 3624 |
| 200 | `insert-spill` | `ff86bfd` run 1 | 200 | 33.0 | 41.9 | 43.3 | 58.2 | 77.5 | 4354 |
| 200 | `insert-spill` | `ff86bfd` run 2 | 200 | 31.5 | 32.7 | 33.5 | 46.8 | 63.6 | 3634 |
| 200 | `update-spill` | `0af3b32` run 1 | 200 | 36.8 | 46.4 | 49.6 | 78.0 | 183.2 | 658 |
| 200 | `update-spill` | `0af3b32` run 2 | 200 | 36.0 | 37.9 | 39.6 | 59.3 | 95.0 | 118 |
| 200 | `update-spill` | `ff86bfd` run 1 | 200 | 36.3 | 45.9 | 47.5 | 66.7 | 111.5 | 507 |
| 200 | `update-spill` | `ff86bfd` run 2 | 200 | 35.8 | 37.6 | 43.0 | 58.7 | 81.2 | 103 |
| 200 | `insert-inline` | `0af3b32` run 1 | 200 | 25.6 | 26.0 | 27.7 | 47.2 | 76.3 | 167 |
| 200 | `insert-inline` | `0af3b32` run 2 | 200 | 26.1 | 26.7 | 26.9 | 36.3 | 55.4 | 556 |
| 200 | `insert-inline` | `ff86bfd` run 1 | 200 | 25.7 | 26.5 | 34.1 | 46.4 | 64.0 | 178 |
| 200 | `insert-inline` | `ff86bfd` run 2 | 200 | 25.9 | 26.2 | 26.5 | 40.0 | 58.0 | 431 |
| 1000 | `insert-spill` | `0af3b32` run 1 | 1000 | 32.5 | 42.2 | 44.1 | 62.4 | 110.3 | 3831 |
| 1000 | `insert-spill` | `0af3b32` run 2 | 1000 | 32.3 | 35.8 | 42.0 | 59.8 | 90.2 | 3929 |
| 1000 | `insert-spill` | `ff86bfd` run 1 | 1000 | 32.3 | 40.4 | 43.8 | 62.2 | 90.1 | 3793 |
| 1000 | `insert-spill` | `ff86bfd` run 2 | 1000 | 33.2 | 42.4 | 44.2 | 60.7 | 483.9 | 3748 |
| 1000 | `update-spill` | `0af3b32` run 1 | 200 | 38.2 | 40.3 | 47.1 | 78.1 | 140.2 | 1125 |
| 1000 | `update-spill` | `0af3b32` run 2 | 200 | 37.6 | 39.7 | 40.8 | 59.2 | 113.2 | 210 |
| 1000 | `update-spill` | `ff86bfd` run 1 | 200 | 37.6 | 47.8 | 49.7 | 65.6 | 95.5 | 164 |
| 1000 | `update-spill` | `ff86bfd` run 2 | 200 | 45.4 | 47.6 | 49.2 | 62.9 | 86.2 | 721 |
| 1000 | `insert-inline` | `0af3b32` run 1 | 1000 | 25.7 | 26.4 | 33.0 | 42.4 | 56.5 | 171 |
| 1000 | `insert-inline` | `0af3b32` run 2 | 1000 | 25.2 | 26.0 | 26.9 | 41.0 | 52.7 | 139 |
| 1000 | `insert-inline` | `ff86bfd` run 1 | 1000 | 24.5 | 26.0 | 34.5 | 46.3 | 65.8 | 1966 |
| 1000 | `insert-inline` | `ff86bfd` run 2 | 1000 | 25.3 | 33.3 | 35.6 | 46.9 | 72.1 | 1006 |
| 10000 | `insert-spill` | `0af3b32` run 1 | 10000 | 32.3 | 48.3 | 55.0 | 82.0 | 127.7 | 32820 |
| 10000 | `insert-spill` | `0af3b32` run 2 | 10000 | 33.0 | 49.7 | 58.1 | 82.1 | 127.6 | 45477 |
| 10000 | `insert-spill` | `ff86bfd` run 1 | 10000 | 31.8 | 48.9 | 55.1 | 81.1 | 175.3 | 54538 |
| 10000 | `insert-spill` | `ff86bfd` run 2 | 10000 | 31.9 | 49.1 | 57.4 | 80.1 | 99.0 | 55671 |
| 10000 | `update-spill` | `0af3b32` run 1 | 200 | 66.4 | 74.8 | 76.9 | 97.6 | 205.1 | 451 |
| 10000 | `update-spill` | `0af3b32` run 2 | 200 | 65.4 | 68.9 | 75.6 | 114.5 | 215.1 | 1086 |
| 10000 | `update-spill` | `ff86bfd` run 1 | 200 | 65.6 | 71.4 | 76.7 | 98.7 | 148.3 | 295 |
| 10000 | `update-spill` | `ff86bfd` run 2 | 200 | 67.0 | 75.7 | 78.1 | 103.3 | 160.8 | 208 |
| 10000 | `insert-inline` | `0af3b32` run 1 | 10000 | 25.1 | 26.5 | 34.8 | 44.2 | 56.0 | 11809 |
| 10000 | `insert-inline` | `0af3b32` run 2 | 10000 | 25.1 | 26.1 | 34.9 | 44.1 | 55.7 | 10951 |
| 10000 | `insert-inline` | `ff86bfd` run 1 | 10000 | 25.1 | 26.7 | 35.4 | 44.7 | 55.3 | 11352 |
| 10000 | `insert-inline` | `ff86bfd` run 2 | 10000 | 25.4 | 26.2 | 26.7 | 40.7 | 56.2 | 14786 |

All times µs. Rows are the size sweep; `--rows` **is** the row count, and the
update phases run 200 statements at every size so the work compared is equal.

Nothing here is a finding. The largest p50 gap on `insert-spill` is 10.8 µs
(200 rows, `0af3b32` run 1 at 44.3 against `ff86bfd` run 2 at 33.5) and the
same pair of runs differ by 7.6 µs on the *control*, where no var-heap record
exists. At 10,000 rows the two builds agree to 1 µs of p50 in both directions.
`update-spill` shows `0af3b32` **faster** than `ff86bfd` at 1000 rows, which
is by itself proof that the floor is wider than the effect. At the realistic
value length, this change costs no measurable per-statement time.

### The latency, with a page filled on every row

To make the FPI's cost as visible as the engine allows, the same matrix at
8100-byte values: one `PAGE_INIT`, one 8 KiB FPI and one 8100-byte append per
statement.

| rows | phase | build | ops | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|---|
| 200 | `insert-spill` | `0af3b32` run 1 | 200 | 58.6 | 70.2 | 73.4 | 113.9 | 143.2 | 3710 |
| 200 | `insert-spill` | `0af3b32` run 2 | 200 | 62.0 | 72.2 | 75.3 | 114.8 | 1512.4 | 3639 |
| 200 | `insert-spill` | `ff86bfd` run 1 | 200 | 59.0 | 69.5 | 72.8 | 103.1 | 143.4 | 4084 |
| 200 | `insert-spill` | `ff86bfd` run 2 | 200 | 63.0 | 69.3 | 72.0 | 154.4 | 1079.7 | 4008 |
| 200 | `update-spill` | `0af3b32` run 1 | 200 | 75.3 | 84.8 | 88.0 | 129.5 | 160.2 | 544 |
| 200 | `update-spill` | `0af3b32` run 2 | 200 | 77.3 | 86.3 | 90.3 | 136.5 | 230.3 | 778 |
| 200 | `update-spill` | `ff86bfd` run 1 | 200 | 71.3 | 83.3 | 87.0 | 120.9 | 189.5 | 1320 |
| 200 | `update-spill` | `ff86bfd` run 2 | 200 | 75.0 | 85.2 | 89.5 | 407.7 | 960.5 | 2470 |
| 200 | `insert-inline` | `0af3b32` run 1 | 200 | 25.6 | 26.1 | 26.5 | 43.4 | 95.8 | 160 |
| 200 | `insert-inline` | `0af3b32` run 2 | 200 | 25.9 | 33.3 | 34.1 | 47.9 | 68.7 | 522 |
| 200 | `insert-inline` | `ff86bfd` run 1 | 200 | 25.5 | 27.6 | 34.1 | 52.0 | 241.0 | 981 |
| 200 | `insert-inline` | `ff86bfd` run 2 | 200 | 29.5 | 33.7 | 35.3 | 48.5 | 228.4 | 1159 |
| 1000 | `insert-spill` | `0af3b32` run 1 | 1000 | 58.6 | 75.2 | 79.8 | 117.6 | 148.0 | 4670 |
| 1000 | `insert-spill` | `0af3b32` run 2 | 1000 | 62.5 | 74.7 | 79.8 | 120.1 | 184.7 | 3870 |
| 1000 | `insert-spill` | `ff86bfd` run 1 | 1000 | 61.4 | 72.7 | 76.8 | 109.3 | 496.6 | 4496 |
| 1000 | `insert-spill` | `ff86bfd` run 2 | 1000 | 59.5 | 72.1 | 77.4 | 114.4 | 180.3 | 4661 |
| 1000 | `update-spill` | `0af3b32` run 1 | 200 | 91.1 | 96.0 | 99.8 | 137.6 | 224.6 | 405 |
| 1000 | `update-spill` | `0af3b32` run 2 | 200 | 84.2 | 95.3 | 100.0 | 149.7 | 760.8 | 1201 |
| 1000 | `update-spill` | `ff86bfd` run 1 | 200 | 83.7 | 90.7 | 94.7 | 121.6 | 154.7 | 646 |
| 1000 | `update-spill` | `ff86bfd` run 2 | 200 | 81.2 | 88.2 | 91.8 | 130.4 | 875.5 | 1837 |
| 1000 | `insert-inline` | `0af3b32` run 1 | 1000 | 25.8 | 34.1 | 35.8 | 46.6 | 60.1 | 170 |
| 1000 | `insert-inline` | `0af3b32` run 2 | 1000 | 25.6 | 26.2 | 26.5 | 38.6 | 52.1 | 248 |
| 1000 | `insert-inline` | `ff86bfd` run 1 | 1000 | 26.3 | 35.6 | 36.4 | 47.4 | 53.9 | 202 |
| 1000 | `insert-inline` | `ff86bfd` run 2 | 1000 | 25.8 | 26.5 | 26.7 | 32.7 | 42.7 | 235 |
| 10000 | `insert-spill` | `0af3b32` run 1 | 10000 | 60.8 | 107.2 | 140.6 | 202.2 | 560.7 | 497443 |
| 10000 | `insert-spill` | `0af3b32` run 2 | 10000 | 62.9 | 111.3 | 168.4 | 1530.6 | 2376.9 | 479763 |
| 10000 | `insert-spill` | `ff86bfd` run 1 | 10000 | 58.0 | 126.5 | 158.9 | 1778.5 | 2441.6 | 489049 |
| 10000 | `insert-spill` | `ff86bfd` run 2 | 10000 | 60.5 | 135.6 | 171.9 | 1813.1 | 2632.5 | 504884 |
| 10000 | `update-spill` | `0af3b32` run 1 | 200 | 190.2 | 198.1 | 203.8 | 263.5 | 355.4 | 394 |
| 10000 | `update-spill` | `0af3b32` run 2 | 200 | 190.1 | 197.8 | 204.3 | 2062.4 | 2500.0 | 3179 |
| 10000 | `update-spill` | `ff86bfd` run 1 | 200 | 193.8 | 199.6 | 204.0 | 284.1 | 2009.8 | 2626 |
| 10000 | `update-spill` | `ff86bfd` run 2 | 200 | 194.0 | 213.3 | 255.2 | 1459.1 | 1640.6 | 1772 |
| 10000 | `insert-inline` | `0af3b32` run 1 | 10000 | 25.1 | 26.2 | 26.9 | 46.8 | 575.6 | 93248 |
| 10000 | `insert-inline` | `0af3b32` run 2 | 10000 | 25.2 | 27.5 | 30.4 | 84.6 | 1391.5 | 33986 |
| 10000 | `insert-inline` | `ff86bfd` run 1 | 10000 | 25.5 | 26.6 | 27.8 | 45.0 | 823.8 | 28769 |
| 10000 | `insert-inline` | `ff86bfd` run 2 | 10000 | 25.3 | 27.6 | 28.2 | 66.8 | 976.6 | 98912 |

The only place a signal comes through is `insert-spill` at 1000 rows, and it
is small: `0af3b32` reports p50 79.8 µs in **both** runs against `ff86bfd`'s
76.8 and 77.4, i.e. **+2.7 µs (+3.5%)**, while the control's within-pass bias
between the two builds is 0.6 µs and 0.2 µs. `update-spill` at the same size
is +5.1 to +8.2 µs of p50, which is the cost of logging a value that used to
be logged not at all. Everything else is inside the floor, and p0 is
statistically identical on both sides at every size — the added work is
per-growth, not a new fixed cost on the path.

That +2.7 µs is about what the arithmetic predicts: an 8 KiB FPI is one page
fetch plus two 8 KiB copies (encode, then into the log buffer), and this box
copies at roughly 8 GB/s from warm memory, so ~2 µs. At the realistic
1600-byte value the same cost is amortized over five rows — ~0.5 µs a
statement, well under a tenth of the noise floor, which is why it cannot be seen
in the previous table.

### Where the added volume does cost the client: segment creation

The 10,000-row rows above carry maxima of 480-505 **ms**, on both builds. That
is not the FPI; it is `FileLogDevice::CreateSegment`, which `posix_fallocate`s
a 64 MiB segment, then zero-prewrites it in 64 × 1 MiB `pwrite`s, then
`fsync`s the file and its directory — deliberately, so that no commit's fsync
pays an extent conversion. Timed directly on this device, that sequence is:

| operation | median of 3 | notes |
|---|---|---|
| create one 64 MiB WAL segment | **487 ms** | `posix_fallocate` + 64 × 1 MiB `pwrite` + `fsync` + directory `fsync` |

That is a table of one measurement, so it carries no percentiles. It happens
on the statement thread, which is why it appears as a half-second maximum in
the phase that triggers it.

The connection to this change is arithmetic. In the 8100-byte, 10,000-row
cell, `0af3b32` writes 169 MB of log where `ff86bfd` writes 86 MB, so the
directories left behind hold **three** segments against **two**:

| cell | WAL written by `insert-spill` | segments created | 487 ms stalls |
|---|---|---|---|
| 8100 B, 10,000 rows, `0af3b32` | 169.1 MB | 3 | 3 |
| 8100 B, 10,000 rows, `ff86bfd` | 86.4 MB | 2 | 2 |
| 1600 B, 10,000 rows, either | 37.9 / 21.3 MB | 1 | 1 |

So the accurate statement of this change's write-path cost is not "+3.5% on a
statement". It is: **the var-heap growth records roughly double the log volume
of a spill-heavy workload, and log volume is the thing that decides how often
a client waits half a second.** At 1600-byte values and 10,000 rows neither
build rolls a segment and the change is free; push the volume up and it buys
one extra 487 ms stall per 64 MiB of extra log.

### The waits inside one statement

Measured with the driver's `ping` arm (`SHOW META`: round trip, dispatch,
reply, no relation touched) at 1000 rows, `relaxed`, on `0af3b32`. p0 is used
rather than p50 because these particular cells ran at load 1.6-2.4 and the
body of the distribution is contaminated where the floor is not.

| wait | how it was isolated | 1600 B value | 8100 B value |
|---|---|---|---|
| client + socket round trip | `ping` p0 | 23.6 µs | 24.5 µs |
| statement work: parse, catalog bind, btree descent, tuple encode, heap write, WAL append | `insert-inline` p0 − `ping` p0 | 1.8 µs | 2.3 µs |
| var-heap append: chain walk, page write, `VARHEAP_APPEND`, and — at `0af3b32` — `PAGE_INIT` + FPI | `insert-spill` p0 − `insert-inline` p0 | 7.7 µs | 34.0 µs |
| durability / commit fsync | `strict` p0 − `relaxed` p0, same phase | ~945 µs | not measured at 8100 B |
| lock or conflict wait | — | **not applicable**: one connection, no contention, and this engine does not wait on a conflict — it aborts (`docs/txn.md`) |
| segment creation | the 487 ms measurement above | 0 at this size | 0 at this size (3 at 10,000 rows) |

Two readings worth stating. First, the round trip is 93% of an inline INSERT:
this driver measures what a client pays, and the engine is a sliver of it.
Second, the fsync is three orders of magnitude above everything else, which is
why the whole `strict` matrix below shows no delta at all.

### `strict` durability: the fsync buries it

| rows | phase | build | ops | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|---|
| 200 | `insert-spill` | `0af3b32` run 1 | 200 | 1007.0 | 1081.0 | 1156.2 | 2698.6 | 4304.2 | 5386 |
| 200 | `insert-spill` | `0af3b32` run 2 | 200 | 980.0 | 1061.5 | 1121.3 | 2789.8 | 3577.0 | 5497 |
| 200 | `insert-spill` | `ff86bfd` run 1 | 200 | 944.5 | 1077.5 | 1127.0 | 1968.3 | 2868.2 | 44611 |
| 200 | `insert-spill` | `ff86bfd` run 2 | 200 | 1026.1 | 1126.8 | 1179.1 | 3015.0 | 4401.4 | 9026 |
| 200 | `insert-inline` | `0af3b32` run 1 | 200 | 923.4 | 1071.2 | 1121.2 | 2394.2 | 3891.9 | 4907 |
| 200 | `insert-inline` | `0af3b32` run 2 | 200 | 918.5 | 999.2 | 1037.3 | 1689.9 | 1962.1 | 2123 |
| 200 | `insert-inline` | `ff86bfd` run 1 | 200 | 902.9 | 992.3 | 1025.7 | 1990.6 | 3655.4 | 27677 |
| 200 | `insert-inline` | `ff86bfd` run 2 | 200 | 975.5 | 1086.6 | 1148.3 | 2198.5 | 2927.9 | 3250 |
| 1000 | `insert-spill` | `0af3b32` run 1 | 1000 | 977.7 | 1098.4 | 1150.6 | 2545.2 | 3759.8 | 52753 |
| 1000 | `insert-spill` | `0af3b32` run 2 | 1000 | 967.3 | 1090.9 | 1163.2 | 2558.2 | 4524.7 | 7678 |
| 1000 | `insert-spill` | `ff86bfd` run 1 | 1000 | 971.7 | 1083.0 | 1149.9 | 2157.2 | 4126.7 | 19742 |
| 1000 | `insert-spill` | `ff86bfd` run 2 | 1000 | 930.3 | 1070.9 | 1128.4 | 2313.9 | 3562.3 | 32228 |
| 1000 | `insert-inline` | `0af3b32` run 1 | 1000 | 892.9 | 1021.2 | 1065.3 | 2098.0 | 3350.7 | 19237 |
| 1000 | `insert-inline` | `0af3b32` run 2 | 1000 | 910.6 | 1033.5 | 1083.0 | 2363.6 | 3750.5 | 36042 |
| 1000 | `insert-inline` | `ff86bfd` run 1 | 1000 | 917.4 | 1024.1 | 1067.1 | 2321.9 | 4043.8 | 39800 |
| 1000 | `insert-inline` | `ff86bfd` run 2 | 1000 | 883.1 | 1011.7 | 1062.9 | 2098.4 | 3362.5 | 44448 |
| 10000 | `insert-spill` | `0af3b32` run 1 | 10000 | 919.6 | 1084.6 | 1154.2 | 2344.9 | 3958.3 | 69835 |
| 10000 | `insert-spill` | `0af3b32` run 2 | 10000 | 911.7 | 1069.0 | 1136.7 | 1449.3 | 2090.5 | 68915 |
| 10000 | `insert-spill` | `ff86bfd` run 1 | 10000 | 926.1 | 1064.5 | 1117.6 | 1912.7 | 3088.3 | 67992 |
| 10000 | `insert-spill` | `ff86bfd` run 2 | 10000 | 894.6 | 1054.3 | 1101.8 | 1505.7 | 2594.7 | 86772 |
| 10000 | `insert-inline` | `0af3b32` run 1 | 10000 | 864.5 | 1015.9 | 1071.7 | 2003.3 | 3096.4 | 49778 |
| 10000 | `insert-inline` | `0af3b32` run 2 | 10000 | 857.0 | 959.6 | 998.5 | 1213.6 | 2035.6 | 209932 |
| 10000 | `insert-inline` | `ff86bfd` run 1 | 10000 | 853.1 | 958.9 | 998.4 | 1223.3 | 1866.2 | 73961 |
| 10000 | `insert-inline` | `ff86bfd` run 2 | 10000 | 860.4 | 994.8 | 1042.6 | 1895.5 | 3221.0 | 46061 |

`strict` cells ran at load 2.6-2.9, the busiest part of the window, and their
run-to-run p50 spread is 40 µs — larger than any credible effect. Nothing is
resolvable here and nothing is claimed. What the table is good for is the
scale: 1.1 ms of p50 against 44 µs at `relaxed`, so a durable INSERT's cost is
96% device and 4% everything else, and the extra 1642 bytes ride along in a
flush that was happening anyway.

---

## Part II — what a mount now costs

Nine mounts of one data file per cell, alternating builds, in a verified quiet
window. Wall time is `exec` to the server's own "listening on" line; the
attribution columns are `SHOW META`'s counters, which `ff86bfd` does not have
— its cells report `-`, which is not the same as zero.

### The headline: 50 ms became 140 ms

| cell | log state | ops | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| `ff86bfd` run 1 | empty | 9 | 48.49 | 49.15 | **49.48** | 55.34 | 55.34 | 55.34 |
| `ff86bfd` run 2 | empty | 9 | 47.07 | 49.29 | **49.65** | 59.52 | 59.52 | 59.52 |
| `0af3b32` run 1 | empty | 9 | 134.56 | 137.58 | **140.47** | 167.48 | 167.48 | 167.48 |
| `0af3b32` run 2 | empty | 9 | 128.88 | 131.92 | **132.48** | 154.40 | 154.40 | 154.40 |

All times ms. Nine mounts is a small sample, so p95 and p99 collapse onto the
maximum — stated rather than hidden. The noise floor is 0.17 ms of p50 on the
baseline and 8.0 ms on `0af3b32`; the delta is **+83 to +91 ms**, ten times
the larger floor.

### Where those milliseconds go

Per-phase medians over the steady-state mounts of each cell:

| cell | anchor sits at | body scanned | open + catalog + listener | analysis | redo | high-water | undo | completion checkpoint | p50 wall |
|---|---|---|---|---|---|---|---|---|---|
| `0af3b32`, empty log | 5 KB | 64.00 MiB | 50.6 | 41.6 | 40.3 | 0.0 | 0.0 | 8.0 | 140.5 |
| `0af3b32`, 200-row crash log | 0.45 MiB | 63.55 MiB | 46.0 | 39.9 | 40.1 | 0.0 | 0.0 | 7.0 | 133.1 |
| `0af3b32`, 2000-row crash log | 7.06 MiB | 56.94 MiB | 49.2 | 35.9 | 35.3 | 0.0 | 0.0 | 7.7 | 128.0 |
| `0af3b32`, 10000-row crash log | 35.87 MiB | 28.13 MiB | 53.1 | 17.8 | 15.5 | 0.0 | 0.0 | 7.4 | 93.8 |
| `ff86bfd`, empty log | n/a | none | 49.5 | — | — | — | — | — | 49.5 |

All times ms; the "open + catalog + listener" column is the residual, p50 wall
minus the five counters. **It lands within 4 ms of `ff86bfd`'s entire mount in
every row**, which is the accounting closing: `0af3b32`'s mount is the old
mount plus recovery, with nothing unexplained in between.

Two counters are flat zero in every cell and are worth naming rather than
skipping: the high-water repair and undo. Neither has work to do here — the
crash cells' losers were already zero (`recovery_rolled_back=0`, because a
single-connection loader leaves no in-flight transaction at SIGKILL), and no
page needed its high water raised.

### The scan cost is bytes, not records

The four `0af3b32` rows above are a sweep of one variable: how far into the
64 MiB segment the recovery anchor sits, and therefore how much segment body
`ScanLog` reads. Dividing:

| cell | body scanned | analysis | ms per MiB | records scanned at the steady mount |
|---|---|---|---|---|
| empty log | 64.00 MiB | 41.6 ms | 0.650 | 2 |
| 200-row crash log | 63.55 MiB | 39.9 ms | 0.628 | 2 |
| 2000-row crash log | 56.94 MiB | 35.9 ms | 0.630 | 2 |
| 10000-row crash log | 28.13 MiB | 17.8 ms | 0.632 | 2 |

0.63 ms per MiB, constant to 4%, over a 2.3× range of segment body — and the
record count is 2 in every row. **The cost is the segment span, not the log's
contents.** The same holds on the redo scan (0.63, 0.63, 0.62, 0.55 ms/MiB).

An independent measurement of what that span costs, on the same file and the
same device:

| operation | 64 MiB | notes |
|---|---|---|
| fresh 64 MiB allocation + zero-fill | 30.3 ms | 0.47 ms/MiB — what `body.assign(body_bytes, std::byte{0})` pays, mostly first-touch page faults |
| `pread` of 64 MiB into that buffer, page cache warm | 8.4 ms | 0.13 ms/MiB |
| sum | 38.7 ms | **0.60 ms/MiB**, against 0.63 measured inside the mount |

So the mechanism is confirmed, with one correction to how it has been
described: **the read is the cheap part.** Roughly 78% of a scan is
allocating and zeroing a buffer that the very next syscall overwrites. Two
scans do it twice.

### Against `docs/known-gaps.md`

That file records this as a performance finding: "`recovery_analysis_us≈44000`
+ `recovery_redo_us≈42000` — 86 ms of a ~90 ms mount, reproduced on three
consecutive mounts."

- **The 86 ms reproduces.** 41.6 + 40.3 = 81.9 ms of medians on an empty log,
  with individual mounts up to 49.3 + 60.0 ms. Same phenomenon, same size.
- **The "~90 ms mount" does not.** A mount of an empty log here is 132-140 ms
  of p50, and 49.5 ms of it belongs to work that predates recovery entirely —
  measured directly by running `ff86bfd`'s server against its own empty file.
  Recovery is **64%** of a mount, not 96%. The gap entry should say so; as
  written it implies a mount was ~90 ms before the change too, when the
  measured before-figure is 49.5 ms.
- **"reads each WAL segment's whole body, twice" is right but understates
  it.** It reads from the *anchor* to the segment's end, twice — which is why
  a nearly-full segment mounts in 94 ms and an empty one in 140 ms — and the
  dominant term is the buffer's zero-fill rather than the read.

---

## Part III — the completion checkpoint, across mounts

RC08 has each mount publish an anchor past everything it replayed. Whether
that pays for itself cannot be seen in a single mount, which is the reason it
previously measured as cost-neutral: one mount pays the checkpoint and never
collects the cheaper scan. Nine mounts of one file collect it.

| log written before the sweep | mount 1 wall | mount 1 records / redone / checkpoint | mounts 2-9 p50 wall | mounts 2-9 records / checkpoint | saved from mount 2 on |
|---|---|---|---|---|---|
| 200 rows, SIGKILL | 142.7 ms | 701 / 441 / 10.3 ms | 133.1 ms | 2 / 7.0 ms | **9.6 ms** |
| 2000 rows, SIGKILL | 184.2 ms | 10,703 / 6,767 / 41.7 ms | 128.0 ms | 2 / 7.7 ms | **56.2 ms** |
| 10000 rows, SIGKILL | 336.9 ms | 54,319 / 6,162 / 172.2 ms | 93.8 ms | 2 / 7.4 ms | **243.1 ms** |
| 2000 rows, clean stop | 187.0 ms | 10,883 / 0 / 44.3 ms | 125.4 ms | 2 / 7.6 ms | **61.6 ms** |

**The benefit is real, and it grows with the log the crash left**: 3.6× at
10,000 rows. It is also not mainly the record replay. Breaking mount 1 of the
10,000-row cell against its own steady state: analysis 51.1 → 17.8 ms, redo
61.8 → 15.5 ms, checkpoint 172.2 → 7.4 ms. Two thirds of the 243 ms saved is
the *checkpoint* mount 1 had to write for the pages redo dirtied; the anchor's
own contribution is that mounts 2-9 scan 2 records instead of 54,319 and start
28 MiB from the segment's end instead of at its beginning.

The recurring cost is visible in the same table: **7.0-8.0 ms of checkpoint on
every mount, forever**, plus ~2.5-4.3 KB of log (an `ff86bfd` mount writes 36
bytes; an `0af3b32` mount writes 4338). Against 9.6-243 ms saved on the next
mount, that is a good trade at every size measured, and it is the smallest of
the five recovery counters.

One finding falls out of the last row, and it is not about cost. **A clean
shutdown does not publish an anchor.** The cleanly stopped 2000-row cell's
first mount scans 10,883 records — every record the run wrote — and redoes
none of them, because the pages were already flushed. It then writes a 44.3 ms
checkpoint and only from that point on are its mounts cheap. So the anchor is
a product of *recovery*, not of shutdown: stopping a server politely does not
save its successor any work.

---

## Versus PostgreSQL

**No comparison was measured, and no twin was run: PostgreSQL is not installed
on this host.** There is no `pg_ctl`, `initdb`, `psql` or `postgres` on
`PATH`, no `/usr/lib/postgresql`, and `tools/pg_setup.sh status` fails at
`pg_ctl: command not found`, so the port-15433 scratch cluster the other
results files compare against cannot be created here. This is stated rather
than skipped, and it is the one requirement of a results file this document
does not meet.

Two twins would be needed, and both are worth building because both questions
have a sharp PostgreSQL answer:

1. **`tools/pg_varheap_spill_benchmark.py`** — the same four phases at the
   same value lengths and the same three row counts, importing the phase
   shapes from `tools/varheap_spill_benchmark.py`. It would price the var-heap
   against TOAST, and it can report WAL volume *exactly* where this document's
   ckdbs figures are the position of the last non-zero byte:
   `pg_current_wal_lsn()` deltas around each phase. The comparison is not a
   like-for-like structure and that is the interest of it — at defaults
   PostgreSQL keeps a 1600-byte value in the heap tuple (compressing it first)
   and TOASTs an 8100-byte one, and it writes a full-page image once per page
   per checkpoint cycle rather than once per link edit, so "how many log bytes
   does one 8 KB string cost" is a genuinely different answer on the two
   engines. Cluster tuning stays at PostgreSQL defaults.
2. **A startup twin** — `pg_ctl start` to first accepted connection, on a
   clean shutdown and after `kill -9`, over the same nine-mount shape. This is
   the baseline that says whether 140 ms is a lot. PostgreSQL starts its
   redo at the last checkpoint's redo pointer and reads to the end of WAL,
   which is precisely the design this document finds ckdbs does *not* have:
   ckdbs reads to the end of the **segment**. A twin would turn that
   structural difference into a number.

---

## What this run says about the engine

**The FPI is not a CPU problem, it is a log-volume problem, and log volume is
a latency problem through one specific door.** Doubling the bytes a spill
writes costs +2.7 µs at the worst growth rate the engine can be given and
nothing measurable at a realistic one. But 64 MiB of extra log is one extra
487 ms segment creation on the statement thread, and that is two orders of
magnitude more than the FPI's own cost. If the var-heap write path is ever
optimized, the reason will be segment-creation frequency, not memcpy.

**A mount's cost is proportional to the distance from the anchor to the end of
the current segment, which makes an empty log the worst case.** This inverts
the usual reading of a recovery cost, and it has a direct consequence for an
open decision: `docs/wal.md` §15 leaves segment sizing open, and this run is
the first data point that segment size is a **startup-latency** parameter and
not only a space/rotation one. The measured law is 0.63 ms per MiB of segment
span, twice per mount, over a 2.3× range — so at the default 64 MiB a fresh
instance spends 82 ms of scanning to discover that its log is empty, and that
figure is set by `kDefaultSegmentSize` and by nothing else in the workload.
No size is argued for here: the fix `known-gaps` already names — read only as
far as the durable end — removes the coupling altogether and is the better
answer. But the sizing decision can no longer be made without this number.

**Three quarters of the scan is zeroing memory nobody reads.** `ScanLog`'s
`body.assign(body_bytes, std::byte{0})` costs 0.47 ms/MiB against the
`pread`'s 0.13. Whoever implements the streaming read should size the buffer
once and reuse it across segments and across the two passes; that alone is
most of the win, and it is smaller than the change that bounds the read.

**RC08's anchor is worth what it costs, and the evidence needed more than one
mount to exist.** 7-8 ms and 4 KB per mount buys 9.6 ms at 200 rows and 243 ms
at 10,000. But it only ever fires on the recovery path, so a cleanly stopped
server hands its successor a full re-scan — if that is not intended, the
place to fix it is shutdown, and it would be measurable the same way.

**The pre-existing per-append chain walk is the var-heap's real scaling
problem, and this run makes it visible.** `varheap::ChainAppend` walks from
the chain root to the tail on **every** append with no tail cache, so
`insert-spill` p25 rises 71.7 → 107-135 µs between 1000 and 10,000 rows at
8100-byte values, where the inline control does not move at all. It is
identical on both builds, so it is not this change's — but it is why the FPI's
+2.7 µs is unresolvable at 10,000 rows, and it will grow without bound as a
relation's var-heap chain does. `docs/known-gaps.md` does not carry it; on the
evidence here it should.

**One thing the numbers do not support.** The `strict` matrix cannot say
anything about this change, and the `10000`-row cells of the relaxed matrix
cannot either. Both are reported above with their spreads rather than mined
for a delta, because at 1.1 ms of fsync and 480 ms of segment creation the
quantity being asked about is four decimal places down.

---

## Reproducing this

Both drivers are documented in `bench/docs/README.md`. The exact shape of this
run: 36 var-heap cells (3 sizes × {1600 B relaxed, 8100 B relaxed, 1600 B
strict} × 2 builds × 2 runs), 8 decomposition cells with the `ping` arm, and 9
mount cells of 9 mounts each. `--verify` passed in all 44 var-heap cells; a
throughput number over a workload that lost a var-heap value would be a
measurement of nothing, and the var-heap is exactly where a lost write looks
like a fast one.


---

## Follow-up: the scan buffer stopped being zeroed (measured 2026-08-12)

The finding above — that most of a scan is `body.assign(body_bytes,
std::byte{0})` filling a buffer the next `ReadAt` overwrites — was acted on:
`wal::ScanLog` and `WalStream::ScanTail` now allocate with
`std::make_unique_for_overwrite` and hand `ReadAt` a span of exactly the range
it fills, so no byte is written twice and none is read before it is written.

**Interleaved A/B, `build-release`, one pair per rep so a load change hits both
arms**, 5 mounts per cell × 3 reps, empty log, `relaxed`. Baseline is this same
tree with only those two edits reverted, so the two binaries differ in nothing
else.

| | zeroed | unzeroed | delta |
|---|---|---|---|
| mount wall, p50 | 137.1 / 138.4 / 136.9 ms | 111.1 / 113.6 / 112.2 ms | **−24.7 to −26.0 ms** |
| `recovery_analysis_us` p50 | 41.3 ms | 33.2 ms | −8.1 ms |
| `recovery_redo_us` p50 | 40.7 ms | 32.7 ms | −8.0 ms |
| `recovery_checkpoint_us` p50 | 7.1 ms | 7.1 ms | ±0.0 ms |

**~18% off every mount, and the accounting closes at three scans.** The two
phase counters account for 16.1 ms of the 24.7; the rest is
`WalStream::ScanTail`, which runs at `WalManager::Open` *before* recovery and so
appears in no phase counter. Three scans, ~8 ms saved each.

**One correction to the attribution above.** The 78%-of-a-scan figure
over-predicts what removing the zeroing buys: a scan went 41.3 → 33.2 ms, so the
value-initialisation was **20% of it, not 78%**. The isolated micro-measurement
(30.3 ms to alloc+zero, 8.4 ms to `pread` warm) is not additive with the scan,
because the `pread` into a freshly-mapped 64 MiB buffer still takes the
first-touch page faults and still writes every byte — removing the `memset`
removes one pass over the buffer, not the fault-in. What remains is therefore
the read itself, and only a **narrower read** closes it: to the durable end
rather than to the segment's end. That is the fix `docs/known-gaps.md` still
carries as open, and this measurement is the argument for its size — ~65 ms of a
112 ms mount, on a log holding two records.
