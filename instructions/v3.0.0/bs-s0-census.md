# BS-S0 census - what the sanitizers cost, what a sweep has to cover, what a differential generator must not say

**Measured 2026-10-13** on `bs-s0-census` at `691beee3`
(`v2.7.0-968-g691beee3`), for `workorder-bs-test-hardening.md` §3 BS-S0. No
code entered the tree: every build below is a scratch CMake project outside
it that compiles the tree's sources unchanged, and the one instrumented file
(a copy of `sim/loop.cpp` with counters, plus a counting `operator new`)
lives only in that scratch project.

Host: 8 CPUs, gcc 13.3.0, kernel 7.0.0-1014-azure. Other sessions were
building on the host during part of the run; the load average at each
run's start is given beside it.

## 1. Toolchain facts the order did not know

| # | Fact | Consequence |
|---|---|---|
| T1 | **clang is not installed** (`clang++` absent; `/usr/lib/llvm-18/lib` holds only the shared libraries another package pulled in) and installing it needs `sudo`, which asks for a password | BS-Q2 (a) cannot be met on this host as it stands. gcc 13 gives ASan, UBSan, TSan, LSan and gcov; it gives **no libFuzzer and no llvm-cov**, so S2 and S5 as marked wait on the install |
| T2 | **gcc 13's TSan refuses to start on this kernel**: `FATAL: ThreadSanitizer: unexpected memory mapping`, exit 66, before `main` (seen in the census session; the log was overwritten by the later `setarch` runs) - most likely the kernel's mmap randomization is wider than this libtsan's shadow layout expects, which cannot be confirmed while `/proc/sys/vm/mmap_rnd_bits` is unreadable | Every TSan run goes under `setarch x86_64 -R` (ASLR off for that process), which needs no privilege and was verified to work. S1's runner has to carry it |
| T3 | **`tests/alloc_counter.cpp` breaks under ASan**: it replaces `operator new(size_t)` and every `operator delete` with `malloc`/`free`, but not `operator new(size_t, nothrow_t)`. gtest allocates with the nothrow form, which ASan supplies, and frees through the replaced `delete`: **31 alloc-dealloc-mismatch reports** in the subset (§2), all at `alloc_counter.cpp:32` under `testing::TestSuite::Run` | A harness defect, not an engine one. The timed runs used `halt_on_error=0`; **with ASan's default options the subset aborts at the first report** (`ABORTING`, exit 1, no cell completed - verified once), so the suite cannot run under ASan until this is fixed. S1 replaces the nothrow forms too (a test-file fix, BS-R2, and outside §6.1's ruling) |

## 2. Sanitizer wall time

The subset is 20 test sources - `wal_*_test.cpp` (16), `mount_recovery`,
`recovery_undo`, `btree`, `btree_race` - with `alloc_counter.cpp` and
`heap_suspension_env.cpp`: **333 cells, about 9 % of the suite's ~3,659**.
Chosen for the recovery and I/O paths and for one concurrency rig
(`btree_race`). Every configuration is Debug (`-O0 -g`), the suite's own
build type, with the sanitizer flags added; the binary runs the cells
serially in one process. The sim run is `ckdbs-sim --seed 1 --ops 1500
--iterations 2 --mode clean --profile uniform`, the corpus's own shape.

| Configuration | Flags | Subset, round 1 / 2 | × Debug | Sim, round 1 / 2 | × Debug | Cells |
|---|---|---|---|---|---|---|
| Debug | - | 44.5 s / 44.5 s (load 9.3 / 1.7) | 1.0 | 1.21 s / 1.21 s | 1.0 | 333 pass |
| ASan + UBSan | `-fsanitize=address,undefined -fno-omit-frame-pointer` | 130.9 s / 131.1 s (load 4.9 / 1.5) | **2.9** | 4.40 s / 4.49 s | **3.7** | 333 pass |
| TSan | `-fsanitize=thread`, run under `setarch -R` | 180.2 s / 218.3 s (load 0.4 / 1.4) | **4.0-4.9** | 5.66 s / 5.79 s | **4.7** | 333 pass |

Findings in those runs:

- **ASan + UBSan: no engine report.** No UBSan `runtime error`, no
  LeakSanitizer report, nothing in the sim run. The only reports are T3's 31.
- **TSan: no `WARNING: ThreadSanitizer` report** in either round, in the
  cells or the sim. The subset holds one concurrency rig (`btree_race`) and
  the sim drives one session, so this says nothing about the rig-heavy rest
  of the suite, where the order expects false reports from coroutine and
  latch synchronization TSan cannot see (§3 BS-S1). The two rounds differ by
  21 % with no load to explain it; the higher figure is the one to budget.

Build cost, for the runner's budget: the library, the subset and
`ckdbs-sim` built at `-j6` in 52 s (Debug, partly warm), 155 s (ASan) and
88 s (TSan) on a host other builds were sharing.

**Extrapolation, not measured**: the full Debug suite took ~272 s serially
at 3,329 cells (at `25c58497`, 2026-09-07) and 62-101 s at `ctest -j8` on
~3,008 cells (2026-09-28). At this subset's ratios the full suite would
take roughly 13-15 min serially under ASan + UBSan and 18-24 min under
TSan (each lower figure at the 3,329-cell count, each higher one scaled to
today's ~3,659); `-j8` divides that by the parallelism the rig cells
allow. The estimate is a floor: the subset ran as one process, while
`ctest` starts one process per cell, and a sanitizer's per-process start-up
times ~3,659 is not in the ratio. S1 measures the real figure; nothing here
says the other 91 % of the cells are clean. The nightly's budget (BS-Q1
(b)) is set from that figure, and every BS configuration runs there, never
from the push hook (`workorder-bs-test-hardening.md` §6.1).

The Debug and ASan load averages above were read when each run started, in
the census session; the driver's progress file was overwritten by the TSan
rounds, so only the TSan figures survive in the scratch evidence.

## 3. Operation counts of a typical sim run (bounds S3)

Counted by the instrumented copy of the loop over the corpus's six committed
seeds × three value profiles × `clean` and `crash`, at `--ops 1500
--iterations 2 --faults none` (72 iterations, every one `SIM ok`) - no
injected fault, the baseline a sweep counts its failure points from. "Inside statements"
is what `ExecuteOp` does on the driving thread; "per iteration" is
everything the devices saw from creation through the reboot, recovery and
reconcile.

**Device operations per `clean` iteration (1,500 ops)** - min / median / max
over 36 iterations:

| Device op | min | median | max |
|---|---|---|---|
| page read | 32 | 45 | 78 |
| page write | 300 | 420 | 535 |
| page sync | 109 | 113 | 120 |
| page grow | 1 | 1 | 1 |
| log write | 552 | 626 | 723 |
| log sync | 587 | 665 | 761 |
| log read | 6 | 6 | 6 |
| **total** | **1,591** | **1,869** | **2,217** |

`crash` iterations stop at a seed-drawn op (median 816 ops) and see a median
963 device operations. Page reads under a statement are rare - 1 to 3 in
27 of the 72 iterations, none in the rest - so almost every read failure
lands in the reboot's recovery or in the harness's own integrity sweep
(`sim/integrity.cpp`'s `SweepAllocatedPages`, which fetches every
allocated page and is counted in the per-iteration figure); S3 has to drive
a smaller pool or a longer run if it wants more than a handful of failed
reads under a statement.

**Allocations inside `ExecuteOp`** (global `operator new`, driving thread,
`clean` iterations; the largest op over both modes). `ExecuteOp` also runs
the oracle's update and the reply check, so these are an upper bound on
the statement's own allocations:

| Profile | per op (mean) | largest single op |
|---|---|---|
| uniform | 773 | 119,944 |
| zipfian | 794 | 119,944 |
| colliding | 1,169 | 159,960 |
| all `clean` iterations | median 841; 0.95 M - 2.37 M per iteration | 159,960 |

Which statement allocates 160 k times was not identified.

**What that means for S3**, with a Debug iteration at ~0.6 s:

- **I/O sweep: exhaustive is affordable.** ~1,870 failure points per
  `clean` iteration, one run each: ~19 min per seed-iteration in Debug,
  ~2 h for one `clean` iteration of one profile of each of the six
  committed seeds; the corpus's three profiles and two iterations make it
  ~11 h before the crash modes, divided by the cores it runs on - a
  nightly cost, not a gate one (§6.1).
- **Allocation sweep: exhaustive is not.** ~1.26 M failure points per
  iteration at one forked child each is ~200 h of Debug even if a child
  costs one iteration; a child forked just before its statement costs that
  statement plus a crash, reboot and reconcile, which was not measured. S3
  has to choose: a short plan (tens of ops) swept exhaustively, a fixed
  budget of failure points drawn per statement kind, or both. The order's
  "one forked process per failure point" stands; the number of points does
  not fit a night without one of those.

## 4. KDS against the PostgreSQL 18 twin: what a generator must avoid or normalize

Read from the tree at `691beee3` (specs, `manual/sql/sql.md`, the parser
and executor sources); PostgreSQL behaviour checked on the twin,
`/home/cdkbs/pg-bench-ba`, port 15434: **PostgreSQL 18.6, `datcollate = C`,
`TimeZone = Etc/UTC`**, databases `postgres` and `bench`.

**The governing fact: KDS's SQL surface is a narrow subset of
PostgreSQL's**, so most differences are "never generate", not "rewrite the
answer". A `WHERE` is an AND-list of simple predicates, a select list is
columns and the five aggregates, the one join is the inner equi-join.

| Area | KDS | PostgreSQL 18 | Generator |
|---|---|---|---|
| Order with no `ORDER BY`, ties under one, `GROUP BY` output | Defined: pk order, emission order, first-seen (`parser-v2.md:130,136,141`; `aggregate.md:22`) | Unspecified | **Normalize**: compare as sorted multisets; end every `ORDER BY` in a unique key |
| `LIMIT`/`OFFSET` without a total order | A prefix of the emission order | Unspecified | **Avoid** |
| NULL ordering | Largest: last `ASC`, first `DESC`; no `NULLS FIRST/LAST` (`null.md:195-198`) | Same defaults | Safe; avoid the `NULLS` clause |
| Column nullability default | `NOT NULL` unless `NULL` (`null.md:126-135`) | Nullable | **Normalize DDL**: spell `NULL`/`NOT NULL` on every column |
| Primary key | First column, integer, `[1, 2^48-1]`, never updatable, a committed key refused until `PURGE` (`heap-and-tuple.md` §4.1; `sql.md:164-202, 622-631`) | Any | **Normalize DDL** (`bigint PRIMARY KEY`); **avoid** omitted keys, reused keys, pk updates, out-of-range ids |
| Integer names | `int8`/`int16`/`int32`/`int64` are 1/2/4/8 bytes; `uint64` exists (`sql.md:458`; `src/exec/row_codec.cpp:88-95`) | `int8` is 8 bytes; no 1-byte or unsigned type | **Normalize DDL**; avoid `int8` and `uint64` or map them with a `CHECK` |
| Implicit conversion | None: `int_col = '20'`, `int_col = 20.0`, cross-family and mismatched-`(p,s)` comparisons refused at compile (`types.md:171-198`; commit `691beee3`) | Coerces | **Avoid**: a literal's kind matches its column; compare columns within one family |
| Epoch integer against a date or timestamp | `d = 20000` accepted, and an epoch integer against a timestamp too (`types.md:184-187`) | Error | Avoid |
| Boolean | Literals `0`/`1` only, rendered `0`/`1` (`row_codec.cpp:178-184, 1244-1261`) | `true`/`false`, rendered `t`/`f` | **Normalize** literals and output |
| `decimal(p,s)` | Both arguments required, p ≤ 38; excess fractional digits refused, never rounded (`types.md:15-19, 130-155`) | Optional; `INSERT` rounds | **Avoid** bare `decimal`, p > 38, literals with more than s fractional digits |
| Numeric literal forms | `12.`, `.5`, `1e5` not numbers (`types.md:64-71`) | Valid | Avoid |
| `float` | Refused (`types.md:14`) | Supported | Avoid |
| `char(N)` | Zero-padded, byte-compared on the unpadded value, N in bytes (`types.md:90-95`) | Space-padded, trailing spaces ignored, N in characters | **Normalize** to PG `varchar(N)`; avoid trailing spaces and non-ASCII |
| `varchar(N)` | N is a cell width in `[16, 4096]`, not a cap; 8,144-byte limit (`types.md:97-105`) | A length cap | **Normalize** to PG `text`; avoid N < 16 and values over 8,144 bytes |
| String literals | No escape: a literal cannot hold `'` (`src/parser/lexer.cpp:112-122`) | `''` | Avoid `'` in strings |
| Collation | Byte order (`row_codec.cpp:1368-1389`) | `C` on the twin: byte order | Safe on this twin; keep strings ASCII |
| Dates and timestamps | `YYYY-MM-DD[ HH:MM:SS[.ffffff]]`, 1900-2999, UTC, no `TIME`/`INTERVAL`/tz types (`types.md:14-20`) | Wider | Avoid other forms and ranges |
| Timestamp rendering | Six fractional digits when nonzero (`types.md:395-396`) | Trailing zeros trimmed | **Normalize** to six digits, or avoid sub-second values |
| `AVG` | Decimal columns only; result at the column's scale, half-even (`aggregate.md:18, 159-180`) | Any numeric; about 16 significant digits, never fewer than s fractional ones: on the twin 8 fractional digits at an 11-digit integer part, 4 at 14-17, and s itself - already rounded half away from zero - at 25 | Avoid on integers; keep integer parts small, then **normalize** PG's value half-even to s in the comparator (PG's `round()` is half away from zero) |
| `SUM` | Checked int64 / int128, overflow fails the statement (`aggregate.md:19, 141-157`) | `sum(bigint)` is `numeric`, never overflows | Avoid sums near ±2^63 |
| Aggregated output | No `ORDER BY`, `LIMIT`, `HAVING`; `GROUP BY` plain columns only (`aggregate.md:23, 95-108`) | All | Avoid |
| Select list | `*` (one relation) or columns and aggregates; no expressions, literals, aliases (`sql.md:722-726`) | All | Avoid |
| `WHERE` shape | AND-list; no `OR`, no bare `NOT`, no parentheses; a literal may not stand on the left (`sql.md:748-762`; `parser.cpp:1094-1121`) | All | Avoid |
| Operators | `= != < <= > >=`; **`<>` does not lex** (`lexer.cpp:209-227`) | Both spellings | Emit `!=` |
| `IN (list)`, `LIKE`, `NOT BETWEEN`, `IS TRUE`, `IS DISTINCT FROM` | Refused (`parser-v2.md:22-24`; `parser.cpp:1139-1153, 1255-1258`) | Supported | Avoid; `BETWEEN` and `IS [NOT] NULL` are safe |
| Subqueries | Predicate position only; depth ≤ 4; none with aggregates, `ORDER BY` or `LIMIT` inside; no derived tables or CTEs (`parser-v2.md:74-79`) | All | Avoid the rest; make a scalar subquery a pk lookup |
| Joins | Inner equi-join, one qualified equality, `AS` required on an alias, no `SELECT *` over a join, outer joins refused (`sql.md:728-744`; `parser.cpp:2631-2731`) | All | Avoid the rest |
| Functions | `DATE(col)` and `NOW()` in a `WHERE` comparison only (`functions.hpp:21-22`) | Everywhere | `DATE(ts) = 'YYYY-MM-DD'` safe; **avoid `NOW()`** (not deterministic) |
| `UPDATE ... SET` expressions | No widening (`int32 + int64` refused); division by zero and overflow refuse the row; decimal `/` scale up to s+16; `ROUND`/`CAST` half away from zero (`types.md:240-332`) | Widens; its own `numeric` scale rule | Same-type integer arithmetic and the string functions safe; avoid mixed types, decimal `/` without `ROUND`, `NOW`/`CURRENT_DATE`, and every error-raising input |
| Identifiers | Keywords and column names case-insensitive, table names byte-exact; eleven reserved words (`sql.md:1194-1201`; `catalog.cpp:1454`) | Folds to lower case; many reserved words | Lower-case names only, reserved in neither |
| `INSERT` | No column list, literals only, ≤ 1,024 rows (`sql.md:524-571`) | Wider | Positional `VALUES` to both |
| Caps | Sort > 1,048,576 rows, > 65,536 groups, > 100 M rows touched fail (`parser-v2.md:130, 196`; `aggregate.md:27`) | None | Keep data small |
| Transactions | No `SAVEPOINT`, `ROLLBACK TO`, `AND CHAIN`; `PURGE` autocommit only | Supported | Avoid |

**Safe to generate for both, compared as sorted multisets unless the order
is total**: lower-case DDL with an `int64` pk (`bigint PRIMARY KEY` on the
twin), explicit nullability, `int16`/`int32`/`int64`, `decimal(p,s)` with
p ≤ 38, `date`, `varchar` (`text` on the twin); `INSERT` naming fresh keys
with literals of the column's kind; `SELECT` of `*` or columns from one
relation or a chain of `JOIN ... AS ... ON a.x = b.y` within one type family;
a `WHERE` AND-list of column-to-literal and same-family column-to-column
comparisons, `BETWEEN`, `IS [NOT] NULL`, `[NOT] IN (SELECT ...)`, `[NOT]
EXISTS`, pk-lookup scalar subqueries and `DATE(ts) op 'YYYY-MM-DD'`;
`ORDER BY` up to eight keys ending in a unique one, then `LIMIT`/`OFFSET`;
`COUNT`, `MIN`, `MAX` and small-valued `SUM` with plain-column `GROUP BY`;
`AVG(decimal)` after half-even normalization; `UPDATE` with same-type
arithmetic, `CASE`, `COALESCE`, `NULLIF`, `GREATEST`, `LEAST` and the ASCII
string functions on non-pk columns; `DELETE`.

Unverified in source and to be checked before the generator relies on it:
whether `SELECT DISTINCT` parses (the select-list parser would read
`DISTINCT` as a column name), and whether `INNER`, `CROSS`, `USING` or a
comma `FROM` list is refused (none appears in the join parser).
