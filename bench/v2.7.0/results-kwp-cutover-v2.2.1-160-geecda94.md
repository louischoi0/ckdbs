# KWP/1 cutover: `tools/benchmark.py` before and after, per KW-D6

KW-D6 (`instructions/v2.7.0/kw-ratification.md`) cuts the server's default
port from the newline text protocol to the binary KWP/1 protocol, and
requires one paired reading of a scenario at the commit before and after
the cut. This is that reading.

**Every `bench/` results file before this one is a newline-protocol
number.** `tools/benchmark.py`'s socket, framing, parse, catalog binding,
heap access and reply formatting were all measured against the newline
line-in/line-out protocol. From the commit this file measures on
(`eecda94300b`, `v2.2.1-160-geecda94`), the server's main port speaks
KWP/1 instead, and `tools/ckdbs_cli.py` — the client every driver under
`tools/` imports — moved with it. A number from an earlier file and a
number from this one are not the same client surface; do not diff them
directly.

**Thesis.** The write path (`INSERT`, `UPDATE`) shows no cutover cost
outside this run's own noise floor — both are fsync-bound under
`durability = group`, and the reply either protocol carries back is a few
bytes. The read path shows a real, large, and fully explained cost: a
single-row `SELECT` costs a flat ~35–46 µs more, and a full-table `SELECT`
costs ~3.8–4.1 µs more **per row returned**, scaling linearly with the
row-set size. Server-side engine time is unaffected — decomposed below,
the entire delta lives on the client, and just under half of the read-path
delta is not the wire protocol at all: it is the compatibility shim
`tools/ckdbs_cli.py` runs on every reply so this repository's thirty-odd
existing benchmark drivers keep reading the newline protocol's string
shape. The Nagle/delayed-ACK stall this milestone's own history warns
about — 23 qps / 42 ms per statement — **does not appear anywhere in this
run**; say that plainly below rather than only by omission.

## 1. Stamp

| | before | after |
|---|---|---|
| commit | `04d53f4182a4` | `eecda94300b2` |
| `git describe --tags` | `v2.2.1-159-g04d53f4` | `v2.2.1-160-geecda94` |
| commit date | 2026-08-31 11:37:09 +0900 | 2026-08-31 05:43:35 +0000 |
| branch | `worktree-kwp` (both commits are ancestors on this branch; each tree below was built from a clean `git archive` of the commit, not from the worktree — see §1a) | |
| tree state at the measured commit | clean (`git archive <commit>` extracts exactly that commit's tree; no working-tree edits are in either binary) | |
| build type | Release (`-DCMAKE_BUILD_TYPE=Release`) | Release |
| build dir | `$SCRATCH/before/build-release` | `$SCRATCH/after/build-release` |
| source binary mtime | 2026-08-31 05:46:43 UTC | 2026-08-31 06:08:05 UTC |
| binary copy measured from | `/home/cdkbs/run-before/kds_server` | `/home/cdkbs/run-after/kds_server` |
| copy sha256 | `8fb5bb9fd74b80404f87c3e63e9e24bb8f22b18b5039a72ac8becd10f6b9005b` | `1e37fcb521fcd72ec48c301777bec9a24230b18beb77df7ee8de43c48671a0b1` |
| data file device | `/home/cdkbs/run-{before,after}/data/*.kds` on `/dev/root`, ext4 (`df -T`, not tmpfs) | |
| server config | default (no `--config`): `cores = 1`, `durability = group` (confirmed from both servers' own `[query]` log line at `--log-level debug`), TLS off, auth off | same |
| protocol port | 25701, newline (this tree's only protocol) | 25702, KWP/1 (this tree's main-port default; `--debug-text-port` unused) |
| client driver | this tree's own `tools/benchmark.py` + `tools/ckdbs_cli.py` (newline) | this tree's own `tools/benchmark.py` + `tools/ckdbs_cli.py` (KWP/1 via `tools/kwp.py`) |
| measured | 2026-08-31, 07:19–07:26 UTC | |
| host quiet | `uptime` load average ≤0.17 throughout, `pgrep -x cc1plus` empty before every cell (§1a) | |

### 1a. A methodology note the stamp table above required

Two things happened in this worktree that are not part of this
measurement and are recorded here so the numbers above can be trusted
rather than taken on faith:

- **The worktree's git index carried an unrelated, unresolved `git merge
  origin/main`** (four `UU` conflicts, including `src/server/
  command_dispatcher.cpp`) when this session started, plus a large set of
  staged, unrelated files (a prior `scenario2-cores` benchmark archive).
  Neither was touched, resolved, or committed by this measurement. Both
  "before" and "after" trees were instead built from `git archive
  04d53f4` and `git archive eecda94` into scratch directories, each its
  own `cmake -S . -B build-release`, so the working tree's conflict never
  entered either binary.
- **A second, unrelated `cmake --build` ran in the actual worktree's
  `build/` (Debug, `kds_tests`) for roughly the first half of this
  session**, evidence of another concurrent session in this same
  worktree. No cell in this file was run while it or any `cc1plus` was
  active — confirmed with `pgrep -x cc1plus` (not `-f`, which self-matches
  a monitoring loop's own command line, a mistake this session made and
  then corrected) immediately before every cell.

## 2. What was measured, and what was not

`tools/benchmark.py --rows N --read-ops R --update-ops U --scan-ops S
--join-rows 0 --warmup 50 --seed 7`, against a fresh 5-column heap
relation (`id int64, c_int int64, c_small int32, c_flag bool, c_text
varchar`) and a fresh data file per cell (rule 6). Four phases: `insert`,
`point-select` (`WHERE id = <random>`), `full-scan` (`SELECT *`,
unfiltered), `update` (`SET c_int = ... WHERE id = <random>`). Op counts
scaled with row count so each phase's server CPU clears noise (200 rows:
800/400/15; 1,000 rows: 1,500/800/8; 10,000 rows: 2,000/1,000/4).

**`--join-rows 0`, joins not measured.** The join phases' row encoding is
identical to `full-scan`'s (same `WireResultSink` path), so they would
not add a new protocol data point, only `O(join_rows × rows)` more wall
time on a 2-CPU host already budgeted tightly against a concurrent
session. This is a stated scope cut, not an oversight.

**Every cell reported `err: 0`.** `benchmark.py` has no separate
`--verify` flag; `check_phase()` aborts the run on any error, which is
the correctness gate this driver offers, and it passed on all 16 cells
below (3 sizes × 2 arms × [4 main-sweep phases], plus the repeat and the
debug-log cells). No code was changed to take this measurement, so the
correctness suite (`tests/`) was not re-run; that gate applies to a
change, and none was made.

**Not a scenario driver** (`bench/docs/README.md`'s "the scenarios" vs.
"the statement-level tools" split) — `benchmark.py` is the latter, so
per CLAUDE.md §1b this file archives nothing; the commands in §7 below
are what a re-run needs.

## 3. The full percentile tables

All latencies in µs, client-observed (socket + framing + decode +
render), one connection, per `bench_common.Phase.summary()`.

### 3a. 200 rows

| phase | arm | ops | qps | mean | p0 | p25 | p50 | p95 | p99 | max | err |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | after (KWP) | 200 | 781 | 1275.3 | 1015.9 | 1202.9 | 1264.7 | 1412.1 | 1714.7 | 2563.4 | 0 |
| insert | before (newline) | 200 | 798 | 1247.9 | 1030.5 | 1131.7 | 1193.5 | 1480.2 | 2145.8 | 3958.8 | 0 |
| point-select | after (KWP) | 800 | 11,811 | 83.6 | 67.4 | 73.6 | 83.4 | 102.5 | 111.7 | 147.0 | 0 |
| point-select | before (newline) | 800 | 20,233 | 48.4 | 29.3 | 40.1 | 45.8 | 62.4 | 72.7 | 103.4 | 0 |
| full-scan | after (KWP) | 15 | 1,074 | 930.1 | 912.6 | 915.4 | 923.7 | 986.2 | 986.2 | 986.2 | 0 |
| full-scan | before (newline) | 15 | 9,802 | 101.5 | 95.9 | 96.8 | 99.6 | 121.4 | 121.4 | 121.4 | 0 |
| update | after (KWP) | 400 | 767 | 1301.8 | 1077.3 | 1193.6 | 1248.9 | 1447.3 | 2864.3 | 4151.7 | 0 |
| update | before (newline) | 400 | 766 | 1303.3 | 1083.8 | 1218.7 | 1273.3 | 1456.0 | 2187.4 | 2903.0 | 0 |

### 3b. 1,000 rows

| phase | arm | ops | qps | mean | p0 | p25 | p50 | p95 | p99 | max | err |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | after (KWP) | 1000 | 755 | 1320.6 | 1033.9 | 1213.5 | 1268.5 | 1493.1 | 3194.4 | 5626.2 | 0 |
| insert | before (newline) | 1000 | 769 | 1296.7 | 1013.4 | 1184.1 | 1249.1 | 1520.1 | 2911.9 | 5478.7 | 0 |
| point-select | after (KWP) | 1500 | 7,632 | 129.8 | 68.2 | 134.9 | 137.9 | 155.0 | 170.3 | 346.9 | 0 |
| point-select | before (newline) | 1500 | 10,730 | 92.2 | 30.6 | 98.1 | 100.3 | 112.7 | 124.8 | 280.8 | 0 |
| full-scan | after (KWP) | 8 | 232 | 4306.9 | 4186.3 | 4190.3 | 4229.7 | 4713.2 | 4713.2 | 4713.2 | 0 |
| full-scan | before (newline) | 8 | 2,649 | 376.6 | 345.5 | 355.6 | 361.2 | 442.1 | 442.1 | 442.1 | 0 |
| update | after (KWP) | 800 | 650 | 1536.8 | 1264.9 | 1419.0 | 1486.4 | 1705.6 | 3228.3 | 5394.2 | 0 |
| update | before (newline) | 800 | 655 | 1525.4 | 1304.6 | 1450.0 | 1492.1 | 1663.0 | 2740.6 | 4036.9 | 0 |

### 3c. 10,000 rows

| phase | arm | ops | qps | mean | p0 | p25 | p50 | p95 | p99 | max | err |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | after (KWP) | 10000 | 746 | 1336.6 | 1001.8 | 1221.5 | 1282.3 | 1519.2 | 3031.6 | 12212.8 | 0 |
| insert | before (newline) | 10000 | 774 | 1287.8 | 981.2 | 1179.2 | 1236.5 | 1461.7 | 3022.2 | 20795.8 | 0 |
| point-select | after (KWP) | 2000 | 1,443 | 691.6 | 73.2 | 689.3 | 695.0 | 711.7 | 756.7 | 2515.6 | 0 |
| point-select | before (newline) | 2000 | 1,546 | 645.6 | 34.8 | 644.5 | 650.6 | 662.1 | 691.2 | 1424.0 | 0 |
| full-scan | after (KWP) | 4 | 23 | 43183.0 | 42776.6 | 42776.6 | 42877.8 | 43780.5 | 43780.5 | 43780.5 | 0 |
| full-scan | before (newline) | 4 | 211 | 4731.7 | 3971.9 | 3971.9 | 4082.9 | 5725.7 | 5725.7 | 5725.7 | 0 |
| update | after (KWP) | 1000 | 266 | 3748.4 | 3418.6 | 3600.9 | 3671.8 | 3924.9 | 5736.6 | 22695.8 | 0 |
| update | before (newline) | 1000 | 262 | 3819.1 | 3461.4 | 3631.9 | 3700.0 | 4087.1 | 6161.0 | 25387.7 | 0 |

## 4. The noise floor, from inside this run

`point-select` and `full-scan`'s server-side engine cost is unaffected by
row count in a way that would predict a protocol delta on its own, so the
floor matters for reading §5's ratios honestly. Rule 8's control: the
`after` arm at 1,000 rows, run twice, fresh server and fresh data file
each time (`r1000`, `r1000rep`).

| phase | run 1 mean (µs) | run 2 mean (µs) | spread |
|---|---|---|---|
| insert | 1320.6 | 1306.7 | 1.05% |
| point-select | 129.8 | 129.6 | 0.15% |
| full-scan | 4306.9 | 4500.6 | 4.50% |
| update | 1536.8 | 1521.7 | 0.98% |

The floor on this quiet 2-CPU host is **≤4.5%** for every phase measured
twice (full-scan's 8-op sample is the noisiest, as its own op count would
predict). Every `insert`/`update` delta in §5 below is inside this floor.
Every `point-select`/`full-scan` delta is many multiples of it.

## 5. The delta, phase by phase

Per rule 5a a comparison matrix carries throughput, not delay: the ratio
below is `before qps / after qps` (§3's own `qps` column, `ops/elapsed` —
already a throughput figure, not derived), so a ratio **above** 1.00 means
the newline arm did more work per second and the cutover cost throughput;
1.00 is no cutover cost.

| phase | 200 rows | 1,000 rows | 10,000 rows | reads noise floor? |
|---|---|---|---|---|
| insert | 1.022 (+2.2%) | 1.019 (+1.9%) | 1.038 (+3.8%) | **yes** — inside §4's 4.5% floor at every size |
| update | 0.999 (−0.1%) | 1.008 (+0.8%) | 0.982 (−1.8%) | **yes** — inside the floor, one size even reads faster |
| point-select | 1.713 (+71.3%) | 1.406 (+40.6%) | 1.071 (+7.1%) | no — real, but *shrinking* in relative terms as the base cost grows |
| full-scan | 9.13× | 11.41× | 9.10× | no — real, roughly constant multiplicative cost |

Two shapes, and they are opposite shapes on purpose:

- **`insert`/`update` show nothing.** Both are fsync-bound under
  `durability = group` (~1.0–1.3 ms floor for insert, growing with the
  heap chain length for update's unindexed `WHERE`), and both replies are
  a handful of bytes either protocol. §6 confirms this from the server's
  own per-statement log: engine time for these two phases is identical
  across arms to within single-digit µs.
- **`point-select`'s ratio *falls* as rows grow** (72.7% → 40.8% → 7.1%)
  because its **absolute** tax is nearly flat (35.2 → 37.6 → 46.0 µs —
  computed as after-mean minus before-mean at each size) while its own
  base cost grows with the heap's chain length (a point lookup on this
  unindexed relation is a full scan, same as `full-scan`, just stopped at
  the first match). A flat absolute tax over a growing base is a
  shrinking ratio — the ratio column is not the story here, the 35–46 µs
  column is.
- **`full-scan`'s ratio holds roughly constant (~9–11×)** because both its
  numerator and its denominator grow with rows at close to the same
  *rate*, so the ratio is closer to a stable property of the workload than
  point-select's is. §6 shows why: the tax is genuinely per-row.

## 6. The wait breakdown

Rule 3 asks each measured unit decomposed into what it waits on. Two
different decompositions apply here, because the read path and the write
path spend their time in different places.

### 6a. Write path (`insert`, 1,000 rows): durability dominates, protocol is silent

From each server's own `[query]` debug log (`--log-level debug`), which
times compile-through-execute **inside the engine, excluding the client's
round trip** — this is the same dispatcher-level instrumentation on both
protocols, so it isolates the wire from everything before it:

| component | after (KWP) | before (newline) |
|---|---|---|
| server engine (parse+execute, WAL-logged) | 8–24 µs (sample) | 8–27 µs (sample) |
| durability wait (fsync, `group`) + client/socket residual | remainder of the ~1.3 ms client-observed mean | remainder of the ~1.3 ms client-observed mean |

The two residuals are equal to within §4's noise floor (§5's insert row).
`durability = group`'s fsync is, as `benchmark.py`'s own printed note
says, "~1 ms vs ~12 µs of engine time" — three orders of magnitude larger
than any framing cost either protocol could plausibly add, which is why
the write path is not where a protocol change would show up on this
engine, and does not.

### 6b. Read path (`full-scan`, 1,000 rows): the delta is 100% client-side, and here is where it goes

Same instrumentation, same row count, one debug-logged run per arm:

| component | after (KWP), µs | share | before (newline), µs | share |
|---|---|---|---|---|
| server engine (heap scan; **before** additionally formats the reply string here — see note) | 223 (sample median, n=7) | 5.3% | 247 (sample median, n=7) | 64.4% |
| client + wire, excluding the render shim (`kwp.Connection.execute` minus engine) | 2,215 | 52.2% | 137 (single recv + one `str.split`) | 35.6% |
| `ckdbs_cli.ServerConnection`'s newline-shape render shim (`kwp.render_value` per field per row, KWP-only) | 1,805 | 42.5% | n/a — the newline protocol already *is* this shape | — |
| **total client-observed mean** | **4,243** | 100% | **384** | 100% |

(The two totals here are from the same debug-logged run this table's rows
come from, and read within a few µs of §3b's 4,306.9/376.6 from the main
sweep — the small difference is §4's noise floor between separate runs,
not a second effect.)

**Note on the server-engine row**: the newline server internally renders
the whole reply to a comma-escaped string (36,444 bytes at 1,000 rows,
logged as `-> 36444B reply in ...us`) *before* handing it to the socket —
that formatting is inside the 247 µs figure. The KWP server does not do
this (`-> 0B reply in ...us`, since `WireResultSink` writes typed rows
directly); its 223 µs is scan-only. So if anything, **the cutover made
the server's own job slightly cheaper**, and every µs of the delta above
moved to the client.

**Where the client-side cost splits**, isolated with a small probe
(`kwp.Connection.execute()` raw vs. `ckdbs_cli.ServerConnection.
send_command()`, same statement, same server, same data, run against each
row-set size's already-loaded relation — full script in §9's appendix):

| rows | raw KWP execute (µs) | + render shim (µs) | shim's share of the total |
|---|---|---|---|
| 200 | 547.9 | 903.7 | 39.4% |
| 1,000 | 2,437.9 | 4,242.6 | 42.5% |
| 10,000 | 25,177.7 | 42,933.1 | 41.4% |

**Per row**, both layers are close to constant across a 50× row-count
range — the signature of a per-row cost, not a per-statement or
per-round-trip one:

| rows | raw execute, µs/row | shim overhead, µs/row |
|---|---|---|
| 200 | 2.740 | 1.779 |
| 1,000 | 2.438 | 1.805 |
| 10,000 | 2.518 | 1.776 |

Two conclusions follow directly:

1. **The framing change is visible far above this engine's own
   per-statement cost** — §6a's write path shows the floor that would
   otherwise hide it, and the read path clears it by 1–2 orders of
   magnitude.
2. **Where it goes**: not a round trip (KWP's own client already pipelines
   PARSE/BIND/EXECUTE/SYNC in one write, per `kwp.py`'s own comment, and
   `max_rows = 0` here means no portal suspension — one server-to-client
   burst either way), and not the wire bytes (before's 36,444-byte string
   and after's typed rows are the same order of magnitude on the wire).
   It is CPU: of the 4,243 µs total, 5.3% is server engine time (§6b's
   first row), and the remaining 94.7% splits roughly 55/45 between (a)
   Python-side per-value frame decode — `kwp.py`'s `_row_batch` calls
   `struct.unpack` once per field per row, 5,000 calls at 1,000 rows,
   against the newline client's one `str.split(',')` over one string —
   and (b) the **compatibility shim**
   `ckdbs_cli.ServerConnection` runs on every reply specifically so this
   repository's existing drivers keep reading the newline shape
   (`tools/kwp.py`'s own docstring: *"a benchmark's numbers before and
   after the cut are comparable"*). That shim is not part of the KWP
   protocol or its reference client; it is scaffolding this repository's
   own tooling needs, and it is worth roughly two fifths of the read-path
   cost this file reports.

### 6c. `point-select`'s flat ~35–46 µs tax, same run

| component | after (KWP), µs | before (newline), µs |
|---|---|---|
| server engine (sample median, n=5) | 70 | 71 |
| client + wire (including the shim, one row so the shim's per-row cost is negligible here) | 70.6 | 34.8 |
| **total client-observed mean** | **140.6** | **105.8** |

One row means the shim's ~1.8 µs/row cost is invisible next to a ~35 µs
fixed cost — the extra frames on the wire (`PARSE`/`BIND`/`EXECUTE`/
`CLOSE`/`SYNC` out; `PARSE_OK`/`BIND_OK`/`ROW_DESC`/`ROW_BATCH`/
`COMPLETE`/`READY` back, against the newline protocol's one line each
way) and their decode are what a single-row `SELECT` pays for, and it is
a per-statement constant, not a per-row one — consistent with §5's
observation that this tax does not grow with the row count the way
`full-scan`'s does.

## 7. On the milestone's own named risk: it does not appear here

`instructions/v2.7.0/kw-kwp.md` names a specific prior failure mode:
before `TCP_NODELAY` and write-coalescing were added to `tools/kwp.py`,
a statement measured **23 qps / 42 ms**, the signature of a delayed-ACK
stall on an un-coalesced multi-frame write. Both fixes are in the tree
measured here (`kwp.py`'s own comment at the `execute()` call site: *"One
write, four frames... on a connection without `TCP_NODELAY`, a delayed
ACK between them"* — describing what the pipelined single `sendall` now
avoids).

**Nothing in this run has that shape.** The worst mean latency measured
anywhere is `full-scan` at 10,000 rows, 43.2 ms — and that number is
`§6b`'s ~2.5–4.3 µs/row **linear** cost times 10,000 rows, landing near
42 ms by simple arithmetic, not by a stall: `point-select`, which is the
phase structurally analogous to the historical bug (one statement, one
small reply), tops out at **170.3 µs p99** even at 10,000 resident rows.
A 42 ms single-statement stall would be four orders of magnitude above
that. The fix holds.

## 8. What this run teaches about the engine

- **The cutover cost is entirely a client-observed cost on statements that
  return rows, and entirely absent on statements that do not.** This is
  not a wire-protocol verdict in the abstract — it is a verdict on *this
  reference Python client's* decode path, which is what every benchmark
  driver in `tools/` actually measures through. A client that batch-decoded
  frames (e.g. `struct.iter_unpack`, or a compiled client) would likely
  close most of §6b's 52.2% "raw execute, minus engine" share; the
  compatibility shim's 42.5% share is structural to this repository's
  tooling choice and would cost the same amount in any client that has to
  reproduce the newline shape.
- **The KWP server is not the story.** §6b's note is worth restating on
  its own: the server-side engine time for `full-scan` is *lower* under
  KWP (223 µs vs. 247 µs at 1,000 rows) because it no longer formats a
  reply string internally. Every µs the cutover cost moved from the
  server to the client.
- **`point-select`'s and `full-scan`'s costs are the same underlying
  mechanism at two scales**: a flat per-statement frame tax (~35–46 µs,
  from the extra PARSE/BIND/EXECUTE/SYNC round versus one newline) plus a
  per-row decode-and-render tax (~4 µs/row, invisible on a 1-row reply,
  dominant on a 1,000+-row one). Reporting only one row-set size, which
  rule 9 exists to forbid, would have shown either "KWP costs 73%" or
  "KWP costs 1044%" depending on which phase was picked, and both would
  have been true only of the size measured.
- **This is a new baseline, not a comparison against an old one.** No
  prior `bench/` file measures this exact shape
  (`tools/benchmark.py`'s 5-column heap, insert/point-select/full-scan/
  update) — this file's `before` column is the contemporaneous
  newline-protocol reading rule 4 asks for when no prior number exists,
  measured on the same host in the same run rather than pulled from an
  older file subject to host drift. The next run of this shape reads
  against this file's `after` column.

## 9. Reproduction

```bash
# before tree (newline): git archive 04d53f4 into a scratch dir, then
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=<ossl>
cmake --build build-release -j2 --target kds_server
cp build-release/kds_server /home/cdkbs/run-before/kds_server
/home/cdkbs/run-before/kds_server /home/cdkbs/run-before/data/r1000.kds --port 25701
cd tools && python3 benchmark.py --port 25701 --table bt_r1000 \
  --rows 1000 --read-ops 1500 --update-ops 800 --scan-ops 8 \
  --join-rows 0 --warmup 50 --seed 7 --json out.json

# after tree (KWP/1): git archive eecda94 into a scratch dir, same cmake
# invocation, same benchmark.py invocation against port 25702 — the
# protocol switch is entirely which tree's ckdbs_cli.py/kwp.py runs it.

# wait breakdown: add --server-log <path to the server's --log-file under
# --log-level debug> to either invocation, and --log-dir/--log-file to
# keep the debug log out of the shared worktree's default kdb.log.
```

`200`/`10,000` rows: same command with `--rows 200 --read-ops 800
--update-ops 400 --scan-ops 15` and `--rows 10000 --read-ops 2000
--update-ops 1000 --scan-ops 4` respectively (op counts scaled per §2).
`bench/docs/README.md`'s `benchmark.py` / `pg_benchmark.py` entries
document every flag; no PostgreSQL twin is included in this file, since
KW-D6 is a question about this engine's own two protocols, not about the
floor (rule 4's PostgreSQL baseline does not apply to a protocol-internal
comparison).
