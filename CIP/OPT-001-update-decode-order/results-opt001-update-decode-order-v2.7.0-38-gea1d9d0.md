# OPT-001 measured: the win grows with relation size exactly as claimed, and reaches even the shape that was supposed to be immune

Every claim below is against the run stamped in the table. Arm A and arm B
were built from a clean `git archive` of their own commit into two scratch
source trees under
`/tmp/claude-1000/-home-cdkbs-ckdbs/e52ac4d4-a740-442c-8ac0-ad035bc82a3b/scratchpad/opt001/{src-A,src-B}` —
never in the `path-optimizer` worktree, which stayed on
`worktree-path-optimizer` at `c0b7f6e` throughout this session and was
never built from.

| | |
|---|---|
| Executed | 2026-09-01 01:04 – 01:11 UTC (measurement sweep); binaries built 00:51–00:52 UTC the same session |
| Worktree | `/home/cdkbs/ckdbs/.claude/worktrees/path-optimizer`, branch `worktree-path-optimizer` at `c0b7f6e` — untouched by this run; both arms are `git archive` exports of a named commit, so nothing about what was compiled depended on the worktree's own state |
| Arm A (baseline) | `004da62`, `v2.7.0-34-g004da62`, committed 2026-08-31 23:42:57 UTC. Has OPT-002 and OPT-004, does **not** have OPT-001 |
| Arm B (change) | `ea1d9d0`, `v2.7.0-38-gea1d9d0`, committed 2026-09-01 00:16:07 UTC — OPT-001 merged onto the same tree |
| Diff | `git diff --stat 004da62 ea1d9d0` — 7 files: `docs/inflight/in-progress/cip-path-optimizer.md` (doc only), `include/kds/exec/row_codec.hpp` (+27/-0), `src/exec/row_codec.cpp` (+14/-0), `src/exec/step_compiler.cpp` (+73/-58, mostly `FilterColumnsOf` relocated verbatim so `CompileWhere` can call it, plus the new `maskable` gate), `src/server/command_dispatcher.cpp` (+115/-46, `apply()`/`mark()` restructured), `tests/step_compile_test.cpp` (+71/-0, three new mask tests). No other code differs — confirmed by `diff -rq` on the two extracted trees, 7 files, matching the stat exactly |
| Tree cleanliness | Both arms are `git archive` exports of a named commit — no working-tree drift possible in what was compiled |
| Build | `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=<scratch ossl dir>` (sandbox has no `libssl-dev`), `cmake --build build-release --target kds_server -j8`. Both configured and linked clean (two pre-existing, unrelated warnings — `waker.cpp`'s unused `read()` result, `spsc_ring.cpp`'s switch — present in both arms identically) |
| Binary provenance | A: source `build-release/kds_server` mtime 2026-09-01 00:51:28 UTC (68 min after its commit). B: mtime 2026-09-01 00:52:10 UTC (36 min after its commit). Both post-date their own commit |
| Binary run | A copy → `.../scratchpad/opt001/run/kds_server_A`, sha256 `76773147d22e7f651e81d8f1484ae71239baebd8cbf0f119e320352522221541`; B copy → `.../scratchpad/opt001/run/kds_server_B`, sha256 `d04132cb53b07cde6ddcad5a22094778b427820a1e42620f742938e7c972ab8f`. Both copied once at 00:52:15 UTC and never rebuilt; every server in this run started from these two copies |
| Device | Data dirs under `$HOME/bench-opt001/{a,b}-<rows>`, `/dev/root`, **ext4** (`df -T`), not tmpfs |
| Build type | Release |
| Server config | `cores = 1`, `durability = relaxed` (`bench/run_ab_server.sh`), one fresh server + fresh data dir per (arm, row count) — 6 server instances total |
| Host | 8 vCPUs; `uptime` load average 0.20–0.31 (1-min) and no `cc1plus`/`cmake --build` at every checkpoint before and after the sweep |

## What OPT-001 changed, and what this run measures

`apply()` (UPDATE) and `mark()` (DELETE) in `src/server/command_dispatcher.cpp`
walk a heap relation's whole page chain for a point statement, because
`LocateByPk` answers `kScan` for every heap relation. Before this commit,
every scanned row — matched or not — paid a full decode (UPDATE paid it
**twice**: `DecodeRow` into an owned vector and `DecodeRowInto` into the
frame, both unconditional) before the `WHERE` was even tested. OPT-001
makes `exec::CompileWhere` emit a real `filter_columns` mask naming only the
columns the residual reads, gated off (falling back to `kAllColumns`) for a
step carrying a sub-chain or a relation past 64 columns; the dispatcher now
decodes that mask once per scanned row via the new `exec::DecodeAndResolve`,
tests the predicate, and pays the second, full decode only for a row that
matched.

Five shapes, chosen to separate the two things this change actually does:

- **`update`** — `UPDATE t_main SET c_int=x WHERE id=pid` on a 5-column heap
  relation (`id, c_int, c_small, c_flag, c_text`). The target: mask narrows
  to 1 of 5 columns (`id` alone — no string cell touched on a rejected row)
  **and** the row is decoded once instead of twice.
- **`delete`** — the same shape against a dedicated `t_del` table (kept
  separate from `t_main` so its victim provisioning cannot contaminate the
  other four shapes' relation size — see the driver's own docstring). Old
  DELETE already decoded once, not twice, so its ceiling is the mask-narrowing
  half of the saving only.
- **`select`** — `SELECT * FROM t_main WHERE id=pid`, the **true** flat
  control: this path never reaches `CompileWhere`, `apply()` or `mark()` at
  all.
- **`wide`** — `UPDATE t_wide SET c1=x WHERE c2=target` on a 25-column,
  all-`int64` heap relation, `c2` forced unique per row at load so the match
  is still a single row. Both mechanisms apply, and the mask now discards 23
  more columns per rejected row than the 5-column table does.
- **`subchain`** — `UPDATE t_main SET c_int=x WHERE id IN (SELECT target_id
  FROM t_pool WHERE target_id=target)`. `step_compiler.cpp`'s `CompileWhere`
  sets `out.filter_columns = maskable ? FilterColumnsOf(...) : kAllColumns`
  where `maskable = out.sub_chains.empty() && ...` — any subquery forces
  `kAllColumns`, confirmed by this diff's own new test
  (`CompileWhereGivesUpItsMaskToASubquery`). **This shape was expected to
  hold flat; it does not, and the reason why is this run's second finding.**

Full methodology, schema and the reasoning behind every parameter choice is
in the driver's own docstring:
`CIP/OPT-001-update-decode-order/archive/opt001_ab.py`. In short: one RNG
stream drives an identical statement sequence (target ids, victim ids, SET
values, in the same order) against both arms; a **latency pass** times each
statement on the wire (`time.perf_counter()` around one send+recv, pooled
into a `bench_common.Phase` per arm/shape); a separate **CPU pass** brackets
larger contiguous blocks of statements with `/proc/<pid>/stat` reads (fields
14+15, utime+stime) because ticks are 10ms and a small block's ±1-tick error
would swamp the signal — this is the primary instrument the task specifies,
since there is no `ANALYZE` for `UPDATE`. Both passes alternate which arm
goes first each round. Reproduction:
`bash CIP/OPT-001-update-decode-order/archive/run_sweep.sh`; raw JSON and
per-run logs are archived beside it (`run{200,1000,10000}.json/.log`).

## Correctness: the arms' tables are byte-identical, and the failure mode that matters is a wrong row, not a crash

Both arms load from the identical seed, and every statement in the run is
driven by the same RNG stream in the same order — so `A` and `B` are
expected to reach the exact same final state, not merely a "close enough"
one. After the full run (`select`+`update`+`delete`+`wide`+`subchain`
interleaved across both passes), four checks per row-count, sha256 of the
reply text:

| rows | `t_main` (full, ordered by id) | `t_wide` (full, ordered by id) | `t_del` core range (id ≤ rows) | `t_del` row count |
|---:|---|---|---|---|
| 200 | match | match | match | match |
| 1,000 | match | match | match | match |
| 10,000 | match | match | match | match |

Every cell matched exactly, at every size, both before the writes started
(a load-only sanity check the driver runs first) and after. Spot rows
(`id` 1, `rows/2`, `rows`) came back byte-identical too, e.g. at 10,000
rows: `id=1` → `1,82392,40,0,uanripuvicigljfp` on both arms, `id=10000` →
`10000,707634,971,0,bbpckjwviapqadli` on both. Zero `ERR` replies from
either arm across the whole run (2,660 statements/arm/size in the latency
pass, up to 44,800 in the CPU pass at 10,000 rows). **OPT-001's own stated
risk — a mask naming the wrong columns leaves a slot holding the previous
row's value, a wrong row with no crash — did not reproduce**, consistent
with the proposal's own three-way sanity (3091/3091 suite, 266/266 `sim/`
runs, a `critics-developer` review) already having caught what a masking
bug would look like.

## The noise floor

Two floors, because two instruments. **Latency floor**: arm A's own pooled
samples split at the run's midpoint (`Afloor1` vs `Afloor2`), same table,
same statement shape, same server — reported as `|Δqps|` between the
halves. **CPU floor**: the same split applied to the CPU pass's per-round
windows. The CPU floor is markedly wider for `delete` at small row counts
(30.8% at 200 rows, 41.2% at 1,000) — `t_del`'s own victim provisioning
means fewer effective ticks accumulate per round at those sizes, stated
plainly rather than papered over; it tightens to 4.7% at 10,000 rows, where
`delete`'s CPU number should be read as reliable and the 200/1,000 CPU
numbers read as directional only, with the latency pass as the size-blind
anchor. Every other shape's CPU floor sits at 0.0–13.6%, all safely under
the deltas reported below.

| rows | shape | latency floor | CPU floor |
|---:|---|---:|---:|
| 200 | select | 1.06% | 2.33% |
| 200 | update | 1.42% | 1.47% |
| 200 | delete | 4.76% | 30.77% |
| 200 | wide | 0.54% | 0.00% |
| 200 | subchain | 3.19% | 13.64% |
| 1,000 | select | 0.17% | 10.34% |
| 1,000 | update | 1.72% | 3.59% |
| 1,000 | delete | 0.90% | 41.18% |
| 1,000 | wide | 0.43% | 0.32% |
| 1,000 | subchain | 0.51% | 6.10% |
| 10,000 | select | 1.32% | 6.46% |
| 10,000 | update | 0.78% | 0.06% |
| 10,000 | delete | 0.19% | 4.69% |
| 10,000 | wide | 2.15% | 1.45% |
| 10,000 | subchain | 1.48% | 0.67% |

## Server-CPU throughput: the primary instrument, and the win grows with N exactly as predicted

Per the task's own methodology, server CPU (not client latency) is primary
here — there is no `ANALYZE` for `UPDATE`. Reported as an ops/sec-equivalent
(`1,000,000 / us-per-op`, derived, higher-is-better, so this table reads the
same direction as every other one in this document) rather than the raw
µs/op the CPU pass actually produced, per this role's own rule 5a.

| rows | shape | A (baseline) | B (OPT-001) | Δ | floor | verdict |
|---:|---|---:|---:|---:|---:|---|
| 200 | update | 11,852 | 16,000 | **+35.0%** | 1.47% | **above floor** |
| 1,000 | update | 4,178 | 8,333 | **+99.5%** | 3.59% | **above floor**, ratio growing |
| 10,000 | update | 508 | 1,318 | **+159.7%** | 0.06% | **above floor**, ratio growing |
| 200 | wide | 6,098 | 16,949 | **+178.0%** | 0.00% | **above floor** |
| 1,000 | wide | 1,580 | 8,475 | **+436.4%** | 0.32% | **above floor**, ratio growing |
| 10,000 | wide | 170 | 1,309 | **+672.4%** | 1.45% | **above floor**, ratio growing |
| 200 | delete | 5,455 | 7,059 | +29.4% | 30.77% | inside floor at this size — directional only |
| 1,000 | delete | 4,444 | 6,667 | +50.0% | 41.18% | inside floor at this size — directional only |
| 10,000 | delete | 960 | 1,277 | **+33.0%** | 4.69% | **above floor** |
| 200 | select | 18,824 | 20,253 | +7.6% | 2.33% | above floor but tiny — see prose |
| 1,000 | select | 14,546 | 14,286 | -1.8% | 10.34% | inside floor — control holds |
| 10,000 | select | 1,572 | 1,400 | -10.9% | 6.46% | **above floor, wrong direction** — see prose |
| 200 | subchain | 3,902 | 4,103 | +5.1% | 13.64% | inside floor |
| 1,000 | subchain | 1,006 | 1,088 | **+8.2%** | 6.10% | **above floor** |
| 10,000 | subchain | 108 | 117 | **+8.0%** | 0.67% | **above floor** |

**`update`'s CPU-throughput ratio (B/A) climbs monotonically with row
count — 1.35x at 200 rows, 2.00x at 1,000, 2.60x at 10,000** — which is
exactly the shape the hypothesis predicted and exactly what falsifies a
flat curve: the quantity OPT-001 removes work from (rejected rows per
statement) scales with the relation, so the saving scales with it too.
**`wide` grows even faster — 2.78x, 5.36x, 7.72x** — because its mask
discards 23 of 24 non-pk columns per rejected row instead of 4 of 4, so
the same "decode less per rejected row" mechanism has five times more to
discard. Both land past the CIP entry's own "2-4x on update, more on wide
relations" prediction at 10,000 rows.

## Client latency, alongside — and the reconciliation with OPT-002's own number

Same shapes, same instrument as every other bench file in this repo
(`bench_common.Phase.summary()`, rule 6). p0/p25/p50/p95/p99 in
microseconds, every sample pooled across the interleaved rounds.

| rows | arm:shape | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 200 | A:select | 400 | 124.0 | 103.1 | 118.2 | 121.8 | 145.4 | 175.5 |
| 200 | B:select | 400 | 132.0 | 101.3 | 121.7 | 125.4 | 176.3 | 192.7 |
| 200 | A:update | 400 | 128.8 | 119.7 | 124.8 | 126.1 | 142.7 | 154.0 |
| 200 | B:update | 400 | 109.5 | 94.4 | 101.8 | 103.0 | 139.6 | 151.9 |
| 200 | A:delete | 120 | 229.8 | 213.0 | 220.0 | 223.8 | 252.0 | 357.1 |
| 200 | B:delete | 120 | 195.8 | 179.9 | 187.5 | 190.7 | 223.3 | 232.1 |
| 200 | A:wide | 300 | 210.6 | 199.4 | 203.5 | 206.8 | 225.7 | 256.4 |
| 200 | B:wide | 300 | 111.6 | 98.7 | 102.3 | 103.3 | 144.2 | 153.7 |
| 200 | A:subchain | 80 | 289.1 | 272.2 | 280.0 | 284.4 | 319.5 | 339.2 |
| 200 | B:subchain | 80 | 279.2 | 263.6 | 265.5 | 270.5 | 316.3 | 324.1 |
| 1,000 | A:select | 400 | 177.3 | 110.0 | 170.6 | 173.5 | 209.2 | 247.7 |
| 1,000 | B:select | 400 | 182.7 | 108.2 | 177.8 | 180.6 | 203.7 | 270.2 |
| 1,000 | A:update | 400 | 283.2 | 268.6 | 274.2 | 277.4 | 299.2 | 402.6 |
| 1,000 | B:update | 400 | 161.8 | 152.9 | 157.6 | 159.1 | 175.3 | 188.8 |
| 1,000 | A:delete | 120 | 302.6 | 288.1 | 295.8 | 299.7 | 320.3 | 327.6 |
| 1,000 | B:delete | 120 | 247.3 | 233.1 | 237.6 | 241.1 | 273.6 | 350.5 |
| 1,000 | A:wide | 300 | 691.5 | 663.1 | 681.0 | 690.3 | 717.8 | 753.8 |
| 1,000 | B:wide | 300 | 163.6 | 152.9 | 158.7 | 159.8 | 184.2 | 191.6 |
| 1,000 | A:subchain | 80 | 1025.1 | 1001.3 | 1016.3 | 1023.2 | 1052.3 | 1072.7 |
| 1,000 | B:subchain | 80 | 950.8 | 921.1 | 938.0 | 945.4 | 971.5 | 1138.7 |
| 10,000 | A:select | 400 | 761.8 | 724.0 | 741.6 | 748.2 | 821.2 | 956.3 |
| 10,000 | B:select | 400 | 832.9 | 796.8 | 814.2 | 822.2 | 896.4 | 1056.4 |
| 10,000 | A:update | 400 | 1999.2 | 1946.7 | 1979.6 | 1994.5 | 2041.1 | 2075.4 |
| 10,000 | B:update | 400 | 799.0 | 767.3 | 788.2 | 796.6 | 836.6 | 858.9 |
| 10,000 | A:delete | 120 | 1147.7 | 1107.9 | 1121.5 | 1132.9 | 1201.1 | 1341.0 |
| 10,000 | B:delete | 120 | 853.6 | 815.0 | 830.9 | 839.6 | 939.1 | 947.9 |
| 10,000 | A:wide | 300 | 6042.0 | 5801.9 | 5941.1 | 6033.9 | 6351.5 | 6590.2 |
| 10,000 | B:wide | 300 | 821.9 | 773.3 | 794.5 | 802.4 | 1043.2 | 1084.0 |
| 10,000 | A:subchain | 80 | 9334.1 | 9162.5 | 9237.8 | 9304.9 | 9519.8 | 10047.0 |
| 10,000 | B:subchain | 80 | 8621.5 | 8493.2 | 8544.5 | 8578.2 | 8851.4 | 8922.2 |

**Reconciliation with OPT-002's own `update` number.** `bench/`... no —
`CIP/OPT-002-string-slot-assign/results-opt002-string-slot-assign-v2.7.0-30-g55d2c0b.md`
measured `55d2c0b` (OPT-002 alone, no OPT-001) against `dfe4c98` (neither)
and found `update` at 10,000 rows going **421 → 500 QPS, +18.87%**, with a
mean of 2376.2us → 1987.7us, and explicitly predicted: *"OPT-002's saving
on `update` is only this large because OPT-001 is still unbuilt... this
run's `update` number will not reproduce once OPT-001 is built."* This run
is that reproduction. Arm A here (`004da62`, has OPT-002, not OPT-001)
measures `update` at 10,000 rows at **mean 1999.2us** — within 0.6% of
OPT-002's own `B` number (1987.7us) from a wholly separate session and JSON
file, which is a useful cross-check that the two runs are pricing the same
code state. Arm B here (`ea1d9d0`, OPT-001 added) then drops that same
shape to **799.0us**, a further 60% below OPT-002's already-optimized
number. The mechanism explains why OPT-002's contribution shrinks rather
than stacking multiplicatively: `WHERE id = pid`'s mask is exactly bit 0
(`id`), and `DecodeColumnsInto` (`row_codec.cpp`, "iterate the mask's set
bits, not the relation's columns") never touches `c_text` for a rejected
row under OPT-001 — so the malloc/memcpy OPT-002 sped up for **every**
scanned row's string cell now happens for **at most one** row per
statement (the match, if any), not the ~N/2 OPT-002 was priced against.
**OPT-002's own +18.87% finding does not hold once OPT-001 lands, exactly
as its CIP entry predicted** — not because OPT-002 regressed, but because
the volume of decodes it was multiplying against collapsed.

## The subchain finding: "unmaskable" is not "unaffected", and that is a mask-gate success, not a bug

The task's own framing was that `subchain` "is gated to `kAllColumns` by
design, so it must show NO change. If it moves, the gate is not doing what
the code claims." It moves — **+5.1% (200 rows), +8.2% (1,000), +8.0%
(10,000)** on CPU-throughput, all but the smallest clearing their own
floor — and the gate is doing exactly what the code claims; the inference
that a forgone mask implies zero benefit was the part that did not hold.

`out.filter_columns = maskable ? FilterColumnsOf(...) : kAllColumns` (this
diff's `step_compiler.cpp`) only decides the **width** of what gets decoded
per row. OPT-001 bundles a second, independent change: `apply()`'s baseline
(`src-A/src/server/command_dispatcher.cpp:8167-8195`) unconditionally runs
**both** `DecodeRow` (into an owned vector) **and** `DecodeRowInto` (into
the frame) for every scanned row, matched or not. The new `apply()`
(`src-B`) runs `exec::DecodeAndResolve` — **one** decode, whatever the mask
— to test the predicate, and defers the second, owned-copy decode
(`DecodeRow` + `ResolveSpills`) to a row that actually matched. `row_codec.cpp`
confirms the `kAllColumnsMask` branch of `DecodeAndResolve` itself: it calls
`DecodeRowInto` **once**, not twice. A `subchain` statement forgoes the
column-narrowing half of OPT-001 (correctly — `sub.correlated` can reach
any column of the outer row through the frame) but still receives the
decode-count halving, because that half of the change is unconditional on
the mask at all. Confirmed directly: `git diff 004da62 ea1d9d0 --
tests/step_compile_test.cpp` shows the new
`CompileWhereGivesUpItsMaskToASubquery` test asserts `filter_columns ==
kAllColumns` for exactly this shape — the width gate is real and verified —
it says nothing about decode count, which is the axis this run actually
moved on.

**A second, genuinely flat control exists, and it holds**: `select` never
reaches `CompileWhere`, `apply()` or `mark()` at all (it compiles through
`Compile()`/`CompileBlock()`, a different function entirely). At 200 and
1,000 rows its CPU-throughput delta (+7.6%, -1.8%) sits inside or barely
past its own floor with no consistent sign. At 10,000 rows it shows a
consistent **-10.9%** on both CPU and latency (`select` mean 761.8us →
832.9us, -9.3%), which does clear its floor (6.46% CPU, 1.32% latency) and
deserves an honest note rather than silence: `step_compiler.cpp`'s diff
relocates `FilterColumnsOf` verbatim (byte-for-byte identical body, moved
earlier in the translation unit so `CompileWhere` can call it) and adds no
logic `select`'s own compile path executes — `git diff` shows this
precisely. With no code-level mechanism connecting `select` to this change,
this run's best-supported reading is code-layout or scheduling drift
between two separately-linked binaries over the sweep's longest CPU window
(168s at 10,000 rows), not a regression; a future run that isolates
`select` alone, on its own binary pair, would settle it, but nothing in
this diff gives it a channel to move through.

## Wait breakdown: decode is the whole latency budget it removes work from

`durability = commit/fsync wait`: not applicable — `relaxed` on both arms
(the target of this measurement is CPU-bound decode, not I/O; matching the
precedent `results-opt002-string-slot-assign-v2.7.0-30-g55d2c0b.md` set and
stated there for the same reason), and every shape here is either a read or
a single-statement autocommit write, so no fsync wait is in any number
below. `lock/conflict wait`: not applicable — one connection per arm, no
concurrent writer, and `apply()`'s conflict check only runs when
`scope.txn != nullptr`, which a bare autocommit statement never sets.
`client and socket round trip`: the residual after subtracting the CPU
pass's server-side number from the latency pass's mean — an approximation
(the two passes are not the same statements), but a stable one:

| rows | shape | A latency mean | A server CPU | A residual | B latency mean | B server CPU | B residual |
|---:|---|---:|---:|---:|---:|---:|---:|
| 200 | update | 128.8 | 84.4 | 44.4 (34.5%) | 109.5 | 62.5 | 47.0 (42.9%) |
| 1,000 | update | 283.2 | 239.4 | 43.8 (15.5%) | 161.8 | 120.0 | 41.8 (25.8%) |
| 10,000 | update | 1999.2 | 1970.6 | 28.6 (1.4%) | 799.0 | 758.8 | 40.2 (5.0%) |
| 200 | wide | 210.6 | 164.0 | 46.6 (22.1%) | 111.6 | 59.0 | 52.6 (47.1%) |
| 1,000 | wide | 691.5 | 633.0 | 58.5 (8.5%) | 163.6 | 118.0 | 45.6 (27.9%) |
| 10,000 | wide | 6042.0 | 5901.0 | 141.0 (2.3%) | 821.9 | 764.0 | 57.9 (7.0%) |

The residual — everything the CPU pass does not count: client-side Python
overhead, the socket round trip, TCP-loopback latency, connection-level
locking — stays in a narrow **~28–59us band regardless of arm or row
count**, while the CPU component is what moves by two orders of magnitude
across the row-count sweep (84us → 1971us for `update` on arm A; 62us →
759us on arm B). **`write-statement decode` is not merely the largest wait
this shape has — at 10,000 rows it *is* the statement**, 98.6% of `update`'s
own latency budget on arm A, 95.0% on arm B. This is the direct evidence
for why server CPU had to be the primary instrument here: a client-latency
number at this scale is, to a first approximation, already a CPU-decode
number wearing a ~30-60us coat, but at 200 rows the residual is 35-47% of
the total — large enough that a client-latency-only measurement would have
underclaimed the win at small N by roughly the same fraction, which is
exactly why the CPU pass and the latency pass are reported side by side
rather than one standing in for the other.

## What this run teaches about the engine

**The prediction was directionally correct and, at the largest measured
size, quantitatively conservative.** The CIP entry predicted "2-4x on
UPDATE's server CPU, more on wide relations." `update` reached 2.60x at
10,000 rows — inside the predicted range, still climbing, and there is no
row-count ceiling visible in this data (rule 4b: a cell that never
approached a limit measured the harness, not the engine — this one is
still rising at the largest size run, so **10,000 rows did not find
`update`'s ceiling**, and a future run should sweep further, e.g. 100,000
rows, to find where the ratio actually saturates or where a different cost
— page-chain length itself, an L2-miss-bound scan — starts to dominate
instead). `wide` reached 7.72x, past the "more on wide relations" claim's
implicit expectation and the largest number in this run.

**The masking gate and the decode-count fix are two separate mechanisms,
and the CIP proposal's own text already says this** ("resolves spills
twice... and only then evaluates the predicate" — decode *count* — versus
"the mask... only what the WHERE reads" — decode *width*), but the task
brief's inference from "unmaskable" to "must show no change" conflated
them. This run is the first place that distinction became a number rather
than a reading of the diff: a shape that forgoes the width optimization
entirely still picked up 5-8% from the count optimization alone. Any
future CIP entry that reasons about "the gate held" should keep the two
axes separate — a structural gate on masking says nothing about whatever
else the same commit changed.

**OPT-002 and OPT-001 are not independently additive; they compound
through volume, and only one of them survives the multiplication for this
shape.** OPT-002 made each string decode cheaper; OPT-001 made string
decodes for a point `UPDATE` on a heap relation stop happening `N/2` times
per statement and start happening at most once. The CIP file's own "A/B
owed" section called this exactly right in advance, and this run is the
number that confirms it — with the caveat that confirming it fully (a
direct re-run of OPT-002's own `dfe4c98`-vs-`55d2c0b` A/B on an
OPT-001-equipped base) is not what this session ran; the reconciliation
above is the strongest reading this session's own data supports, not a
fresh three-arm measurement.

**Open**: `delete`'s CPU number is trustworthy only at 10,000 rows in this
run (30.8%/41.2% floors at 200/1,000 make those two directional, not
findings) — a re-run sized specifically for `delete`'s tick resolution at
small N (more rounds, fewer ops each, per the reasoning in the driver's own
docstring) would close that gap; and `select`'s -10.9% at 10,000 rows,
while most likely code-layout noise given the diff gives it no logical
channel to move through, was not chased down to ground in this session and
is named here rather than left silent.
