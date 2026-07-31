# Waystone vs PostgreSQL 17 — pk point access, 2026-07-30

The comparison `bench/results.md` could not make. That file's central caveat was
"ckdbs has no index today, so a `WHERE id = <n>` SELECT scans the whole page
chain" — which is why it ran PostgreSQL with indexes *disabled* to get an
apples-to-apples number. As of 2026-07-30 a Waystone-enabled relation has an
O(1) pk lookup on both `SELECT` and `UPDATE`, so the interesting comparison is
now against **PostgreSQL with its primary key**, doing the thing it actually
does for real users.

Raw output in `bench/results/pg-vs-waystone/*.json` (PostgreSQL) and
`bench/results/waystone/*.json` (ckdbs).

## 1. Setup

Same host, same row shape, same harness (`tools/bench_common.py`), same
single-connection one-request-at-a-time driver, same Python socket cost on both
sides.

| | ckdbs | PostgreSQL 17 |
|---|---|---|
| Build | Release | distro package, **stock config** |
| Port | 15670-15680 | 15433 (`tools/pg_setup.sh`) |
| Table | Keystone pk + `int64`, `int32`, `bool`, `varchar(16)` | same, `id bigint GENERATED ALWAYS AS IDENTITY` |
| pk access | `WAYSTONE ENABLE` | `PRIMARY KEY` (btree) |
| Data | `/`, xfs on EBS gp3 | same filesystem |

PostgreSQL is left at defaults on purpose. A hand-tuned baseline is not a
baseline — but "stock" cuts both ways and §5 says where it costs PostgreSQL.

**Server-side p50 is the column to read.** It is each engine's own measurement
of the whole statement, excluding the ~110-210 µs of Python round trip both
sides pay. Client qps is included because it is what a caller sees, but at
these latencies it mostly measures the socket.

## 2. Point SELECT by pk — `WHERE id = <n>`

Server-side p50:

| rows | ckdbs scan (no Waystone) | PostgreSQL seqscan | PostgreSQL **btree pk** | ckdbs **Waystone** |
|---|---|---|---|---|
| 1,000 | 562 µs | — | **69 µs** | **11 µs** |
| 10,000 | 5,177 µs | — | — | **12 µs** |
| 50,000 | 28,044 µs | — | **74 µs** | **12 µs** |

Client-visible qps, for the same runs:

| rows | ckdbs scan | PG seqscan | PG btree pk | ckdbs Waystone |
|---|---|---|---|---|
| 1,000 | 1,331 | 2,945 | 4,299 | **8,547** |
| 10,000 | 167 | 701 | 3,840 | **8,784** |
| 50,000 | 33 | 168 | 4,221 | **8,261** |

**ckdbs is ~6× faster than PostgreSQL's btree on this one operation** —
12 µs against 74 µs at 50,000 rows — and both are flat in row count, as any
real index must be. The ckdbs scan and the PostgreSQL seqscan are both linear
and both irrelevant to anyone who would enable an index; they are here to show
what the probe replaced.

That 6× is a narrow, real, and *expected* result rather than a surprise, and §5
is where the reasons live. It is not a claim that ckdbs reads 6× faster than
PostgreSQL in general — it does one thing, and this is that thing.

## 3. Point UPDATE by pk — `SET c_int = <n> WHERE id = <n>`

Server-side p50 at 50,000 rows:

| | ckdbs | PostgreSQL |
|---|---|---|
| durable (fsync per commit) | *not implemented* | **1,080 µs** (`synchronous_commit = on`) |
| non-durable | **12 µs** (UPDATE is unlogged) | **100 µs** (`synchronous_commit = off`) |

**Do not read the durable row as a win.** ckdbs `UPDATE` appends nothing to the
WAL at all — it is not a faster durable update, it is a *non-durable* one. The
honest comparison is the second row: 12 µs against 100 µs, both engines writing
to memory and deferring the fsync, which is again the ~8× that §5 explains.

When ckdbs `UPDATE` starts logging, its number will move to wherever its fsync
lands, exactly as `INSERT` already has (§4).

## 4. INSERT — where the gap closes completely

Server-side p50:

| | ckdbs | PostgreSQL |
|---|---|---|
| durable | **944-949 µs** (`durability = group`) | **1,048-1,051 µs** (`synchronous_commit = on`) |
| non-durable | **12 µs** (`durability = relaxed`) | **81 µs** (`synchronous_commit = off`) |

Client qps, durable: ckdbs 725-731, PostgreSQL 760-786. **PostgreSQL is
slightly ahead**, and both are simply measuring the same EBS fsync — ~1 ms on
this volume, which swamps every difference in the engines above it. This is the
row that should temper the other three: the moment a write has to reach a
platter, a 12-µs engine and an 81-µs engine are the same engine.

Neither side batches here. Both run one implicit transaction per statement over
one connection, so ckdbs `group` cannot form a batch and PostgreSQL cannot
group-commit either. Both would improve with concurrency, and PostgreSQL would
improve more, because it can actually serve concurrent connections today.

## 5. Why ckdbs wins the point lookups, stated plainly

The 6-8× on pk access is real and reproducible, and every one of these reasons
is a thing ckdbs does not yet have to do:

- **No MVCC snapshot.** PostgreSQL's index scan takes a snapshot, checks tuple
  visibility, and may follow HOT chains. ckdbs reads the slot. MVCC tuple
  headers exist in the format (`docs/wal.md` §5.1) but nothing evaluates
  visibility, so this cost is deferred, not eliminated.
- **No plan.** PostgreSQL parses, rewrites, plans and executes every statement
  here — the driver sends simple queries, not prepared statements, so no plan
  cache helps it. ckdbs re-parses too, but its "plan" for a pk equality is one
  `if`. The parser blueprint (`docs/parser.md`) makes this a deliberate
  property, not an accident of immaturity.
- **One arithmetic hop vs a tree descent.** `pk >> 8` for the page and
  `pk & 0xFF` for the slot, through a directory of at most 3 interior pages.
  A btree descends and compares. This is the actual architectural difference
  and the only one that survives the others being fixed.
- **No concurrency control.** No lock manager, no buffer-pool pin contention,
  no shared-memory coordination — because there is one thread and one
  connection.
- **No process/connection overhead.** PostgreSQL is a process per connection
  with shared buffers; ckdbs is a single loop.

A fair one-line summary: **on the one access pattern it has built,
pk point access, ckdbs is ~6-8× faster than stock PostgreSQL 17 server-side —
and it achieves that partly through a genuinely better addressing scheme and
partly by not yet implementing MVCC visibility, planning, or concurrency.**

## 6. What PostgreSQL does that this benchmark never asks for

Listed because a benchmark that omits them and a summary that omits the
omission are two different documents:

joins, subqueries, aggregates, `GROUP BY`, `ORDER BY`, secondary indexes,
range scans, multi-statement transactions, isolation levels, concurrent
readers and writers, crash recovery, replication, backup/PITR, `DELETE`,
`ALTER TABLE`, `DROP TABLE`, NULLs, `float`/`decimal`, constraints, foreign
keys, triggers, views, and every type outside the five this table uses.

ckdbs has **none** of these. Two are load-bearing for the numbers above:
recovery does not exist (so no ckdbs figure here is a durability claim beyond
the fsync it pays), and there is no `DELETE`, so no run above ever reclaims
space or exercises a delete-marked tuple.

## 7. Reproducing

```sh
./tools/pg_setup.sh init          # scratch PG 17 cluster on :15433
./tools/pg_setup.sh timing on     # log_min_duration_statement = 0

printf 'durability = relaxed\n' > /tmp/b.conf
./build-release/kds_server /home/ec2-user/bench.db --config /tmp/b.conf \
    --port 15670 --log-dir /home/ec2-user --log-file b.log --log-level debug &

python3 tools/benchmark.py    --port 15670 --rows 50000 --read-ops 500 \
    --update-ops 500 --waystone --server-log /home/ec2-user/b.log
python3 tools/pg_benchmark.py --port 15433 --database bench --rows 50000 \
    --read-ops 500 --update-ops 500 --index pk --server-log ~/pg-bench/pg.log
```

Do not put either data directory on tmpfs: `fsync` is free there and every
durability number becomes meaningless (`bench/results-waystone.md` §5).
