# DN close: a btree relation pays nothing, a heap's 1,000-row index range is 27-32 % cheaper in CPU, a heap INSERT costs 15 % more, and a hidden-key heap reuses its space

**Thesis.** On `worktree-dn-heap-s2` (worktree directory `dn-dd-s4-into-dn-s2`) with the engine at `9c061907` (`v3.5.0-48-g9c061907`, DN closed) against `b7262f9b` (`v3.5.0-43-gb7262f9b`, the commit DN-S2 opened from): **DN-R7's bar is met as measured - no btree cell is slower than A outside its in-run control's range** (24 cells: 7 shapes at 10K and 100K rows, 6 at 1K, 4 at 200) - **with one named limit**: for the 20-50 us statements the control's own range is -20 to +45 %, so a regression of a few percent is not excluded there; the 0.8-2.4 ms cells, where the control reads -3 to +5 %, show none. The heap premise of DN-S0 holds with the real feature rather than a spike: **a 1,000-row secondary-index range over a heap is 37 % (10K rows) and 48 % (100K rows) faster in derived throughput than A's heap (27 % and 32 % less server CPU)**, a 20-row range 11-22 % less CPU but inside the control's range, a point read inside noise. DN's cost is on the write side: **a heap INSERT costs 3.7-3.8 us more server CPU than its btree twin on B at 1K and 10K rows (+15 %, 16 of 16 and 14 of 16 rounds)**, the Keystone index every heap now maintains. And **the slot release works for a hidden-key heap and not for one that declares its Keystone**: deleting and re-inserting a quarter of a 20,000-row heap leaves its page count flat (141, 142, 142 pages) where the btree twin grows by 30 pages a cycle (121, 151, 181); a declared-Keystone heap reuses its space only after a `PURGE` (121, 121, 122).

| field | value |
|---|---|
| date executed | 2026-11-21, 11:54-12:11 UTC (run 1 11:54-11:59, run 2 12:01-12:08, release cell 12:09-12:11) |
| worktree / branch / commit | `/home/louis/ckdbs/.claude/worktrees/dn-dd-s4-into-dn-s2`, branch `worktree-dn-heap-s2`, HEAD `9c061907` = `v3.5.0-48-g9c061907`; tree clean before this file was written |
| **A** | detached `git worktree` of `b7262f9b` (`v3.5.0-43-gb7262f9b`, committed 07:58 UTC) built in `/home/louis/dn-close-run/buildA`; measured copy `kds_server_A`, sha256 `05f1498a8da9c8896d7449709afcb3a0d099d711a467e6fb04fd7955b3564887`, source binary mtime 11:51:41 UTC (after the commit) |
| **A2** | the same file as A, started as a second server: the in-run control |
| **B** | detached `git worktree` of `9c061907` (committed 11:47 UTC) built in `/home/louis/dn-close-run/buildB`; measured copy `kds_server_B`, sha256 `474ef68ea128a4b62df85399283f54acce6e2adf24f296954295f21e95ae9d8e`, source binary mtime 11:51:43 UTC (after the commit) |
| build | Release (`CMAKE_BUILD_TYPE=Release` read from both `CMakeCache.txt`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, trees outside the repository; every server started from its copy in `/home/louis/dn-close-run/run/` |
| device | data files and WAL under `/home/louis/dn-close-run/data*`, `/dev/nvme0n1p1` (ext4, checked with `df -T` at run time); no data file on tmpfs |
| server | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536` (every page resident), `log_level = warn`; one KWP connection per server; servers pinned to cores 4, 5, 6 (A, A2, B), the driver to cores 0-3; fresh server and data file per arm per row count |
| host | 8 cores; load average at most 1.58 (run 1) and 1.27 (run 2) and zero `cc1plus` in every round of every cell (recorded per round in the JSON); no other session's build or suite ran during either run |
| PostgreSQL floor | not measured for these shapes |
| prior number for this shape | none: A is the baseline, measured in the same run |
| raw data | `/home/louis/dn-close-run/out/` (`main.json`, `main2.json`, `release.json`) and the drivers in `/home/louis/dn-close-run/drivers/`; not archived under `bench/` (rule 1b: a narrower measurement archives nothing) |

## 1. Method, and what was cut

**Relations.** Per server, two relations holding the same rows: `tb (id int64, v int64, w int64) BTREE` and `th (id int64, v int64, w int64) HEAP`, `v` a random permutation of 1..N, `w = id mod 97`, `CREATE INDEX ix_<t> ON <t> (v)` on each. **On B, `th`'s `id` is `PRIMARY KEY`**: the heap's Keystone is `id` and key reads go through its Keystone index. **On A a heap's key is the hidden `$tuple_key`** (`PRIMARY KEY` on a heap is B's syntax) and `id` is an ordinary column, so **a heap key read and a heap `UPDATE` by key exist on B only**. N is the driver's `--n`: 200, 1,000, 10,000 and 100,000 (rule 9). 200 rows covers the read cells only (a 100-statement insert block would grow a 200-row table six-fold over the rounds), and a 1,000-row range is the whole of a 1,000-row table, so `r1000` starts at 10K. `ANALYZE` of each cell's first statement on B: `pt` and `r20` are `JoinSelect` with `examined=1 pages=2` and `examined=20 pages=21` on both relations (index probe, index range), `tk` is `PointSelect examined=1`, `scan` is `examined=N`.

| cell | statement | statements per timed block |
|---|---|---|
| `pt` | `SELECT * FROM t WHERE v = <k>` | 1,000 distinct k |
| `r20` | `... WHERE v BETWEEN <lo> AND <lo+19>` | 100 |
| `r1000` | `... WHERE v BETWEEN <lo> AND <lo+999>` | 40 (10K and 100K) |
| `tk` | `SELECT * FROM t WHERE id = <k>` | 1,000 |
| `scan` | `SELECT * FROM t WHERE w = 5 AND v > 0` (walks every row; `v > 0` keeps the planner off the index) | 5 |
| `ins` | `INSERT INTO t VALUES (<fresh id>, <random v>, <id mod 97>)`, autocommit, id named, same ids on every arm | 100 (run 1), 200 (run 2) |
| `upd` | `UPDATE t SET w = <random> WHERE id = <k>`, autocommit | 200 (run 1), 400 (run 2) |

**Protocol.** Three servers run at once. Per cell: one untimed pass of the read set on every server (row counts asserted equal), then **rounds** of one timed block per server, the same statements in each (equal work), the arm order rotated and reversed per round. Server CPU is the sum of `/proc/<pid>/task/*/schedstat` run time over the server's threads divided by the statements in the block; every CPU figure is the **median over rounds**, every range a min..max over rounds. A read block is 0.3 s (run 1, 10 rounds) or 0.4 s (run 2, 16 rounds). **The tables pool both runs** (26 rounds; 200 rows from run 1 only, 10 rounds), except section 3, which is run 2. **The noise floor is A2 against A**: same binary, same data, another server. A B-against-A cell is called *outside the floor* only when its median lies beyond the control's per-round extreme on that side. **Throughput is derived**: statements/s = 1,000,000 / median server CPU in us, the server-CPU bound of a serial connection; the client's wall rate is lower (section 5).

**Time.** Both builds at once (`-j4` each) about 2 minutes, run 1 about 5, run 2 about 7, the release cell about 2: about 25 minutes of the hour. The host was quiet, so no quiet-gate hold was taken.

**Cut.** `cores > 1`, `strict` and `group` durability (every cell is `relaxed` at `cores = 1`; no commit wait is measured), a cold device read, a PostgreSQL twin, a heap key read and heap `UPDATE` on A (not expressible), and a hidden-key heap in the interleaved matrix (B's `th` declares its Keystone; the hidden-key heap appears in section 6 only, un-interleaved). DN-S0's cells were not run as a separate job: its premise legs (`pt`, `r20`, `r1000` over a heap against A's heap) are cells 2 and 3 of this run, read in section 4.

Reproduce: `python3 -I dn_close.py --binA <A copy> --binB <B copy> --workdir <dir on a block device> --out <json> --n 200,1000,10000,100000 --rounds 10 --block-seconds 0.3` (run 1, `dn_close_run1.py`), then `--n 1000,10000,100000 --rounds 16 --block-seconds 0.4` (run 2); `dn_release.py <B copy> <dir> <json> 20000` for section 6. The drivers derive from DN-S0's `dn_s0_premise.py`; **none is in `tools/` and `bench/docs/` does not yet document them**.

## 2. DN-R7: a btree relation pays nothing

Every row is btree-only, A against B. **No cell is slower outside the control's range, and 22 of 24 pooled medians are within 5.5 % of A in either direction; the two others are the 200-row key read (B +13.6 % faster, control -16 to +21 %) and the 100K INSERT below.** The control's own range is wide for 20-50 us statements and narrow for long ones, so the long cells are where a regression of a few percent would show, and they show none: the 1,000-row index range is +0.5 % (10K rows) and -0.4 % (100K) against a control of -1 to +5 % and -3 to +5 %, and the scans are -1.4 % and +0.3 % against -3 to +3 % and -2 to +2 %.

The one large median is **the 100K single-row INSERT, B -11.0 %** (29.3 us against 26.0 us of server CPU, 18 of 26 rounds slower). The same cell reads -0.4 % at 1K and +1.1 % at 10K; run 1 alone had B slower in 10 of 10 rounds (+4.7 us) and run 2 alone in 8 of 16 (+1.2 us); the control's range there is -25 to +13 %. It is inside the floor and the only btree cell whose sign held in both runs, so it is named: **not a finding, not cleared.** Swapping the arms' ports and cores and measuring it again is how to settle it; that was not run.

Derived server-CPU throughput (statements/s = 1,000,000 / median server CPU us), A against A2 (control) against B, pooled runs:

| rows | cell | A stmts/s | A2 (control) stmts/s | B stmts/s | B vs A | control A2 vs A, per-round range | B vs A, per-round range | rounds B slower | verdict |
|---|---|---|---|---|---|---|---|---|---|
| 200 | btree tk | 39,183/s | 43,313/s | 44,513/s | +13.6 % | -16 to +21 % | -18 to +25 % | 1/10 | inside floor |
| 200 | btree pt | 39,062/s | 37,600/s | 40,403/s | +3.4 % | -22 to +17 % | -2 to +15 % | 1/10 | inside floor |
| 200 | btree r20 | 24,857/s | 21,073/s | 24,308/s | -2.2 % | -30 to +10 % | -7 to +12 % | 7/10 | inside floor |
| 200 | btree scan | 32,859/s | 30,349/s | 33,663/s | +2.4 % | -20 to +12 % | -10 to +18 % | 5/10 | inside floor |
| 1000 | btree tk | 44,449/s | 44,701/s | 44,417/s | -0.1 % | -20 to +23 % | -19 to +25 % | 10/26 | inside floor |
| 1000 | btree pt | 37,346/s | 38,187/s | 39,270/s | +5.2 % | -20 to +25 % | -11 to +25 % | 9/26 | inside floor |
| 1000 | btree r20 | 23,920/s | 23,937/s | 22,608/s | -5.5 % | -27 to +45 % | -28 to +37 % | 18/26 | inside floor |
| 1000 | btree ins | 41,583/s | 33,642/s | 41,438/s | -0.4 % | -21 to +14 % | -8 to +31 % | 12/26 | inside floor |
| 1000 | btree upd | 42,137/s | 37,268/s | 42,702/s | +1.3 % | -21 to +14 % | -19 to +17 % | 8/26 | inside floor |
| 1000 | btree scan | 21,154/s | 19,999/s | 20,351/s | -3.8 % | -36 to +41 % | -31 to +33 % | 17/26 | inside floor |
| 10000 | btree tk | 44,984/s | 44,885/s | 43,628/s | -3.0 % | -20 to +15 % | -18 to +12 % | 20/26 | inside floor |
| 10000 | btree pt | 38,418/s | 36,342/s | 37,608/s | -2.1 % | -20 to +14 % | -23 to +10 % | 18/26 | inside floor |
| 10000 | btree r20 | 22,744/s | 19,606/s | 22,515/s | -1.0 % | -30 to +43 % | -26 to +39 % | 13/26 | inside floor |
| 10000 | btree r1000 | 1,226/s | 1,254/s | 1,232/s | +0.5 % | -1 to +5 % | -5 to +4 % | 10/26 | inside floor |
| 10000 | btree ins | 39,774/s | 35,237/s | 40,209/s | +1.1 % | -22 to +29 % | -18 to +27 % | 7/26 | inside floor |
| 10000 | btree upd | 42,073/s | 38,507/s | 42,303/s | +0.5 % | -21 to +5 % | -22 to +6 % | 11/26 | inside floor |
| 10000 | btree scan | 3,972/s | 3,967/s | 3,917/s | -1.4 % | -3 to +3 % | -9 to +1 % | 18/26 | inside floor |
| 100000 | btree tk | 43,649/s | 42,959/s | 44,226/s | +1.3 % | -20 to +16 % | -12 to +15 % | 10/26 | inside floor |
| 100000 | btree pt | 37,230/s | 36,808/s | 36,590/s | -1.7 % | -19 to +28 % | -14 to +24 % | 9/26 | inside floor |
| 100000 | btree r20 | 20,911/s | 20,809/s | 20,957/s | +0.2 % | -29 to +43 % | -29 to +42 % | 12/26 | inside floor |
| 100000 | btree r1000 | 1,020/s | 1,036/s | 1,016/s | -0.4 % | -3 to +5 % | -11 to +5 % | 16/26 | inside floor |
| 100000 | btree ins | 38,433/s | 36,174/s | 34,187/s | -11.0 % | -25 to +13 % | -24 to +10 % | 18/26 | inside floor |
| 100000 | btree upd | 39,220/s | 40,640/s | 40,467/s | +3.2 % | -2 to +29 % | -17 to +32 % | 8/26 | inside floor |
| 100000 | btree scan | 420/s | 420/s | 421/s | +0.3 % | -2 to +2 % | -5 to +3 % | 14/26 | inside floor |

Reading the columns: *B vs A* is the change in derived throughput at the median (positive = B faster); the two range columns are the per-round extremes of the same quantity, so the control column is what a difference between **identical** engines looked like. *rounds B slower* counts rounds in which B's CPU per statement exceeded A's. Rows named `btree` are the relation `tb`.

A btree's index entries carry no rowid on B, and its tuple-key, secondary-point and `UPDATE` paths are unchanged in the matrix: what DN-R3 (the slot check) and the wider heap index entry could have added to a btree relation is not visible here.

## 3. Heap against its btree twin on B

Same server, same round, so the per-round difference removes the server-to-server offset that sets the matrix's control range. `th` (heap, Keystone `id`) against `tb` (btree), run 2, 16 rounds each. *heap vs btree* is the derived throughput change (positive = heap faster).

| rows | shape | btree | heap | heap vs btree | per-round range, heap CPU minus btree CPU | rounds heap slower |
|---|---|---|---|---|---|---|
| 1,000 | secondary point read | 39,270/s | 36,849/s | -6.2 % | CPU -6.3 to +5.7 us (median +2.0) | 11/16 |
| 1,000 | index range, 20 rows | 21,499/s | 26,781/s | +24.6 % | CPU -18.7 to -3.3 us (median -7.1) | 0/16 |
| 1,000 | key read (heap: Keystone index) | 45,194/s | 43,056/s | -4.7 % | CPU -2.1 to +3.5 us (median +1.2) | 10/16 |
| 1,000 | full scan | 19,381/s | 18,899/s | -2.5 % | CPU -20.8 to +12.7 us (median -3.1) | 7/16 |
| 1,000 | single-row INSERT | 41,757/s | 36,419/s | -12.8 % | CPU +2.7 to +5.5 us (median +3.7) | 16/16 |
| 1,000 | UPDATE non-key by key | 42,442/s | 41,083/s | -3.2 % | CPU -4.7 to +2.3 us (median +0.9) | 11/16 |
| 10,000 | secondary point read | 38,073/s | 38,156/s | +0.2 % | CPU -8.2 to +4.6 us (median -0.8) | 5/16 |
| 10,000 | index range, 20 rows | 23,212/s | 25,587/s | +10.2 % | CPU -8.4 to +8.3 us (median -5.3) | 4/16 |
| 10,000 | index range, 1,000 rows | 1,228/s | 1,676/s | +36.5 % | CPU -265.8 to -198.9 us (median -216.6) | 0/16 |
| 10,000 | key read (heap: Keystone index) | 42,233/s | 39,605/s | -6.2 % | CPU -2.4 to +9.5 us (median +2.5) | 12/16 |
| 10,000 | full scan | 3,919/s | 3,954/s | +0.9 % | CPU -20.7 to +21.0 us (median -2.3) | 4/16 |
| 10,000 | single-row INSERT | 40,575/s | 35,219/s | -13.2 % | CPU -0.8 to +7.1 us (median +3.8) | 14/16 |
| 10,000 | UPDATE non-key by key | 42,266/s | 40,698/s | -3.7 % | CPU -7.3 to +1.6 us (median +0.9) | 14/16 |
| 100,000 | secondary point read | 36,688/s | 38,386/s | +4.6 % | CPU -5.2 to +4.1 us (median -1.0) | 5/16 |
| 100,000 | index range, 20 rows | 20,680/s | 24,968/s | +20.7 % | CPU -14.6 to +4.2 us (median -8.9) | 4/16 |
| 100,000 | index range, 1,000 rows | 1,025/s | 1,426/s | +39.2 % | CPU -330.7 to -226.1 us (median -281.9) | 0/16 |
| 100,000 | key read (heap: Keystone index) | 44,226/s | 42,048/s | -4.9 % | CPU -4.2 to +7.3 us (median +1.5) | 13/16 |
| 100,000 | full scan | 424/s | 417/s | -1.6 % | CPU -26.4 to +191.6 us (median +24.8) | 13/16 |
| 100,000 | single-row INSERT | 36,555/s | 27,714/s | -24.2 % | CPU -1.0 to +17.0 us (median +7.5) | 14/16 |
| 100,000 | UPDATE non-key by key | 40,721/s | 38,912/s | -4.4 % | CPU -6.0 to +7.3 us (median +0.9) | 11/16 |

**The heap pays on insert and nowhere else it can be told apart.** A single-row `INSERT` costs 3.7-3.8 us more server CPU on the heap at 1K and 10K rows (+15 % per statement, in a band of +2.7 to +5.5 us at 1K, 16 of 16 rounds; 14 of 16 at 10K), and the 100K row (+7.5 us, band -1.0 to +17.0 us) is noisier but the same sign. The heap on A, where it is a btree with a hidden key, shows no such gap against its twin (A's own heap and btree inserts, same server and round, differ by -0.5 %, -2.0 % and +0.7 % at 1K, 10K and 100K, bands straddling zero), so the +3.7 us is what DN's design adds: a heap row is a data page slot **and** an entry in its Keystone index, on top of the secondary index both relations carry. The key read through the Keystone index costs +1.2 to +2.5 us (+5-10 %) over the clustered descent, in 10-13 of 16 rounds at each size, which is the price of the extra hop that a btree key read does not take; the `UPDATE` by key costs +0.9 us (+4 %, 11-14 of 16 rounds). Both bands straddle zero or reach only a few us either way, and both are read as small, not as findings. A full scan is level (-2.5 % to +0.9 %, bands straddling zero) except 100K, where its median is +24.8 us of 2.4 ms (+1 %, 13 of 16 rounds, band -26 to +192 us): a heap walk reads pages in order and the scan is as cheap as the clustered leaf walk. The index range and the secondary point read are section 4's subject.

## 4. DN's premise: the descent a rowid saves

Heap against A's heap, B against A, pooled runs. A resolves each secondary-index entry through a clustered descent on the hidden key; B reads the page the entry's rowid names. The saving is per resolved row, so it grows with the rows a statement returns: **0.22-0.34 us of CPU per row on the 1,000-row range (27-32 % of it), 0.24-0.53 us per row on the 20-row range (11-22 %), and nothing a one-row point read can show.**

| rows | cell | A stmts/s | A2 (control) stmts/s | B stmts/s | B vs A | control A2 vs A, per-round range | B vs A, per-round range | rounds B slower | verdict |
|---|---|---|---|---|---|---|---|---|---|
| rows | cell | A stmts/s | A2 (control) stmts/s | B stmts/s | B vs A | control A2 vs A, per-round range | B vs A, per-round range | rounds B slower | verdict |
|---|---|---|---|---|---|---|---|---|---|
| 200 | heap pt | 36,243/s | 39,783/s | 38,979/s | +7.5 % | -18 to +21 % | -15 to +24 % | 4/10 | inside floor |
| 200 | heap r20 | 23,165/s | 24,165/s | 26,444/s | +14.2 % | -28 to +36 % | -21 to +48 % | 1/10 | inside floor |
| 200 | heap scan | 34,309/s | 26,023/s | 34,996/s | +2.0 % | -27 to +2 % | -23 to +18 % | 1/10 | faster outside floor |
| 1000 | heap pt | 38,742/s | 39,089/s | 39,126/s | +1.0 % | -18 to +26 % | -18 to +30 % | 10/26 | inside floor |
| 1000 | heap r20 | 23,335/s | 23,429/s | 26,684/s | +14.4 % | -31 to +32 % | -11 to +55 % | 5/26 | inside floor |
| 1000 | heap scan | 20,646/s | 20,675/s | 18,899/s | -8.5 % | -32 to +52 % | -33 to +54 % | 13/26 | inside floor |
| 10000 | heap pt | 38,614/s | 35,602/s | 38,348/s | -0.7 % | -20 to +7 % | -15 to +10 % | 13/26 | inside floor |
| 10000 | heap r20 | 22,943/s | 23,113/s | 25,562/s | +11.4 % | -28 to +46 % | -15 to +66 % | 4/26 | inside floor |
| 10000 | heap r1000 | 1,214/s | 1,222/s | 1,666/s | +37.2 % | -3 to +4 % | +22 to +44 % | 0/26 | faster outside floor |
| 10000 | heap scan | 3,894/s | 3,901/s | 3,953/s | +1.5 % | -3 to +5 % | -7 to +6 % | 8/26 | inside floor |
| 100000 | heap pt | 36,277/s | 37,106/s | 38,116/s | +5.1 % | -16 to +20 % | -11 to +27 % | 10/26 | inside floor |
| 100000 | heap r20 | 20,419/s | 20,409/s | 24,968/s | +22.3 % | -20 to +31 % | -6 to +61 % | 3/26 | inside floor |
| 100000 | heap r1000 | 960/s | 960/s | 1,417/s | +47.5 % | -3 to +4 % | +37 to +54 % | 0/26 | faster outside floor |
| 100000 | heap scan | 414/s | 417/s | 419/s | +1.3 % | -2 to +3 % | -6 to +4 % | 6/26 | inside floor |

The 1,000-row range is the finding: B's median is 27 % (10K rows) and 32 % (100K) below A's CPU with every one of 26 rounds faster, against a control of -3 to +5 %. The 20-row range moves the same way (-11 % at 1K and 10K, -22 % at 100K in CPU) but its control range is -28 to +46 %, so it is *inside the floor* round by round and read as a direction, not a result; only 1 to 5 of 26 rounds had B slower. The point read is -2.8 %, -0.1 % and -6.8 % at 1K, 10K and 100K, none beyond its control. The scans are level, as expected of a path DN did not change. Nothing here was measured at a heap depth other than the one a 10K-100K row table gives (the clustered tree's height was not read), and the saving per row on the 1,000-row range is 0.23 us at 10K and 0.34 us at 100K, larger on the larger table (consistent with a deeper tree to descend; its height was not read).

The heap `INSERT` is the one write shape both servers can run, but not like for like: B's heap declares its Keystone (a Keystone index beside the secondary one), A's keeps a hidden key in a clustered tree. B against A, therefore the price of DN's design on that shape:

| rows | cell | A stmts/s | A2 (control) stmts/s | B stmts/s | B vs A | control A2 vs A, per-round range | B vs A, per-round range | rounds B slower | verdict |
|---|---|---|---|---|---|---|---|---|---|
| rows | cell | A stmts/s | A2 (control) stmts/s | B stmts/s | B vs A | control A2 vs A, per-round range | B vs A, per-round range | rounds B slower | verdict |
|---|---|---|---|---|---|---|---|---|---|
| 1000 | heap ins | 41,840/s | 38,632/s | 35,263/s | -15.7 % | -21 to +5 % | -28 to -6 % | 26/26 | inside floor |
| 10000 | heap ins | 40,703/s | 34,302/s | 34,672/s | -14.8 % | -20 to +7 % | -25 to -8 % | 26/26 | inside floor |
| 100000 | heap ins | 37,566/s | 39,002/s | 31,220/s | -16.9 % | -24 to +135 % | -35 to +55 % | 23/26 | inside floor |

The `verdict` column reads *inside floor* for the heap insert though B was slower in 26 of 26 rounds at 1K and 10K: A2's inserts run 13-17 % slower than A's at 10K in this same cell, so the control's range is wide on exactly this shape (an insert block is 100-200 statements, 2-5 ms, and server-to-server offsets of 4-5 us survive). **The paired comparison of section 3, one server and one round, is the reliable read of the heap insert cost.**

## 5. Distributions and where the time goes

**Latency distributions.** Client wall time around one statement over the loopback with the Python KWP client (it counts rows and walks field lengths and builds no Python objects per field), us, run 2 (16 rounds, A and B; the A2 rows are in the JSON). It includes the client; it is not a server cost. The `scan` rows have 80-160 operations and so a coarse p95 and p99.

| rows | cell | arm | ops | p0 us | p25 us | p50 us | p95 us | p99 us | p100 us |
|---|---|---|---|---|---|---|---|---|---|
| 1000 | btree pt | A | 128,000 | 37.8 | 41.7 | 43.1 | 54.7 | 62.2 | 4286.5 |
| 1000 | btree pt | B | 128,000 | 38.0 | 41.3 | 42.6 | 53.9 | 62.6 | 1465.6 |
| 1000 | btree r20 | A | 81,600 | 60.5 | 64.3 | 66.1 | 107.7 | 113.9 | 1646.9 |
| 1000 | btree r20 | B | 81,600 | 60.1 | 64.3 | 66.9 | 109.5 | 133.9 | 1035.7 |
| 1000 | btree tk | A | 128,000 | 34.5 | 38.8 | 39.9 | 50.0 | 56.5 | 932.4 |
| 1000 | btree tk | B | 128,000 | 33.9 | 38.4 | 39.0 | 48.6 | 59.1 | 5629.3 |
| 1000 | btree scan | A | 18,800 | 63.4 | 66.3 | 67.6 | 111.1 | 117.2 | 361.6 |
| 1000 | btree scan | B | 18,800 | 63.5 | 66.3 | 68.5 | 120.2 | 177.2 | 430.8 |
| 1000 | btree ins | A | 3,200 | 35.0 | 37.2 | 37.8 | 50.3 | 114.5 | 860.1 |
| 1000 | btree ins | B | 3,200 | 34.7 | 37.0 | 37.7 | 50.5 | 117.6 | 285.5 |
| 1000 | btree upd | A | 6,400 | 33.5 | 37.5 | 38.4 | 49.7 | 75.1 | 281.7 |
| 1000 | btree upd | B | 6,400 | 34.4 | 37.3 | 38.4 | 49.9 | 80.2 | 1715.4 |
| 1000 | heap pt | A | 144,000 | 36.9 | 41.5 | 42.8 | 54.0 | 60.2 | 1568.8 |
| 1000 | heap pt | B | 144,000 | 37.7 | 41.5 | 43.9 | 55.4 | 65.7 | 4064.1 |
| 1000 | heap r20 | A | 88,000 | 60.1 | 64.6 | 66.5 | 109.0 | 114.9 | 2488.3 |
| 1000 | heap r20 | B | 88,000 | 55.8 | 59.6 | 60.9 | 84.2 | 121.2 | 2973.2 |
| 1000 | heap tk | B | 96,000 | 36.8 | 39.4 | 40.3 | 49.7 | 55.6 | 3038.6 |
| 1000 | heap scan | A | 60,400 | 63.7 | 66.8 | 69.5 | 112.9 | 118.6 | 4236.7 |
| 1000 | heap scan | B | 60,400 | 63.4 | 66.1 | 67.4 | 112.6 | 119.0 | 514.3 |
| 1000 | heap ins | A | 3,200 | 35.1 | 37.1 | 37.8 | 49.5 | 78.2 | 594.9 |
| 1000 | heap ins | B | 3,200 | 36.1 | 40.9 | 41.8 | 52.9 | 95.6 | 239.5 |
| 1000 | heap upd | B | 6,400 | 32.6 | 38.2 | 39.1 | 46.8 | 71.3 | 240.4 |
| 10000 | btree pt | A | 144,000 | 36.4 | 41.8 | 43.1 | 54.0 | 61.0 | 1266.9 |
| 10000 | btree pt | B | 144,000 | 37.1 | 42.0 | 43.4 | 55.8 | 68.7 | 3639.6 |
| 10000 | btree r20 | A | 88,000 | 61.2 | 65.4 | 67.1 | 108.2 | 114.4 | 1417.1 |
| 10000 | btree r20 | B | 88,000 | 58.2 | 65.0 | 66.5 | 83.5 | 136.0 | 622.7 |
| 10000 | btree r1000 | A | 5,120 | 1155.5 | 1215.8 | 1239.7 | 1374.3 | 1447.4 | 1838.1 |
| 10000 | btree r1000 | B | 5,120 | 1153.0 | 1214.7 | 1242.3 | 1382.0 | 1546.9 | 4416.3 |
| 10000 | btree tk | A | 160,000 | 35.6 | 38.4 | 39.1 | 48.5 | 55.2 | 4732.4 |
| 10000 | btree tk | B | 160,000 | 36.0 | 38.5 | 40.2 | 51.0 | 60.7 | 4831.7 |
| 10000 | btree scan | A | 12,560 | 299.2 | 314.5 | 332.1 | 384.7 | 421.0 | 719.6 |
| 10000 | btree scan | B | 12,560 | 299.3 | 316.4 | 341.5 | 400.3 | 461.3 | 3195.6 |
| 10000 | btree ins | A | 3,200 | 34.9 | 37.4 | 38.2 | 51.0 | 103.7 | 246.0 |
| 10000 | btree ins | B | 3,200 | 35.0 | 37.5 | 38.9 | 53.3 | 91.3 | 253.6 |
| 10000 | btree upd | A | 6,400 | 35.5 | 37.4 | 38.1 | 46.4 | 74.8 | 287.5 |
| 10000 | btree upd | B | 6,400 | 34.9 | 37.2 | 38.0 | 48.6 | 72.3 | 561.1 |
| 10000 | heap pt | A | 144,000 | 38.0 | 41.5 | 42.7 | 52.6 | 60.4 | 2311.5 |
| 10000 | heap pt | B | 144,000 | 37.9 | 41.2 | 42.5 | 54.2 | 68.0 | 1074.4 |
| 10000 | heap r20 | A | 88,000 | 61.1 | 65.5 | 67.3 | 110.0 | 119.0 | 1679.9 |
| 10000 | heap r20 | B | 88,000 | 55.5 | 59.5 | 61.3 | 89.0 | 131.3 | 1889.6 |
| 10000 | heap r1000 | A | 5,120 | 1165.2 | 1223.4 | 1248.2 | 1381.3 | 1479.7 | 2401.8 |
| 10000 | heap r1000 | B | 5,120 | 939.6 | 989.1 | 1011.7 | 1152.7 | 1296.4 | 1547.2 |
| 10000 | heap tk | B | 16,000 | 37.0 | 40.0 | 41.3 | 56.6 | 140.8 | 324.7 |
| 10000 | heap scan | A | 14,960 | 303.2 | 316.9 | 331.8 | 388.6 | 477.8 | 2898.8 |
| 10000 | heap scan | B | 14,960 | 298.9 | 314.3 | 336.3 | 403.1 | 458.5 | 755.2 |
| 10000 | heap ins | A | 3,200 | 31.9 | 37.4 | 38.1 | 50.8 | 94.0 | 358.0 |
| 10000 | heap ins | B | 3,200 | 36.6 | 41.1 | 42.1 | 56.5 | 98.4 | 635.5 |
| 10000 | heap upd | B | 6,400 | 33.8 | 38.4 | 39.4 | 49.0 | 67.6 | 1633.8 |
| 100000 | btree pt | A | 128,000 | 38.8 | 42.6 | 44.3 | 56.9 | 63.9 | 1649.5 |
| 100000 | btree pt | B | 128,000 | 38.1 | 42.1 | 43.9 | 56.4 | 68.4 | 1666.2 |
| 100000 | btree r20 | A | 52,800 | 63.6 | 68.4 | 70.4 | 110.0 | 118.0 | 2692.2 |
| 100000 | btree r20 | B | 52,800 | 63.0 | 68.1 | 70.0 | 109.6 | 143.0 | 1264.3 |
| 100000 | btree r1000 | A | 4,480 | 1308.2 | 1384.9 | 1415.9 | 1565.6 | 1723.2 | 3055.6 |
| 100000 | btree r1000 | B | 4,480 | 1303.8 | 1382.5 | 1410.0 | 1578.3 | 1818.7 | 3847.4 |
| 100000 | btree tk | A | 160,000 | 34.5 | 39.1 | 40.0 | 50.4 | 57.2 | 855.0 |
| 100000 | btree tk | B | 160,000 | 33.7 | 38.6 | 39.4 | 49.9 | 57.2 | 3241.2 |
| 100000 | btree scan | A | 2,160 | 2706.3 | 2799.9 | 2834.4 | 2987.7 | 3301.7 | 5223.3 |
| 100000 | btree scan | B | 2,160 | 2696.4 | 2787.0 | 2826.2 | 3028.8 | 3337.0 | 4726.0 |
| 100000 | btree ins | A | 3,200 | 35.6 | 38.1 | 39.0 | 52.8 | 97.6 | 310.1 |
| 100000 | btree ins | B | 3,200 | 35.2 | 38.2 | 44.1 | 55.1 | 104.6 | 250.1 |
| 100000 | btree upd | A | 6,400 | 35.5 | 38.0 | 39.3 | 55.2 | 90.7 | 1234.4 |
| 100000 | btree upd | B | 6,400 | 35.1 | 37.6 | 39.3 | 55.8 | 79.4 | 1646.0 |
| 100000 | heap pt | A | 144,000 | 38.8 | 43.0 | 45.0 | 57.7 | 64.3 | 1776.7 |
| 100000 | heap pt | B | 144,000 | 38.0 | 41.3 | 42.7 | 54.9 | 67.5 | 1365.0 |
| 100000 | heap r20 | A | 76,800 | 64.8 | 70.2 | 72.5 | 114.4 | 123.1 | 592.8 |
| 100000 | heap r20 | B | 76,800 | 55.7 | 59.9 | 62.4 | 89.0 | 125.9 | 425.3 |
| 100000 | heap r1000 | A | 4,480 | 1378.8 | 1446.7 | 1477.1 | 1620.6 | 1759.5 | 2143.7 |
| 100000 | heap r1000 | B | 4,480 | 1018.4 | 1092.5 | 1123.5 | 1291.6 | 1431.7 | 2561.8 |
| 100000 | heap tk | B | 16,000 | 37.1 | 39.8 | 41.1 | 53.6 | 64.3 | 453.1 |
| 100000 | heap scan | A | 2,000 | 2744.8 | 2836.4 | 2867.7 | 3054.9 | 3419.4 | 4061.6 |
| 100000 | heap scan | B | 2,000 | 2713.5 | 2814.2 | 2856.3 | 3089.6 | 3402.7 | 4069.3 |
| 100000 | heap ins | A | 3,200 | 31.3 | 38.3 | 39.8 | 58.7 | 122.3 | 1370.7 |
| 100000 | heap ins | B | 3,200 | 38.8 | 43.8 | 51.6 | 67.9 | 132.0 | 686.6 |
| 100000 | heap upd | B | 6,400 | 33.1 | 38.9 | 41.4 | 53.5 | 88.8 | 1684.5 |

**Waits.** A latency here is a sum of: the server's CPU (the matrix above), the client and loopback round trip, and waits inside the server. For a 10K-row secondary point read the wall p50 is 43 us against a server CPU median of 26 us: **about 60 % server, about 17 us (40 %) client, Python and socket round trip** (the same 17 us at B and A). A 1,000-row range is 1,240 us of wall against 815 us of server CPU (about 425 us of client, mostly walking 1,000 rows' field lengths). A single-row insert or update is 38 us of wall against 24-25 us of CPU. The waits inside the server, from `sys.wait_stats` (the instance's total row per event, cumulative over the server's whole life at that row count: load, the untimed pass and every timed block, about 4-6 minutes):

| rows | arm | `sysio.checkpoint` events / total / max | `sysio.wal_write` events / total / max | `sched.run_queue` events / total / max |
|---|---|---|---|---|
| 1,000 | A | 46 / 68.5 ms / 3.2 ms | 46 / 44.7 ms / 2.6 ms | 669,098 / 167.0 ms / 0.1 ms |
| 1,000 | B | 46 / 62.7 ms / 2.2 ms | 46 / 40.5 ms / 1.2 ms | 768,296 / 185.9 ms / 1.6 ms |
| 10,000 | A | 68 / 98.8 ms / 3.2 ms | 68 / 66.9 ms / 2.8 ms | 698,261 / 184.0 ms / 0.4 ms |
| 10,000 | B | 68 / 99.5 ms / 4.3 ms | 76 / 73.7 ms / 2.7 ms | 701,414 / 181.7 ms / 1.5 ms |
| 100,000 | A | 70 / 120.1 ms / 7.8 ms | 83 / 318.9 ms / 237.2 ms | 648,310 / 208.0 ms / 1.6 ms |
| 100,000 | B | 70 / 215.8 ms / 94.5 ms | 71 / 130.7 ms / 21.5 ms | 652,341 / 256.4 ms / 18.0 ms |

Those are the only events either engine recorded: **no lock wait, no page read (every page resident), no commit fsync wait (`relaxed`), no deadlock**. Checkpoint and log-write time is the background tick's, off the statement path at `relaxed`; the largest single event in the table (237 ms `wal_write`, A, 100K) and B's 94.5 ms checkpoint are one event each in a 5-minute lifetime, one on each engine, and are not read as a difference. The wait breakdown of a `strict` commit is not measured here (cut, section 1).

## 6. Cell 4: a heap's released slots are reused, a btree's are not

`dn_release.py` on B, one server, 20,000 rows loaded into three relations with a secondary index each (`tb` btree; `th` heap with a hidden Keystone; `tp` heap with `id int64 PRIMARY KEY`), `DELETE` of the first 5,000 ids (a quarter), a 10 s wait spanning 200 of core 0's 50 ms release ticks, 5,000 fresh single-row inserts, then a second delete of 5,000 more, 10 s, 5,000 inserts. The page count is `sys.relation_stats.leaves` (the clustered tree's leaves for a btree, the data pages for a heap), polled every 0.25 s during the wait. A single run, not interleaved: the page counts are exact, the CPU figures are not compared.

| relation | loaded | after 1st delete and 10 s | after 1st re-insert | after 2nd cycle | re-insert server CPU / statement (1st, 2nd) |
|---|---|---|---|---|---|
| `tb` btree | 121 pages | 121 | 151 | **181** | 23.2 us, 24.5 us |
| `th` heap, hidden Keystone | 141 pages | 141 | **142** | **142** | 26.9 us, 27.0 us |
| `tp` heap, declared Keystone, `PURGE` after each delete | 121 pages | 121 | **121** | **122** | 27.5 us, 26.9 us |

The `DELETE` of 5,000 rows took 4.3 ms (`tb`), 8.5 ms (`th`, a walk of an unindexed column) and 5.3 ms (`tp`) of wall time. **The page count is flat across both cycles for both heaps** (a btree grows 30 pages each cycle: a delete-marked row keeps its room until a `PURGE`, and the btree here is not purged). **A declared-Keystone heap's tombstone is kept until a `PURGE` frees the key** (`heap_release.cpp`, `Collect`: `retires` is true for a hidden Keystone only), which is why `tp` was measured with the `PURGE`: without it the release reuses nothing. A discarded first attempt of this cell, with `th` declared `PRIMARY KEY` and no `PURGE`, grew exactly as the btree does (121, 121, 151, 181 pages): the evidence for the rule. For the hidden-Keystone heap the count was 141 at every 2 s sample of the 10 s wait (`leaves_during_wait_10s`), so nothing grew back; a release frees slots for reuse and returns no page, and no view shows free slots, so the reuse is read from the count not growing.

## 7. What it teaches

1. **DN's bet is paid where it was meant to be and only there.** The per-row resolve of a secondary index entry is where a heap wins (0.22-0.34 us of CPU per returned row, 27-32 % of a 1,000-row range) and the point read is untouched; the lesson of DN-S0's spike (a point read's saving is of the order of the noise) survives the real build, so nothing in this feature makes a one-row secondary read faster.
2. **The heap's price is a second index on the write path, not the slot check.** A heap INSERT costs 3.7-3.8 us (15 %) more than its btree twin on B and does not cost that on A, where the heap was a btree with a hidden key. The key read through the Keystone index costs +1.2 to +2.5 us over a clustered descent, and the UPDATE by key +0.9 us. A workload that is mostly single-row INSERTs into a heap pays this; one that reads index ranges earns it back at about 15 resolved rows (3.8 us / 0.25 us per row, a rough figure from the 1,000-row cells).
3. **DN-R7 is met by absence**: a btree relation's code path did not change and its numbers did not move. The measurable limit is the sub-50 us cells' control range (-20 to +45 %), which cannot exclude a +-5 % regression; a deterministic count (instructions retired) would, and was not taken.
4. **Hidden and declared Keystone heaps are two storage policies, not one.** Only the hidden Keystone's tombstones are retired by the release; a declared key's are held for a `PURGE` so that the key stays bound (`CLAUDE.md`, invariant 11). A user who declares `id int64 PRIMARY KEY` on a heap and deletes rows gets btree-style growth until a `PURGE` - the page count in section 6 is the evidence. That is by design (`heap-and-tuple.md` §4.1c), but it means "heaps reuse space" is true of the hidden-key heap only.
5. **No open decision moved and no stated expectation is contradicted**; DN-S0's premise and DN-R7 are the two expectations this run gives data points to.

## 8. Limits of this run

- **Resolution.** The 20-50 us cells' control range (A2 against A, per round) is -20 to +45 %. Only long cells and paired comparisons (section 3) resolve a few percent.
- **The 100K single-row btree INSERT** (section 2) is inside the floor and not cleared.
- **The A heap is not a heap.** A's heap is a btree with a hidden key; "heap on A" in section 4 is the BZ heap DN replaces.
- **One host, one session, 26 rounds** (10 + 16; 200 rows 10 rounds), cores 4-6 pinned; no `cores > 1`, no `strict`, no cold reads, no PostgreSQL floor.
- **The page count of a heap after a release** is `sys.relation_stats.leaves`; no view shows free slots, so slot reuse is inferred from the count not growing.
- **Tests were not run** (this stage measures a build; no code was changed). No `--verify` mode exists in these drivers; row counts were asserted equal across the three servers for every read cell and `SELECT COUNT(*)` of the release cell read 20,000 on `tb` and `th` after 10,000 deletes and 10,000 inserts.
