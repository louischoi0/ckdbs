# ckdbs vs PostgreSQL 17 — client-path benchmark, 2026-07-30

One run of `tools/benchmark.py` (ckdbs) and `tools/pg_benchmark.py` (PostgreSQL)
on the same host, same row shape, same phases, same harness
(`tools/bench_common.py`). Raw output is in `bench/results/*.json`; every number
below comes from those files or from the two servers' own statement logs.

**Read the caveats before quoting anything.** ckdbs has no index and appends
nothing to its WAL yet, so two of the four phases are not measuring the same
work on both sides. That is stated per phase rather than averaged away.

Not related to `bench/bench_main.cpp`: that binary measures WAL/page internals
in-process with no server, no parser and no socket. There is no PostgreSQL
counterpart to it and the numbers do not combine.

## 1. Environment

| | |
|---|---|
| Host | AWS EC2, AMD EPYC 7571, 2 vCPU, 7 GiB RAM |
| Storage | NVMe-backed EBS root volume, ext4 |
| Kernel | 6.18.38-73.137.amzn2023.x86_64 |
| ckdbs | commit `b5b0184`, `CMAKE_BUILD_TYPE=Release`, `build-release/kds_server`, newline text protocol, port 15432 |
| PostgreSQL | 17.10, packaged build, **untuned defaults**, scratch cluster from `tools/pg_setup.sh init`, port 15433 |
| Client | Python 3.9, single connection, one request at a time |
| Scale | 2000 rows resident, 1000 point selects, 20 full scans, 1000 updates, seed 1 |

PostgreSQL was left at package defaults on purpose (128 MB `shared_buffers`,
default WAL settings): a baseline tuned by hand is not a baseline. Both servers
were otherwise idle; the box has 2 vCPUs, so the Python client and the server
share the machine on both sides equally.

## 2. What each phase does

| Phase | ckdbs | PostgreSQL |
|---|---|---|
| `insert` | `INSERT INTO t VALUES (4 body cols)` | `INSERT INTO t (4 body cols) VALUES (...)` |
| `point-select` | `SELECT * FROM t WHERE id = <n>` | same |
| `full-scan` | `SELECT * FROM t` | same |
| `update` | `UPDATE t SET c_int = <n> WHERE id = <n>` | same |
| `sync` / `checkpoint` | `SYNC` (flush page store) | `CHECKPOINT` |

Row shape is 5 columns on both sides — the server-generated `id` plus
`c_int bigint`, `c_small integer`, `c_flag boolean`, `c_text varchar` written
once at 16 chars. `id` is server-issued on both sides (ckdbs invariant 10;
PostgreSQL `GENERATED ALWAYS AS IDENTITY`), so no client supplies a pk.

Statements are sent as inline-literal text and parsed by the server on every
execution on both sides — no prepared statements anywhere, which is the point:
ckdbs has no PARSE/BIND path yet, so giving PostgreSQL one would flatter it.

Three variants were run:

* **ckdbs** — indexless by construction.
* **PG `noidx`** — identity column, zero indexes (asserted against `pg_index`
  at runtime): `WHERE id = <n>` is a seqscan. This is the apples-to-apples
  comparison with ckdbs.
* **PG `pk`** — `PRIMARY KEY` on `id`, btree index scan. What PostgreSQL
  actually does for anyone, and the target ckdbs's B+ tree has to beat.

Each PG variant was run twice: `synchronous_commit = on` (PostgreSQL's default;
every write waits for a WAL fsync) and `off`.

## 3. Client-visible throughput (qps, one connection)

| Phase | ckdbs | PG `noidx` durable | PG `pk` durable | PG `noidx` sync=off | PG `pk` sync=off |
|---|---|---|---|---|---|
| insert | **6,713** | 848 | 830 | 4,640 | 4,557 |
| point-select | 623 | 2,424 | **4,487** | 2,490 | 4,857 |
| full-scan | **305** | 92 | 93 | 107 | 110 |
| update | 643 | 688 | 830 | 2,389 | **4,531** |

Latency detail, same runs (microseconds, includes the Python client's round trip):

| Phase | ckdbs p50 / p95 / p99 | PG `noidx` durable | PG `pk` durable | PG `noidx` sync=off | PG `pk` sync=off |
|---|---|---|---|---|---|
| insert | 97 / 141 / 331 | 1142 / 1249 / 1419 | 1163 / 1338 / 1613 | 188 / 226 / 305 | 194 / 231 / 284 |
| point-select | 1520 / 1801 / 3801 | 380 / 503 / 573 | 198 / 268 / 461 | 378 / 491 / 582 | 195 / 226 / 277 |
| full-scan | 3233 / 3623 / 3924 | 10150 / 15234 / 18965 | 9948 / 14928 / 16283 | 9423 / 10155 / 10582 | 8831 / 10484 / 10519 |
| update | 1522 / 1679 / 2057 | 1376 / 1708 / 2367 | 1167 / 1306 / 1779 | 398 / 486 / 562 | 208 / 238 / 292 |
| sync / checkpoint | 206 | 35,137 | 16,814 | — | — |

Full-scan rows/s out: ckdbs **610k**, PG 184–220k. See caveat 3 — most of that
gap is the Python client, not the servers.

## 4. Engine cost with the client removed

Both servers log their own per-statement duration (ckdbs at `--log-level debug`,
PostgreSQL at `log_min_duration_statement = 0`). This is the honest comparison:
it excludes the socket, the Python driver and the reply formatting the client
does. p50 microseconds, 2000 rows resident, `SELECT` split by shape:

| Statement | ckdbs | PG `noidx` durable | PG `pk` durable | PG `noidx` sync=off | PG `pk` sync=off |
|---|---|---|---|---|---|
| INSERT | **13** | 1017 | 1032 | 67 | 72 |
| point SELECT | 1401 | 249 | **68** | 250 | 67 |
| full scan (2000 rows) | 2449 | 1649 | 1646 | n/a¹ | n/a¹ |
| UPDATE | 1415 | 1248 | 1040 | 276 | **87** |
| CREATE TABLE | 646 | 3309 | 2602 | 1478 | 1561 |
| SYNC / CHECKPOINT | 117 | 34,951 | 34,951 | — | — |

Per-row scan cost, server-side: ckdbs **1.22 us/row**, PostgreSQL **0.82
us/row**.

¹ Not broken out for the `sync=off` runs. `synchronous_commit` only affects
statements that commit a write, so the scan cost there is the durable columns'
— but the number was not measured separately, so it is not quoted as if it had
been.

## 5. What the numbers say

1. **The read path is the gap, and it is not about the index.** On identical
   work — no index on either side, seqscan vs page-chain scan — PostgreSQL is
   **5.6x faster** (249 us vs 1401 us server-side). With a primary key it is
   **20x** (68 us). The index will close most of that, but the indexless
   comparison says the scan itself is also slower per row (1.22 vs 0.82
   us/row), so a B+ tree alone will not land ckdbs at PostgreSQL's `pk`
   numbers.
2. **UPDATE is the read path wearing a hat.** ckdbs UPDATE costs 1415 us
   against a point SELECT's 1401 us: the in-place overwrite is ~14 us and
   everything else is finding the row. PostgreSQL's `pk` UPDATE at
   `synchronous_commit=off` is 87 us *including* writing a new row version.
   Nothing about ckdbs's update path needs work before its lookup path does.
3. **INSERT is genuinely fast, and genuinely not durable.** 13 us per insert
   is 5x PostgreSQL's non-durable insert (67 us) and 78x its durable one
   (1017 us) — but the ckdbs server appends nothing to its WAL, so the write
   is only in a dirty page frame until `SYNC`. The number to beat once WAL
   append and group commit land is the **1017 us** column, not the 67 us one.
   PostgreSQL pays ~950 us of fsync per statement here; that is what D1 strict
   will cost ckdbs too, and what D2 group commit exists to amortize.
4. **`SYNC` is cheap because there is little to flush.** 117 us vs
   PostgreSQL's 35 ms checkpoint — a page-store flush of one small relation
   against a server-wide checkpoint with WAL, `pg_control`, and full-page
   writes. Not a comparison, just a note on why the row is small.
5. **DDL is fast on ckdbs** (646 us vs 2.6–3.3 ms): catalog insert plus one
   page, against PostgreSQL's full system-catalog transaction. The catalog
   cache that landed 2026-07-30 is not exercised by this run in any way worth
   quoting — one `CREATE TABLE` per run.

## 6. Caveats — the run does not support claims beyond these

1. **No index on ckdbs.** Its `point-select` qps falls roughly as 1/rows.
   Comparing it to PG `pk` measures the absence of an index, not the engine.
   The comparable column is PG `noidx`.
2. **No WAL append on ckdbs.** Its write phases skip work PostgreSQL does on
   every statement. `synchronous_commit=off` is closer to today's ckdbs write
   path but is not a durable configuration on either side.
3. **The client floor is not symmetric on scans.** A ckdbs full scan returns
   one line; a PostgreSQL full scan returns 2000 `DataRow` messages, each one
   a separate framed read in `tools/pg_wire.py`. The wall-clock `full-scan`
   row (ckdbs 305 qps vs PG ~100) is therefore mostly a client artifact — the
   server-side row (2449 us vs 1649 us) is the real one. For cheap statements
   the Python round trip is ~100–200 us, which is larger than PostgreSQL's
   `pk` point select (68 us), so wall-clock qps there is measuring Python.
4. **One connection, no concurrency.** The ckdbs server serves one connection
   at a time, so this says nothing about scaling. Revisit when the
   thread-per-core scheduler lands; PostgreSQL's numbers here are its
   single-session numbers, not its throughput.
5. **Small working set.** 2000 rows fit in memory on both sides — no buffer
   pool pressure, no disk reads on the read path. Only the write phases touch
   storage.
6. **No VACUUM between PostgreSQL phases**, so its update phase leaves dead
   tuples the scan phases then walk. Its `noidx` scan numbers are slightly
   pessimistic for that reason.
7. **One run each, no repetitions.** p99/max columns on a 2-vCPU shared box
   include scheduler noise; treat sub-10 us differences as noise everywhere.

## 7. Reproducing this

```sh
# PostgreSQL: scratch cluster on :15433, defaults, trust auth on loopback
./tools/pg_setup.sh init
./tools/pg_setup.sh timing on            # log_min_duration_statement = 0

# ckdbs: release build, scratch data file, debug log for server-side timings
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release -j
./build-release/kds_server /tmp/bench.db --log-dir /tmp --log-file kds_bench.log \
    --log-level debug &

python3 tools/benchmark.py --rows 2000 --read-ops 1000 --scan-ops 20 \
    --update-ops 1000 --sync --json bench/results/ckdbs.json \
    --server-log /tmp/kds_bench.log

python3 tools/pg_benchmark.py --rows 2000 --read-ops 1000 --scan-ops 20 \
    --update-ops 1000 --analyze --sync --json bench/results/pg_sync_on.json \
    --server-log ~/pg-bench/pg.log

python3 tools/pg_benchmark.py --rows 2000 --read-ops 1000 --scan-ops 20 \
    --update-ops 1000 --analyze --synchronous-commit off \
    --json bench/results/pg_sync_off.json --server-log ~/pg-bench/pg.log

./tools/pg_setup.sh timing off            # logging costs write throughput
```

`--seed` is shared by both tools, so the same seed generates the same rows and
probes the same ids on both engines. `./tools/pg_setup.sh destroy --yes` removes
the cluster and nothing else.

## 8. What would make the next run better

* **A libpq/C++ driver in `bench/`** to remove the Python floor. Today only the
  server-side table (§4) is free of it, and that depends on both servers'
  debug logging being on, which itself costs throughput.
* **Larger scales** (100k, 1M rows) to show ckdbs's 1/rows read curve against
  PostgreSQL's flat indexed one, and to get both past their buffer pools.
* **Re-run after the B+ tree lands** — §5.1 and §5.2 are the two rows that
  should move, and PG `noidx` (249 us) then PG `pk` (68 us) are the two
  milestones in order.
* **Re-run after WAL append lands**, comparing against the durable PostgreSQL
  column, and again with D2 group commit to show what batching recovers.
