# AM-S1 `cores = 1` A/B — the page latch compile-out

`tools/scenario0_stockmarket.py` at `cores = 1`, `group` and `strict`
durability, comparing A (the parent engine, `92cb654`, byte-identical to
`a68dbc3`) against B (`c985d37`, the commit AM-S1's page latch landed on).
This is AM-S1's own definition of done, second half
(`instructions/v3.0.0/workorder-am-m1-shared-pool.md` line 224): "a
`cores = 1` A/B showing the acquire/release pair costs nothing
measurable." The primitive itself — `include/kds/storage/page_latch.hpp`,
a `uint32` word per frame driven by `std::atomic_ref` CAS — is armed only
when `core_count > 1` (`SetLatchArmed(core_count > 1)` in
`src/server/expeditor.cpp:825` and `src/server/core_runtime.cpp:208`), so
at `cores = 1` the store never touches the word. **What this document
prices is therefore not the latch — it is the compile-out**: the
`latch_armed_` branch taken and found false on every pin and unpin
(`src/storage/device_page_store.cpp`), the `PinMode` argument now carried
through the virtual `PinFrame(PageId, PinMode)`
(`include/kds/storage/page_store.hpp:205`), the unchanged 32-byte `Frame`
layout (`static_assert(sizeof(Frame) == 32, ...)`,
`include/kds/storage/device_page_store.hpp:827`), and the one unpin the
armed census moved *earlier* on each leaf write-descent
(`src/storage/btree/btree.cpp:103` and
`src/storage/index/index_tree.cpp:87`: the read handle is released before
the leaf's write re-fetch instead of at scope exit, to keep the latch's
never-upgraded rule true under contention — the added line is a single
`Release()`, so the pin and unpin *counts* are unchanged, only their
order). **The armed cost — the CAS pair itself, and contention at
`cores > 1` — is not measured here.** It has only the unit-level
`PageLatch.EightThreadsNeverShareAFrameExclusivelyAndTheCountsBalance`
cell (`tests/page_latch_test.cpp:188`) behind it today; the multi-core
number is AM-S6's, against AL-S8's own baseline, and does not exist yet.
This document is deliberately not at the concurrency extreme rule 4b
asks for (**every numbered "rule" in this file is a documentation rule
from `.claude/agents/ck-tester.md`, not one of `bench/README.md`'s five
run-validity rules** — those five are satisfied and shown in §1: release
build at the measured commit, a named block device, a hashed binary
copy per arm, per-cell load and `pgrep`, and chosen ports) — that is
AM-S6's cell by design, and AM-S1's own definition of
done calls for exactly the quiet end instead: proving the unarmed path
costs nothing *before* asking what the armed path costs under load. The
durability axis is swept in every arm, but not to both ends: the engine
has three classes (`strict`/`group`/`relaxed` = D1/D2/D3,
`include/kds/wal/manager.hpp:87`), and this run covers the strict end and
the `group` default. **`relaxed` — the fast end, where a fixed per-pin
cost would show up least buried — was not run.**

**The result: nothing measurable.** Both durability classes show a B−A
delta that sits inside this run's own clean-cell run-to-run spread, and
the four matched same-pass A/B pairs that survive (§7) do not even agree
on which arm is faster. §8 reads this against this engine's own last
numbers for the shape; §11 draws out what is, and is not, established by
that.

## 1. Stamp

| Field | Value |
|---|---|
| Date/time | 2026-09-03, 13:15:25–13:23:08 UTC (three passes; per-cell times in §2) |
| Worktree | `ar2-borrow-model` (branch `worktree-ar2-borrow-model`) |
| Commits measured | A: `92cb654` (byte-identical to `a68dbc3`, verified empty `git diff --stat 92cb654 a68dbc3 -- src include tools CMakeLists.txt`); B: `c985d37`, `git describe --tags` = `v2.7.0-183-gc985d37` |
| Tree cleanliness | **No engine file is modified**: `git status --short` in this worktree shows one modified tracked file — `instructions/v3.0.0/workorder-am-m1-shared-pool.md`, whose AM-6 `AM-S1 A/B` row this run's outcome is written into — and two untracked paths, this document and its archive directory. Nothing under `src`, `include` or `tools` is touched, so the tree the measured binary was built from is `c985d37` exactly. |
| Binary provenance, A | `/home/cdkbs/bench-runs/am-s1-page-latch/kds_server_A`, 6,229,408 bytes, `sha256 d6b2c4202a929e545bace8d570cd6fec42640a58461536d4c4ab3709e3872f52` (re-hashed this session; matches). Same copy `results-ar2-c1-colocation-v2.7.0-178-g92cb654.md` §1 measured — identical hash, so it is literally the same binary, not merely the same source commit. |
| Binary provenance, B | `/home/cdkbs/bench-runs/am-s1-page-latch/kds_server_B`, 6,229,848 bytes, `sha256 6724bbb4094bab35c208a6b5fe60bc8dd7b2a5fc361ede870db8a823df979d88` (re-hashed this session; matches). Source `build-release/kds_server` mtime `2026-09-03 11:12:14 UTC`; commit `c985d37`'s own timestamp is `11:11:43 UTC` — **the binary postdates the commit it measures by 31 s**, the correct order. |
| Engine delta A→B | `git diff --stat a68dbc3 c985d37 -- src include tools CMakeLists.txt` (verified this session): 8 files, **535 insertions(+), 14 deletions(-)**. Per file, from `--numstat` (+ins/−del): `include/kds/storage/device_page_store.hpp` +128/−5, `include/kds/storage/page_latch.hpp` (new) +246, `include/kds/storage/page_store.hpp` +25/−8, `src/server/core_runtime.cpp` +7, `src/server/expeditor.cpp` +10, `src/storage/btree/btree.cpp` +9, `src/storage/device_page_store.cpp` +102/−1, `src/storage/index/index_tree.cpp` +8. `tools/` and `CMakeLists.txt` untouched. |
| Device | `/home/cdkbs/bench-runs/am-s1-page-latch`, `ext4`, `/dev/root` (`df -T`, recorded per cell; 49% used throughout — a real block device, not tmpfs). |
| Build type | `build-release` (Release; `CMAKE_BUILD_TYPE=Release`, `CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG`, `KDS_WITH_TLS=ON` per `CMakeCache.txt`). |
| Host | 8 logical CPUs (`nproc`), Azure VM, `Linux 6.17.0-1022-azure`. |
| Ports | 15610–15621. `pgrep -a -f "cc1plus|cmake --build|ctest|kds_server"` in every cell's precheck shows only the unrelated resident `kds_server` (pid 899, port 15432, `/home/cdkbs/autotrade/`) and the orchestrator's own process tree — no concurrent build in any cell. |
| Server config (common) | `cores = 1`, `peer_listeners = off`, `placement = namespace`, `checkpoint_interval_ms = 5000`, `auth = off`, `tls = off`, `log_level = warn`. Varied: `durability` (`group` / `strict`). |
| Driver | `tools/scenario0_stockmarket.py`, unmodified, AL-S8's scenario-0 arguments verbatim: `--users 100 --accounts-per-user 3 --assets 30 --traders 8 --txn-per-user 50 --verify 200 --seed 1 --sync` — a work target of 5,000 committed business transactions (2 `INSERT trades` + 2 `UPDATE accounts` each), split 625 per trader. |

## 2. What was run, and in what order

Twelve cells, three passes (`run-log.txt` timestamps; `run.json`'s own
`passes` list mis-records pass 2's `started` as pass 1's — an artifact of
`run_ab.py:172` reloading the previous `run.json` and appending to it,
which is what carries the run's original `started` into each later
entry, not a second run — see the archive README). Pass 2 was added because `g1-B`
came back degraded, leaving `group` with only one clean `B` cell; pass 3
because both `g3` cells also came back degraded. Interleaved A/B
throughout: within each two-cell pair the arms alternate, and each
durability family's starting arm reverses pair to pair (group: `g1`
A-first, `g2` B-first, `g3` A-first, `g4` B-first; strict: `s1` A-first,
`s2` B-first). The same arm does run twice in a row three times, at the
seams between pairs where adjacency was not controlled for — `s1-B`
(13:16:44) into `g2-B` (13:17:15), `s2-A` (13:18:08) into `g3-A`
(13:20:47), and `g3-B` (13:21:09) into `g4-B` (13:22:45), all visible in
the run order below.

*Corrected on `ar2-borrow-model` after `c40b3cc` (archive re-read
2026-09-04): this sentence originally read "no arm run twice in a row."
`run-log.txt`'s own arm sequence is A B A B B A B A A B B A — alternation
holds inside every pair and the starting arm reverses pair to pair
within each durability family, exactly as stated above, but the same
arm does repeat at three pair boundaries, named above.*

| Cell | Arm | Port | Durability | Start (UTC) | Work window (s) | Pre-load (1 min) | Post-load | `recovery_checkpoint_us` | State |
|---|---|---|---|---|---|---|---|---|---|
| `g1-A` | A | 15610 | group | 13:15:25 | 7.5 | 0.15 | 0.21 | 6,093 | clean |
| `g1-B` | B | 15611 | group | 13:15:37 | 25.5 | 0.20 | 0.58 | **434,131** | **degraded** |
| `s1-A` | A | 15612 | strict | 13:16:11 | 26.8 | 0.54 | 0.79 | 5,421 | clean |
| `s1-B` | B | 15613 | strict | 13:16:44 | 25.7 | 0.79 | 0.93 | 5,403 | clean |
| `g2-B` | B | 15614 | group | 13:17:15 | 7.0 | 0.85 | 0.87 | 7,208 | clean |
| `g2-A` | A | 15615 | group | 13:17:26 | 6.8 | 0.80 | 0.81 | 5,465 | clean |
| `s2-B` | B | 15616 | strict | 13:17:37 | 25.9 | 0.75 | 0.92 | 5,216 | clean |
| `s2-A` | A | 15617 | strict | 13:18:08 | 26.1 | 0.92 | 1.02 | 7,433 | clean |
| `g3-A` | A | 15618 | group | 13:20:47 | 14.8 | 0.11 | 0.43 | **167,931** | **degraded** |
| `g3-B` | B | 15619 | group | 13:21:09 | 9.5 | 0.43 | 0.62 | **60,931** | **degraded** |
| `g4-B` | B | 15620 | group | 13:22:45 | 6.9 | 0.25 | 0.31 | 5,334 | clean |
| `g4-A` | A | 15621 | group | 13:22:56 | 6.8 | 0.29 | 0.34 | 5,199 | clean |

Between passes 2 and 3, at 13:22:27 UTC a device probe (40×4 KiB
`pwrite`+`fdatasync` into the run directory, loadavg 0.27) was run
outside the orchestrator to check whether the device had recovered:
p50 1.50 ms, p90 3.29 ms, max 9.70 ms, min 1.32 ms. That probe was not
captured to a file and is recorded here only as a stated fact carried
over from the run's own operator record — it is not independently
re-verifiable from this archive, unlike every number elsewhere in this
document.

Every cell: fresh data file, fresh server started from the arm's hashed
binary copy, precheck (`/proc/loadavg`, `pgrep`, `df -T`) before the
server started, the driver, `SHOW META`, `SIGTERM`. **"Work window" is
the driver's own measured span** (`<cell>.json`'s `meta.seconds`, the
denominator of every TPS below), not the cell's wall clock — the whole
cell, including bootstrap and stop, runs 0.7–3.7 s longer
(`<cell>.cell.json`'s `driver.seconds`, and `run-log.txt`'s start/done
pair).

## 3. Headline: TPS and statements/sec (rule 5a)

| Cell | Arm | Durability | TPS | statements/sec | State |
|---|---|---|---|---|---|
| `g1-A` | A | group | 668.4 | 2,673.8 | clean |
| `g1-B` | B | group | 196.3 | 785.3 | degraded |
| `s1-A` | A | strict | 186.8 | 747.0 | clean |
| `s1-B` | B | strict | 194.2 | 776.8 | clean |
| `g2-B` | B | group | 711.9 | 2,847.4 | clean |
| `g2-A` | A | group | 731.1 | 2,924.4 | clean |
| `s2-B` | B | strict | 193.3 | 773.1 | clean |
| `s2-A` | A | strict | 191.6 | 766.4 | clean |
| `g3-A` | A | group | 338.0 | 1,352.1 | degraded |
| `g3-B` | B | group | 527.4 | 2,109.5 | degraded |
| `g4-B` | B | group | 723.9 | 2,895.2 | clean |
| `g4-A` | A | group | 732.6 | 2,930.4 | clean |

**Clean-pair means, and the A−B delta beside each arm's own spread:**

| Durability | Arm | n (clean) | Mean TPS | Mean stmt/s | Own spread (max−min)/min |
|---|---|---|---|---|---|
| group | A | 3 (`g1-A`, `g2-A`, `g4-A`) | 710.7 | 2,842.9 | **9.6%** |
| group | B | 2 (`g2-B`, `g4-B`) | 717.9 | 2,871.3 | 1.7% |
| strict | A | 2 (`s1-A`, `s2-A`) | 189.2 | 756.7 | 2.6% |
| strict | B | 2 (`s1-B`, `s2-B`) | 193.7 | 775.0 | 0.5% |

| Durability | B − A (TPS) | B vs A | Compare against A's own spread |
|---|---|---|---|
| group | +7.1 | **+1.0%** | inside A's 9.6% |
| strict | +4.6 | **+2.4%** | roughly the size of A's own 2.6% |

Both deltas are the same sign (B slightly faster) and both are at or
inside the spread this run's own repeated cells already show for one
arm alone. Neither reads as a finding; §7 develops why, including the
one place (group A) where the "own spread" number is doing more work
than a two-cell floor normally would.

**Both readings above are cell-wise: they exclude the three degraded
*cells* (`g1-B`, `g3-A`, `g3-B`) but keep a degraded cell's clean
*partner* in the arm mean whenever that partner is itself clean. Only
`g1` has such a partner — `g1-B` is degraded and dropped, but `g1-A` is
clean and stays in the arm-A mean; `g3`'s own partner, `g3-A`, is
independently degraded too, so that pair is already excluded whole. This
document's own
noise-floor rule is to repeat the *pair*, not the cell — a device stall
on one arm of a pair says nothing about whether its partner's result
would have looked the same on a quiet host, so the honest exclusion unit
is the pair, not the individual cell.** Applying that: `g1` (both
`g1-A` and `g1-B`) and `g3` (both `g3-A` and `g3-B`) drop entirely,
leaving group A with the same two cells as group B (`g2-A`/`g4-A` vs
`g2-B`/`g4-B`, n=2 each) instead of three vs two; strict is unaffected
(both its pairs were already clean).

| Durability | Arm | n (pair-wise clean) | Mean TPS | Own spread (max−min)/min |
|---|---|---|---|---|
| group | A | 2 (`g2-A`, `g4-A`) | 731.9 | 0.2% |
| group | B | 2 (`g2-B`, `g4-B`) | 717.9 | 1.7% |
| strict | A | 2 (`s1-A`, `s2-A`) | 189.2 | 2.6% |
| strict | B | 2 (`s1-B`, `s2-B`) | 193.7 | 0.5% |

| Durability | B − A (TPS, pair-wise) | B vs A |
|---|---|---|
| group | −14.0 | **−1.9%** |
| strict | +4.6 | **+2.4%** |

**Under pair-wise exclusion, group flips sign: A is faster by 1.9%, not
B faster by 1.0%.** This is the same direction as both matched group
pairs in §7 (`g2`: A by 2.7%; `g4`: A by 1.2%) — unsurprising, since
dropping the unpaired `g1-A` is what was pulling the cell-wise A mean
down into a tie with B in the first place (§7 traces `g1-A`'s own TPS as
the outlier driving group A's 9.6% cell-wise spread). Strict is
unchanged because neither of its two pairs lost a partner. Read as: at
`cores = 1`, the compile-out's bounded cost is **−1.9% (group, A ahead,
matching both group pairs) and +2.4% (strict, B ahead, matching both
strict pairs)** — opposite signs across the two durability classes,
each within a few percent, not the "B inside A's own spread" reading
the cell-wise table above gives on its own. §7 carries the pair-level
evidence this rests on; §11 folds it into the run's overall reading.

*Added on `ar2-borrow-model` after `c40b3cc` (archive re-read
2026-09-04). The cell-wise table and its "+1.0%"/"inside A's 9.6%"
reading above are the run's own first report and are left as measured —
they are not wrong, only wider than the exclusion this document's own
rules call for once the pair-wise treatment already given to `g3` (both
cells dropped) is applied to `g1` too. Both readings
now stand: the numbers agree with each other about where the noise floor
is, and disagree about which arm they say is ahead.*

## 4. Percentiles (rule 6: p0/p25/p50/p95/p99/max, every row)

All values from `<cell>.json`'s `phases` (`bench_common.Phase.summary()`),
µs unless noted. **Degraded cells are shown for completeness and marked;
they are excluded from every comparison in §3 and §8.**

### `txn` — 2×`INSERT trades` + 2×`UPDATE accounts`, one client round trip each; `qps` here is the TPS row above

| Cell | Arm | Dur | ops | p0 | p25 | p50 | p95 | p99 | max | State |
|---|---|---|---|---|---|---|---|---|---|---|
| `g1-A` | A | group | 5,000 | 8,406.5 | 10,797.2 | 11,173.5 | 14,267.3 | 20,987.5 | 133,745.6 | clean |
| `g1-B` | B | group | 5,000 | 9,066.8 | 10,576.6 | 11,074.9 | 189,952.6 | 572,507.6 | 1,276,437.4 | **degraded** |
| `s1-A` | A | strict | 5,000 | 4,580.3 | 35,032.1 | 37,891.9 | 74,520.7 | 88,167.7 | 198,507.6 | clean |
| `s1-B` | B | strict | 5,000 | 4,458.1 | 34,238.4 | 37,172.9 | 71,358.4 | 80,392.7 | 114,421.0 | clean |
| `g2-B` | B | group | 5,000 | 8,214.9 | 10,486.4 | 10,840.1 | 13,632.4 | 19,128.8 | 28,910.6 | clean |
| `g2-A` | A | group | 5,000 | 8,932.5 | 10,256.9 | 10,575.7 | 13,254.8 | 17,860.7 | 24,132.9 | clean |
| `s2-B` | B | strict | 5,000 | 4,644.7 | 34,620.3 | 37,267.0 | 72,485.4 | 80,652.0 | 113,027.4 | clean |
| `s2-A` | A | strict | 5,000 | 4,618.5 | 35,139.5 | 37,724.6 | 73,445.6 | 83,166.2 | 112,965.7 | clean |
| `g3-A` | A | group | 5,000 | 8,872.1 | 10,932.8 | 11,315.5 | 65,372.6 | 343,283.2 | 754,687.4 | **degraded** |
| `g3-B` | B | group | 5,000 | 8,780.5 | 10,489.8 | 10,804.1 | 16,212.0 | 121,341.6 | 770,824.3 | **degraded** |
| `g4-B` | B | group | 5,000 | 8,294.4 | 10,250.4 | 10,643.5 | 13,467.4 | 20,618.2 | 26,650.3 | clean |
| `g4-A` | A | group | 5,000 | 8,313.5 | 10,162.7 | 10,503.5 | 13,337.1 | 17,971.0 | 29,569.3 | clean |

The degraded cells' bodies are normal — `g3-A`'s p25 (10,932.8) and
`g1-B`'s p25 (10,576.6) sit inside the clean group range (10,162.7–
10,797.2) — and only their p95/p99/max are out, by one to three orders
of magnitude; §7 carries the evidence and the reading.

### `trade-insert` (`INSERT`) and `account-update` (`UPDATE`) — both WAL-logged

| Cell / phase | ops | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| `g1-A` trade-insert | 10,000 | 1,438.0 | 2,649.4 | 2,757.9 | 3,362.8 | 6,489.8 | 125,176.8 |
| `g1-A` account-update | 10,000 | 2,342.1 | 2,652.8 | 2,766.3 | 3,382.3 | 6,554.3 | 125,175.3 |
| `g1-B` trade-insert | 10,000 | 1,549.5 | 2,579.8 | 2,697.8 | 21,191.2 | 209,690.4 | 882,283.4 |
| `g1-B` account-update | 10,000 | 2,245.7 | 2,581.3 | 2,700.0 | 26,956.9 | 219,744.6 | 882,260.2 |
| `s1-A` trade-insert | 10,000 | 1,116.1 | 8,483.1 | 9,318.0 | 18,448.7 | 23,381.9 | 138,811.7 |
| `s1-A` account-update | 10,000 | 1,120.9 | 8,479.9 | 9,326.9 | 18,276.2 | 22,174.2 | 140,410.9 |
| `s1-B` trade-insert | 10,000 | 1,074.7 | 8,328.9 | 9,149.6 | 17,872.4 | 21,033.0 | 50,296.0 |
| `s1-B` account-update | 10,000 | 1,067.3 | 8,334.1 | 9,172.2 | 17,785.2 | 20,905.5 | 41,142.0 |
| `g2-B` trade-insert | 10,000 | 1,681.1 | 2,562.0 | 2,662.6 | 3,316.6 | 5,453.0 | 20,457.2 |
| `g2-B` account-update | 10,000 | 1,661.0 | 2,573.3 | 2,673.6 | 3,406.4 | 5,458.4 | 20,596.6 |
| `g2-A` trade-insert | 10,000 | 1,479.4 | 2,517.5 | 2,602.1 | 3,166.2 | 5,478.1 | 16,014.9 |
| `g2-A` account-update | 10,000 | 2,233.6 | 2,522.5 | 2,607.8 | 3,154.0 | 5,594.6 | 16,023.6 |
| `s2-B` trade-insert | 10,000 | 1,132.6 | 8,346.9 | 9,188.0 | 17,720.3 | 20,696.9 | 42,137.8 |
| `s2-B` account-update | 10,000 | 1,095.5 | 8,366.8 | 9,189.3 | 17,804.3 | 21,143.4 | 51,760.2 |
| `s2-A` trade-insert | 10,000 | 1,096.7 | 8,451.2 | 9,323.6 | 18,300.0 | 21,582.5 | 45,419.3 |
| `s2-A` account-update | 10,000 | 1,085.6 | 8,442.1 | 9,294.3 | 18,203.8 | 21,758.6 | 44,101.4 |
| `g3-A` trade-insert | 10,000 | 1,491.6 | 2,666.5 | 2,790.6 | 3,632.1 | 89,712.5 | 745,883.7 |
| `g3-A` account-update | 10,000 | 2,384.8 | 2,675.0 | 2,795.3 | 3,657.7 | 100,964.7 | 745,871.7 |
| `g3-B` trade-insert | 10,000 | 1,536.8 | 2,569.6 | 2,655.8 | 3,259.2 | 10,774.8 | 762,355.6 |
| `g3-B` account-update | 10,000 | 2,286.4 | 2,575.2 | 2,662.7 | 3,288.7 | 11,682.2 | 762,326.9 |
| `g4-B` trade-insert | 10,000 | 1,439.7 | 2,512.4 | 2,616.4 | 3,234.5 | 6,077.1 | 15,979.9 |
| `g4-B` account-update | 10,000 | 2,219.0 | 2,517.5 | 2,620.8 | 3,263.9 | 5,664.9 | 15,969.0 |
| `g4-A` trade-insert | 10,000 | 1,444.2 | 2,488.0 | 2,586.8 | 3,217.2 | 5,469.4 | 17,196.4 |
| `g4-A` account-update | 10,000 | 2,242.4 | 2,491.8 | 2,592.5 | 3,245.2 | 5,577.3 | 17,211.5 |

**`trade-insert` and `account-update` track within a few percent of each
other in every clean cell**, matching AL-S8's own scenario0 document
(§4 there): the commit's durability wait, not the row mutation,
dominates both statements' client-perceived cost about equally.

**A correction to that prior document, carried here so the number is not
re-read wrongly**: AL-S8's §4 attributed the agreement to one statement
being logged and the other not (`account-update` "page-only, unlogged"),
and that file now carries the correction in the same commit as this one.
The claim was false, and already false at AL-S8's own `f6ed10c` — an
`UPDATE` appends `HEAP_OVERWRITE` like every other data mutation
(`src/server/command_dispatcher.cpp:10134`,
`include/kds/wal/record.hpp:55`; `CLAUDE.md`'s WAL row states the rule).
The two phases agree because they are the *same* shape — one logged row
mutation each under its own autocommit envelope — so their agreement says
the commit wait dominates, and says nothing at all about the cost of
logging.

### `profit-scan` (FilterScan, unindexed `WHERE user_id = <n>`) and `profit-insert`

| Cell / phase | ops | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| `g1-A` profit-scan | 300 | 590.0 | 1,443.6 | 1,505.2 | 1,930.3 | 3,010.8 | 35,299.0 |
| `g1-A` profit-insert | 300 | 2,290.8 | 2,617.2 | 2,729.9 | 3,360.7 | 5,233.8 | 62,184.5 |
| `g1-B` profit-scan | 700 | 716.0 | 1,395.4 | 1,453.7 | 5,925.4 | 227,253.6 | 587,514.9 |
| `g1-B` profit-insert | 700 | 2,269.3 | 2,531.5 | 2,636.3 | 8,381.6 | 99,868.6 | 702,292.3 |
| `s1-A` profit-scan | 354 | 12,485.0 | 18,143.4 | 18,763.6 | 25,498.5 | 35,169.8 | 66,331.4 |
| `s1-A` profit-insert | 354 | 16,659.4 | 19,095.3 | 19,728.0 | 24,213.1 | 33,151.3 | 48,265.0 |
| `s1-B` profit-scan | 376 | 7,879.6 | 17,839.4 | 18,438.3 | 22,283.0 | 26,911.2 | 29,042.4 |
| `s1-B` profit-insert | 376 | 8,821.6 | 18,723.3 | 19,378.1 | 24,075.8 | 34,456.4 | 49,623.2 |
| `g2-B` profit-scan | 250 | 634.2 | 1,411.7 | 1,468.9 | 1,741.8 | 3,105.3 | 14,725.7 |
| `g2-B` profit-insert | 250 | 2,350.2 | 2,536.3 | 2,643.8 | 3,816.4 | 6,546.8 | 10,466.5 |
| `g2-A` profit-scan | 250 | 475.0 | 1,385.1 | 1,440.7 | 1,761.3 | 3,159.9 | 5,577.6 |
| `g2-A` profit-insert | 250 | 2,224.9 | 2,484.6 | 2,567.3 | 2,964.3 | 9,594.1 | 14,118.8 |
| `s2-B` profit-scan | 397 | 8,310.9 | 17,935.1 | 18,540.9 | 23,136.1 | 28,105.3 | 30,522.8 |
| `s2-B` profit-insert | 397 | 16,964.4 | 18,577.3 | 19,228.4 | 23,416.4 | 29,281.4 | 52,184.4 |
| `s2-A` profit-scan | 376 | 8,796.5 | 18,108.6 | 18,744.6 | 23,237.4 | 27,799.2 | 40,575.0 |
| `s2-A` profit-insert | 376 | 16,555.4 | 18,866.8 | 19,552.9 | 25,332.7 | 29,526.1 | 30,620.0 |
| `g3-A` profit-scan | 400 | 494.9 | 1,452.6 | 1,519.2 | 2,018.7 | 108,730.7 | 333,333.7 |
| `g3-A` profit-insert | 400 | 2,366.6 | 2,616.3 | 2,722.2 | 3,437.0 | 216,100.7 | 697,315.4 |
| `g3-B` profit-scan | 300 | 478.8 | 1,397.9 | 1,445.6 | 1,802.2 | 4,603.5 | 227,098.2 |
| `g3-B` profit-insert | 300 | 2,283.5 | 2,494.6 | 2,588.3 | 2,958.4 | 7,879.5 | 63,817.2 |
| `g4-B` profit-scan | 250 | 661.4 | 1,376.2 | 1,429.9 | 1,682.8 | 2,068.5 | 14,659.3 |
| `g4-B` profit-insert | 250 | 2,136.0 | 2,468.6 | 2,569.8 | 3,045.3 | 5,077.4 | 13,924.8 |
| `g4-A` profit-scan | 250 | 532.5 | 1,380.2 | 1,428.8 | 1,736.8 | 3,532.2 | 7,965.8 |
| `g4-A` profit-insert | 250 | 2,265.6 | 2,483.8 | 2,596.2 | 3,579.2 | 4,731.4 | 8,022.6 |

`profit-scan`/`profit-insert` `ops` differs by cell — the reporting
process runs one cycle per 30 simulated days regardless of how long the
cell took wall-clock, so a slower (degraded) cell accumulates more
periods and more `ops` (`g1-B`: 15 periods, 700 rows, against `g1-A`'s 6
periods, 300 rows, for the identical 180-day span) — not a difference in
what each cell measured.

## 5. Wait breakdown (rule 3)

**Durability/commit dominates, and it is the whole gap between group and
strict.** Reading the clean cells' `trade-insert` p50 (statistically
identical to `account-update`'s, per §4):

| | group p50 (mean, clean) | strict p50 (mean, clean) | Δ (durability wait) |
|---|---|---|---|
| Arm A | 2,648.9 µs | 9,320.8 µs | **6,671.9 µs** |
| Arm B | 2,639.5 µs | 9,168.8 µs | **6,529.3 µs** |

This delta is what one `fsync` (strict) or a share of one group-batched
`fsync` (group) costs on this device. Against the same statement's own
arm-to-arm difference — 9.4 µs in group, 152.0 µs in strict, from the
p50 means in the table above — the durability wait is 707× and 43×
larger: the compile-out this document exists to price is nowhere near
this wait's scale.

*Corrected on `ar2-borrow-model` after `c40b3cc` (archive re-read
2026-09-04): this sentence originally said 710×, dividing 6,671.9 by the
table's rounded 9.4 µs (6,671.9 / 9.4 = 709.8, which itself rounds to
710). The group arm-A/arm-B p50 means round to 2,648.9/2,639.5 for
display, but computed from the underlying per-cell `trade-insert` p50s
(`g1-A`/`g2-A`/`g4-A` and `g2-B`/`g4-B`) they are 2,648.93/2,639.50 µs,
a difference of 9.43 µs, not 9.4. `6,671.9 / 9.43 ≈ 707.3`, which is
where 707× comes from. (The table's own `+7.1` TPS delta at §3 has the
same source: the unrounded group-arm means are 710.71/717.86 TPS,
7.15 apart, which standard rounding renders as 7.1 rather than 7.2 —
not a separate error, the same effect of computing from finer-grained
inputs than the table displays.)*

**The mechanism is visible directly in `SHOW META`.** Group durability
batches: `wal_syncs` ≈ 5,434–5,441 against `wal_group_commits` ≈
20,672–21,122 in every group cell (clean or degraded — the sync *count*
does not move with the stalls, only their latency does, which is itself
evidence the degraded cells are host-level and not extra I/O), with
`wal_mean_group_batch` ≈ 3.81–3.89 statements sharing each `fsync`.
Strict durability does not batch at all: `wal_syncs` ≈ 20,790–20,833 —
one sync roughly per statement — with `wal_group_commits` and
`wal_group_batches` both `0` in every strict cell. The ≈3.67–3.79×
TPS gap between group and strict (mean group 710.7–717.9 TPS vs mean
strict 189.2–193.7 TPS) is this batching factor, not the arm.

**Client/socket round trip.** `load-accounts`' own p50 (a single
uncontended connection, no concurrent committers to batch with) sits at
1,158.2–1,347.9 µs across every cell in this run — the floor every
statement pays before durability is even asked for, matching AL-S8's own
1,169–1,235 µs finding for the same phase (stated there across every
cell of its matrix, not per core count).

**Write-statement execution** is not separately measurable this run
(would need `--log-level debug`, avoided so as not to add I/O to the
fsync path being priced): this run separates neither the mutation nor the
append from the commit wait that dominates both write phases.

**Read wait** has no separable share of `txn` — no read *statement* is in
the measured unit (the two `UPDATE`s' own pk lookups are page reads this
run cannot price apart), and `profit-scan`'s FilterScan runs concurrently
without gating `txn`.

**Lock/conflict wait** is ~0: `torn = 0` in all twelve cells (§6), and
each trader owns a disjoint account partition by construction.

## 6. Correctness

Every one of the twelve cells — clean and degraded alike — committed
5,000 transactions, 0 torn, 179 underfunded (skipped before any statement
was sent), and `--verify 200` read all 200 accounts back matching the
driver's own arithmetic (`<cell>.run.stdout.txt`, confirmed for every
cell). The degraded cells' host-level stalls cost latency, not
correctness.

## 7. Noise floor and the three degraded cells

**Evidence for degradation is `recovery_checkpoint_us`, not the TPS
number itself.** The mount-time completion checkpoint — which runs on a
freshly bootstrapped, empty data file before the driver sends its first
statement — took 434,131 µs (`g1-B`), 167,931 µs (`g3-A`) and 60,931 µs
(`g3-B`) against 5,199–7,433 µs in the other nine cells: one to two
orders of magnitude, on an operation that touches nothing the driver's
workload does. Their `txn` p25/p50 sit inside the clean band (§4); only
p95/p99/max blow out — a rare, severe host-level device stall on top of
an otherwise-normal run, the same shape
`results-ar2-c2-spreading-v2.7.0-178-g92cb654.md` §9 named for its own
`s0-c1` outlier ("a rare, severe tail event on an otherwise-normal
body"). No cause beyond "device stalls at the host level" is supportable
from this run's data — the stalls hit one A cell and two B cells, so
whatever they are, they are arm-independent. All three are excluded from
§3's and §8's comparisons and appear only in their own table rows above.
Read the thermometer first: it flags a cell before the driver has sent a
statement, which is cheaper and cleaner than inferring contamination from
the `txn` tail.

**The run's own clean-cell floor.** Group durability's three clean A
cells (668.4 / 731.1 / 732.6 TPS) spread 9.6% — wider than the two clean
B cells' 1.7%, wider than either arm's strict spread (2.6% A / 0.5% B),
and wider than AL-S8's previously recorded 1.5% same-configuration
floor. That spread is driven almost entirely by `g1-A`: `g1-A`
vs `g2-A` is 9.4%, `g1-A` vs `g4-A` is 9.6%, but `g2-A` vs `g4-A` is only
0.2%. `g1-A` was the very first cell of the whole run — its own
`recovery_checkpoint_us` (6,093) is unremarkable, so it is not
"degraded" by this document's own thermometer, but it is the one clean
cell whose TPS reads as an outlier within its own arm. Read conservatively:
**the true group-A floor may be closer to `g2-A`/`g4-A`'s own 0.2%
agreement, with `g1-A` itself a milder, sub-threshold version of the same
host effect that produced the three flagged degraded cells** — this
document cannot distinguish that from ordinary first-cell noise with one
sample. A future same-shape run that opens with a throwaway warm-up cell,
discarded before the measured pairs begin, would settle it cheaply.

**`g1-A` is not just a milder version of the same host effect — it is
the one clean cell this document's own noise-floor rule (repeat the
pair, not the cell) says should have been dropped alongside its
degraded partner `g1-B`, and dropping it changes which arm the group
headline favors.** §3 now carries that reading: excluding both cells of
every pair with a degraded partner (`g1` and `g3`, not just their
degraded halves) leaves group A at 731.9 TPS (`g2-A`/`g4-A`, spread
0.2% — the tight agreement this paragraph already flagged as the more
trustworthy floor) against group B's unchanged 717.9 TPS, a **1.9% A
lead**, not the cell-wise table's 1.0% B lead. That 1.9% is bounded by,
and consistent with, the two matched group pairs below (`g2`: A by
2.7%; `g4`: A by 1.2%) — all three now agree on direction, where the
cell-wise headline alone did not.

**The matched same-pass pairs do not agree on direction, which is itself
evidence the delta is noise.** Four same-pass, interleaved pairs survive
exclusion (`g1` is dropped, `g3` is dropped — both have a degraded
partner):

| Pair | A | B | Faster arm | Margin |
|---|---|---|---|---|
| `s1` | 186.8 | 194.2 | B | 4.0% |
| `g2` | 731.1 | 711.9 | A | 2.7% |
| `s2` | 191.6 | 193.3 | B | 0.9% |
| `g4` | 732.6 | 723.9 | A | 1.2% |

Group pairs favor A; strict pairs favor B; every margin is under 4%.
**Each durability class is internally consistent — both group pairs
favor A (2.7%, 1.2%), both strict pairs favor B (4.0%, 0.9%) — but the
two classes disagree with each other.** Four samples, two per
durability, is too few to trust either sign as a real effect on its
own, and a compile-out that costs A a few percent at `group` while
costing B a few percent at `strict`, with no mechanism tying the sign to
the durability setting, is not what a real regression or win in the
compile-out itself would produce — that is the evidence this run reads
as noise, not the absence of any pattern within a class.

*Corrected on `ar2-borrow-model` after `c40b3cc` (archive re-read
2026-09-04): this paragraph originally said "a real regression or win
would be expected to show the same sign in both matched pairs of its
durability class, and neither class does" — but the table two lines
above it shows exactly the opposite: `g2`/`g4` both favor A and
`s1`/`s2` both favor B, so both classes *do* show a consistent sign
within themselves. What is inconsistent is between the two classes, not
within either.*

## 8. Delta against this engine's own history (rule 4)

**The normal baseline is this engine's own last run for the shape.**
Two exist for `cores = 1` group: `results-ar2-c2-spreading-v2.7.0-178-g92cb654.md`'s
`s0-c1-r2` (720.1 TPS, measured on arm A's own binary — same commit,
same hash) and AL-S8's `s0-c1-g`
(`results-scenario0-stockmarket-v2.7.0-157-gf6ed10c.md`, 700.9 TPS, an
earlier commit, `f6ed10c`). For strict, AL-S8's file holds one number for
this shape: `s0-c1-s`, 192.6 TPS.

| Comparison | This run | Prior | Delta |
|---|---|---|---|
| Group, A mean (710.7) vs C2's `s0-c1-r2` (same binary, 720.1) | 710.7 | 720.1 | −1.3% |
| Group, A mean (710.7) vs AL-S8's `s0-c1-g` (700.9, different commit) | 710.7 | 700.9 | +1.4% |
| Group, B mean (717.9) vs AL-S8's `s0-c1-g` (700.9) | 717.9 | 700.9 | +2.4% |
| Strict, A mean (189.2) vs AL-S8's `s0-c1-s` (192.6) | 189.2 | 192.6 | −1.8% |
| Strict, B mean (193.7) vs AL-S8's `s0-c1-s` (192.6) | 193.7 | 192.6 | +0.6% |

Every one of these deltas is smaller than this run's own clean group-A
spread (9.6%, §7) and comparable to or smaller than AL-S8's previously
stated 1.5% same-configuration floor. **None of them is a finding** —
this run's numbers for both arms sit inside the band this engine's own
history for the shape already occupies. `g1-A` alone, read against C2's
`s0-c1-r2`, is the one individual cell that reads larger (−7.2%,
668.4 vs 720.1) — consistent with §7's reading of `g1-A` as the run's
own noisiest cell, not with an engine regression, since `g2-A` and
`g4-A` (+1.5% and +1.7% against the same 720.1 reference) do not show it.

**PostgreSQL.** No v3.0.0 results file has established a PostgreSQL
floor for this shape (`scenario0_stockmarket.py`, `cores = 1`) —
`results-ar2-c1-colocation-v2.7.0-178-g92cb654.md` and
`results-ar2-c2-spreading-v2.7.0-178-g92cb654.md` both say so explicitly
for their own shapes, and AL-S8's own scenario0 document did not run the
PostgreSQL twin either. Per rule 4, that is a gap to name, not one to
apologise for; it is not measured in this document either.

## 9. Row-set size — not swept, named as a gap

Every cell runs AL-S8's own fixed shape — 100 users, 287 accounts, a
5,000-transaction work target — and rule 9's sweep at 200/1K/10K rows,
which would tell a fixed cost from a per-row one, was not run. Whether
the compile-out's cost has any row-count dependence is unanswered here.

## 10. Conclusion against AM-S1's definition of done

**Met, for the `cores = 1` half.** The work order's words are "a
`cores = 1` A/B showing the acquire/release pair costs nothing
measurable": every A−B delta this document can compute (§3) sits inside
the run's own floor (§7) and inside the band this engine's history
already occupies for the shape (§8), and the surviving matched pairs do
not even agree on which arm is faster. **The other half — "contention
cell at 8 cores" — remains unmet**: only the 8-thread unit cell in
`tests/page_latch_test.cpp` stands behind it, asserting that contention
occurred and the counts balance, not a throughput number; AM-S6 is where
that number belongs, against AL-S8's own 8-core baseline.

## 11. What this measurement teaches about the engine

- **The bound is on the compile-out, not the latch.** Nothing in this
  document says anything about what the latch costs once armed — that
  question is untouched here by construction (see the opening), and reading
  this result as evidence the latch itself is free would be a category
  error the work order's own two-cell split exists to prevent.
- **Group durability's own run-to-run spread (9.6% on one arm's three
  clean cells, cell-wise) is wider than either arm's mean difference from
  the other under that same cell-wise counting** (1.0% group, 2.4%
  strict, §3/§7). But §3's pair-wise reading — dropping `g1-A` alongside
  its degraded partner, the way `g3`'s pair already was dropped — tightens
  group's own floor to 0.2%/1.7% and turns the group delta into a
  **1.9% A lead**, matching both matched group pairs (§7). The honest
  statement is not "nothing measurable inside a 9.6% spread": it is that
  the compile-out's bounded cost is **−1.9% (group, A ahead, both pairs
  agree) and +2.4% (strict, B ahead, both pairs agree)** — opposite
  signs across the two durability classes, each a few percent, neither
  large enough on four samples to call a real effect, but not the flat
  "no signal" the wider cell-wise spread implied either. On this host,
  at this shape, the engine's own day-to-day variance at `cores = 1` is
  still a larger source of uncertainty than a 535-line,
  mechanically-reasoned-to-be-inert code change should need to clear;
  any future `cores = 1` A/B on this class of change should exclude by
  pair, not by cell, from the start, and should budget for a floor at
  least this wide before calling a delta a result.

Raw driver JSON, `SHOW META` text, cell records, server configs and the
orchestrator/summary scripts for all twelve cells are archived at
`bench/v3.0.0/archive/am-s1-page-latch-v2.7.0-183-gc985d37/`.
