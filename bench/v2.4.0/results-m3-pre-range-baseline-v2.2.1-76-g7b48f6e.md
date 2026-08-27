# M3 — the pre-range baseline at v16 (RD9(a)'s statement mix, before RD5)

`instructions/v2.4.0/range-foundation.md` §7 M3: the statement mix the
order's own M3 row names — **(arm R) single-relation read, (arm I)
single-relation insert, (arm S) the shipped-statement path** — captured at
superblock v16 with `sys.ranges` existing and empty, before RD5 can ever
allocate a second range. The re-runner is RD9 row (a)
(`docs/inflight/in-progress/workplan-range-directory.md` §7), whose own
wording is narrower — "RD3's zero-cost and RD8's fast-path claims" — so
the three-arm mix definition is the order's, carried here, not the
workplan's. This reads against **H5** (`range-foundation.md` §4): *"the
pre-range baseline is capturable now and not later... capturing it at the
format epoch is the only honest before."* H5's own falsifier is `none` — it
is a plan, not a claim — so this file is **a results file, not a verdict**:
no hypothesis is scored here. What follows is what was measured, the noise
it was measured against, and — in §9 — how RD9(a) must use it rather than
read it as a finding on its own. Measured on the worktree
`v2.4.0-range-foundation-1` at `7b48f6e` (`v2.2.1-76-g7b48f6e`).

## 1. Stamp

| | |
|---|---|
| Date/time executed | 2026-08-27, ~14:59–15:20 UTC |
| Version directory | `bench/v2.4.0/` names the work order's target version; **no `v2.4.0` tag exists** — the operator names versions — so `git describe --tags` reads `v2.2.1-76-g7b48f6e`, and that string, not the directory, is what dates every number here |
| Worktree | `v2.4.0-range-foundation-1` |
| Branch | `worktree-v2.4.0-range-foundation-1` |
| Commit | `7b48f6e` (`v2.2.1-76-g7b48f6e`), committer date `2026-08-27T14:53:24Z` |
| Tree cleanliness | clean at measurement time (`git status` — nothing to commit) both before and after the run; `touch src/server/core_runtime.cpp` was used to force a rebuild (§2) but left no content diff — `git diff --stat` reads empty throughout |
| Host | `ip-172-31-1-92`, Linux 7.0.0-1011-aws, Ubuntu 26.04 — same host M1/M2 ran on |
| CPU | Intel Xeon Platinum 8488C, 1 socket × 4 cores × 2 threads/core = 8 logical CPUs (SMT on), 1 NUMA node (`lscpu`) |
| Data-file / workdir device | `$HOME` (`/home/ubuntu/m3-bench/...`) → `/dev/root`, **ext4** (`df -T`); binary copy also on this ext4 filesystem. `/tmp` on this host is tmpfs (`df -T /tmp`) and was not used for any data file or workdir this session |
| Build type | Release: `CMAKE_BUILD_TYPE=Release`, `CMAKE_CXX_FLAGS_RELEASE = -O3 -DNDEBUG`, generator Unix Makefiles, compiler `/usr/bin/c++` = GCC 15.2.0, `nproc`=8 |
| Server config, null1/null4/b1 (arm R excepted) | `cores` per cell (1 for `null1`'s forced-single arms, 4 for `null4`/`b1`), `durability = group` (the driver's own default, non-relaxed — a real commit-cycle cost is in every number below), `placement = rotate`, `peer_listeners = on` for the `cores=4` cells (`single_relation_probe.py`'s own config writer, §4) |
| Server config, arm R (`order_by_benchmark.py`) | `cores = 4`, `durability = group`, no `peer_listeners` (default off) and no `placement` override (default `creating`) — chosen so every connection lands deterministically on core 0 (§4 explains why) |

## 2. Binary provenance

| | |
|---|---|
| Source commit | `7b48f6ed52250359b4f83f4ee6710c098459c62a` |
| `git describe --tags` | `v2.2.1-76-g7b48f6e` |
| Build tree | `.../v2.4.0-range-foundation-1/build-release` (in-tree, not `git archive` — this cell measures one commit, not two, so M1's out-of-tree-checkout method does not apply here; §9 explains why it will apply to RD9(a)'s later re-run) |
| Build-tree binary mtime, before this session | 2026-08-27 14:45:03 |
| Commit committer-date | 2026-08-27T14:53:24Z |
| Binary older than HEAD by wall-clock stamps? | yes, by the standing check (14:45:03 < 14:53:24) — per the provenance rule this required a rebuild before measuring, not an assumption that the two were equivalent |
| What the rebuild found | `HEAD`'s own commit (`7b48f6e`, "RA0f") touches exactly one file, `src/server/core_runtime.cpp`, and is a **comment-only** correction (+5/-4, no non-comment line either side, per the commit message itself). `cmake --build build-release --target kds_server -j8` immediately after the mtime check reported `Built target kds` / `Built target kds_server` with **no recompilation** — the object file was already newer than the source edit (source mtime 14:44:50, object/binary mtime 14:45:03), so the binary already carried the comment-only commit's (identical) code; the mtime gap was against the *commit* timestamp, not against uncompiled content. To make the binary's own mtime post-date HEAD's commit time as the provenance rule requires, `touch src/server/core_runtime.cpp` was run and the target rebuilt again, which did recompile and relink |
| Build-tree binary mtime, after forced rebuild | 2026-08-27 15:00:23 (now after `7b48f6e`'s commit time) |
| Run copy | `/home/ubuntu/m3-runbins/kds_server-v16` |
| Copy sha256 | `640c669bfba5a19e5f4a55c71db27571b094cf56e6b62dd6f0dfbf71024b2c02` |
| Build-tree binary sha256 (post-rebuild) | `640c669bfba5a19e5f4a55c71db27571b094cf56e6b62dd6f0dfbf71024b2c02` — identical to the copy |
| Copy = build-tree binary? | yes, byte-identical hash |
| Cross-check against M1 | This hash is **byte-identical to M1's own v16 arm binary** (`bench/v2.4.0/results-m1-mount-cost-v2.2.1-68-g7318e7e.md` §2, built from `7318e7e`). This is **not** simply because the eight intervening commits were docs/comments — `355fe78` ("RA3/RD4: RangeEligible built") added real, compiled code (`src/exec/range_eligible.cpp`, part of the `kds` static-library target per `CMakeLists.txt:135`, confirmed present and compiled in `build-release/CMakeFiles/kds.dir/src/exec/range_eligible.cpp.o`), and `82bdf92` touched a header the server *does* include — `TableAccess::AnyCabin()`, `include/kds/catalog/schema.hpp:300` — whose only caller is `range_eligible.cpp`, so as an uncalled inline it emits nothing into any other translation unit. The binary is identical anyway because **that translation unit is never pulled into `kds_server`'s link**: `nm -C build-release/kds_server \| grep -i rangeeligible` returns nothing, where the same command against the `.o` file itself finds `kds::exec::RangeEligible` defined. A static archive member is only linked in when something in the link graph references one of its symbols; `kds_server` references none, `kds_tests` does (`range_eligible_test.cpp.o` is in the test binary's own object list). This is an independent, linker-level confirmation of **H2** ("`RangeEligible` is built and tested at RA3 with exactly one caller — its test... nothing on a statement path calls it until RD5") — not just RA3's own grep-based claim, but a fact about what the shipped server binary actually contains |

The copy was made once, before the first cell, and every server in this
run — the sanity check, `null1`/`null4`/`b1`, both shipping-verification
runs, and the `order_by_benchmark.py` arm — started from this one copy,
never from the build tree's own binary.

## 3. Sanity check — before any measurement

Fresh single-shot server (`--config` file, `cores = 4`, no `peer_listeners`),
fresh data file:

```
ckdbs on /home/ubuntu/m3-bench/sanity/kds.db: 16 pages, superblock version 16
listening on 127.0.0.1:15930
```

- `SHOW TABLES`: `types objects columns tables indexes patterns access_stats
  cabins fkeys ranges pattern_defs assertions` — 12 relations, `ranges`
  present, matching M1's finding (`kSysTables` widened 9 → 10 at RA2;
  `pattern_defs`/`assertions` are the two real row-codec relations outside
  that array).
- `SELECT COUNT(*) FROM ranges` → `ERR no columns for this rel_id` — RD1's
  product is the relation's existence and emptiness; RD2 (the row format) is
  unbuilt, so the relation is nameable but carries no columns, exactly as M1
  §3 found.
- Re-checked **after** the `order_by_benchmark.py` workload ran against a
  fresh file (§4/§6c): `SHOW TABLES` lists `ranges` plus the workload's own
  `ob_m3rows3000` relation, and `SELECT COUNT(*) FROM ranges` still answers
  the same `ERR no columns` — `sys.ranges` is untouched by any statement
  this session ran, which is the expected shape given H2 (`RangeEligible`
  has no caller until RD5) rather than a separate finding.

Arm identity confirmed before any timing was taken.

## 4. Instrument and cell shape

**Arms I and S** (`bench/docs/README.md` §"The cross-core probes"):
`bench/run_ssb.py` driving `bench/single_relation_probe.py`. Cells run,
in the fixed order the driver itself uses (owner arm `a` first, foreign
arm `b` second, per cell):

- `null1` — `cores = 1` against `cores = 1`, S = 4 sessions on one relation,
  both arms identical (`--arm single` forces `cores = 1` regardless of the
  `--cores` passed). The ordering-bias control.
- `null4` — `cores = 4`, both arms seated on the owner, S = 4 on one
  relation: the same code path on both arms with the one thing under test
  (seat) held fixed, so its ratio is the harness's own bias at this core
  count. It is **not** `b1`'s shape — `b1` spreads 3 sessions over 3
  relations on 3 owners, and §10 prices that difference — so no same-shape
  null was run for `b1`; `null4` bounds the bias, it does not replicate
  the cell.
- `b1` — one relation per writer core (3 relations, 3 writer cores),
  one session each: `a` = every session seated on its own relation's owner
  (**arm I**, single-relation insert, local), `b` = every session seated on
  a core that does not own the relation it writes, so every insert ships
  (**arm S**, the shipped-statement path).

Exact invocation (verbatim, reproducible):

```
cp build-release/kds_server /home/ubuntu/m3-runbins/kds_server-v16
sha256sum /home/ubuntu/m3-runbins/kds_server-v16
python3 bench/run_ssb.py --server /home/ubuntu/m3-runbins/kds_server-v16 \
    --workdir /home/ubuntu/m3-bench/ssb-run \
    --cells null1,null4,b1 --reps 8 --rows 3000 --port 17300
```

(`--archive` omitted — §8's archive decision. `--rows 3000` is rows **per
session**, and `b1` deals its sessions one per relation
(`rel_of[i] = names[i % R]`), so each of its 3 relations takes
3000 + 1 warm-up row = **3,001**; `null1`/`null4` put all 4 sessions on one
relation, which takes 4 × 3000 + 1 = 12,001.)

**Arm R** (single-relation read): no probe in this suite covers reads, so
`tools/order_by_benchmark.py` pass 2 was used — a single relation
(`id, val, grp, amount, tag`, BTREE-clustered), one connection, fifteen
read arms including `plain` (unsorted full scan, the shape closest to "a
single-relation read"), `pk-point` (a single clustered descent, the
tightest read shape) and `plain-again` (the in-run noise floor). This
driver was chosen over building a new one because it already gives, in one
run, a full-scan read, a point read, server CPU per op and an internal
noise floor — exactly the ingredients a read baseline needs — where a
purpose-built prober would have to reinvent all four. **Only pass 2** was
run (no `--ab-port`, no `--pre-port`): a single binary, single side, one
`--rows` size (3000, matching arm I/S's per-session row count so the three
arms' relations end up comparably sized). `--server-pid` was passed so
server CPU per op is reported alongside client-observed wall time.

```
# server started by hand (order_by_benchmark.py does not start its own):
cat > /home/ubuntu/m3-bench/obr/kds.conf <<'EOF'
data_file = /home/ubuntu/m3-bench/obr/kds.db
port = 17450
wal_dir = /home/ubuntu/m3-bench/obr/wal
log_dir = /home/ubuntu/m3-bench/obr/log
cores = 4
durability = group
EOF
/home/ubuntu/m3-runbins/kds_server-v16 --config /home/ubuntu/m3-bench/obr/kds.conf &

python3 tools/order_by_benchmark.py --port 17450 --rows 3000 --ops 2000 \
    --rounds 10 --limit 20 --seed 1 --suffix m3rows3000 \
    --server-pid <pid> --label v16-7b48f6e \
    --json /home/ubuntu/m3-bench/obr/obr-out.json
```

**Why arm R's server omits `peer_listeners`**: `single_relation_probe.py`'s
`--seat` shape depends on `peer_listeners = on` (`SO_REUSEPORT`, kernel
picks the accepting core, and the driver opens connections until it lands
one on the core it wants — `tools/multicore_benchmark.py:187`,
`collect_connections`). `order_by_benchmark.py` has no such retry logic: it
opens one connection to `--port` and issues `CREATE TABLE` on it directly,
which is DDL and must land on core 0 (`CommandDispatcher::CheckWriteAffinity`'s
sibling check — "core 3 takes no DDL: the catalog has one writer, the
system core"). With `peer_listeners = on` the very first sanity attempt at
this cell landed on core 3 and refused; leaving `peer_listeners` off (its
default) and `placement` at its default (`creating`) makes every connection
land on core 0 deterministically, which is what a client that has never
heard of core placement gets for a plain read — the honest shape for "arm
R" as a control, distinct from arm S's shipped shape by construction rather
than by accident.

**Verifying arm S actually shipped** — two extra, untimed runs of
`single_relation_probe.py` in `b1`'s exact shape (`--cores 4 --sessions 3
--relations 3 --rows 3000`, one `--seat owner`, one `--seat foreign`),
each with `--json` so `meta_by_core`'s `shipped_*` counters could be read
directly (`run_ssb.py`'s own summary strips them out before printing).
Results in §6c.

Confounds and how the protocol controls them:

- **Ordering bias** — `run_ssb.py` always runs the owner-seated arm first
  and the foreign-seated arm second within a rep; `null1` and `null4` are
  the two controls whose two arms are the *same* arm, which is what makes
  their ratio the bias and nothing else, so `b1`'s ratio can be read
  against it rather than against 1.0 (they are controls on the bias, not
  replicas of `b1`'s shape — see the cell list above).
- **Page cache / fresh files** — fresh data file and fresh server process
  per rep (`run_ssb.py:191` deletes each arm's workdir when the arm ends,
  and the probe writes a fresh config, server and data file per
  invocation), so residual cache warmth lands on both arms of a rep, not
  preferentially on one; `order_by_benchmark.py`'s arm R is one server, one
  data file, all fifteen arms read the same loaded rows.
- **The within-sitting warm-up step M1 found** — M1 §5 found cell a's
  absolute wall time step from ~150 ms to ~340 ms at rep 2→3 of every
  sitting, common to both arms. **Checked for here and not found**: `null1`
  arm `a`'s IPS series across its 8 reps is `2128.9, 1791.5, 1934.9,
  1759.7, 2024.0, 1744.3, 1886.7, 1669.4` — rep 1 is the *highest* value in
  the series, not the lowest, and there is no step at rep 2→3 in any of the
  three cells' per-rep tables (§6). This is not a contradiction of M1, and
  it does not name M1's mechanism either — M1 §5 says outright that *"its
  mechanism is not identified by this run"*, and the step landed on its
  create cell and not on its remount cell. What differs here is the
  window: every rep still creates a fresh file and mounts it, but that
  boot is **outside** the timed window, which opens thousands of
  statements into an already-running server. Reported either way per the
  order's instruction.
- **Background load** — `bench/wait_quiet.sh` run and reported quiet
  (loadavg 0.06–0.66, no `cc1plus`/`ld`/`kds_tests`/`cc1`/`as`) before the
  `run_ssb.py` sweep and before each manual server start (sanity, both
  ship-verification runs, arm R); `run_ssb.py`'s own per-rep gate recorded
  `gate_wait_max_s: 0` and `contended: []` for all three cells — no rep was
  delayed or discarded for contention. One unrelated `kds_server` process
  (`/home/ubuntu/cws/ckdbs/build-release/kds_server`, a different worktree's
  session) was running throughout at negligible CPU — loadavg stayed under
  0.7 at every quiet-check, so it is named but not treated as a confound.
- **Polling / instrument resolution** — `single_relation_probe.py` times
  each statement client-side around the socket call, so its floor is Python
  socket RTT, not a polling loop; `order_by_benchmark.py`'s `--server-pid`
  CPU figures come from `/proc/<pid>/stat`, whole-jiffy (10 ms) resolution,
  divided over each arm's 2000 ops — at ~600 µs/op the CPU total per arm is
  ~1.2 s, well above single-jiffy noise.

## 5. Noise bands — from the controls, before the real numbers are read

**Arms I/S** (IPS is the throughput metric per `run_ssb.py`'s own design;
per ck-tester rule 5a this is what the per-cell tables report, not a
converted delay):

| control | shape | a median (ips) | b median (ips) | ratio median | ratio spread |
|---|---|---|---|---|---|
| `null1` | `cores=1`, both arms identical | 1839.1 | 1839.05 | **1.0247** | 0.8444 .. 1.1616 |
| `null4` | `cores=4`, both arms owner-seated | 2247.05 | 2296.8 | **0.9865** | 0.9516 .. 1.1431 |

Both controls' ratios straddle 1.0 (1.0247 and 0.9865), and both carry a
wide per-rep spread at 8 reps — `null1` −15.6%/+16.2%, `null4`
−4.8%/+14.3% — which is the harness's own noise at this session length,
not an engine effect (both arms are the same code path in both controls). **Any `a`-vs-`b` ratio inside
[0.84, 1.16] is not distinguishable from this floor.** `b1`'s ratio, read
in §6, is 0.7581 — outside both controls' full min..max range, not only
outside their medians, which is what makes it readable as a finding rather
than noise (§6b).

**Arm R**: no cross-rep control exists (arm R is one server, one run, not
repeated invocations), so its noise floor comes from **inside** the run
per ck-tester rule 8 and the driver's own design: `plain` measured twice
(`plain`, `plain-again`) as the same configuration by construction.

| | plain | plain-again | delta |
|---|---|---|---|
| qps | 1249.5 | 1259.7 | 10.2 (0.8%) |
| mean µs | 800.3 | 793.6 | 6.7 |
| p50 µs | 758.2 | 754.7 | 3.5 |
| p95 µs | 979.8 | 970.2 | 9.6 |
| p99 µs | 1097.5 | 1066.5 | 31.0 |

**Noise band for arm R: ≤1% at p50, widening to ~3% at p99.** `pk-order`'s
p50 (763.5 µs) is 0.7% above `plain`'s (758.2 µs) — inside this band —
consistent with the compile-time elision the driver's `ANALYZE` check
confirms (§6d: `pk-order`'s `sorted=None`, matching `plain`), not a
separate measurement of sort cost.

## 6. Per-arm results

### 6a. Arms I and S — per-rep IPS, then percentiles

`null1` (S=4, both arms `cores=1`):

| rep | a ips | b ips | ratio (b/a) |
|---|---|---|---|
| 1 | 2128.9 | 2073.4 | 0.9739 |
| 2 | 1791.5 | 1773.6 | 0.9900 |
| 3 | 1934.9 | 1809.1 | 0.9350 |
| 4 | 1759.7 | 1869.0 | 1.0621 |
| 5 | 2024.0 | 1709.0 | 0.8444 |
| 6 | 1744.3 | 2026.1 | 1.1616 |
| 7 | 1886.7 | 2100.4 | 1.1133 |
| 8 | 1669.4 | 1768.5 | 1.0594 |

median a = 1839.1, median b = 1839.05, min/max a = 1669.4/2128.9, min/max
b = 1709.0/2100.4. (IPS carries min/median/max only — the driver's own
`_ips_min`/`_ips_median`/`_ips_max`, not a five-point distribution — so no
percentile table is invented for it, per rule 6; the five-point tables
below are the **latency** distributions.)

Latency percentiles (µs, median across the 8 reps' own p0/p25/p50/p95/p99
— `run_ssb.py`'s `summarize()` design, stated so the table is read
correctly: each cell below is a median-of-per-rep-percentiles, not a flat
percentile over all 96,000 pooled ops):

| | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|
| a (owner) | 974.45 | 1974.55 | 2189.35 | 2616.85 | 2807.95 |
| b (owner, identical arm) | 1075.65 | 1972.10 | 2173.45 | 2599.95 | 2795.25 |

`attempted=executed=96000` both arms, `refused=0`, `retries=0`,
`verify_bad=[]` — every rep's `COUNT(*)` matched expectation.

`null4` (S=4, both arms `cores=4`, owner-seated):

| rep | a ips | b ips | ratio (b/a) |
|---|---|---|---|
| 1 | 2175.9 | 2367.0 | 1.0878 |
| 2 | 2216.4 | 2161.8 | 0.9754 |
| 3 | 2380.9 | 2273.5 | 0.9549 |
| 4 | 2337.4 | 2331.8 | 0.9976 |
| 5 | 2273.5 | 2197.3 | 0.9665 |
| 6 | 2306.5 | 2194.8 | 0.9516 |
| 7 | 2220.6 | 2320.1 | 1.0448 |
| 8 | 2162.8 | 2472.2 | 1.1431 |

median a = 2247.05, median b = 2296.8, min/max a = 2162.8/2380.9, min/max
b = 2161.8/2472.2.

| | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|
| a (owner) | 857.25 | 1554.05 | 1773.70 | 2256.75 | 2421.30 |
| b (owner, identical arm) | 805.10 | 1528.75 | 1753.80 | 2221.50 | 2429.30 |

`attempted=executed=96000` both arms, `refused=0`, `retries=0`,
`verify_bad=[]`.

`b1` (3 relations, 1 session per writer core, `a` = **arm I**, `b` =
**arm S**):

| rep | I ips (a) | S ips (b) | ratio (S/I) |
|---|---|---|---|
| 1 | 2701.9 | 2115.9 | 0.7831 |
| 2 | 2800.0 | 2133.3 | 0.7619 |
| 3 | 2736.6 | 2046.1 | 0.7477 |
| 4 | 2721.1 | 2092.6 | 0.7690 |
| 5 | 2753.8 | 2077.0 | 0.7542 |
| 6 | 2706.5 | 2011.2 | 0.7431 |
| 7 | 2708.1 | 2112.9 | 0.7802 |
| 8 | 2755.5 | 1946.3 | 0.7063 |

median I = 2728.85, median S = 2084.8, min/max I = 2701.9/2800.0, min/max
S = 1946.3/2133.3. Ratio median **0.7581**, spread 0.7063..0.7831 — every
rep's ratio sits below the `null1`/`null4` controls' full spread (§5), so
this is not read as noise.

| | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|
| I (owner-seated insert) | 636.15 | 922.30 | 1034.85 | 1338.30 | 1486.80 |
| S (shipped) | 663.95 | 988.00 | 1150.75 | 2231.05 | 2619.30 |

`attempted=executed=72000` both arms, `refused=0`, `retries=0`,
`verify_bad=[]`. The p50 gap (I 1034.85 → S 1150.75, +116 µs, +11%) is
modest next to the p95/p99 gap (+893 µs at p95, +1133 µs at p99, roughly
+67%/+76%) — shipping's cost concentrates in the tail, not the typical
case, which §6b's counters explain.

### 6b. Wait attribution — arm S against arm I

`single_relation_probe.py` gives per-statement client latency and, from
`SHOW META`, the shipping counters (§6c) but no server-side per-phase
timer for an individual statement (unlike M1's mount-time recovery
counters, there is no `shipped_statement_us` breakdown on this engine
today — stated as **not measurable**, per ck-tester rule 3, rather than
omitted). What is measurable:

- **Durability/commit (fsync) wait** — identical in both arms:
  `durability = group` on both `single_relation_probe.py` invocations, so
  every insert in both arms pays the same group-commit cycle. Not
  decomposable further by this instrument; §7's PostgreSQL section and
  `run_ssb.py`'s `sync` cell (not run this session — outside M3's cell
  list) are what would isolate it.
- **Write-statement wait (the local insert path itself)** — arm I's own
  p50/p95/p99 (1034.85/1338.30/1486.80 µs) **is** this leg; it is what
  arm S pays *in addition to* whatever shipping adds, since arm S's
  statement still executes on the owner core through the same insert path
  once it arrives.
- **Shipping wait (ship + park + owner-execute + reply)** — the extra leg
  arm S carries. §6c's verification run (same shape as `b1`'s `b` side)
  reports `shipped_wait_us_max` of 3946–4891 µs on the three arrival cores
  (core 0: 4644, core 1: 4891, core 2: 3946) — a **maximum** over ~3000
  shipped statements per core, not a distribution, so it bounds the worst
  parked wait rather than describing the typical one. Read beside the p50
  gap (+116 µs) and the p99 gap
  (+1133 µs), the picture is consistent: most shipped statements pay a
  small, roughly constant routing cost close to the CLAUDE.md milestone
  entry's cited "~20 µs of wire" (a different cell's number, not
  reproduced here, so read as a rough consistency check and not a
  cross-run comparison), while a minority queue behind a parked waiter and
  pay up to several milliseconds — which is exactly what moves p95/p99
  much more than p50.
- **Client and socket round trip** — not separately measured for arms I/S
  (`single_relation_probe.py` has no bare-ping arm the way
  `order_by_benchmark.py` does); arm R's `ping` figure (§6d below, mean
  20.2 µs) is a rough scale reference for this host's floor, not a value
  from this server configuration.
- **Lock/conflict wait** — arms I/S insert engine-issued keys at an
  ascending tail on separate relations (`b1`) or one shared relation
  (`null1`/`null4`); `refused=0`/`retries=0` on every rep of every cell
  means no lease-refill or write-affinity refusal was hit, so this leg
  reads as zero for these cells by construction, not by measurement of an
  absent contention path.

### 6c. Shipping verification (untimed, informational — confirms arm S actually shipped)

Two extra runs, `b1`'s exact shape (`--cores 4 --sessions 3 --relations 3
--rows 3000`), not part of the 8 timed reps above:

| | `--seat owner` | `--seat foreign` |
|---|---|---|
| `inserts_per_second` | 2771.4 (inside `b1` arm I's 2701.9–2800.0 range) | 2122.9 (inside `b1` arm S's 1946.3–2133.3 range) |
| `attempted` / `executed` / `refused` | 9000 / 9000 / 0 | 9000 / 9000 / 0 |
| `verify` | rows as expected | rows as expected |
| `retries` (driver-level) | 0 | 0 |
| `cpu_busy`, cores 1/2/3 (the three owners in both runs) | 0.0872 / 0.0786 / 0.0627 | 0.0725 / 0.0558 / 0.0481 |
| `cpu_busy`, core 0 (idle in the owner run, an arrival core in the foreign run) | 0.0156 | 0.0240 |

The owner cores' `cpu_busy` is *lower* in the foreign run than in the
owner run (0.228 summed vs. 0.176) even though they still execute every
insert — consistent with the owner-seated run's sessions also owning the
socket read/parse/dispatch on those same cores, work the foreign run moves
to the arrival cores (core 0's `cpu_busy` rises from 0.016 to 0.024; the
remainder is not accounted here — cores 4–7's figures were not carried
into this file, so where the rest landed is not asserted).

**A busy fraction is not a per-statement cost, and normalised it changes
sign.** Both runs insert 9000 rows, but the shipped run's window is longer
(9000/2122.9 = 4.24 s against 9000/2771.4 = 3.25 s), so CPUs 0–3's summed
busy comes to **88.1 µs of CPU per insert owner-seated against 94.4 µs
shipped — +7%**, where the fractions alone read −23%. The +7% is the
honest direction for a routing hop that adds a ring send, a park and a
reply; it is also an upper bound on the *engine's* share, since these
four CPUs also carry whatever the Python driver's threads were scheduled
onto them.

`SHOW META` read from every core on the foreign-seated run:

| core | role | `shipped_statements` | `shipped_executed` | `shipped_refusals` |
|---|---|---|---|---|
| 0 | arrival | 3005 | 0 | 1 |
| 1 | arrival + owner | 3002 | 3003 | 1 |
| 2 | arrival + owner | 3004 | 3003 | 3 |
| 3 | owner only | 0 | 3005 | 0 |

Three cores carry an arrival-side count and three an owner-side count —
**shipping is confirmed**: every one of the 9000 inserts in the
foreign-seated run arrived on a non-owning core and was executed on the
owner. **Both columns sum to 9011, not 9000**, and the extra 11 reconcile
exactly: **3** warm-up inserts (one per relation, the probe's pre-window
warm-up loop — itself foreign-seated, so itself shipped), **5** retries of
those warm-up inserts (the driver's own `warmup_retries: 5`, matching
`shipped_refusals` 1+1+3 core for core — a retryable lease-refill refusal,
which the owner still counts in `shipped_executed` because it ran the
statement and answered `ERR`, `shipped_statement_executor.cpp:148`), and
**3** post-window `SELECT COUNT(*)` verifications, all issued on session
0's core-0 connection over relations cores 1/2/3 own, so all three
shipped. That is 3000+1+1+3 = 3005, 3000+1+1 = 3002 and 3000+1+3 = 3004
arrival-side, and 3003/3003/3005 owner-side once each refusal is charged
to the owner it reached — every cell of the table to the unit. None of the
11 falls inside the timed window, so none is a refusal counted against the
9000 attempted/executed statements above. The owner-seated run
shows `owner_cores_distinct: [1, 2, 3]`, the same three relations' owners
as the foreign run, confirming both verification runs measured the same
`b1` shape.

### 6d. Arm R — `order_by_benchmark.py` pass 2, 3000 rows, `v16-7b48f6e`

One server, one data file, 2000 ops per arm over 10 interleaved rounds
(block = 200), 1 connection. `plain` (the closest shape to "a single-relation
read" — unsorted `SELECT` of all five columns) and `pk-point` (a single
clustered descent) are arm R's two representative shapes; the other
thirteen arms are the instrument's own decomposition (§4) and are reported
for completeness.

| arm | ops | qps | mean µs | p0 | p25 | p50 | p95 | p99 | cpu µs/op |
|---|---|---|---|---|---|---|---|---|---|
| `ping` (client+socket floor) | 2000 | 49,611 | 20.2 | 9.9 | 17.3 | 20.5 | 29.5 | 46.7 | 5.0 |
| `plain` (unsorted full scan — **the arm R shape**) | 2000 | 1,250 | 800.3 | 659.3 | 722.4 | 758.2 | 979.8 | 1097.5 | 615.0 |
| `star` (`SELECT *`) | 2000 | 1,238 | 808.1 | 653.3 | 718.4 | 759.4 | 976.2 | 1070.3 | 600.0 |
| `pk-order` (`ORDER BY id ASC`, elided) | 2000 | 1,236 | 808.7 | 666.0 | 728.9 | 763.5 | 990.8 | 1317.4 | 625.0 |
| `nonpk` (`ORDER BY val`) | 2000 | 651 | 1536.2 | 1319.9 | 1437.0 | 1512.2 | 1716.2 | 1968.6 | 1345.0 |
| `nonpk-desc` | 2000 | 659 | 1517.0 | 1339.9 | 1428.9 | 1508.3 | 1683.1 | 1786.1 | 1305.0 |
| `nonpk-str` (`ORDER BY tag`) | 2000 | 589 | 1698.7 | 1496.5 | 1598.2 | 1667.4 | 1868.0 | 2049.6 | 1520.0 |
| `nonpk-lim` (`ORDER BY val LIMIT 20`) | 2000 | 2,497 | 400.5 | 314.4 | 340.7 | 355.6 | 549.1 | 628.5 | 345.0 |
| `plain-lim` (`LIMIT 20`, no order) | 2000 | 34,006 | 29.4 | 18.8 | 28.8 | 29.4 | 38.0 | 45.8 | 10.0 |
| `pk-point` (`WHERE id = ?` — **arm R's point-read control**) | 2000 | 38,155 | 26.2 | 16.2 | 25.5 | 26.2 | 35.0 | 47.2 | 10.0 |
| `plain-again` (noise floor) | 2000 | 1,260 | 793.6 | 655.0 | 721.8 | 754.7 | 970.2 | 1066.5 | 620.0 |
| `an-plain` (ANALYZE, no render) | 2000 | 2,996 | 333.8 | 257.8 | 281.2 | 293.6 | 483.4 | 545.6 | 290.0 |
| `an-nonpk` | 2000 | 1,104 | 906.0 | 762.5 | 824.2 | 867.5 | 1059.3 | 1172.1 | 840.0 |
| `an-nonpk-lim` | 2000 | 2,484 | 402.6 | 313.5 | 340.5 | 357.8 | 553.6 | 621.6 | 340.0 |
| `an-plain-lim` | 2000 | 30,067 | 33.3 | 20.0 | 29.8 | 30.4 | 42.1 | 138.2 | 25.0 |

`ANALYZE` confirms every arm compiled as expected — `pk-order`:
`sorted=None` (elided, matches `plain`'s own `sorted=None`), `nonpk*`:
`sorted=3000` (a real sort over every row), `nonpk-lim`: `sorted=20` (the
top-N heap holds only the limit), `plain-lim`: `examined=20` (the quota
stopped the walk, it did not scan all 3000 rows). `--verify` (on by
default): `{'rows': 3000, 'checks': 8}` — 8 row-for-row checks passed, no
abort.

**Server CPU share for the arm R shape**: `plain`'s 615.0 µs/op against its
800.3 µs mean wall time is 77% server CPU; the residual ~185 µs is client
and socket round trip beyond `ping`'s own 20.2 µs floor (the two are not
directly subtractable — `ping` carries no scan/render — but the gap is the
scale of what a Python client adds atop the raw wire, which
`tools/order_by_benchmark.py`'s own header names as "most of the number"
for a large reply). `pk-point`'s 10.0 µs/op against its 26.2 µs
mean is 38% CPU — for a single-row reply, round-trip overhead dominates.

## 7. Versus PostgreSQL

**Arms I and S**: no PostgreSQL twin exists for `single_relation_probe.py`'s
N-sessions-one-relation shape. `bench/docs/README.md`'s entry for the
cross-core probes names no `pg_*` counterpart for `run_ssb.py` or
`single_relation_probe.py`, and none was found under `tools/pg_*.py`
(checked: `pg_benchmark.py`, `pg_bulk_insert_benchmark.py`,
`pg_cabin_optimizer_benchmark.py`, `pg_index_benchmark.py`, the five
`pg_scenario*.py` files, `pg_wire.py` — none drives this shape).
`bench/run_pw6.py --pg` is the closest existing twin in spirit (N sessions
across tables against `pg_wire.py`) but measures `multicore_benchmark.py`'s
non-interfering-relations shape, not this driver's single-relation
contention shape. **The task this leaves**: a `pg_single_relation_probe.py`
importing arm I's shape (N PostgreSQL sessions inserting into one table,
serial pk) would give arm I a baseline; **arm S has no PostgreSQL
analogue at all** — statement shipping is ckdbs's own cross-core routing
over a single logical process's core-partitioned catalog, and PostgreSQL's
backend-per-connection model has no equivalent concept to ship a statement
to. Say this plainly rather than force a comparison: arm S's versus-PostgreSQL
section is **not applicable**, not merely unrun.

**Arm R**: `bench/docs/README.md`'s entry for `order_by_benchmark.py`
already states it — **"No PostgreSQL twin exists yet — `tools/
pg_order_by_benchmark.py` is the task"** — not attempted this session.

## 8. What this run does not measure

- **The per-statement overhead A/B gate.** Suspended by the operator's
  2026-08-24 amendment for v2-stage development. Stated as **not run**,
  never implied — and this session made no engine-code change, so the gate
  would have had nothing to bracket even if it were active.
- **Populated or split-relation states.** RD5 (allocation) is out of scope
  for R3-A; `sys.ranges` stayed empty through every cell this session ran
  (re-confirmed in §3 after the arm R workload), by construction rather
  than by a targeted check of RD5 behaviour that does not exist yet.
- **`b2`/`b3`/`b4`/`sz`/`sync`/`b6`** — the rest of `run_ssb.py`'s cell
  family. The order's suggested set (`null1`, `null4`, `b1`) was run as
  suggested; nothing beyond it was added, since M3's charter is a baseline
  capture of RD9(a)'s three-arm mix, not a re-run of the SS-B sweep that
  `bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md` already owns.
- **The `sync` durability control.** `run_ssb.py`'s `sync` cell (S=1,
  `group` vs `relaxed`) would isolate how much of arm S's gap is a device
  sync; not run this session (§6b names it as the instrument that would
  close this gap).
- **A row-set sweep for arms I/S, or a second `--rows` size for arm R.**
  `ck-tester`'s own rule 9 asks every test and matrix to sweep 200/1K/10K
  at minimum; this cell ran one size. **The order does not exempt it in so
  many words**: `range-foundation.md` §7 says only *"Thin by design; this
  milestone is mostly structural"* and its M3 row names no row count, so
  the single size is this run's own reading of that instruction and not a
  licence quoted from it. Named here as an explicit deviation from the
  standing rule rather than a silent omission: `--rows 3000` was the one
  size run for the mix (matching arm R's size), `run_ssb.py`'s own `sz`
  cell is the 200/1000/10000 sweep for arms I/S and was not part of this
  session's cell list, and `order_by_benchmark.py` was run at one size for
  arm R.
- **Arm R's cross-binary or cross-arm control for `pk-point`.** Only one
  side ran (`--port`, no `--ab-port`/`--pre-port`), so `pk-point`'s "must
  not move" property is stated as a design property of the instrument
  here — the phrase is the driver's own header
  (`tools/order_by_benchmark.py`: "a single-row clustered descent that no
  sort can touch, so it must not move across arms or across binaries";
  the README's entry calls it only "a control") — not verified against a
  second measurement in this run.
- **Per-statement server-side wait decomposition for arms I/S.** No
  `SHOW META` counter or driver instrumentation gives a per-op breakdown
  of the local insert path or the shipped path's legs beyond
  `shipped_wait_us_max` (a maximum, not a distribution) — named as not
  measurable in §6b rather than omitted.
- **The correctness suite.** No engine code was changed this session
  (`git diff --stat` is empty throughout; the one `touch` was to force a
  binary rebuild past a commit-timestamp check, not a content edit), so
  the suite-before/after gate this agent's charter names for a code change
  does not apply here and was not run.

**Archive decision — nothing archived**, per the 2026-08-25 rule's
narrower-measurement side, as M1 and M2 already decided on the same
ground. The reason specific to M3's shape, since its whole product is a baseline meant
to be re-read: every per-rep number is already inline in §5/§6's tables,
and §9's re-read contract makes archived raw JSON worthless as a
comparison arm anyway — RD9(a)'s honest method is a same-sitting rebuild
of `7b48f6e`, not a cross-sitting read of this session's raw output, so
the archive would preserve exactly the kind of number §9 forbids using.

## 9. How RD9(a) must re-read this

**M1's own finding governs how this file may be used**: M1 §5/§10 found
that absolute wall time on this host is **sitting-dependent** — a step from
~150 ms to ~340 ms partway through every sitting it ran, clearing between
sittings and not attributable to either binary under test. This session's
own cells did not show that specific step (§4 names the check and its
result), but the lesson generalizes past the one instrument M1 measured it
on: **nothing in §5/§6 above should be read as an absolute number valid in
a different sitting.** `b1`'s ratio (0.7581) is a same-sitting,
paired-per-rep quantity, measured against same-sitting controls (`null1`,
`null4`) — that is what makes it readable at all, and it is exactly the
structure that does not survive being read against a number from a
different session.

What **is** durable, and what RD9(a) should carry forward rather than
re-derive:

1. **The commit.** `7b48f6e` (`v2.2.1-76-g7b48f6e`) is the binary this file's
   numbers came from, byte-identical (§2) to M1's own `7318e7e` build — so
   RD9(a)'s "before" state is anchored to a specific, reproducible
   toolchain output, not a description of behaviour. When RD5 lands, RD9(a)
   should build **this** commit the way M1 built its v15 arm — `git archive
   7b48f6e | tar -x -C <scratch>/src && cmake --build` out of tree — rather
   than trust this file's numbers against a differently-sitting run on
   whatever `main` looks like then. M1 §2 is the precedent for exactly this
   method, applied there to a two-commit A/B; here it is the method for
   reproducing this file's single commit as one arm of a future one.
2. **The exact invocations** (§4) — `run_ssb.py --cells null1,null4,b1
   --reps 8 --rows 3000`, and `order_by_benchmark.py --port <p> --rows 3000
   --ops 2000 --rounds 10 --limit 20 --seed 1 --server-pid <pid>` — so
   RD9(a) can run the *identical* cell shapes against a post-RD5 binary in
   one sitting, rather than approximating them.
3. **The mix definition** — arm R = `order_by_benchmark.py`'s `plain` (and
   `pk-point` as the point-read control), arm I = `single_relation_probe.py
   --seat owner` (`b1`'s `a` side), arm S = `--seat foreign` (`b1`'s `b`
   side) — stated once here so RD9(a) does not have to re-derive which
   driver arm stands for which leg of the order's three-arm mix.
4. **The noise bands** (§5) — `null1`/`null4`'s ratio spread
   (0.84–1.16 at 8 reps) and arm R's `plain`/`plain-again` gap (≤1% at
   p50) — as the floor below which a post-RD5 delta is not a finding.
   These bands are a property of this host and this rep count, not of the
   commit, so they should be **re-established** alongside a future run
   (its own `null1`/`null4`/`plain-again`) rather than imported wholesale —
   but their *order of magnitude* here is what tells RD9(a) whether eight
   reps is enough, or whether RD5's expected effect (if any) needs more.

**What RD9(a) must not do with this file**: read `b1`'s 0.7581 ratio, or
arm R's 800.3 µs `plain` mean, as a number to subtract a post-RD5 run's
number from directly. The valid comparison is the one M1 already
demonstrated the shape of — same sitting, interleaved, paired per-rep,
against a same-sitting control — with `7b48f6e` rebuilt as one arm of it.

## 10. What this teaches about the engine

**Source-read** (§2's `nm` evidence, not repeated here): H2
("`RangeEligible`... nothing on a statement path calls it until RD5") holds
at the **linker** level, not only by RA3's grep of caller sites — the
symbol is defined in the compiled object file and absent from `kds_server`,
which is why §2's sha256 cross-check lands on M1's own hash. RD4's cost to
the shipped server binary is zero in the most literal sense: the code is
not present in the artifact being measured at all.

**Measured** (`v2.2.1-76-g7b48f6e`, worktree `v2.4.0-range-foundation-1`,
commit `7b48f6e`): shipping's cost at this row count and session count is
not a flat per-statement tax — it concentrates in the tail. `b1`'s p50 gap
between arm I and arm S is +116 µs (+11%), a figure in the same rough
order as the "~20 µs of wire" the milestone table for cross-core execution
already names (a different cell's measurement, cited only as a scale
check, not reproduced here) plus this cell's own routing and queueing
overhead; the p99 gap is +1133 µs (+76%), and §6c's `shipped_wait_us_max`
(3.9–4.9 ms) shows the mechanism — a minority of shipped statements queue
behind a parked waiter on the arrival core, and that minority is what the
tail percentiles see and the median mostly does not. This is a data point
for whichever open decision in `docs/spec/crosscore.md` §9 eventually
prices the 2PC protocol's routing cost: the number to budget against is
the tail, not the median, if the workload that motivates it is sensitive
to p99 (an OLTP client usually is).

**Source-read** (`src/server/command_dispatcher.cpp:534`, "A peer takes no
DDL (workplan-peer-writer.md PW4)" — the gate the arm-R setup hit first,
§4): this is a distinct check from `CheckWriteAffinity` (`:3599`, the write
gate `range-foundation.md` §0 names as the function R6-8/RD6 will rewrite)
— PW4's gate refuses **any** DDL on any core but the system core,
independent of whether the relation being created would end up
peer-owned, where `CheckWriteAffinity` refuses a **write to an existing
relation** whose `owner_core` differs from the executing core. This
session's `CREATE TABLE ob_m3rows3000` landed on core 3 under
`peer_listeners = on` (§4) and was refused by the PW4 gate, not
`CheckWriteAffinity` — named precisely here because §0 is about
the latter, and this run's own refusal is evidence for a neighbouring but
different gate, not a repetition of R3-A's §0 finding.

**What H5 gets from this run**: H5 claimed the pre-range baseline is
capturable *now* and said nothing about what it would show — its own
falsifier is `none`. This file is that capture. The one thing worth naming
for the decision session C1–C4 feed (`range-foundation.md` §3): arm S's
tail cost (§6b, §6c) is a real, measured price statement shipping pays
today, on a one-range-per-relation world where RD5 has not yet added a
second range to widen or narrow it. RD3's zero-cost invariant — "a
one-range relation on its owner core must add zero instructions over
today" — is a claim about arm I (the local path), which this file shows
paying no shipping cost at all. `b1`'s arm I median (2728.85 ips) is
**higher**, not equal to, `null4`'s owner-seated control range
(2247.05–2296.8 ips median, 2161.8–2472.2 ips full spread) — but the two
are not the same shape and the gap is not a shipping cost either way:
`null4` puts all 4 sessions on **one** relation's owner core (contending
for the same ascending tail), while `b1`'s arm I spreads 3 sessions across
3 relations on 3 different owner cores (one session per tail, near-zero
per-relation contention). The higher IPS is what less contention predicts,
not a property RD3's invariant makes a claim about; the invariant's
"today" comparison point is a single relation's local path in isolation,
which this file does not isolate from contention in either control, and a
future RD9(a) run should say so explicitly if it needs an apples-to-apples
single-session reading.

Arm S is not what RD3's invariant is about, and this file does not
narrow that invariant either way — it only fixes the "today" arm S will be
compared against once RD5 exists.
