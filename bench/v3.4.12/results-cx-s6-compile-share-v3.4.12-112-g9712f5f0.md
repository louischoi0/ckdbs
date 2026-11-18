# CX-S6, first cell: the compile's share of a repeated statement's latency

**Read §8 first.** The server's own `exec::Compile`, timed inside the
server by a spike (§8) - the most a compiled-chain cache keyed by the fetch
id could remove - is **2.8 % to 4.1 % of the client's p50 in every plain
shape at both core counts, and over CX-Q10's 5 % line only where a plan
governs the statement**: a two-variant plan at 5.7 % (`cores = 1`) and 6.3 %
(`cores = 2`), a one-relation plan at 5.3 % at `cores = 2` alone. Against
the server's own time every cell is over 5 % (5.3 % to 15.2 %). §7's
in-process figures ran in a hot loop and are superseded by §8. What follows
in this paragraph and §1-§6 is the first run, as it was written.

**Finding (the first run).** Every one of the 12 cells (6 statement shapes at `cores = 1` and
`cores = 2`) puts the compile at **7.6 % to 11.1 % of the client's p50**,
above CX-Q10's provisional 5 % line, and the cells repeat to within 0.3
percentage points across five rounds, so no cell is close enough to the line
for noise to decide it. The denominator is a Python client's wall time, about
33 to 36 µs of which is the client and the socket (§3); against the server's
own statement time the compile is 12 % to 47 %. **The qualification that
matters is in §4: `compile_us` is parse plus name resolution plus chain
compile, so it bounds from above what a compiled-chain cache could remove,
and it does not measure that.**

## 1. Stamp

| | |
|---|---|
| Executed | 2026-11-18, 13:35:55 to 13:37:02 UTC (cores 1 then cores 2, one server each, one after the other) |
| Branch / commit | `worktree-cx-plan-change-option` at `9712f5f0`, `git describe --tags` = `v3.4.12-112-g9712f5f0` |
| Tree | clean at the build and at the copy (`git status` showed only this run's new directory); after the run two files under `instructions/v3.2.0/` show modified, edited by the caller, no source or build input |
| Build | `build-release` in this worktree, `CMAKE_BUILD_TYPE=Release`, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, built at this commit in this session |
| Binary | source `build-release/kds_server`, mtime 2026-11-18 13:33:10 UTC (after the commit, built this session); copy `/home/louis/cxs6-run/kds_server` made 13:33:13 UTC; copy `sha256` `adaec037b95e83f147a4c9575c51945f5ab8643cad95dabfdf593434450f54d0` (`archive/.../binary.sha256`). Every server started from the copy |
| Device | data files and WAL under `/home/louis/cxs6-run/`: `/dev/nvme0n1p1`, ext4 (`df -T` at run time, `archive/.../host-before.txt`); not tmpfs |
| Server config | `cores` = 1 and 2, `durability = group`, `buffer_pool_frames = 16384`, `log_level = warn`, every other key default, so `join_order = fixed`; full config in each `cores<N>.json` |
| Ports | 25641 (cores 1), 25642 (cores 2); never the default |
| Pinning | server `taskset -c 2` (cores 1) and `-c 2,3` (cores 2; CPUs 2 and 3 are different physical cores); client `taskset -c 5` |
| Host | load average 0.38 / 0.92 / 1.74 at the start; no `cc1plus`, `cmake --build` or `ctest` process before any cell (per cell in each JSON's `host`); one idle unrelated `kds_server` (a web gateway's, 1.1 % CPU) was resident throughout |
| Driver | `archive/cx-s6-compile-share-v3.4.12-112-g9712f5f0/cx_s6_compile_share.py` (a new driver, copied from CB's close driver for its data shapes; nothing in `tools/` was changed), summarised by `summarize.py` into `summary.txt` |
| Client | `tools/kwp.py` `Connection.query`, one connection, Python |

## 2. What the number is, and one defect found on the way

The compile time comes from `sys.statement_stats.compile_us`, as the order
asked. **In this build the column is not the last run's time. `docs/spec/sys-views.md`
§6.1 says `compile_us` is "its last run's", but the code sums it:
`src/stats/statement_stats.cpp:113` does `shape.compile_ns += cost.compile_ns`
and `src/server/sys_views.cpp:1125` prints `Us(s.compile_ns)`, so the column is
the total over every call that compiled a chain, in microseconds.** (A first
probe on a fresh server read 4,141 µs on a point read, which is the sum, not
a 4 ms compile.) The spec and the code disagree; this file did not change
either. *(Review at `29401d0f`: they do not disagree. "Its last run's" in
§6.1 is a call's last run against its re-runs (DF-S1's review item 2), the
same words §6.1 uses for `rows`, which is a sum too; every cost column is a
sum over calls, `tests/statement_stats_test.cpp` pins `compile_ns` as one,
and §6.1 only fails to say "summed" for `compile_us` as it does for
`rows_examined`. The clause is now in §6.1 (the bugs entry that first recorded this was
deleted with the fix).)*

Used as a sum it is a good instrument. The sum carries sub-microsecond time
(it is summed in ns and truncated once), so the **per-call compile is
`delta(compile_us) / delta(calls)` over a pass of 10,000 calls**, read from
the view before and after each pass and outside the timed loop. The cost of
that is that **only a mean exists, never a distribution**: no compile p50 or
p99 is reported, because none can be derived from the sums. A sampling pass
that read the view after every statement was tried and discarded: it
measured a compile 3x larger (about 12 µs on the point read against 3.4 µs),
because the view's own scan evicts the caches the next compile runs in.

`total_us` is a sum of per-call elapsed times, each truncated to a whole
microsecond, so it reads low by up to 1 µs a call; on the point read that is
up to 14 % of the 7.2 µs, which is why the share against the server's time
(last column of §3) is secondary and the share against the client's p50 is
the headline. The server's `cpu_us` per call reads above its `total_us` for
the same reason (7.8 against 7.2 µs on the point read).

## 3. The table

Each cell: 2,000 warm-up statements, then **5 rounds x 10,000 statements**,
one statement at a time, each with a fresh literal drawn from a seeded RNG,
on one connection; the plain table cells never ran with a plan enabled, and
cells 4, 5 and 5b ran after both plans were created. p0 to p99 are the
**median of the five rounds' percentiles**. `compile` is the median of the
five rounds' means. The share is `compile / p50` per round, median, with the
range in brackets.

**`cores = 1`** (all times in µs; the share in %)

| cell | rounds x ops | p0 | p25 | p50 | p95 | p99 | compile (mean, per call) | compile / client p50 | server `total_us` per call | compile / server time |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 pk point read, `pb_bars`, 10,000 rows | 5 x 10,000 | 37.7 µs | 39.7 µs | **40.2 µs** | 45.8 µs | **52.8 µs** | 3.41 µs (3.35-3.44) | **8.4 %** (8.4-8.5) | 7.21 µs | 47.3 % |
| 2 two-relation join, captured | 5 x 10,000 | 77.3 µs | 93.5 µs | **98.2 µs** | 114.5 µs | **131.3 µs** | 7.84 µs (7.82-8.01) | **8.0 %** (8.0-8.1) | 50.56 µs | 15.5 % |
| 3 three-relation join, captured | 5 x 10,000 | 98.3 µs | 123.7 µs | **131.3 µs** | 157.4 µs | **192.5 µs** | 10.03 µs (9.85-10.43) | **7.6 %** (7.6-7.8) | 82.36 µs | 12.2 % |
| 4 cell 2 under a 2-variant plan | 5 x 10,000 | 79.7 µs | 102.2 µs | **110.6 µs** | 140.4 µs | **169.4 µs** | 11.11 µs (10.83-11.18) | **10.0 %** (9.9-10.1) | 63.12 µs | 17.6 % |
| 5 one relation, 1-relation plan, `SCAN` pin, 200 rows | 5 x 10,000 | 42.9 µs | 44.8 µs | **45.4 µs** | 53.8 µs | **63.1 µs** | 4.52 µs (4.46-4.55) | **9.9 %** (9.8-10.0) | 13.26 µs | 34.1 % |
| 5b control: cell 5's statement on a twin table, no plan | 5 x 10,000 | 41.9 µs | 43.9 µs | **44.4 µs** | 51.3 µs | **62.7 µs** | 3.63 µs (3.60-3.68) | **8.2 %** (8.1-8.3) | 12.25 µs | 29.6 % |

**`cores = 2`** (same cells, same driver, a fresh server and data file)

| cell | rounds x ops | p0 | p25 | p50 | p95 | p99 | compile (mean, per call) | compile / client p50 | server `total_us` per call | compile / server time |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 pk point read | 5 x 10,000 | 48.5 µs | 50.9 µs | **51.4 µs** | 56.8 µs | **63.7 µs** | 5.26 µs (5.21-5.34) | **10.2 %** (10.1-10.3) | 11.96 µs | 44.0 % |
| 2 two-relation join, captured | 5 x 10,000 | 131.3 µs | 153.9 µs | **160.4 µs** | 183.5 µs | **202.0 µs** | 14.27 µs (14.15-14.60) | **8.9 %** (8.9-8.9) | 83.35 µs | 17.1 % |
| 3 three-relation join, captured | 5 x 10,000 | 161.2 µs | 196.3 µs | **206.6 µs** | 237.6 µs | **258.8 µs** | 17.89 µs (17.87-17.95) | **8.7 %** (8.7-8.7) | 123.70 µs | 14.5 % |
| 4 cell 2 under a 2-variant plan | 5 x 10,000 | 134.4 µs | 163.7 µs | **174.4 µs** | 205.7 µs | **225.0 µs** | 18.95 µs (18.67-19.03) | **10.8 %** (10.8-10.9) | 95.70 µs | 19.8 % |
| 5 one-relation plan, `SCAN` pin | 5 x 10,000 | 57.6 µs | 60.2 µs | **60.8 µs** | 66.4 µs | **71.6 µs** | 6.74 µs (6.71-6.81) | **11.1 %** (11.0-11.1) | 21.62 µs | 31.2 % |
| 5b control, no plan | 5 x 10,000 | 55.7 µs | 58.5 µs | **59.0 µs** | 63.5 µs | **68.7 µs** | 5.42 µs (5.38-5.47) | **9.2 %** (9.1-9.2) | 20.11 µs | 26.9 % |

The shapes, as `ANALYZE` printed them in the run (`cores<N>.json`, `analyze`):

| cell | statement | what ran |
|---|---|---|
| 1 | `SELECT * FROM pb_bars WHERE id = <1..10000>` | `PointSelect`, one `Lookup`, no plan lookup (CX-R6) |
| 2 | `SELECT l.book_id, u.member_code FROM pc_users AS u JOIN pc_loans AS l ON l.user_id = u.id WHERE u.id = <1..200>` | `planned order=captured`: `Lookup` on `pc_users`, then an `IndexProbe` on `pc_loans (user_id)` (an index created for the run), about 50 rows a statement. The capture took one second of steward time to arrive; the cell ran after `order=captured` showed |
| 3 | the same, `JOIN pc_books AS b ON l.book_id = b.id` added | `order=captured`: `Lookup`, `IndexProbe`, then a `Probe` of `pc_books` per loan, about 50 rows |
| 4 | cell 2's statement | `CREATE PLAN p ... VARIANT a WHEN ARG(1) < 100 ORDER (u, l) VARIANT b ELSE ORDER (l, u)`; `order=plan plan=p/a` or `p/b` by the literal, so the two variants alternate and are not separated here |
| 5 | `SELECT * FROM t5 WHERE v = <1..50>`, 200 rows | `CREATE PLAN q ... VARIANT v ELSE ORDER (t5) ACCESS t5 SCAN`: `FilterScan t5 arm=plan` |
| 5b | the same on `t5b` | `FilterScan t5b`, no plan; with a one-relation plan enabled it still pays the plan table's lookup and misses |

**Noise floor.** The five rounds of one cell repeat to 0.0 to 0.3
percentage points on the share (largest: cell 3 at `cores = 1`, 7.6 to 7.8
%, and cell 3's compile 9.85 to 10.43 µs, 6 %), and to 3.0 % or less (max minus min over the first round's p50) on p50.
Nothing in the table is within a floor of the 5 % line. Cell 5 against its
control 5b is the one comparison where the plan's own cost can be read: 0.9
µs more compile at `cores = 1` (4.52 against 3.63 µs, the five rounds of
neither overlapping) and 1.3 µs at `cores = 2`, a one-relation statement
compiling twice. Cell 4 against cell 2 is 3.3 µs at `cores = 1` and 4.7 µs
at `cores = 2`, but cell 4 also runs a different join order half the time
(variant `b`, `pc_loans` first), so that difference is the second lex plus
the rule plus a different chain, and is not the second lex alone.

**Where the wait time is.** None of the cells waits on anything the engine
can name: they are autocommit reads, one statement at a time, and the view's
cumulative split on the final stats row of each cell (`final_stats_row` in
the JSON) shows `cpu_us` carrying the server's whole time with `sched_us`,
`commit_us`, `io_us`, `net_us`, `app_us` and `other_us` at 0 on the cells
read. A statement's latency is then client + socket + server CPU, and the
server CPU is itself parse + resolution + chain compile (`compile_us`) + run.
On the point read at `cores = 1`: 40.2 µs p50 = about 33 µs client and
socket (the client's `Connection.query` is Python; this figure is the
remainder after the server's 7.2 µs, so it is a difference of two
measurements and not a third one) + 7.2 µs server, of which 3.4 µs is the
compile. A write's commit wait is not in any cell.

## 4. Reading: does any cell cross 5 %?

By the order's definition, compile as a share of the client's p50, **every
cell crosses it, at both core counts**: the lowest is 7.6 % (cell 3, `cores
= 1`) and the highest is 11.1 % (cell 5, `cores = 2`), and two cores raise
the share in every cell (point read 8.4 % to 10.2 %, two-relation join 8.0 %
to 8.9 %). So the order's condition for building the cache is met on the
numbers as defined, and it is met at `cores = 2` as well.

Four things stop that from being the end of the reading. (1) **The client
inflates the denominator.** About 33 to 36 µs of every p50 is the Python
client and the socket, which a native client would not pay; the compile is a
larger share of anything the engine alone controls (the last column: 12 % to
47 % of server time, with the up-to-1-µs `total_us` truncation of §2 on
it). This direction favours building. (2) **`compile_us` is not what a
compiled-chain cache removes.** It runs from the head of the call to its
chain compiled, so lex, fingerprint, parse and name resolution are inside
it, and CX-S6's key is `(fetch_id, oids, plan, variant, cache_built_at,
catalog_version)` - a hit needs the fetch id, which needs the lex and the
fingerprint, so a cache removes the part after them, which this view cannot
separate; a point read's 3.4 µs, with no search and no plan lookup, is the
floor of what is inside. This direction opposes building, and it is the
reason the 5 % line cannot be applied to a cache's gain from this table
alone: the share of the compile after the fingerprint was not measured, and
measuring it needs an engine-side timer. (3) **The joins' compile is the
captured-plan path** (cells 2 and 3: a captured-order lookup and a full
ladder compile), and the plan cells add to it: 0.9 µs for a one-relation
plan, about 3 to 5 µs for the two-variant plan. The cache's key includes the
plan, which is where those microseconds go. (4) Cell 4's extra 3.3 µs over
cell 2 is the known second lex (`known-gaps.md`, "A multi-variant plan lexes
the statement a second time") together with the rule and the variant-b
chains; a cache that holds variant selection per execution (CX-S6) would
still pay the rule and the second lex's replacement, so the second lex is the
nearest thing in this table to a cost the cache does not help with.

**What the run supports.** The compile is a measurable share of every
repeated statement at both core counts, 7.6 to 11.1 % of the client p50; the
conservative reading of CX-Q10 (the line is on the compile's share, as
`compile_us` reports it) is crossed. What it does not support is a claim
about how much of that a cache returns, which §4(2) says needs a timer after
the fingerprint. A caller who sets the cache's threshold on `compile_us` as
this run did gets "build"; one who sets it on the removable part gets
"unmeasured".

## 5. What was cut, and what makes the number less trustworthy

- **Row-count sweep (rule 9): not run.** The cells are fixed at one size
  each (10,000 rows for the point read, 200 users and 10,000 loans for the
  joins, 200 rows for the one-relation scans), because the compile is a
  fixed cost and the question was its share of a repeated *small* statement;
  a larger table raises the run and lowers the share, which is exactly what
  cells 2 and 3 show (about 50 rows a statement) against cell 1. The
  compile itself does not depend on rows, but the share does, and its curve
  was not drawn: 1K and 10K versions of cells 2, 3 and 5 are the cut.
- **No distribution of the compile**, only the mean, and no split between
  lex/fingerprint and the rest (§2, §4).
- **No PostgreSQL floor and no prior kdbs number**: there is no earlier
  result for this shape, so this file is the baseline the next run reads
  against; the floor was not measured for this shape.
- **Cell 4 mixes two variants** (about half the literals each, by `ARG(1) <
  100` over `1..200`) and is reported as one row.
- **One server per core count, one connection**, so no concurrency, no
  conflicting writers, no `strict` durability and no saturation (rule 4b);
  every cell is a serial autocommit read, so what bounded it is the
  client's round trip, which was the bulk of the p50.
- **The client is Python**, so absolute p50 and p99 are the client's more
  than the engine's; `cores = 2` is 1.3 to 1.6x slower than `cores = 1` in
  every cell, in both the server's own time and the client's, and the cause
  (a cross-core wake on every statement is the first thing to check) was not
  investigated.
- **A second `sys.statement_stats` read per round** (before and after, outside
  the timed loop) is the only interference with the timed loops.
- No `--verify`: the driver exits on any statement error (none occurred) but
  does not compare result sets, and the engine suite was not run, since no
  engine code changed.
- Elapsed wall time for the two servers' cells was 67 s, inside the 20-minute
  budget; the cut above is a choice of scope and not of time.

## 6. Rerun

```
cp build-release/kds_server <dir>/kds_server    # then sha256sum it
taskset -c 5 python3 archive/cx-s6-compile-share-v3.4.12-112-g9712f5f0/cx_s6_compile_share.py \
  --bin <dir>/kds_server --wd <dir>/run1 --json cores1.json --status status1 \
  --cores 1 --port 25641 --cpus 2 --rounds 5 --ops 10000 --warm 2000
taskset -c 5 python3 .../cx_s6_compile_share.py --bin <dir>/kds_server --wd <dir>/run2 \
  --json cores2.json --status status2 --cores 2 --port 25642 --cpus 2,3 --rounds 5 --ops 10000 --warm 2000
python3 summarize.py <archive dir>
```

Raw files: `archive/cx-s6-compile-share-v3.4.12-112-g9712f5f0/`:
`cores1.json`, `cores2.json` (every round, every cell, `ANALYZE` lines, host
state, the plans' `sys.plans` rows, the final stats row), `cores1.log`,
`cores2.log`, `summary.txt`, `host-before.txt`, `binary.sha256`, the driver and
`summarize.py`. No data file or WAL segment is archived.

## 7. The removable part: the compile without the parse (added after §4)

§4's open question - how much of `compile_us` a cache keyed by the fetch id could remove, given that the key is made by the lex and the fingerprint - is answered here in-process, by a microbenchmark rather than an engine timer (`bench/compile_split_bench.cpp`, target `kds_compile_split_bench`, `-DKDS_BUILD_BENCH=ON`). It times `parser::Parser::Parse` (which lexes and fingerprints as it goes: what every statement pays, cached or not) apart from `exec::Compile` over the parsed statement (binding, the access ladder, the chain: **the most a cache could remove**, before its own lookup and the rebinding of a cached chain's literals).

| | |
|---|---|
| Executed | 2026-11-18, after §1's run, at the same commit `9712f5f0` with `bench/compile_split_bench.cpp` and its `CMakeLists.txt` target added (no engine source changed; `git describe --dirty` = `v3.4.12-112-g9712f5f0-dirty`) |
| Build | `build-release`, Release, reconfigured `-DKDS_BUILD_BENCH=ON`; copy `/home/louis/cxs6-split/kds_compile_split_bench`, sha256 `456135c0ee2fca77482e591e8892e99ab93baa3b4af396467f0e918df9723368` |
| Pinning, load | `taskset -c 2`; load average 0.40 / 0.44 / 1.18 before, no build or ctest running (`archive/.../load-before.txt`) |
| Method | one in-memory instance (`InMemoryPageStore`, no log), shapes *like* §3's at 2,000 / 200 / 2,000 rows; 16 literals cycled; 7 runs of 20,000 statements per shape, the median and the minimum per statement (`archive/.../split.txt`) |
| **Not §3's shapes** (found at review) | The joins filter `u.member_code > <k>` (a walk of `pc_users`) where §3's cells filter `u.id = <k>` (a `Lookup`), and the benchmark creates no index on `pc_loans (user_id)`, so its chains are not §3's `Lookup` / `IndexProbe` / `Probe`; `member_code` and `title` are `int64`, not `varchar`. Cell 5's statement runs on `pb_bars`, not `t5`. The plain shapes compile with no options, where §3's server, under `fixed`, ran the captured path with a captured order equal to the written one (one compile, a map lookup); the plan shapes run with `search` off (`written`), not `fixed`. The two-variant shape's literals choose the reordering variant 7 times in 16, §3's about half. **The "captured order not the written one" row is no §3 cell**: §3's cells 2 and 3 captured the written order (`sys.plans` in `cores<N>.json`), so that row is a synthetic case set beside cell 3's p50 |

| shape | parse p50 | compile p50 (removable at most) | removable / §3's cores 1 client p50 |
|---|---|---|---|
| pk point read | 1,409 ns | 574 ns | 1.4 % of 40.2 µs |
| two-relation join, one compile | 3,280 ns | 1,555 ns | 1.6 % of 98.2 µs |
| three-relation join, one compile | 4,654 ns | 2,462 ns | 1.9 % of 131.3 µs |
| three-relation join, a captured order not the written one (no §3 cell) | 4,589 ns | 5,300 ns | 4.0 % of 131.3 µs |
| two-relation join, 2-variant plan | 3,184 ns | 4,198 ns | 3.8 % of 110.6 µs |
| one-relation statement, no plan | 1,345 ns | 602 ns | 1.4 % of 44.4 µs |
| one-relation statement, its plan | 1,341 ns | 1,419 ns | 3.1 % of 45.4 µs |

Every minimum is within 4 % of its median (the three-relation compile's is 3.5 % below). **Against the client's p50, as CX-Q10 is written, no shape's hot-loop removable part reaches 5 %**: 1.4 % to 4.0 %. The highest are a compile that runs twice - a block run in an order other than its written one compiles the written order first (`CompileBlock`: its refusal is the statement's and it is the decline's fallback), then the order in force; the two-variant plan also pays the second lex of `AdminPlan::Choose`. Skipping the first compile is not free: the written order's refusal is what the statement answers, so a skip needs that refusal known without the compile.

**What bounds the reading** (the review's, at `29401d0f`):

- **The loop is hot.** Each shape compiles 20,000 times back to back, so its code and the catalog it reads stay in cache; in the server the run, the reply and the socket sit between two compiles, and §2 saw a compile triple when a view scan ran between them. In §3's server `compile_us` is 1.4x to 1.9x the in-process parse plus compile (3.41 µs against 1.98 µs on the point read); part of that gap is the dispatcher's borrow and name resolution, which no cache removes, and part is cache state, which a hot loop hides. Scaled by that ratio - an upper bound, since it counts the dispatcher's part as removable - the removable part is 2.5 % to 5.7 % of the client's p50: **cell 4 at about 5.7 % and cell 5 at about 5.1 %, on the line**. Only a timer around `exec::Compile` inside the server separates the two.
- **Against the server's own time three cells cross 5 %**: the point read 8.0 % (0.57 of 7.2 µs), cell 4 6.7 %, cell 5 10.7 % (5b 4.9 %, cells 2 and 3 about 3 %), each from the hot loop, over a `total_us` that reads low by up to 1 µs a call (§2). The line was drawn against a Python client's p50, whose 33 to 36 µs of client and socket (§4) the engine does not pay; a client that costs less moves every share up.
- **`cores = 2` is not measured in-process** (the benchmark is one thread). Scaled by §3's `compile_us` ratio between the core counts, the synthetic reorder row comes to about 4.6 % of cell 3's `cores = 2` p50 and the two-variant plan to about 4.1 %.
- **A cache's own lookup and literal rebinding are not measured**; they come off the removable part.

## 8. The server's own compile, timed (spike)

**Finding.** With a `steady_clock` timer around the one `exec::Compile` call of a client SELECT inside the server, the part a compiled-chain cache keyed by the fetch id could at most remove is **2.8 % to 6.3 % of the client's p50**. Against CX-Q10's 5 % line (the client's p50, as written) **three of twelve cells cross: cell 4 (the two-variant plan) at both core counts (5.7 %, 6.3 %) and cell 5 (the one-relation `SCAN`-pin plan) at `cores = 2` (5.3 %)**; the other nine are under, cell 5 at `cores = 1` closest at 4.4 %. Against the server's own per-call time the line is crossed in **every cell at both core counts** (5.3 % to 15.2 %). The in-process hot loop of §7 (1.4 % to 4.0 %) understated the in-server number by 1.5x to 2x, as §7's review suspected. This measures only the removable ceiling: it excludes a cache's own lookup and literal rebinding, which come off it.

### Stamp

| | |
|---|---|
| Executed | 2026-11-18, 13:57:45 to 13:58:52 UTC (`cores = 1`, then `cores = 2`, one server each, one after the other; 67 s, inside the 20-minute budget) |
| Branch / commit | `worktree-cx-plan-change-option` at `29401d0f`, `git describe --tags` = `v3.4.12-113-g29401d0f` (the order named `-114`; the describe at the run said `-113`, so the archive directory is `cx-s6-compile-spike-v3.4.12-113-g29401d0f`). Tree dirty: the next stage's work (see Binary) and this file's edits |
| Binary | the caller's spiked build, **not built here**: `build-release` at `29401d0f` plus uncommitted next-stage work (`ADVISE PLAN` and its view, called by no cell) plus a two-line spike in `CommandDispatcher`'s SELECT compile, never committed: a `steady_clock` timer around the single `exec::Compile(...)` call of a client SELECT, summed into two process-wide counters that `sys.meta` prints as `spike_compile_ns` and `spike_compile_calls`. It counts binding, the access ladder, the order search or captured/admin plan path and the chain, and excludes lex, parse, fingerprint, the read borrow, name resolution's retry loop and everything after. Source `/home/louis/cxs6-spike/kds_server`, mtime 2026-11-18 13:56:39 UTC; run copy `/home/louis/cxs6-spike-run/kds_server`, `sha256` `2cefa37bd7deb0b6815c431b44a18caa46de7358df803ae0c56d8f80e5d17e3d` (equals the caller's; `archive/.../binary.sha256`). Every server started from the copy. The binary is newer than HEAD's commit and carries uncommitted code, so it is not the engine at `29401d0f` alone; the compile path the cells call is that commit's, but the diff beyond the spike was not audited here |
| Device | data files and WAL under `/home/louis/cxs6-spike-run/run1`, `run2`: `/dev/nvme0n1p1`, ext4 (`df -T`), not tmpfs |
| Server config | as §1: `cores` = 1 and 2, `durability = group`, `buffer_pool_frames = 16384`, `log_level = warn`, default `join_order = fixed` |
| Ports, pinning | 25661 (`cores = 1`), 25662 (`cores = 2`); server `taskset -c 2` and `-c 2,3`; client `taskset -c 5` |
| Load | load average 1.27 / 2.62 / 2.02 at the cell-4 reads of `cores = 1` and 1.20 / 2.45 / 1.98 at `cores = 2`'s (a decaying tail of earlier work; 1.67 / 2.89 / 2.07 four minutes before; top showed 96 % idle); no `cc1plus`, `cmake --build` or `ctest` before any cell (per cell, each JSON's `host`) |
| Driver | `archive/cx-s6-compile-spike-v3.4.12-113-g29401d0f/cx_s6_compile_spike.py`: §1's driver, copied, with one method added (`spike()`, reads the two counters from `sys.meta`) and the counter deltas stored per round; data, statements, plans, cell order, 2,000 warm-up and 5 rounds x 10,000 unchanged. The counters are read immediately before and after each timed loop, with the `sys.statement_stats` reads outside them; `tools/` untouched |
| Watchdog | a 60 s loop writing `/home/louis/cxs6-spike-run/watch.txt` from the drivers' status files; the run finished inside one interval |

### The table

All times in µs, medians of the five rounds. `spike` is `delta(spike_compile_ns) / delta(spike_compile_calls)` per pass; `compile_us` is `sys.statement_stats`' per-call mean as in §3; `front` is `compile_us - spike` (lex, fingerprint, parse, borrow, name resolution, and the view's own truncation). Server time is `total_us` per call, which reads low by up to 1 µs (§2).

**Counter check.** In every one of the 60 passes `delta(spike_compile_calls)` equals the 10,000 timed statements exactly (0 excess): the `sys.meta` and `sys.statement_stats` reads, which are SELECTs, are not counted in the window, so nothing was subtracted.

**`cores = 1`**

| cell | client p50 | spike (server's `exec::Compile`) | `compile_us` | front (`compile_us - spike`) | spike / client p50 (rounds min-max) | spike / server time |
|---|---|---|---|---|---|---|
| 1 pk point read | 40.2 | 1.12 | 3.53 | 2.41 | 2.8 % (2.75-2.89) | 15.1 % |
| 2 two-relation join, captured | 99.3 | 3.50 | 8.17 | 4.67 | 3.5 % (3.44-3.59) | 6.7 % |
| 3 three-relation join, captured | 131.5 | 4.42 | 10.31 | 5.88 | 3.4 % (3.31-3.38) | 5.3 % |
| 4 cell 2 under a 2-variant plan | 109.8 | 6.22 | 11.02 | 4.81 | **5.7 %** (5.60-5.75) | 9.9 % |
| 5 one-relation plan, `SCAN` pin | 45.5 | 2.01 | 4.55 | 2.54 | 4.4 % (4.37-4.43) | 15.0 % |
| 5b control, no plan | 44.7 | 1.29 | 3.82 | 2.53 | 2.9 % (2.84-2.94) | 10.2 % |

**`cores = 2`**

| cell | client p50 | spike | `compile_us` | front | spike / client p50 (rounds min-max) | spike / server time |
|---|---|---|---|---|---|---|
| 1 pk point read | 51.1 | 1.80 | 5.30 | 3.50 | 3.5 % (3.51-3.54) | 15.2 % |
| 2 two-relation join, captured | 161.2 | 6.62 | 14.47 | 7.85 | 4.1 % (4.09-4.16) | 8.0 % |
| 3 three-relation join, captured | 204.2 | 8.26 | 17.93 | 9.67 | 4.0 % (4.05-4.10) | 6.7 % |
| 4 cell 2 under a 2-variant plan | 174.0 | 10.89 | 18.94 | 8.04 | **6.3 %** (6.23-6.28) | 11.4 % |
| 5 one-relation plan, `SCAN` pin | 60.7 | 3.21 | 6.98 | 3.76 | **5.3 %** (5.26-5.32) | 14.6 % |
| 5b control, no plan | 58.6 | 1.95 | 5.59 | 3.64 | 3.3 % (3.32-3.34) | 9.6 % |

Server `total_us` per call behind the last column (µs): `cores = 1` 7.37, 51.86, 82.95, 62.70, 13.40, 12.68; `cores = 2` 11.83, 83.26, 123.70, 95.55, 21.96, 20.27. Noise floor: the five rounds of one cell span at most 0.15 points of share and 0.3 µs of spike; against §3 the same cells' client p50 and `compile_us` differ by 0 to 4 % (p50 40.2 against 40.2, 99.3 against 98.2, 131.5 against 131.3 at `cores = 1`), the only cross-run floor available (no cell was replicated inside this run). Cell 5 at `cores = 2` is 0.3 points over the line, which is inside that cross-run drift, so its crossing is not firm; cell 4's (0.7 and 1.3 points) is outside it.

### Reading

- **Crossing 5 % of client p50 (CX-Q10 as written):** cell 4 at both core counts (5.7 %, 6.3 %) and cell 5 at `cores = 2` (5.3 %, soft). Under it: cells 1, 2, 3, 5b at both core counts and cell 5 at `cores = 1` (4.4 %). Two cores raise every share by 0.6 to 1.0 point, as §3's did.
- **Crossing 5 % of the server's own time:** all twelve cells, 5.3 % to 15.2 %. The point read and cell 5 are the largest (about 15 %): the cheapest statements spend the biggest part of their server time binding and laddering. Cell 3 at `cores = 1` (5.3 %) is the one near the line.
- **What the parse side costs.** `front` is 2.4 to 9.7 µs and larger than the spike in every cell but cell 4 (where the spike is the larger, 6.2 against 4.8 µs and 10.9 against 8.0 µs): the lex, fingerprint, parse, borrow and resolution are 54 % to 68 % of `compile_us` in cells 1, 2, 3, 5 and 5b, and 42 % to 44 % in cell 4. A cache keyed by the fetch id pays all of `front`; it can remove only the spike column.
- **Where the removable time is.** The plan cells carry more of it: cell 4's spike is 1.8x cell 2's (a variant is chosen at every compile, including `AdminPlan::Choose`'s second lex, and about half the literals compile a reordered block, whose written order compiles first, §7); cell 5's is 1.6x its twin's (the plan table's lookup plus the pinned arm's legality check). §7's hot loop understated cell 4 by about 2x (4.2 µs in the loop against 6.2 µs here, `cores = 1`).
- **What the cell does not say.** It is a ceiling on removal, not a measured saving: a cache's own lookup, the rebinding of a cached chain's literals and the invalidation test are not subtracted. The client is Python and contributes 33 to 36 µs of every p50; a native client moves every client-p50 share up (the server-time column is that bound). `cores = 2`'s per-call spike is 1.5x to 1.8x `cores = 1`'s in every cell, in the compile itself; the cause was not investigated.
- **For CX-Q37.** (a) as adopted (the removable compile against the client's p50, line at 5 %, `cores = 1`) is **crossed by one cell of six, cell 4 at 5.7 %**; the other five are 2.8 % to 4.4 %. So §7's "no shape reaches 5 %" holds for five of six and fails for the multi-variant plan, the shape the cache's variant key exists for. Cells 1, 2, 3 and 5b are under by 1.5 to 2.2 points, outside the floor. Against the server's time (CX-Q37 (c)) all six cross. Whether one crossing cell justifies a cache is the operator's choice among (a), (b), (c); this run settles the measurement, not that. A cheaper route to part of cell 4's cost is not a cache: the known second lex in `AdminPlan::Choose` (`known-gaps.md`).

### What was cut

- One server and one connection per core count, serial autocommit reads, `group` durability only; the row-count sweep (rule 9) not run (§5's cut stands); no PostgreSQL floor; cell 4 still mixes both variants (about half each) in one row.
- No distribution of the spike, only the per-pass mean (the counters are sums); no replicate of one cell inside the run.
- The binary carries uncommitted next-stage code beyond the spike; the engine suite was not run (this measurement edited only `bench/`).
- Raw files: `archive/cx-s6-compile-spike-v3.4.12-113-g29401d0f/`: `cores1.json`, `cores2.json` (per round: client percentiles, `server_delta`, `spike_ns`, `spike_calls`), `cores1.log`, `cores2.log`, `summary-spike.txt`, `summary.txt` (§3's summariser over the same files), `summ2.py`, `times.txt`, `binary.sha256`, the driver and `summarize.py`. No data file or WAL segment is archived.

Rerun: as §6, with `--bin /home/louis/cxs6-spike-run/kds_server`, ports 25661 / 25662, the driver above, then `python3 summ2.py <archive dir>`.
