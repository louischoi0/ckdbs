# CX-S6, first cell: the compile's share of a repeated statement's latency

**Finding.** Every one of the 12 cells (6 statement shapes at `cores = 1` and
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
either.

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
| Method | one in-memory instance (`InMemoryPageStore`, no log), the shapes of §3 at 2,000 / 200 / 2,000 rows; 16 literals cycled; 7 runs of 20,000 statements per shape, the median and the minimum per statement (`archive/.../split.txt`) |

| shape | parse p50 | compile p50 (removable at most) | removable / §3's cores 1 client p50 |
|---|---|---|---|
| pk point read | 1,409 ns | 574 ns | 1.4 % of 40.2 µs |
| two-relation join, one compile | 3,280 ns | 1,555 ns | 1.6 % of 98.2 µs |
| three-relation join, one compile | 4,654 ns | 2,462 ns | 1.9 % of 131.3 µs |
| three-relation join, a captured order not the written one | 4,589 ns | 5,300 ns | 4.0 % of 131.3 µs |
| two-relation join, 2-variant plan | 3,184 ns | 4,198 ns | 3.8 % of 110.6 µs |
| one-relation statement, no plan | 1,345 ns | 602 ns | 1.4 % of 44.4 µs |
| one-relation statement, its plan | 1,341 ns | 1,419 ns | 3.1 % of 45.4 µs |

Every minimum is within 3 % of its median. **Against the client's p50, as CX-Q10 is written, no cell's removable part reaches 5 %**: 1.4 % to 4.0 %. The two highest are not what a cache is for - a captured order that differs from the written one pays two compiles (the written order's, then the captured one's: `CompileBlock` compiles the written order first for its refusal and its fallback), and the two-variant plan pays the second lex of `AdminPlan::Choose` and the variant's compile - each removable by skipping work the compile already knows it will throw away, not by caching. **Against the server's own time the picture differs**: §3's point read spends about 7.2 µs in the server, of which 0.57 µs is 8 %; the line was drawn against a Python client's p50, whose 33 to 36 µs of client and socket (§4) the engine does not pay. A client that costs less would move every share up. Not measured: `cores = 2` in-process (the benchmark is one thread), and a cache's own lookup and rebinding, which would come off the removable part.
