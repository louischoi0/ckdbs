# Business-scenario stress test, measured

Measured 2026-08-03 with `tools/scenario0_stockmarket.py`, the five-relation
brokerage workload: `users`, `accounts`, `assets`, `trades`,
`user_periodic_profit`. The unit is a **business transaction** — one
executed trade, four statements:

```
INSERT INTO trades ...    buy leg  (side=0, buyer's account)
INSERT INTO trades ...    sell leg (side=1, seller's account)
UPDATE accounts SET ...   buyer:  balance down, asset_qty up
UPDATE accounts SET ...   seller: balance up,   asset_qty down
```

A transaction counts only if all four statements replied without `ERR`.

Two questions: **what TPS does the engine sustain under a realistic OLTP
mix**, and **what does a concurrent analytic reporting job cost it**.

---

## Method

Release build, `durability=group` (the default), one `kds_server` process,
one connection per trader process plus one for the reporting process.
Per-request wall clock around one send + one recv, so every client-side
latency carries Python's own socket cost (~110 µs floor here). The
`server-side` rows are the server's own `in <n>us` per-statement figure,
read back out of its debug log — that is the figure to judge engine changes
on.

Fixed across every run below:

| | |
|---|---|
| users | 10,000 |
| accounts | 20,012 (2±1 per user) |
| assets | 10,000 |
| simulated span | 180 business days, compressed into the run |
| measured phase | 120 s wall clock |
| trader processes | 4, each owning a disjoint account partition |
| storage | `accounts`/`users`/`assets` BTREE, `trades`/`user_periodic_profit` HEAP |

Host: AMD EPYC 7571, **2 cores**. That matters for the concurrency reading
below — 4 trader processes are not 4 cores of client.

**The balance check passed on every run**: 200 accounts read back and
compared against what the drivers believe they wrote, all matching, and
`torn = 0` throughout. No transaction was partially applied, so nothing
below is a measurement of the error path.

---

## 1. Put the data file on a real disk

The tool's footer says not to measure on tmpfs. This is what ignoring it
looks like — same configuration, same binary, only the filesystem differs:

| | tmpfs (`/tmp`) | xfs root volume |
|---|---|---|
| **TPS** | **1,730.6** | **166.8** |
| committed in 120 s | 207,670 | 20,015 |
| txn p50 (client) | 1.78 ms | 11.80 ms |
| INSERT p50 (server-side) | 129 µs | 987 µs |
| UPDATE p50 (server-side) | 18 µs | 21 µs |
| load of 40 K rows | 5.2 s | 90.1 s |

**10.4×.** `INSERT` is the one logged statement, and on tmpfs `fsync` is
free. The `UPDATE` column is the control: unlogged, and it does not move
(18 → 21 µs). A tmpfs number here is not a fast result, it is a different
measurement.

Every number in the rest of this document is from the xfs root volume.

## 2. What the reporting job costs

The reporting process is the scenario's second half: every wake-up it
checks whether a simulated period has passed and, if so, reads each sampled
user's accounts with `SELECT * FROM accounts WHERE user_id = <n>` — a
non-pk equality, so a `FilterScan`, a walk of the whole 20,012-row relation
per user — and appends one `user_periodic_profit` row.

| | reporter **on** | reporter **off** (`--no-profit`) |
|---|---|---|
| **TPS** | **166.8** | **321.7** |
| committed in 120 s | 20,015 | 38,607 |
| txn p50 | 11.80 ms | 10.49 ms |
| txn p95 | 108.13 ms | 25.94 ms |
| txn p99 | 211.23 ms | 49.09 ms |
| trade-insert p50 | 3.26 ms | 3.08 ms |
| account-update p50 | 2.12 ms | 2.00 ms |
| INSERT p50 (server-side) | 987 µs | 965 µs |
| UPDATE p50 (server-side) | 21 µs | 21 µs |
| SELECT p50 (server-side) | 28,663 µs | 27 µs |

**One reporting process, running about 10 scans a second, costs 48% of the
engine's transaction throughput.**

Two things make that number what it is, and they are worth separating:

**The scans are enormous and the server is one thread.** A `FilterScan`
over 20,012 accounts costs **28.7 ms server-side**, against 21 µs for the
pk `UPDATE` beside it — a factor of 1,365. `TcpServer` is a reactor
participant: many clients, cooperatively, on one thread
(`include/kds/server/tcp_server.hpp`). While that 28.7 ms scan runs, no
trader makes progress. 1,165 scans × 28.663 ms = **33.4 s of the 120 s
window**, 28% of the wall clock, spent inside the reporting job's reads.

**The tail is where it shows, not the median.** txn p50 barely moves (10.5
→ 11.8 ms) while p95 goes 25.9 → 108.1 ms and p99 goes 49.1 → 211.2 ms. A
transaction that does not land inside a scan is nearly unaffected; one that
does waits out the whole scan. That is the signature of head-of-line
blocking on a single reactor thread, not of resource exhaustion.

The throughput loss (48%) is larger than the reactor time the scans consume
(28%). The gap is **not measured here** — the plausible mechanism is that a
28 ms stall spans the WAL group-commit window, so committers that would
have shared an fsync end up paying separate ones, but nothing in this run
isolates that. Treat it as an open question, not a finding.

## 3. The WAL fsync is the ceiling

From the `--no-profit` run, where nothing else contends:

- 77,214 `INSERT`s in 120 s = **643 inserts/s**, at **965 µs p50 server-side**
- 77,214 `UPDATE`s in the same window, at **21 µs p50 server-side**

The insert costs **46× the update**, and the only difference between them is
that `INSERT` is logged and `UPDATE` is not (`docs/wal.md` §1). At 643
inserts/s against a ~1 ms fsync, throughput sits near the **single-fsync
ceiling** — so `group` commit is amortizing little at 4 concurrent
committers, which is the concurrency at which it was supposed to start
paying (`docs/client-manual.md`: `group` matching `strict` on one connection
is expected; matching it on four is not).

Two statements per transaction are logged, so ~2 ms of the transaction's
~3.1 ms of server-side work is fsync. **The engine is not the bottleneck in
this workload; the durability class is.** A `durability=relaxed` run would
price the other side of that, and has not been taken.

Also worth stating plainly: **`UPDATE` is unlogged.** The balance moves —
the half of the transaction that represents money — survive only a `SYNC`
or a clean shutdown. The TPS above is a throughput number for a workload
that is, today, half-durable.

## 4. Concurrency does not scale here

| traders | TPS | txn p50 |
|---|---|---|
| 4, reporter on | 166.8 | 11.80 ms |
| 4, reporter off | 321.7 | 10.49 ms |

Per-trader commit counts are even to within 2% in both runs (`#0:9,712
#1:9,547 #2:9,752 #3:9,596` without the reporter), so the round-robin
account partitioning is not starving anyone — the fairness is fine, the
ceiling is shared.

No 1-trader run was taken on this disk, so **the scaling curve is not
measured** and nothing here says what 1 → 4 traders buys. What the numbers
do say is that adding clients cannot buy much: the server dispatches every
one of them on a single thread, and that thread spends most of its time in
`fsync`. Thread-per-core (`docs/rules.md` §3) is Phase 2+, and this is the
workload that would show it.

---

## Files

| file | what |
|---|---|
| `results/business-stress-4t.json` | 4 traders + reporter, xfs — phases and meta |
| `results/business-stress-4t-noprofit.json` | 4 traders, `--no-profit`, xfs |
| `results/business-stress-4t.txt` | full console report, reporter on |
| `results/business-stress-4t-noprofit.txt` | full console report, reporter off |
| `results/business-stress-4t-tmpfs.txt` | the tmpfs run from §1, kept as the cautionary case |

Reproduce:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release -j
# a real disk, NOT /tmp if /tmp is tmpfs:
./build-release/kds_server ~/scratch/s.db --port 15630 \
    --log-dir ~/scratch --log-file s.log --log-level debug &
python3 tools/scenario0_stockmarket.py --port 15630 --traders 4 --seconds 120 \
    --server-log ~/scratch/s.log --json bench/results/business-stress-4t.json
```

Each run creates five fresh relations and there is no `DROP TABLE`. The
catalog's column page does not chain and holds ~62 user columns for the
whole instance (`docs/keystoneid-k0-findings.md`); this scenario spends 27
per run, so **a data file survives two runs and refuses the third**.

## Open, not answered here

- The 48%-vs-28% gap in §2. Needs a run that isolates group-commit
  batching from reactor-thread occupancy.
- `durability=relaxed` and `strict` for the same scenario. §3 argues the
  fsync is the ceiling; only the other two classes prove it.
- The 1 → 2 → 4 trader scaling curve on a real disk.
- What any of this looks like once `UPDATE` is logged, which changes the
  transaction from two logged statements to four.
