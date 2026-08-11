# The output sort, measured

`docs/workplan-order-by.md` OB4/OB5. What `exec::OutputSort` costs a statement
that uses it, what it costs one that does not, and where the time inside a
sorted statement actually goes.

The thesis, and every number below is in service of it: **on this engine the
expensive part of answering a query is formatting the reply, not finding the
rows — and `ORDER BY … LIMIT n` is the shape that proves it.** At 10,000 rows
the walk costs 102.5 ns a row and rendering and shipping the row costs 191.5;
so a top-20 sort that decides which rows can matter *before* it formats them
answers in **1230.4 µs where a binary that formats first takes 2220.1**, and
is now **cheaper than returning the same relation unsorted** (2981.3 µs)
rather than 1.03× its server CPU. `ORDER BY <pk> ASC` is elided at compile and
costs nothing measurable. A real `ORDER BY <non-pk>` costs +26.6% at 200 rows,
+47.2% at 1,000 and +77.0% at 10,000, and the decomposition says three
quarters of that is buffering and `std::sort`, not normalizing keys. And a
plain `SELECT` of any shape is 10.0–15.2% faster than on the commit this
branch left, with `SELECT *` a further 12–16 ns a row faster again because its
schema lookup left the per-row path.

| | |
|---|---|
| **Run** | 2026-08-11, 08:31:31 – 08:35:18 UTC (200 rows, then 1,000, then 10,000) |
| **Worktree / branch** | worktree `feat-order-by`, branch `worktree-feat-order-by` |
| **Commit** | `393b5a4` ("docs: the recovery plan's status, and a false claim in section 4a"), committed 2026-08-11 05:57:06 UTC |
| **Tree** | **dirty, and the feature is the dirt**: 38 tracked paths differ from `393b5a4`, among them `include/kds/exec/sort.hpp`, `src/exec/sort.cpp`, `src/exec/row_codec.{hpp,cpp}`, `src/exec/step_compiler.cpp`, `src/parser/parser.cpp` and `src/server/command_dispatcher.cpp`. The measured engine is therefore *not* any commit. **The compiled surface is pinned by the sha256 of `git diff HEAD -- src include`: `4b1f52f3bbe0516d2d7fdce3e55415670d7a9a54a1e0d4c82025394105233220`**, taken at build time and re-checked after the last run and after the suite — unchanged, so no source moved under the binary while it was being measured. The whole-tree `git diff HEAD` is `d0262df37f9c57b71f5a87c3fa8fc752d0629d264c0defb1173466cf6010914a`; it differs from the build-time value only in `bench/docs/README.md`, which this run's own driver documentation was written into and which is not compiled. **No engine source was touched to produce any number here.** |
| **Binary provenance — `after`** | `build-release/kds_server`, mtime 2026-08-11 08:09:17 UTC, md5 `dd4bbb42ea96a81fc19559bafbd493ae`. **Newer than HEAD** (05:57:06) and newer than the last source edit: `cmake --build` was re-run immediately before the pin above was taken, so the binary measures the working tree and not an older engine. |
| **Binary provenance — `pre-fix`** | mtime 2026-08-11 07:23:38 UTC, md5 `1013e20aef8c43cad7aef991acee4e66`. The same working tree at an earlier state, pinned by `git diff HEAD` = `fcba570e9997c6d24ca19786989f5d92c2daf92da4c15644b91758b8ec921da0`, which is kept beside the binary and re-hashed before this run. It differs from `after` in exactly three compiled files — `include/kds/exec/sort.hpp`, `src/exec/sort.cpp`, `src/server/command_dispatcher.cpp` — and in nothing else; see §0.2 for what those three changes are. It is here so that "the split is worth X" is a row of this run rather than a comparison against a previous results file. |
| **Binary provenance — `base`** | mtime 2026-08-11 07:25:26 UTC, md5 `c2ee1fbfe64052caac8e6dec1bad38bf`, from `git archive 393b5a4` **plus four compile-only repairs** — see §0.3. `393b5a4` does not compile as it stands. |
| **Build type** | Release, all three, `cmake -DCMAKE_BUILD_TYPE=Release`, same toolchain (gcc 13.3.0). `./build` is Debug and was not used for any number here. |
| **Device** | `/dev/nvme0n1p1` (NVMe SSD, `ROTA=0`, ext4) mounted at `/`; data files under `/home/cdkbs/bench-orderby`. **Not tmpfs** — checked with `stat -f`, which reports ext2/ext3 for that path. |
| **Machine** | AMD EPYC 9V74, **2 vCPU**, 15 GiB, kernel 6.17.0-1022-azure. 1-minute load average 1.03 / 1.14 / 1.22 at the start of the three sizes and 1.24 / 1.22 / 1.66 at their ends, recorded per size — the rise is this run's own three servers plus the driver on two cores. `pgrep cc1plus` empty before every size. |
| **Interference, and what was done about it** | This host runs several agents at once. Each size was gated behind a loop that refuses to start while any `scenario*`, `*_benchmark.py`, `ctest`, `kds_tests` or `cc1plus` is running **and** while the 1-minute load average is above 1.3; it held the 200-row start back by 100 seconds waiting for another worktree's `scenario2_freight.py --cargos 100000` to finish. The servers listen on **ports 15871 / 15872 / 15873** rather than the default 15432, so nothing else could attach to them. |
| **Server config** | Defaults throughout: `cores = 1`, `durability = group`, `inline_cell_width = 64`, `sort_max_rows = 1048576`, waystone recording/replay on, access statistics on, no Cabins, no indexes. One server process and one **fresh data file** per binary per row-set size — nine servers, nine data files. |
| **Correctness** | `--verify` passed on all nine server/size combinations: 8 checks per size on the two sort-carrying binaries (row count; `ORDER BY id ASC` equals the unsorted reply row for row; `SELECT *` equals the explicit projection row for row; `ORDER BY val` ascending; same row multiset; `DESC` descending; `ORDER BY tag` ascending; `LIMIT 20` equals the first 20 rows of the full sorted order), 3 per size on `base`. `build-release/tests/kds_tests` — see §0.4, which is **not** a clean pass. |
| **Baseline** | **No PostgreSQL comparison was run.** See §9 — it is an environment failure, not an omission, and it was re-checked live rather than restated. |

Drivers, flags and the exact invocation: `bench/docs/README.md`
(`order_by_benchmark.py`). This file states findings; it does not re-explain
how to run the tool.

---

## 0. How the comparison is kept honest

**Three binaries, one driver, interleaved block by block.** Three binaries
cannot share a data file, so this is three server processes — but they are
driven by one Python process that runs, in each of 20 rounds, one block of
every arm on *every* side before any arm's next block. A machine that gets
busier partway through costs all three equally instead of inventing a result.
The block rather than the single statement is the interleaving unit because
server CPU is sampled around it and `/proc/<pid>/stat` resolves one jiffy.

**Equal work, not equal time.** Every arm at a given size gets the same fixed
operation count: 8,000 at 200 rows, 3,000 at 1,000, 1,200 at 10,000 (`--ops`,
split into 20 interleaved blocks by `--rounds 20`). All three sides hold
byte-identical contents — the row list is generated once from `--seed 1` and
inserted into each.

**The size sweep is `--rows`, and it is literally the row count.**
`--rows 200 / 1000 / 10000` inserts exactly that many rows into one relation;
there is no multiplier to work out. The relation is
`(id int64, val int64, grp int32, amount int64, tag varchar)`,
`BTREE`-clustered, and the sort keys are `val` (random over a domain of
`10 × rows`, so neither pre-sorted nor tie-heavy) and `tag` (a `varchar` over
the same domain). Every arm projects all five columns, so the reply grows with
the relation and the sort's key is independent of it.

**Two passes, because the three binaries do not answer the same statements.**
Pass 1 runs the nine arms all three can answer — a non-pk `ORDER BY` is
`Unsupported` on `base`. Pass 2 runs all fifteen arms across the two binaries
that carry a sort.

**The noise floor comes from inside the run.** `plain` is executed twice per
side per pass — once as `plain`, once as `plain-again` — and the two are the
same configuration by construction:

| rows | pass 1 `after` | pass 1 `pre-fix` | pass 1 `base` | pass 2 `after` | pass 2 `pre-fix` |
|---:|---:|---:|---:|---:|---:|
| 200 | 0.00% | 0.12% | 0.11% | 0.00% | 0.00% |
| 1,000 | 0.07% | 0.04% | 0.64% | 0.07% | 0.18% |
| 10,000 | **1.44%** | 0.76% | 0.14% | 0.57% | 0.53% |

**The floor is 1.44% at p50**, worst case across those fifteen repeats, and
≤0.64% in thirteen of them. Nothing smaller than a size's own floor is
reported as a finding. Three further controls — `PING`, a pk point lookup, and
the unsorted `LIMIT 20` — cannot be moved by a sort, and across the three
binaries they agree to within 0.8, 0.6 and 1.6 µs respectively at p50.

**The driver does not take the elision on trust.** Before timing anything it
runs `ANALYZE` on every arm and reads `sorted=` out of the reply. A binary that
reported a sort for `ORDER BY id ASC`, or for a statement with no clause at
all, or a top-N holding other than `offset + limit`, aborts the run rather
than mislabel a row.

**A caveat on the CPU column at the short arms.** `/proc/<pid>/stat` resolves
one 10 ms jiffy. At 10,000 rows the `LIMIT 20` arms spend about 47 ms of CPU
across their whole 1,200 operations — five jiffies — so their per-operation
CPU (8.3 to 50.0 µs across sides) is quantization, not signal. Every CPU
number quoted as a finding below belongs to an arm spending at least 100
jiffies.

### 0.1 `ANALYZE` is used as an instrument, and here is why that is legitimate

The decomposition in §2 needs to price the walk apart from the formatting, and
no pair of `SELECT`s can do that. `RunAnalyze` can: it compiles the same
chain, runs the same steps through the same sink, arms the same
`OutputSort`, drains it through the same `EmissionQuota`, records the same
trail — and then answers with one line of plan text instead of the rows.
`src/server/command_dispatcher.cpp` says so where it does it: *"The rows are
not rendered — the reply is the plan — so the sort holds keys and empty
text. That is the one respect in which ANALYZE's run is cheaper than the real
one."*

That one respect is exactly what it is used for. `plain` − `an-plain` is
rendering and shipping the rows; `an-nonpk-lim` − `an-plain` is normalizing a
sort key and testing it against the heap; `an-nonpk` − `an-nonpk-lim` is
retaining and sorting every row instead of twenty. The four `an-*` arms are an
instrument, not a workload — nobody's OLTP traffic is `ANALYZE` — and the
decomposition they produce closes against the measured statement to within
0.5% at 10,000 rows, 0.7% at 1,000 and 4.6% at 200, which is the check that
the instrument is measuring what it claims.

### 0.2 What `pre-fix` is, precisely

`pre-fix` differs from `after` in three compiled files and nothing else. The
difference is one design change and two consequences of it:

| what | where |
|---|---|
| `OutputSort::Note` split into `Admit(frame) -> StatusOr<bool>` and `Take(std::string&)`. The dispatcher's sink asks `Admit` — which normalizes the row's sort keys and compares them against the heap's worst retained row — and calls `render` **only if it returns true**. `pre-fix` renders first and offers the text afterwards. | `include/kds/exec/sort.hpp`, `src/exec/sort.cpp`, `src/server/command_dispatcher.cpp` |
| `Take` swaps rather than moves, so an evicted row's key vector and text buffer go back to the caller's scratch and a long top-N run settles into allocating nothing per row | `src/exec/sort.cpp` |
| the `SELECT *` per-row `catalog_.InitTableAccess` hoisted out of `render` and resolved once per statement | `src/server/command_dispatcher.cpp` |
| the post-walk quota drain folded into one `exec::DrainSorted` shared by `HandleSelect` and `RunAnalyze`; `sorted_rows()` folded into `rows().size()`; two predicates added to the cross-core remote-read eligibility test | `include/kds/exec/sort.hpp`, `src/server/command_dispatcher.cpp` |

The last row is behaviour-preserving at the configuration measured: the
eligibility test's outermost condition is `remote_reads_ != nullptr`, which is
null at `cores = 1`, so its two new predicates are not evaluated on any path
below. A run at `cores > 1` would be measuring a different question and has
not been done.

**There is also a drift between the two binaries that no source change
explains, and it is stated rather than absorbed.** `an-plain` — a walk with no
render, over code that is byte-identical in the two trees — is 2.7% faster on
`after` at 10,000 rows (1066.0 vs 1096.2 µs) and within 0.3% at the two
smaller sizes. `plain` carries the same drift (1.5% at 10,000). Nothing in the
three changed files touches the walk, so this is compiler layout: the same
instructions at different addresses. **It sets the real resolution of any
`after`-vs-`pre-fix` claim at about 1%, or 4 ns a row**, and every such claim
below is reported net of it or is far enough above it not to matter.

### 0.3 The four repairs the `base` binary needed

`393b5a4` does not compile. The base tree is `git archive 393b5a4` with
exactly four repairs, all compile-only and none on the SELECT path:

| file | repair | why it is not a behaviour change |
|---|---|---|
| `include/kds/txn/undo_page.hpp` | two `offsetof` `static_assert`s dropped, `kMaxUndoImageLen` assert 8108 → 8092 | assertions only; the constant is *derived* from `kUndoRecordHeaderSize`, which RC06 already widened to 44 |
| `src/wal/redo.cpp` | `std::span<std::byte, kPageSize> page;` → an `std::optional` filled by the three arms | a fixed-extent span has no default constructor; identical control flow |
| `src/server/command_dispatcher.cpp` (×2) | `kNoTrxId` → `txn::kNoTrxId` | name qualification |
| `src/server/command_dispatcher.cpp` | `return {"ERR " + …, false};` → `return "ERR " + …;` in `InsertOneRow` | the function returns `std::optional<std::string>`; error path only |

Diffed against `393b5a4` the base tree's `command_dispatcher.cpp` differs by
three lines, none inside `HandleSelect`.

### 0.4 The test suite was run, and it is not green

`build-release/tests/kds_tests` on the `after` binary's tree: **2,211 tests,
2,196 passed, 15 failed.** The 15 are `UndoPageTest` (2), `UndoLogTest` (2),
`LogScannerTest` (2), `RedoTest` (1) and `RecoveryUndoTest` (8) — every one a
stale expectation of the pre-RV10 28-byte undo record header, e.g.
`TheWidestHeapTupleCannotBeUndone` asserting `7u` against an actual `23`. They
are inherited from the `feat-wal-recovery` line this worktree sits on, they
predate the sort, and **none is in `exec/`, the parser or the dispatcher's
read path.** The targeted suites are clean:
`*OrderBy*:*Sort*:*Pagination*:*StepCompile*:*Parser*` — **266 tests, all
passing**, of which the sort's own are **24 `OrderByExecTest` + 2
`OrderKeyTest`**, run again on their own and green.

---

## 1. The headline: deciding before rendering halves a bounded sort

`ORDER BY val LIMIT 20` over 10,000 rows answers in **1230.4 µs** at p50 and
spends **1258.3 µs** of server CPU. The same statement on `pre-fix`, whose
sink renders every row and only then offers it to the sort, takes **2220.1 µs
and 2200.0 µs of CPU**. Both retain twenty rows; the difference is that one of
them formats 9,980 rows it will throw away and the other does not.

| rows | binary | statement | ops | p0 | p25 | p50 | p95 | p99 | mean | CPU µs/op |
|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 200 | `after` | `ORDER BY val LIMIT 20` | 8000 | 60.5 | 70.1 | **71.1** | 82.1 | 101.8 | 74.9 | 51.2 |
| 200 | `pre-fix` | `ORDER BY val LIMIT 20` | 8000 | 73.0 | 83.5 | **84.8** | 96.3 | 112.2 | 86.6 | 70.0 |
| 200 | `after` | `LIMIT 20`, no `ORDER BY` | 8000 | 29.2 | 37.8 | 38.7 | 46.0 | 55.6 | 39.2 | 25.0 |
| 200 | `pre-fix` | `LIMIT 20`, no `ORDER BY` | 8000 | 29.3 | 37.7 | 39.0 | 47.2 | 54.8 | 40.7 | 26.2 |
| 200 | `after` | no `ORDER BY` | 8000 | 70.1 | 79.8 | 81.2 | 92.6 | 107.3 | 83.7 | 56.2 |
| 200 | `pre-fix` | no `ORDER BY` | 8000 | 71.2 | 81.3 | 82.7 | 94.7 | 109.9 | 85.6 | 61.2 |
| 1,000 | `after` | `ORDER BY val LIMIT 20` | 3000 | 155.8 | 166.4 | **168.6** | 186.1 | 309.5 | 198.6 | 146.7 |
| 1,000 | `pre-fix` | `ORDER BY val LIMIT 20` | 3000 | 240.0 | 252.5 | **255.6** | 276.1 | 368.1 | 262.0 | 243.3 |
| 1,000 | `after` | `LIMIT 20`, no `ORDER BY` | 3000 | 28.6 | 37.1 | 38.4 | 47.4 | 63.9 | 39.4 | 26.7 |
| 1,000 | `pre-fix` | `LIMIT 20`, no `ORDER BY` | 3000 | 28.7 | 37.6 | 38.8 | 47.5 | 59.3 | 39.0 | 20.0 |
| 1,000 | `after` | no `ORDER BY` | 3000 | 258.3 | 272.5 | 276.3 | 296.8 | 343.9 | 281.9 | 236.7 |
| 1,000 | `pre-fix` | no `ORDER BY` | 3000 | 259.7 | 275.1 | 278.8 | 298.5 | 363.8 | 286.2 | 220.0 |
| 10,000 | `after` | `ORDER BY val LIMIT 20` | 1200 | 1204.8 | 1223.1 | **1230.4** | 1291.6 | 2648.0 | 1266.5 | 1258.3 |
| 10,000 | `pre-fix` | `ORDER BY val LIMIT 20` | 1200 | 2187.0 | 2209.9 | **2220.1** | 2317.4 | 3354.4 | 2257.0 | 2200.0 |
| 10,000 | `after` | `LIMIT 20`, no `ORDER BY` | 1200 | 29.0 | 37.5 | 38.8 | 50.8 | 67.7 | 41.2 | 25.0 |
| 10,000 | `pre-fix` | `LIMIT 20`, no `ORDER BY` | 1200 | 28.8 | 37.7 | 39.3 | 50.0 | 65.4 | 39.7 | 33.3 |
| 10,000 | `after` | no `ORDER BY` | 1200 | 2807.3 | 2900.2 | 2981.3 | 4164.5 | 4609.1 | 3228.5 | 2091.7 |
| 10,000 | `pre-fix` | no `ORDER BY` | 1200 | 2829.6 | 2942.9 | 3025.4 | 4177.5 | 5013.3 | 3250.3 | 2141.7 |

**The saving is per skipped row, which is what attributes it to the render.**

| rows | rows skipped | p50 saved, µs | per skipped row, ns | CPU saved, µs | per skipped row, ns |
|---:|---:|---:|---:|---:|---:|
| 200 | 180 | 13.7 | 76.1 | 18.8 | 104.2 |
| 1,000 | 980 | 87.0 | 88.8 | 96.6 | 98.6 |
| 10,000 | 9,980 | 989.7 | 99.2 | 941.7 | 94.4 |

Server CPU saved per skipped row is **94–104 ns across a 50× range of relation
size** — flat, which is the signature of a per-row cost and not of anything
per statement. That number *is* the cost of formatting one five-column row on
this dispatcher, measured by not doing it.

**Against the unsorted `LIMIT 20`, the multiple falls from 56.5× to 31.7×.**

| rows | sorted `LIMIT 20` ÷ unsorted `LIMIT 20`, `after` | same, `pre-fix` |
|---:|---:|---:|
| 200 | **1.84×** | 2.17× |
| 1,000 | **4.39×** | 6.59× |
| 10,000 | **31.7×** | 56.5× |

Forty-four per cent of the multiple is gone at 10,000 rows, and what is left
is not render at all — §2 takes it apart. The `pre-fix` column is worth a
second's attention on its own account: it is the arm that motivated the split,
remeasured here beside its replacement rather than quoted, and it lands at
56.5× where the run that first found it recorded 56.7×. The harness is
reproducing itself to a third of a per cent across four hours and a rebuild.

**And the bounded sort is now cheaper than the unsorted full scan.**

| rows | `ORDER BY val LIMIT 20` ÷ no `ORDER BY`, p50 | same, CPU | `pre-fix`, p50 | `pre-fix`, CPU |
|---:|---:|---:|---:|---:|
| 200 | 0.88× | 0.91× | 1.03× | 1.14× |
| 1,000 | 0.61× | 0.62× | 0.92× | 1.11× |
| 10,000 | **0.41×** | **0.60×** | 0.73× | 1.03× |

That row is the whole finding in one line. Retaining twenty rows out of ten
thousand used to cost as much server CPU as returning all ten thousand
(1.03×); it now costs three fifths of it.

---

## 2. Where the remaining 1230 µs goes

The residual is a walk, and the walk is irreducible: a sorted `LIMIT` cannot
stop early, because rows [0, 20) of the *sorted* reply are not rows [0, 20) of
the emitted one. `ANALYZE` confirms it does not try — `examined=10000` on
every sorted arm at every size, against `examined=20` on the unsorted
`LIMIT 20`. That is `pagination.hpp`'s division as designed: the quota bounds
output, the row-touch budget bounds work.

The four `ANALYZE` arms price the walk apart from the reply. Their own
distributions, `after`, pass 2:

| rows | binary | statement | ops | p0 | p25 | p50 | p95 | p99 | mean | CPU µs/op |
|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 200 | `after` | `ANALYZE LIMIT 20` | 8000 | 30.8 | 39.9 | 40.6 | 48.9 | 61.1 | 42.4 | 25.0 |
| 200 | `after` | `ANALYZE` no `ORDER BY` | 8000 | 49.1 | 58.2 | 59.3 | 69.0 | 82.1 | 61.4 | 45.0 |
| 200 | `after` | `ANALYZE ORDER BY val LIMIT 20` | 8000 | 58.9 | 68.6 | 69.7 | 81.1 | 95.0 | 72.6 | 57.5 |
| 200 | `after` | `ANALYZE ORDER BY val` | 8000 | 66.7 | 77.4 | 78.4 | 90.0 | 128.1 | 82.7 | 62.5 |
| 1,000 | `after` | `ANALYZE LIMIT 20` | 3000 | 30.2 | 39.5 | 40.4 | 50.1 | 69.4 | 41.3 | 20.0 |
| 1,000 | `after` | `ANALYZE` no `ORDER BY` | 3000 | 130.1 | 140.4 | 142.2 | 156.5 | 200.3 | 145.6 | 123.3 |
| 1,000 | `after` | `ANALYZE ORDER BY val LIMIT 20` | 3000 | 157.8 | 169.3 | 171.8 | 189.7 | 252.7 | 175.5 | 156.7 |
| 1,000 | `after` | `ANALYZE ORDER BY val` | 3000 | 230.6 | 246.7 | 253.4 | 281.5 | 417.7 | 260.0 | 243.3 |
| 10,000 | `after` | `ANALYZE LIMIT 20` | 1200 | 30.5 | 38.8 | 40.8 | 52.6 | 112.6 | 44.0 | 25.0 |
| 10,000 | `after` | `ANALYZE` no `ORDER BY` | 1200 | 1045.6 | 1060.2 | 1066.0 | 1113.8 | 1543.4 | 1082.5 | 1075.0 |
| 10,000 | `after` | `ANALYZE ORDER BY val LIMIT 20` | 1200 | 1205.4 | 1220.4 | 1226.9 | 1264.2 | 1908.1 | 1246.0 | 1216.7 |
| 10,000 | `after` | `ANALYZE ORDER BY val` | 1200 | 2808.1 | 2918.9 | 2930.4 | 3136.3 | 3707.4 | 2972.9 | 2900.0 |

### 2.1 The waits, named

Every arm here is a **read-only autocommitted `SELECT` on one connection**, so
three of the wait types a KDS latency can decompose into are absent rather
than small, and it is worth saying which and why instead of reporting a zero:

- **durability / commit (fsync): does not apply.** No arm appends a WAL
  record, so no arm reaches a group-commit drain. `durability = group` is in
  the stamp because it governed the *load*, which is setup and not a measured
  arm.
- **write-statement wait: does not apply.** No arm writes.
- **lock or conflict wait: does not apply.** One connection, no concurrent
  writer, snapshot reads. There is nothing to queue behind.

What remains decomposes into five terms, each the measured difference between
two measured arms rather than a model. At **10,000 rows**, for
`ORDER BY val LIMIT 20` (1230.4 µs at p50):

| wait | µs | share | how it is obtained |
|---|---:|---:|---|
| client + socket round trip | 26.6 | 2.2% | `PING` p50 — one send, one receive, no engine work |
| statement fixed cost: parse, compile, snapshot, relation open, and a 20-row walk | 12.2 | 1.0% | unsorted `LIMIT 20` p50 − `PING` p50 |
| read: walk and decode the other 9,980 rows | 1025.2 | 83.3% | `ANALYZE` no-`ORDER BY` p50 − `ANALYZE LIMIT 20` p50 — a 10,000-row walk minus a 20-row one, neither rendering |
| normalize one sort key per row and test it against the heap, 10,000× | 160.9 | 13.1% | `ANALYZE ORDER BY val LIMIT 20` p50 − `ANALYZE` no-`ORDER BY` p50 |
| render 20 rows and ship them | 5.5 | 0.4% | the residual |
| **total** | **1230.4** | **100%** | |

**Five sixths of a bounded sort is now the walk itself.** The sort's own
contribution — normalizing an `int64` into an `OrderKey` and one comparison
against `buffer_.front()` — is 16.1 ns a row, 13% of the statement. There is
no large optimization left in this shape short of not walking, and a sorted
`LIMIT` cannot not walk.

The same five terms across the sweep, per row rather than per statement, so a
fixed cost separates from a per-row one:

| term | 200 rows, ns/row | 1,000 rows, ns/row | 10,000 rows, ns/row |
|---|---:|---:|---:|
| walk and decode | 93.5 | 101.8 | 102.5 |
| normalize key + heap test | 52.0 | 29.6 | 16.1 |
| buffer every row + `std::sort` | 43.5 | 81.6 | 170.3 |
| render + ship, streamed as walked (`plain`) | 109.5 | 134.1 | 191.5 |
| render + ship, deferred through the buffer (`ORDER BY val`) | 122.0 | 153.2 | 234.6 |

Three readings. **The walk is flat** at ~100 ns a row over a 50× range, which
is what a per-row cost looks like and what makes it a floor. **The
normalize-and-test term falls with size** because it is dominated by the
heap-fill phase — at 200 rows one row in ten is buffered against one in five
hundred at 10,000 — so 16.1 ns is the honest steady-state figure and 52.0 ns
is a small-relation artefact standing on a 10.4 µs difference. **The buffering
term rises fourfold**, and §6 comes back to it.

Closure, which is the check that the instrument is honest: modelling each
measured statement as the sum of its terms gives 1224.9 against a measured
1230.4 for the bounded sort at 10,000 rows (+0.4%), 2979.3 against 2981.3 for
the unsorted scan (+0.1%) and 5274.7 against 5276.7 for the unlimited sort
(+0.0%). At 200 rows the bounded sort closes to +4.6%, which is where the
per-statement terms are large enough that attributing them by subtraction gets
coarse.

---

## 3. The unsorted path is 10–15% faster than the commit this branch left

The one change the feature makes to a statement that does not sort is the
dispatcher's row rendering: `src/server/command_dispatcher.cpp` now appends
every value of a row into one reused `std::string` through a shared `render`
lambda and streams that string once, where `393b5a4` made one `os << …`
insertion per value. The sink also gained one `sorter_.active()` predicate per
row and the statement one `sorter_.Reset` call. Net of all three, the
statement is faster at every size.

| rows | binary | statement | ops | p0 | p25 | p50 | p95 | p99 | mean | CPU µs/op |
|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 200 | `after` | no `ORDER BY` | 8000 | 70.2 | 80.0 | **81.4** | 94.3 | 145.2 | 86.5 | 63.8 |
| 200 | `base` | no `ORDER BY` | 8000 | 79.3 | 89.1 | **90.4** | 102.3 | 151.7 | 95.4 | 71.2 |
| 200 | `after` | no `ORDER BY`, repeated (floor) | 8000 | 70.1 | 80.1 | 81.4 | 93.6 | 147.5 | 87.8 | 61.2 |
| 200 | `base` | no `ORDER BY`, repeated (floor) | 8000 | 79.2 | 88.9 | 90.3 | 102.5 | 151.1 | 95.7 | 75.0 |
| 200 | `after` | `ORDER BY id ASC` (elided) | 8000 | 70.9 | 80.4 | 81.9 | 92.8 | 118.7 | 84.5 | 60.0 |
| 200 | `base` | `ORDER BY id ASC` (elided) | 8000 | 79.5 | 89.7 | 91.0 | 103.3 | 144.6 | 95.8 | 68.8 |
| 200 | `after` | `LIMIT 20`, no `ORDER BY` | 8000 | 29.3 | 37.6 | 38.7 | 46.3 | 53.7 | 40.5 | 26.2 |
| 200 | `base` | `LIMIT 20`, no `ORDER BY` | 8000 | 29.8 | 38.8 | 39.5 | 47.3 | 58.2 | 44.2 | 27.5 |
| 200 | `after` | `WHERE id = ?` (control) | 8000 | 26.0 | 34.5 | 35.8 | 43.4 | 57.6 | 36.8 | 22.5 |
| 200 | `base` | `WHERE id = ?` (control) | 8000 | 25.8 | 32.5 | 35.7 | 43.2 | 56.7 | 36.4 | 10.0 |
| 200 | `after` | `PING` (client + socket floor) | 8000 | 19.9 | 26.8 | 28.0 | 31.6 | 41.0 | 28.5 | 10.0 |
| 200 | `base` | `PING` (client + socket floor) | 8000 | 18.1 | 27.0 | 28.3 | 32.3 | 39.9 | 28.6 | 13.8 |
| 1,000 | `after` | no `ORDER BY` | 3000 | 258.3 | 272.9 | **276.3** | 297.7 | 592.2 | 290.2 | 220.0 |
| 1,000 | `base` | no `ORDER BY` | 3000 | 306.0 | 320.9 | **325.7** | 344.4 | 535.4 | 334.8 | 270.0 |
| 1,000 | `after` | no `ORDER BY`, repeated (floor) | 3000 | 257.9 | 272.5 | 276.1 | 297.6 | 478.6 | 285.8 | 240.0 |
| 1,000 | `base` | no `ORDER BY`, repeated (floor) | 3000 | 303.0 | 319.9 | 323.6 | 340.6 | 432.8 | 330.6 | 273.3 |
| 1,000 | `after` | `ORDER BY id ASC` (elided) | 3000 | 259.4 | 273.3 | 277.0 | 297.2 | 570.1 | 289.0 | 236.7 |
| 1,000 | `base` | `ORDER BY id ASC` (elided) | 3000 | 305.9 | 321.3 | 326.5 | 346.0 | 593.9 | 336.5 | 270.0 |
| 1,000 | `after` | `LIMIT 20`, no `ORDER BY` | 3000 | 28.7 | 37.3 | 38.2 | 46.0 | 62.2 | 39.7 | 20.0 |
| 1,000 | `base` | `LIMIT 20`, no `ORDER BY` | 3000 | 30.0 | 39.0 | 39.8 | 48.4 | 74.0 | 42.6 | 26.7 |
| 1,000 | `after` | `WHERE id = ?` (control) | 3000 | 25.4 | 34.8 | 36.6 | 47.4 | 73.5 | 39.5 | 16.7 |
| 1,000 | `base` | `WHERE id = ?` (control) | 3000 | 25.1 | 35.4 | 37.1 | 48.4 | 71.5 | 39.8 | 13.3 |
| 1,000 | `after` | `PING` (client + socket floor) | 3000 | 19.9 | 27.1 | 27.9 | 33.5 | 45.8 | 29.3 | 23.3 |
| 1,000 | `base` | `PING` (client + socket floor) | 3000 | 19.9 | 27.5 | 28.4 | 33.1 | 40.8 | 29.8 | 16.7 |
| 10,000 | `after` | no `ORDER BY` | 1200 | 2787.9 | 2923.2 | **3025.1** | 4250.1 | 4911.0 | 3345.2 | 2091.7 |
| 10,000 | `base` | no `ORDER BY` | 1200 | 3322.2 | 3450.5 | **3527.7** | 4721.9 | 5638.8 | 3831.1 | 2600.0 |
| 10,000 | `after` | no `ORDER BY`, repeated (floor) | 1200 | 2792.0 | 2913.9 | 2981.6 | 4271.8 | 5131.2 | 3301.3 | 2083.3 |
| 10,000 | `base` | no `ORDER BY`, repeated (floor) | 1200 | 3312.4 | 3457.4 | 3532.5 | 4714.0 | 5589.7 | 3800.4 | 2716.7 |
| 10,000 | `after` | `ORDER BY id ASC` (elided) | 1200 | 2795.9 | 2921.2 | 2995.9 | 4217.6 | 5102.7 | 3314.4 | 2083.3 |
| 10,000 | `base` | `ORDER BY id ASC` (elided) | 1200 | 3334.1 | 3461.7 | 3580.1 | 4835.8 | 5684.2 | 3927.5 | 2633.3 |
| 10,000 | `after` | `LIMIT 20`, no `ORDER BY` | 1200 | 29.0 | 37.6 | 38.7 | 52.5 | 147.1 | 45.8 | 50.0 |
| 10,000 | `base` | `LIMIT 20`, no `ORDER BY` | 1200 | 30.1 | 38.5 | 39.7 | 51.7 | 84.5 | 42.0 | 8.3 |
| 10,000 | `after` | `WHERE id = ?` (control) | 1200 | 26.0 | 30.0 | 36.2 | 47.8 | 112.8 | 39.7 | 16.7 |
| 10,000 | `base` | `WHERE id = ?` (control) | 1200 | 25.9 | 33.7 | 35.6 | 46.1 | 68.0 | 37.2 | 0.0 |
| 10,000 | `after` | `PING` (client + socket floor) | 1200 | 20.0 | 26.4 | 27.0 | 35.6 | 55.3 | 29.4 | 16.7 |
| 10,000 | `base` | `PING` (client + socket floor) | 1200 | 18.9 | 26.5 | 27.8 | 34.9 | 40.0 | 29.1 | 33.3 |

The controls do not move: `PING` is within 0.3 / 0.5 / 0.8 µs across the two
binaries at p50, the pk point lookup within 0.1 / 0.5 / 0.6, and the unsorted
`LIMIT 20` — which examines 20 rows and stops — within 0.8 / 1.6 / 1.0.
Everything that got faster is a whole-relation scan.

| rows | p50 saved, µs | per row, ns | CPU saved, µs | per row, ns | speed-up |
|---:|---:|---:|---:|---:|---:|
| 200 | 9.0 | 45.0 | 7.5 | 37.5 | 1.11× |
| 1,000 | 49.4 | 49.4 | 50.0 | 50.0 | 1.18× |
| 10,000 | 502.6 | 50.3 | 508.3 | 50.8 | 1.17× |

Server CPU saved per row is **37–51 ns, flat across a 50× range**, and the row
carries five values. A per-statement change would have produced a shrinking
relative delta as the relation grew; this one holds at 10–15% and its absolute
per-row value stays put. The only per-row change on this path is the render,
so the render is what paid. Set that beside §1's independent measurement of
the same quantity — 94–104 ns of CPU to format one row, obtained by skipping
the format entirely — and the refactor removed roughly half the cost of a
render that still costs about 100 ns.

---

## 4. `SELECT *` pays for its schema lookup once, and it shows

`SELECT *` renders from the relation's schema rather than from the chain's
projection types, so it needs a `catalog::TableAccess` the projection form
does not. `after` resolves it once per statement; `pre-fix` resolved it inside
`render`, once per row.

| rows | binary | statement | ops | p0 | p25 | p50 | p95 | p99 | mean | CPU µs/op |
|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 200 | `after` | `SELECT *` | 8000 | 67.4 | 77.3 | **78.5** | 89.7 | 106.7 | 81.6 | 53.8 |
| 200 | `pre-fix` | `SELECT *` | 8000 | 71.2 | 81.3 | **82.7** | 95.7 | 148.7 | 88.4 | 66.2 |
| 200 | `after` | no `ORDER BY` | 8000 | 70.1 | 79.8 | 81.2 | 92.6 | 107.3 | 83.7 | 56.2 |
| 200 | `pre-fix` | no `ORDER BY` | 8000 | 71.2 | 81.3 | 82.7 | 94.7 | 109.9 | 85.6 | 61.2 |
| 1,000 | `after` | `SELECT *` | 3000 | 251.5 | 264.9 | **267.8** | 288.6 | 331.0 | 273.6 | 220.0 |
| 1,000 | `pre-fix` | `SELECT *` | 3000 | 262.9 | 279.2 | **282.3** | 300.8 | 349.6 | 289.1 | 240.0 |
| 1,000 | `after` | no `ORDER BY` | 3000 | 258.3 | 272.5 | 276.3 | 296.8 | 343.9 | 281.9 | 236.7 |
| 1,000 | `pre-fix` | no `ORDER BY` | 3000 | 259.7 | 275.1 | 278.8 | 298.5 | 363.8 | 286.2 | 220.0 |
| 10,000 | `after` | `SELECT *` | 1200 | 2723.6 | 2824.8 | **2911.1** | 4053.3 | 4744.7 | 3125.1 | 2016.7 |
| 10,000 | `pre-fix` | `SELECT *` | 1200 | 2900.2 | 3020.3 | **3110.4** | 4312.1 | 5039.2 | 3355.9 | 2233.3 |
| 10,000 | `after` | no `ORDER BY` | 1200 | 2807.3 | 2900.2 | 2981.3 | 4164.5 | 4609.1 | 3228.5 | 2091.7 |
| 10,000 | `pre-fix` | no `ORDER BY` | 1200 | 2829.6 | 2942.9 | 3025.4 | 4177.5 | 5013.3 | 3250.3 | 2141.7 |

One knob, one row, `pre-fix` as the baseline, and the `plain` arm beside it as
the control that says how much of the gap is the layout drift of §0.2 rather
than the hoist:

| rows | `SELECT *` delta | gross, ns/row | `plain` delta (drift) | drift, ns/row | **hoist, net ns/row** |
|---:|---:|---:|---:|---:|---:|
| 200 | −5.1% | 21.0 | −1.8% | 7.5 | **13.5** |
| 1,000 | −5.1% | 14.5 | −0.9% | 2.5 | **12.0** |
| 10,000 | −6.4% | 19.9 | −1.5% | 4.4 | **15.5** |

**Resolving the table access once is worth 12–16 ns a row**, flat, which is a
catalog hash lookup and a `StatusOr` unwrap. Pass 1 measures the same three
numbers independently at 14.0 / 12.1 / 20.4 ns a row, on a different arm
ordering and against a third binary in the rotation.

The consequence a user sees is a sign flip. On `pre-fix`, `SELECT *` was
*slower* than naming the five columns — +1.3% at 1,000 rows and +2.8% at
10,000 — because the per-row lookup outweighed the shorter statement text. On
`after` it is *faster* by 3.1% and 2.4%. The engine no longer charges for the
convenience.

---

## 5. `ORDER BY <pk> ASC` is elided, and the elision is free

`exec::CompileBlock` clears `chain.sort_keys` when the clause is one ascending
key on the driving relation's pk and step 0 is not a Cabin probe. No sort
object is armed and `ANALYZE` prints no `sorted=` field at all — the three
replies as the 10,000-row server gave them, `ORDER BY id ASC` in the middle:

```
analyze rows=10000 … examined=10000 pages=145 opens=1                  -- no ORDER BY
analyze rows=10000 … examined=10000 pages=145 opens=1                  -- ORDER BY id ASC
analyze rows=10000 … examined=10000 pages=145 opens=1 sorted=10000     -- ORDER BY val
```

The latency says the same thing. Against each size's own in-run floor:

| rows | binary | pass | no `ORDER BY`, p50 | `ORDER BY id ASC`, p50 | delta | that side's floor | verdict |
|---:|---|---|---:|---:|---:|---:|---|
| 200 | `after` | 2 | 81.2 | 82.1 | +1.11% | 0.00% | see below |
| 1,000 | `after` | 2 | 276.3 | 275.7 | **−0.22%** | 0.07% | negative — no cost |
| 10,000 | `after` | 2 | 2981.3 | 2969.5 | **−0.40%** | 0.57% | inside the floor |
| 200 | `after` | 1 | 81.4 | 81.9 | +0.61% | 0.00% | see below |
| 1,000 | `after` | 1 | 276.3 | 277.0 | +0.25% | 0.07% | see below |
| 10,000 | `after` | 1 | 3025.1 | 2995.9 | **−0.97%** | 1.44% | inside the floor |

The small positive sign at the two smaller sizes is a position effect, not the
elision, and the third binary proves it. On **`base`** — which has no
`OutputSort` linked into it at all, and where `ORDER BY id ASC` is validated
and thrown away — the same arm sits the same distance above its own `plain`:
+0.66% at 200 rows and +0.25% at 1,000. `pre-fix` reports +0.73% and +0.50%.
Three binaries with three different amounts of sort machinery pay the same
penalty for being the arm that follows `plain` in each round. Server CPU
agrees: in pass 1 at 10,000 rows the `after` binary spends 2091.7 µs/op on
`plain` and 2083.3 on `ORDER BY id ASC`, a 0.4% spread. **The elision costs
nothing that can be measured on this harness.**

---

## 6. What a real sort costs, and what part of it is the sort

`SELECT id, val, grp, amount, tag FROM t ORDER BY val` — a random `int64` key
over a domain ten times the relation, so the input is neither pre-sorted nor
tie-heavy.

| rows | binary | statement | ops | p0 | p25 | p50 | p95 | p99 | mean | CPU µs/op |
|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 200 | `after` | no `ORDER BY` | 8000 | 70.1 | 79.8 | 81.2 | 92.6 | 107.3 | 83.7 | 56.2 |
| 200 | `after` | `ORDER BY val` | 8000 | 90.2 | 101.0 | **102.8** | 116.4 | 155.5 | 107.0 | 88.8 |
| 200 | `after` | `ORDER BY val DESC` | 8000 | 90.5 | 101.1 | 102.6 | 115.2 | 132.8 | 106.2 | 82.5 |
| 200 | `after` | `ORDER BY tag` (varchar key) | 8000 | 97.5 | 109.1 | 111.4 | 125.8 | 165.8 | 116.0 | 96.2 |
| 1,000 | `after` | no `ORDER BY` | 3000 | 258.3 | 272.5 | 276.3 | 296.8 | 343.9 | 281.9 | 236.7 |
| 1,000 | `after` | `ORDER BY val` | 3000 | 375.9 | 398.4 | **406.6** | 440.8 | 528.2 | 414.6 | 346.7 |
| 1,000 | `after` | `ORDER BY val DESC` | 3000 | 371.9 | 397.8 | 407.4 | 442.8 | 694.3 | 416.8 | 370.0 |
| 1,000 | `after` | `ORDER BY tag` (varchar key) | 3000 | 409.0 | 434.3 | 443.1 | 484.5 | 665.3 | 454.1 | 403.3 |
| 10,000 | `after` | no `ORDER BY` | 1200 | 2807.3 | 2900.2 | 2981.3 | 4164.5 | 4609.1 | 3228.5 | 2091.7 |
| 10,000 | `after` | `ORDER BY val` | 1200 | 4959.4 | 5184.2 | **5276.7** | 6452.1 | 7019.6 | 5513.5 | 4408.3 |
| 10,000 | `after` | `ORDER BY val DESC` | 1200 | 5006.4 | 5152.3 | 5242.9 | 6460.8 | 7115.5 | 5448.0 | 4341.7 |
| 10,000 | `after` | `ORDER BY tag` (varchar key) | 1200 | 5675.4 | 5831.6 | 5935.1 | 7042.9 | 7690.5 | 6149.0 | 5041.7 |

The unlimited sort against its own unsorted twin:

| rows | delta p50, µs | ratio | per row, ns | CPU delta, µs | CPU per row, ns |
|---:|---:|---:|---:|---:|---:|
| 200 | +21.6 | 1.27× | 108 | +32.6 | 163 |
| 1,000 | +130.3 | 1.47× | 130 | +110.0 | 110 |
| 10,000 | +2295.4 | 1.77× | 230 | +2316.6 | 232 |

**The unlimited sort is unchanged by the split, exactly as its shape
predicts.** Every row that survives an unlimited sort is rendered by
definition, so `Admit` returns true for all of them and there is nothing to
skip. `pre-fix` measures 103.2 / 416.2 / 5289.4 µs against `after`'s 102.8 /
406.6 / 5276.7 — −0.4%, −2.3% and −0.2%, the middle one above its floor and
not reproduced at either neighbour. Nothing here is a finding.

**The sort's cost grows far faster than `n log n`, and §2 says which part
does.** Dividing the *buffer-and-sort* term — the render removed from both
sides by the `ANALYZE` arms — by `log₂ n` gives 5.7 / 8.2 / 12.8 ns at the
three sizes, a 2.25× rise over the range where comparison counting would have
flattened it. The normalize term over the same range is 16 ns a row and
falling. So it is not the comparator and it is not the key: it is the
buffering. `OutputSort::Row` is a `std::vector<OrderKey>` (24 B) + a `seq`
(8 B) + a `std::string` (32 B) = 64 B in the contiguous buffer, and the
unbounded path takes each row by `std::move`, so each buffered row carries
**two heap allocations beside it** — the key vector's storage (one `OrderKey`
is a 16-byte `Int128` + a `std::string` + a flag ≈ 56 B) and the row's
rendered text, which at ~30 characters is past `std::string`'s small buffer. A
10,000-row sort is ~640 KB of contiguous `Row` plus ~1.0 MB in ~20,000
separate allocations, comfortably past this core's L2, where the same
structure at 1,000 rows (~160 KB) and 200 rows (~32 KB) is not.

The bounded path already avoids this: `Take` swaps rather than moves, handing
the evicted row's key vector and text buffer back to the caller's scratch, so
a steady-state top-N run allocates nothing per row. The unbounded path cannot
use the same trick — it keeps every row — but the 170 ns a row it pays at
10,000 rows is what a `Row` layout change has to aim at.

**Deferring the reply is itself worth 43 ns a row.** Rendering and shipping
10,000 rows costs 191.5 ns a row when the sink streams each row as it walks
(`plain` − `an-plain`) and 234.6 ns a row when the same rows are rendered into
scattered per-row strings and streamed afterwards from the sorted buffer
(`ORDER BY val` − `ANALYZE ORDER BY val`). That 22% is the per-row string
allocation plus the cache cost of walking a buffer of pointers, and it is the
price of the blocking operator quite apart from the ordering.

---

## 7. Descending is free; a varchar key is not

Two knobs, one per row, against `ORDER BY val` as the baseline:

| rows | baseline `ORDER BY val`, p50 | knob | p50 | delta | per row, ns | that side's floor | verdict |
|---:|---:|---|---:|---:|---:|---:|---|
| 200 | 102.8 | `DESC` | 102.6 | −0.19% | −1.0 | 0.00% | negative |
| 1,000 | 406.6 | `DESC` | 407.4 | +0.20% | +0.8 | 0.07% | positive |
| 10,000 | 5276.7 | `DESC` | 5242.9 | −0.64% | −3.4 | 0.57% | negative |
| 200 | 102.8 | key is `tag varchar` | 111.4 | +8.37% | +43.0 | 0.00% | a cost |
| 1,000 | 406.6 | key is `tag varchar` | 443.1 | +8.98% | +36.5 | 0.07% | a cost |
| 10,000 | 5276.7 | key is `tag varchar` | 5935.1 | +12.48% | +65.8 | 0.57% | a cost |

**`DESC` is free, and the reason is the sign.** `OutputSort::Before` reads
`keys_[i].descending` and flips the sign of an already-computed `Compare`, so
descending buys one predictable branch per comparison and nothing else. The
measurement agrees in the only way a zero can be demonstrated: the three sizes
disagree on the direction, at −0.19%, +0.20% and −0.64%, all within a
half-percent of nothing. A cost that is sometimes negative is not a cost.

The varchar key is a cost at every size and by both meters: +43 / +37 / +66 ns
per row of client latency and +37 / +57 / +63 ns per row of server CPU. That
is `OrderKeyOf` copying a string into every `OrderKey` where the `int64` path
fills an `Int128` in place, and `OrderKey::Compare` doing a byte comparison
instead of an integer one.

---

## 8. `LIMIT n` bounds the buffer, and now bounds the formatting too

OB5's top-N heap does exactly what the header claims, and the split did not
change what it retains. `ANALYZE` at every size, on both sort-carrying
binaries, identical:

| rows | statement | `rows=` | `examined=` | `pages=` | `sorted=` |
|---:|---|---:|---:|---:|---:|
| 200 | `ORDER BY val LIMIT 20` | 20 | 200 | 3 | **20** |
| 1,000 | `ORDER BY val LIMIT 20` | 20 | 1,000 | 15 | **20** |
| 10,000 | `ORDER BY val LIMIT 20` | 20 | 10,000 | 145 | **20** |
| 200 | `ORDER BY val` | 200 | 200 | 3 | 200 |
| 1,000 | `ORDER BY val` | 1,000 | 1,000 | 15 | 1,000 |
| 10,000 | `ORDER BY val` | 10,000 | 10,000 | 145 | 10,000 |
| 200 | `LIMIT 20`, no `ORDER BY` | 20 | **20** | 1 | — |
| 1,000 | `LIMIT 20`, no `ORDER BY` | 20 | **20** | 1 | — |
| 10,000 | `LIMIT 20`, no `ORDER BY` | 20 | **20** | 1 | — |

Twenty rows held at every size, the whole relation examined at every size, and
`examined=10000` is correct rather than a defect: the order does not exist
until the walk ends, so the walk cannot stop.

`ANALYZE` reports the memory bound and cannot report the formatting one, which
is why §1 had to measure it. The two are now the same number — `sorted=` rows
retained and `sorted=` rows rendered — and the evidence for the second is the
94–104 ns of CPU per skipped row that stops being spent, and the 0.60× of a
full unsorted scan the bounded statement now costs where it used to cost
1.03×. **`examined=` is the one of the three counts that a `LIMIT` cannot
move.**

---

## 9. Against PostgreSQL: not run

**No PostgreSQL baseline was measured, and none of the numbers above has one.**
This is an environment failure and is stated rather than papered over. It was
re-checked during this session rather than restated from the previous one:
`psql`, `initdb` and `pg_ctl` are all absent from `PATH`,
`/usr/lib/postgresql` does not exist, and `tools/pg_setup.sh status` dies at
`pg_ctl: command not found`, so there is no cluster on port 15433 to compare
against. Installing needs root, and `sudo -n true` fails with "a password is
required".

The task that closes this, named so it can be picked up:

- **`tools/pg_order_by_benchmark.py`** — the twin, importing `COLUMNS`,
  `make_rows` and `arms` from `tools/order_by_benchmark.py` so the two sides
  cannot drift into measuring different questions, driving the same statements
  through `pg_wire.py` against the `tools/pg_setup.sh` cluster on port 15433
  **at default tuning**. Its interesting column is one this engine does not
  have: PostgreSQL's planner can choose an index scan to *serve* the order, and
  can stop a top-N heapsort's input early when a `LIMIT` is bounded by an
  ordered path. `EXPLAIN (ANALYZE, BUFFERS)` per size would say when it does,
  which is the direct baseline both for `sort.hpp`'s argument that a KDS index
  must not, and for §2's claim that the walk is irreducible — it is
  irreducible *for this engine's access paths*, which is a narrower statement
  than it looks. Blocked only on `sudo apt-get install postgresql` on this
  host.

Until that runs, every ratio in this file is KDS against KDS.

---

## 10. What the run says about the engine

**Formatting a reply row costs about 100 ns of server CPU, and that is the
largest single number in this file.** Three independent measurements agree:
skipping the format for 9,980 of 10,000 rows saves 94.4 ns of CPU and 99.2 ns
of client latency per skipped row (§1); the render-and-ship term of an
unsorted scan is 191.5 ns of latency a row, leaving ~92 ns for the socket once
that render is subtracted (§2); and the shared-`render`
refactor, which merely replaced ten `std::ostream::operator<<` insertions per
five-column row with two, took 50 ns a row off the same path (§3). Against a
walk-and-decode cost of 102.5 ns a row, **this engine spends more time
describing a row than finding it.** That is the reusable finding, and it
generalizes past `ORDER BY`: any statement shape that can decide a row is
uninteresting before formatting it has roughly half its per-row cost available
to save.

**`ORDER BY … LIMIT n` was that shape, and taking it was worth 1.8×.** The
`Admit`/`Take` split is a small change — the sink asks a predicate that the
sort could already answer, and only then formats — and it removes 44% of the
statement at 10,000 rows and flips the bounded sort from *costing more server
CPU than returning the whole relation unsorted* (1.03×) to costing three
fifths of it (0.60×). The design point worth carrying forward is that the
split was not a matter of moving a branch: it needed the sort's normalized
keys to be computed and *kept* across the caller's render, which is why
`Admit` and `Take` are two calls with a stated pairing rather than one call
with a flag.

**`include/kds/exec/sort.hpp` now claims slightly more than the data
supports, and this is the place to say so.** Its `Admit` comment ends
"Deciding before rendering is what makes top-N bound work as well as memory."
The bound is on *formatting* work, not on work: at 10,000 rows the bounded
sort still walks and decodes every row for 1025.2 µs of its 1230.4, and still
costs 31.7× the unsorted `LIMIT 20` that stops at twenty. Memory is bounded at
twenty rows, formatting is now bounded at twenty rows, and the walk is
O(relation) and cannot be otherwise while the order is unknown until the last
row arrives. The header should say "bound the formatting as well as the
memory".

**The same header's "the buffer is not a new class of memory" is now
quantified, and it is not quite free either.** Deferring the reply through the
buffer costs 43 ns a row more than streaming it — 234.6 against 191.5 ns at
10,000 rows — because the per-row text becomes a separate allocation read back
in a second pass. A full result set was indeed always resident; what is new is
that it is resident as ten thousand small allocations rather than one stream
buffer.

**The sort proper is a memory-layout problem, not an algorithmic one.** With
the render subtracted by the `ANALYZE` arms, retaining and sorting 10,000 rows
instead of 20 costs 170.3 ns a row against 81.6 at 1,000 and 43.5 at 200 — a
2.25× rise per `log₂ n` that comparison counting does not explain — while
normalizing the key that the comparator actually reads costs 16.1 ns a row and
*falls* with size. The suspect named by the code is the two heap allocations
per buffered `Row`, and a single-key sort — the overwhelmingly common case —
allocates a one-element `std::vector<OrderKey>` per row for a key that would
fit inline. That is a concrete, bounded change with a measured ceiling: 1703.5
µs of a 5276.7 µs statement at 10,000 rows.

**A sorted `LIMIT` is still 31.7× an unsorted one, and that belongs in the
manual.** It is smaller than it was but it is the same shape of surprise: an
OLTP client that adds `ORDER BY <non-pk>` to a `LIMIT 20` page turns a 38.8 µs
statement into a 1.2 ms one, and the factor grows linearly with the relation
because the walk does. `pagination.hpp`'s "the quota bounds output, the budget
bounds work" now has two numbers on it rather than one.

**Nothing here touches an Open Decision in `CLAUDE.md`** — `sort_max_rows`
stayed at its `[PROPOSED]` default of 1,048,576 and was never approached, so
this run says nothing about where it should sit. `docs/feat-index.md` §13's
"whether the measured crossover is ever acted on" acquires a sharper adjacent
data point than a whole-statement ratio could give: an index that served the
order could remove at most the sort's own terms, which are 1864.4 µs of the
5276.7 µs unlimited statement — **35%, not the 44% the 1.77× ratio suggests**,
because the walk and the reply are paid either way. Any index-serves-the-order
proposal has 35% to win and `sort.hpp`'s three correctness problems to solve
first.

**A caution about this host, for whoever reads these numbers next.** The
machine has 2 vCPUs and runs several agents at once; the quiet-machine gate in
front of this run held its first size back by 100 seconds while another
worktree loaded 100,000 cargo rows, and an earlier attempt at this measurement
on a different day was destroyed by exactly that driver arriving mid-run and
looking entirely normal from inside the harness. Runs here need a non-default
port, a process check and a load-average check, and the ones above have all
three.
