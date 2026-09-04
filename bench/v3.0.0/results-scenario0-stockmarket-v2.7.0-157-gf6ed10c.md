# Scenario 0 (brokerage) — AL-S8 baseline, single-WAL-stream engine

AL-S8's scenario matrix, scenario0 half: `tools/scenario0_stockmarket.py`
at `cores = 1` and `cores = 8`, `group` and `strict`, on the engine AR0 M0
produced — one WAL stream for the instance, core 0 owning it, every peer
attaching and asking core 0's writer for a sync instead of issuing one
(`docs/spec/wal.md` §3). This document is scenario0's quarter of the AL-S8
baseline; scenario2's is
`results-scenario2-freight-v2.7.0-157-gf6ed10c.md`, and the three
M0-specific instruments (a peer's commit tail, the fdatasync share of
reactor time, the WAL ring under eight writers) are
`results-wal-single-stream-v2.7.0-157-gf6ed10c.md`. Driver flags and
what they measure are documented in `bench/docs/README.md` at commit
`1769487` (`git show 1769487:bench/docs/README.md`); this file states
findings only.

**This is a fresh series (AR0 D15, ratified AL-R8). No number here is a
delta against any `v2.x` result** — every prior scenario0 run priced a
per-core WAL stream, an engine this build no longer is. Where this run
has no predecessor of its own shape, it is the baseline the next one at
this engine's next state reads against.

## 1. Stamp

| Field | Value |
|---|---|
| Date/time | 2026-09-03, 01:29–01:39 UTC (per-cell times in §2) |
| Worktree | `v3.0.0-arch-revision` (branch `worktree-v3.0.0-arch-revision`) |
| Commit measured | `f6ed10c`, `git describe --tags` = `v2.7.0-157-gf6ed10c` |
| Tree cleanliness | Clean at every cell. A sibling commit, `f027a3c3` ("docs/inflight reopened, M1's work order, and a peer stops answering for the volume with zeros"), landed on this same branch at **01:46:01 UTC — after every cell in this document had already run**. It touches `src/server/core_runtime.cpp`, `expeditor.cpp`, `superblock.{cpp,hpp}` and docs; `git diff --stat f6ed10c f027a3c3 -- tools/ bench/` is empty, so the driver this document's numbers came from is byte-identical throughout, and the binary measured (below) predates the fix and is unaffected by it either way. One consequence of the fix is visible in this run's own raw data — see §6. |
| Binary provenance | Copy at `/home/cdkbs/bench-runs/al-s8-f6ed10c/kds_server`, `sha256 2ab1960bc056e7cc5c59be4946a2cf1250b4e65b941934c96ebf736e80435af3`. Source `build-release/kds_server` mtime `2026-09-03 01:17:56.28 UTC`, copied `01:18:01 UTC` — 5 s later, both after `f6ed10c`'s commit time `01:16:50 UTC`. Every server in this document started from the copy, never from `build-release/` directly. |
| Device | `/home/cdkbs` and `/` are `/dev/root`, `ext4` (`df -T`) — a real block device, not tmpfs. |
| Build type | `build-release` (Release); not rebuilt this session. |
| Host | 8 logical CPUs (`nproc`), AMD EPYC 9V74: `lscpu` reports **1 socket, 4 cores, 2 threads/core** — the 8 is SMT, not 8 independent physical cores. A `cores = 8` server pins 8 reactor threads onto 4 physical cores, so cores 8 numbers below include sibling-thread contention as part of the engine's own cost, not just its own protocol's. |
| Server config (common) | `data_file` fresh per cell, `port` fresh per cell, `checkpoint_interval_ms = 5000` (default), `auth = off`, `tls = off`. Varied: `cores`, `durability`, `peer_listeners` (`on` only at `cores = 8`), `placement = namespace` (the shipped default) in every cell of this document. |

## 2. What was run, and in what order

Interleaved with scenario2's cells (not run block-by-block): the actual
order across both documents was `s0-c1-g`, `s2-c8-s`, `s2-c1-s`,
`s2-c1-g`, `s0-c8-g`, `s0-c1-s`, `s2-c8-g`, `s0-c8-s`. Every cell got a
fresh data file and a fresh server process from the hashed binary copy.
Fixed across all four cells so `cores`/`durability` are the only variable:
`--users 100 --accounts-per-user 3 --assets 30 --traders 8
--txn-per-user 50 --verify 200 --seed 1 --sync` — a work target of
5,000 committed business transactions (2 `INSERT trades` + 2 `UPDATE
accounts` each), split 625 per trader, every cell. `--profit` (the
periodic reporting process) ran at its default, on.

| Cell | Port | Precheck time (UTC) | `/proc/loadavg` (1/5/15 min) | Build check |
|---|---|---|---|---|
| `s0-c1-g` (cores=1, group) | 15570 | 01:29:45 | 0.12 / 0.50 / 1.48 | `pgrep -a -f 'cc1plus\|cmake --build\|ctest'`: none |
| `s0-c8-g` (cores=8, group) | 15572 | 01:36:31 | 0.73 / 0.56 / 1.14 | none |
| `s0-c1-s` (cores=1, strict) | 15571 | 01:36:59 | 0.60 / 0.55 / 1.12 | none |
| `s0-c8-s` (cores=8, strict) | 15573 | 01:38:27 | 0.64 / 0.59 / 1.08 | none |

`placement = namespace` is the shipped default and is what this document
leaves it at. It matters more here than the instruction suggests: neither
`scenario0_stockmarket.py` nor `scenario2_freight.py` issues `CREATE
NAMESPACE`, and `AssignOwnerCore` answers an undeclared namespace with the
**creating core** (`docs/spec/namespace.md` NS10, clause 1) — which is
always core 0, because DDL always executes there (CC2,
`docs/spec/crosscore.md`). Verified directly this run: `DESCRIBE
accounts_s0c8g` / `trades_s0c8g` / `users_s0c8g` /
`user_periodic_profit_s0c8g` on the `cores=8` server all answer
`owner_core=0`. **Every relation in every cell of this document, at both
core counts, is owned by core 0.** `cores = 8` therefore does not
parallelize this workload's writes across cores at all under the default
placement policy with these drivers; what it changes is whether the
*client's own session* lands on core 0 or a peer (`peer_listeners = on`
distributes accepts over all 8 listeners by `SO_REUSEPORT`, and a peer
session's writes are then shipped to core 0 rather than executed where
they landed). Cell B's document uses `placement = rotate` specifically to
get a peer-owned relation; this one does not, on purpose, because it is
what the two drivers actually produce unmodified.

## 3. TPS

Rule 5a: throughput, not delay — `tps` is the driver's own count of
completed business transactions over the wall time they took, not a
derived `1e6/mean`.

| Cell | cores | durability | TPS | committed | torn |
|---|---|---|---|---|---|
| `s0-c1-g` | 1 | group | **700.9** | 5,000 | 0 |
| `s0-c8-g` | 8 | group | **754.7** | 5,000 | 0 |
| `s0-c1-s` | 1 | strict | **192.6** | 5,000 | 0 |
| `s0-c8-s` | 8 | strict | **207.2** | 5,000 | 0 |

**Durability class is the whole story; core count is noise here.** Group
over strict is **3.6×** at `cores=1` and **3.6×** at `cores=8` — the
group-commit amortization this engine has always had, unaffected by the
WAL becoming one stream. Core count itself moves TPS by **+7.7%**
(group) and **+7.6%** (strict), in the *same* direction both times. A
same-configuration repeat of `s0-c1-g` (§8) put the run-to-run floor at
about 1.5%, so the `cores=8` uplift is a real, if small, effect — plausibly
the extra reactor thread pipelining client I/O and command parsing ahead
of the dispatch to core 0, which is genuine parallel work even though
every write still lands on one core's WAL. It is not the multi-core
scaling `cores = 8` is *for*; §2 explains why this shape cannot show that
under the drivers as shipped.

## 4. Percentiles

Every row is a `Phase.summary()` distribution (`tools/bench_common.py`);
`ops` is every value counted.

### `txn` — the measured unit (2 INSERT + 2 UPDATE, one client-perceived round trip per statement)

| Cell | ops | p0 | p25 | p50 | p95 | p99 | max (µs) |
|---|---|---|---|---|---|---|---|
| `s0-c1-g` | 5,000 | 8,646.2 | 10,577.5 | 10,973.9 | 14,641.4 | 18,901.8 | 24,976.2 |
| `s0-c8-g` | 5,000 | 8,597.2 | 9,720.5 | 10,041.2 | 13,781.3 | 17,277.6 | 32,932.5 |
| `s0-c1-s` | 5,000 | 4,602.1 | 34,720.8 | 37,216.2 | 73,428.1 | 83,630.9 | 160,897.6 |
| `s0-c8-s` | 5,000 | 19,422.8 | 31,915.3 | 35,737.8 | 46,317.1 | 53,077.7 | 72,309.6 |

### `trade-insert` (`INSERT`) and `account-update` (`UPDATE`) — p50/p99, µs

| Cell | trade-insert p50 | trade-insert p99 | account-update p50 | account-update p99 |
|---|---|---|---|---|
| `s0-c1-g` | 2,701.5 | 6,483.7 | 2,703.6 | 6,712.4 |
| `s0-c8-g` | 2,461.3 | 6,286.1 | 2,454.4 | 6,256.8 |
| `s0-c1-s` | 9,127.3 | 21,591.6 | 9,216.5 | 21,779.0 |
| `s0-c8-s` | 9,069.7 | 15,843.6 | 9,234.6 | 15,862.6 |

**`trade-insert` and `account-update` are statistically the same
statement here.** *Corrected on `ar2-borrow-model` after `680c763`: this
paragraph and the heading above originally called the `UPDATE`
"page-only, unlogged". It is not — an `UPDATE` appends `HEAP_OVERWRITE`
like every other data mutation (`include/kds/wal/record.hpp`'s
`kHeapOverwrite`), and it did so at `f6ed10c` too; the numbers stand, the
inference built on the contrast does not.* Both statements are WAL-logged,
autocommit wraps both in an implicit transaction with its own commit
envelope, and the commit's durability wait — not the row mutation —
dominates both statements' client-perceived cost about equally, which is
why their latencies track within a few percent of each other in every
cell. §5 separates that wait out.

`s0-c1-s`'s `txn` p0 (4,602.2 µs) sitting below its own `p25`
(34,720.8 µs) by 8× is the shape of a single connection with no
group-commit partner: the very first transactions each trader sends race
ahead of the steady state before the durability wait per statement
settles into its usual ~9 ms, which is also why `trade-insert`/
`account-update`'s own `p0` values (not shown per-percentile above, see
the archived JSON) sit near the load-phase floor while their body sits at
the strict fsync cost.

## 5. Wait breakdown

Rule 3: name each wait and give it a share, or say it does not apply.
No `--log-level debug` server log was used (it would have added its own
I/O to the exact path being priced), so the breakdown below is derived
from the phases available rather than from server-side per-statement
timing. (The WAL-single-stream document's `b-rotate-*` runs did pass
`--server-log`, but at `log_level = warn` the only lines a server ever
writes are the occasional lease-refill `TXN_CONFLICT` warning, and the
driver's log parser reads *any* line ending `in <n>us` as a timed
statement — so the "22 stmts p50 3 µs" that tool prints there is the
retry-wait of a handful of lease-exhaustion races, not a sample of
ordinary statement execution time. Noted here rather than used, since an
earlier draft of this document mis-read it as the latter.)

| Wait | Estimate | How |
|---|---|---|
| **Durability/commit (fsync or its group-batched equivalent)** | dominant: ~7,900 µs of `trade-insert`'s ~9,127 µs strict p50 (cores=1), collapsing to ~1,700 µs of its ~2,701 µs group p50 | the group-vs-strict delta at fixed cores, cleanest at `cores=1` where there is no peer-attach path to also account for |
| **Client/socket round trip + dispatch** | ~1,100–1,200 µs floor | the `load-accounts` phase's own p50 (an uncontended single connection, one row each, no concurrent committers to batch with — "a batch of one is a batch") sits at 1,169–1,235 µs across every cell's load phase; this is the floor every statement in this document pays before durability is even asked for |
| **Write-statement execution (page mutation itself)** | not separately measurable this run — would need `--log-level debug`, which was avoided so as not to add I/O to the fsync path being priced. `trade-insert` and `account-update` cost the same in every cell (§4), but that is not evidence about the size of the mutation term: both are WAL-logged (§4's correction), so the two statements do not differ in whether they log — only in row shape — and their agreement says the commit's durability wait dominates both about equally, not that row mutation is cheap | §4's own comparison |
| **Read wait** | n/a to `txn` itself (no read inside the four statements); `profit-scan`'s FilterScan runs concurrently and does not gate `txn` | — |
| **Lock/conflict wait** | ~0: `torn = 0` in every cell, and each trader owns a disjoint account partition by construction (`--traders 8` against `--users 100`), so no two traders' `UPDATE accounts` can collide | scenario doc's own design; confirmed by the printed `torn` counter |

*Corrected on `ar2-borrow-model` after `c40b3cc` (archive re-read
2026-09-04): the row above originally called `account-update` "(not
logged)" and read the trade-insert/account-update agreement as evidence
row mutation is cheap because the two statements "differ in exactly this
term." §4's own correction (above, dated after `680c763`) already
established that `account-update` is WAL-logged like every other
mutation; this row is fixed to match, and the inference is corrected
along with it — the agreement is still real, it just says the commit
wait dominates, not that logging is free.*

## 6. Correctness

`--verify 200` read 200 accounts back against the drivers' own arithmetic
after every cell; all 200 matched in all four cells, and `torn = 0` in
all four. Nothing here changed the engine's answers — the number above
is a throughput and latency measurement of a workload that also produced
the right data.

## 7. What this run says about the engine

**The single WAL stream is invisible in this shape.** Every relation in
every cell sits on core 0 (§2), so no peer ever attaches to the shared
stream to *write*; what `cores = 8` adds here is a second reactor thread
ahead of the same core-0 commit path, worth ~7% and nothing more. This
document therefore cannot answer "does the shared latch cost anything" —
that is what `results-wal-single-stream-...md`'s `placement = rotate`
cells are for, and they show peer commits do happen and are structurally
free of a peer's own device sync (`wal_syncs=0` on every peer, every
cell). What this document *does* establish is the durability-class
baseline the peer numbers there should be read against: group's ~3.6×
over strict, driven entirely by batching a `trade-insert`/`account-update`
pair that would otherwise pay ~9 ms each into a shared sync — the number
the M0 cutover's whole argument is that a peer should get to share too.

## 8. Noise floor

`s0-c1-g` repeated (fresh server, fresh data file, identical flags):
690.7 TPS against the recorded 700.9 — a **1.5%** spread. Nothing in §3's
core-count deltas (7.6–7.7%) or §4's percentile comparisons is inside
that floor; the group/strict gap (3.6×) is far outside it.

Raw driver JSON, server logs and `SHOW META` dumps for every cell in this
document (plus the `placement = rotate` scenario0 runs the WAL-single-
stream document cites) are archived at
`bench/v3.0.0/archive/scenario0-v2.7.0-157-gf6ed10c/`.
