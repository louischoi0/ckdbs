# What DT9 costs a statement: the unfiltered catalog read, priced

DT9 (`docs/spec/ddl-transactional.md` §5a/§5b) changed one arm of the
catalog's `ScanAll`. A delete-marked catalog row used to count as deleted the
moment the mark was written; it now counts only once its deleter is no longer
in flight, which the read learns by calling
`txn::TransactionManager::IsInFlight` — a linear walk of the core's live
transaction list. The follow-up made `CommandDispatcher::EndDdlScope`
invalidate the catalog cache on **both** endings of a DDL-holding
transaction rather than on rollback alone, so a committed DDL transaction now
pays one `BumpVersion` it did not pay before.

The hypothesis put to this run was that both are unmeasurable on ordinary
statements, because `ScanAll` runs only on a catalog-cache miss and the new
branch runs only for delete-marked rows. **The hypothesis holds for every hot
path and fails for one cold one**, and the failure is not a constant: it is
the product of two quantities the engine currently bounds neither of.

Everything below was measured in the worktree
`/home/cdkbs/ckdbs/.claude/worktrees/enhence-details` on branch
`ddl-null-view`, with the change side pinned at **`5827973`** — whose engine
sources are byte-identical to **`04ae010`**, the commit the change was to be
measured at (`git diff 04ae010 5827973 -- src include` is empty; the two
commits between it and `04ae010` touch `docs/` only) — against the baseline
**`5ea7a2a`**, the commit this line branched from.

---

## The run

| | |
|---|---|
| Executed | 2026-08-18, 05:21–06:00 UTC (per-cell stamps in `~/bench-ddl9/out/*.txt`) |
| Worktree | `/home/cdkbs/ckdbs/.claude/worktrees/enhence-details` |
| Branch | `ddl-null-view` |
| Commit measured (B) | `5827973`; engine identical to `04ae010` |
| Commit measured (A) | `5ea7a2a` |
| Tree cleanliness | clean at both build points (`git status --short` empty) |
| B binary | `~/bench-ddl9/bin/kds_server-B-5827973`, sha256 `9bb64a34a30e48aabd84e8e87bae1d2051cd7ac9dfb51bf2ae56c1250d75d089`, built from `build-release/kds_server` mtime `2026-08-18 04:58:56 UTC` |
| A binary | `~/bench-ddl9/bin/kds_server-A-5ea7a2a`, sha256 `257f4f8796ece4880d44a83d5505739b4d7ca76bb57d7c33d3c2e71cf79c2e87`, built in a separate tree extracted with `git archive 5ea7a2a` into `~/bench-ddl9/srcA` |
| Test binary | `~/bench-ddl9/bin/kds_tests-5827973`, sha256 `e768c2f7c254f63cd521c2527a3b5397c7e174788c448fd6c3797e05c3429f83` |
| Driver | `tools/catalog_read_ab_benchmark.py`, sha256 `ade699f45aa9ac00c76e8738fa2208781cb98505211576e5e98d88818a0eb4e3`, snapshotted to `~/bench-ddl9/tools/` and unchanged across the run |
| Build type | **Release**, `-DCMAKE_BUILD_TYPE=Release -DKDS_WITH_TLS=ON` against a rootless OpenSSL 3.0.13 tree |
| Device | `/dev/root`, **ext4** (`df -T` at run time); data files under `~/bench-ddl9/`. **Not tmpfs** — the binaries are the only thing this run put on a scratch path |
| Host | 2 vCPU, 15 GiB, Linux 6.17.0-1022-azure, `USER_HZ` = 100 |
| Server config | `cores = 1`, `durability = relaxed` and `durability = group` (two matrices), `log_level = warn`, everything else default |
| Row-set sizes | 200 / 1,000 / 10,000, one fresh server pair and one fresh data-file pair per cell |
| Verify | passed on every cell: both sides' three relations carry identical row counts, and eight pk and eight indexed replies per cell agree byte for byte across the binaries |

**The binaries were snapshotted before the first cell and every server was
started from the copy.** That mattered more than usual here: the worktree's
`HEAD` moved four times while this run was in flight (`a82736b` →
`6fa171a` → `5827973` → `c535b39`), and `build-release` was rebuilt by
another session partway through. Neither could reach the measured engine,
because no cell ever executed the build tree's own binary.

**The box was shared, and the run says so rather than hiding it.** Another
session drove a `scenario3_library.py` matrix on the same two vCPUs for the
first twenty minutes of this session, and a full `kds_tests` rebuild ran
later. Both were waited out: `bench/wait_quiet.sh` gates every cell and the
driver refuses a host whose 1-minute average is above 1.5 or 5-minute above
2.0. One cell — the `rows=10000, marks=0` control of the attribution sweep —
**was refused by that gate** at a 5-minute average of 2.03 and was re-run
afterwards; its number below comes from the re-run.

How to reproduce every number: `bench/docs/README.md`, entries
`catalog_read_ab_benchmark.py` and `index_benchmark.py`.

---

## The noise floor, established from inside the run

`pk-select-again` is `pk-select` repeated — same server, same statements,
same relation, later in the same pass. `ping` is `SHOW META`, which resolves
no relation and cannot reach the changed code on either binary. Both are
controls by construction, and together they say what a delta has to clear.

| control | 200 | 1,000 | 10,000 |
|---|---|---|---|
| `pk-select-again` − `pk-select`, p50, side A | −0.3 µs | −0.9 µs | +3.2 µs |
| `pk-select-again` − `pk-select`, p50, side B | −0.2 µs | −0.8 µs | +3.8 µs |
| `ping`, B − A, p50 | +2.0 µs | −0.6 µs | −0.2 µs |

**The floor is ±4 µs at p50** and the 10,000-row cells set it. Nothing
smaller than that is reported below as a finding.

---

## Every hot path is unchanged

The seven arms a client actually issues — a round-trip floor, a pk point
SELECT, an indexed equality, `SHOW TABLES`, and INSERTs into an indexed and
an index-free relation — move by less than the floor at every row-set size.
`durability = relaxed`, so no fsync sits on the statement path and a few
microseconds of engine work would be visible if there were any.

Each cell holds 600 delete-marked catalog rows by the time the run ends, and
the read arms run *before* the drops that create them; the cold section below
is where the marks are present and the cache is not.

`A / B` in every cell, microseconds. `Δp50` is B − A.

| arm | rows | ops/side | p0 | p25 | p50 | p95 | p99 | Δp50 |
|---|---|---|---|---|---|---|---|---|
| `ping` | 200 | 2500 | 21.7 / 21.6 | 22.2 / 22.4 | 28.0 / 30.0 | 35.1 / 35.6 | 42.0 / 42.1 | +2.0 |
| `ping` | 1000 | 2500 | 21.9 / 21.9 | 22.5 / 22.4 | 30.0 / 29.4 | 35.2 / 35.2 | 43.5 / 41.5 | −0.6 |
| `ping` | 10000 | 2500 | 22.0 / 21.8 | 22.4 / 22.2 | 29.4 / 29.2 | 33.8 / 34.0 | 41.8 / 41.6 | −0.2 |
| `pk-select` | 200 | 2500 | 25.4 / 25.6 | 33.6 / 34.7 | 35.5 / 35.6 | 45.4 / 44.3 | 56.4 / 60.8 | +0.1 |
| `pk-select` | 1000 | 2500 | 25.4 / 25.6 | 34.8 / 35.4 | 36.5 / 36.9 | 47.7 / 48.3 | 64.9 / 60.8 | +0.4 |
| `pk-select` | 10000 | 2500 | 25.6 / 25.0 | 34.2 / 27.1 | 35.5 / 35.4 | 45.8 / 44.7 | 56.9 / 55.1 | −0.1 |
| `pk-select-again` | 200 | 2500 | 25.3 / 25.4 | 33.9 / 34.4 | 35.2 / 35.4 | 43.4 / 41.3 | 53.7 / 63.1 | +0.2 |
| `pk-select-again` | 1000 | 2500 | 25.8 / 26.1 | 34.6 / 35.1 | 35.6 / 36.1 | 44.0 / 44.8 | 57.6 / 57.4 | +0.5 |
| `pk-select-again` | 10000 | 2500 | 28.2 / 26.6 | 36.7 / 37.0 | 38.7 / 39.2 | 47.6 / 49.9 | 66.9 / 64.2 | +0.5 |
| `idx-probe` | 200 | 2500 | 28.0 / 28.1 | 32.1 / 32.3 | 39.0 / 39.4 | 49.5 / 48.7 | 74.7 / 71.7 | +0.4 |
| `idx-probe` | 1000 | 2500 | 28.5 / 29.4 | 38.6 / 40.2 | 41.1 / 41.7 | 50.9 / 50.7 | 67.7 / 75.7 | +0.6 |
| `idx-probe` | 10000 | 2500 | 32.5 / 36.0 | 41.5 / 41.8 | 43.2 / 43.3 | 52.0 / 51.1 | 66.6 / 61.3 | +0.1 |
| `show-tables` | 200 | 2500 | 21.1 / 21.1 | 21.4 / 21.4 | 21.8 / 21.9 | 31.1 / 31.4 | 38.1 / 37.8 | +0.1 |
| `show-tables` | 1000 | 2500 | 21.1 / 21.0 | 22.6 / 21.7 | 29.4 / 29.0 | 33.5 / 34.1 | 40.2 / 40.7 | −0.4 |
| `show-tables` | 10000 | 2500 | 21.2 / 21.0 | 29.0 / 28.9 | 29.4 / 29.6 | 34.6 / 34.4 | 41.7 / 44.5 | +0.2 |
| `ins-idx` | 200 | 400 | 30.3 / 24.7 | 33.5 / 33.1 | 34.0 / 34.0 | 42.7 / 45.4 | 57.5 / 56.8 | +0.0 |
| `ins-idx` | 1000 | 400 | 25.1 / 24.9 | 32.6 / 26.0 | 34.5 / 33.3 | 45.8 / 45.0 | 63.1 / 62.4 | −1.2 |
| `ins-idx` | 10000 | 400 | 31.3 / 30.0 | 33.4 / 33.4 | 33.9 / 35.0 | 41.4 / 45.3 | 54.8 / 59.5 | +1.1 |
| `ins-plain` | 200 | 400 | 23.4 / 23.1 | 25.8 / 24.1 | 32.4 / 31.7 | 40.4 / 40.9 | 56.2 / 50.5 | −0.7 |
| `ins-plain` | 1000 | 400 | 23.8 / 23.8 | 24.3 / 24.2 | 30.7 / 25.7 | 38.6 / 39.1 | 50.2 / 49.1 | −5.0 |
| `ins-plain` | 10000 | 400 | 25.1 / 29.3 | 31.7 / 32.3 | 32.3 / 32.7 | 41.3 / 42.7 | 49.5 / 51.6 | +0.4 |

Two rows deserve a word rather than a shrug. `ins-plain` at 1,000 rows reads
−5.0 µs, which is *B faster than A* — a delta of that sign on a relation with
no secondary index, where the changed branch cannot execute at all, is what
the floor looks like at 400 operations, not an improvement. And `show-tables`
is flat despite being the one arm that runs an unfiltered
`ScanAll<SysObjectRow>` on **every** statement rather than on a cache miss:
`ListTables` walks sys.objects each time, and DT9 still costs it nothing,
because sys.objects carries no delete-marks (a `DROP TABLE` *retypes* that
row in place — §5a's limit — while the marks land on sys.tables and
sys.columns).

Server CPU, sampled from `/proc/<pid>/stat` over contiguous windows of 15,000
operations per side per arm, agrees and adds nothing:

| arm | rows | ops/side | A µs/op | B µs/op | Δ |
|---|---|---|---|---|---|
| `ping` | 1000 | 15000 | 14.7 | 13.3 | −1.3 |
| `pk-select` | 1000 | 15000 | 19.3 | 20.0 | +0.7 |
| `pk-select-again` | 1000 | 15000 | 20.0 | 20.0 | +0.0 |
| `idx-probe` | 1000 | 15000 | 26.7 | 27.3 | +0.7 |
| `show-tables` | 1000 | 15000 | 16.7 | 16.0 | −0.7 |
| `ping` | 10000 | 15000 | 13.3 | 14.7 | +1.3 |
| `pk-select` | 10000 | 15000 | 21.3 | 20.0 | −1.3 |
| `idx-probe` | 10000 | 15000 | 27.3 | 29.3 | +2.0 |
| `show-tables` | 10000 | 15000 | 16.0 | 15.3 | −0.7 |

The meter's own resolution is the caveat: one scheduler tick is 10 ms, so a
15,000-operation window resolves 0.67 µs/op and every number above is one or
two ticks of quantization. **It cannot resolve a sub-µs effect and is not
claimed to** — it is here to rule out a *large* CPU cost that the wall clock
might have hidden behind the socket, and it does.

---

## DDL is unchanged too, including the commit that now invalidates

`04ae010` made a committed DDL transaction pay a `BumpVersion` — a cache
clear plus the `on_invalidate_` hook, which flushes the catalog pages and
broadcasts `kCatalogInvalidate`. That is a real new cost on a real path, so
it was measured on its own: a cell with every read and write arm switched
off and the DDL arms given 150 operations each, at all three sizes.

**`txn-commit` does not move.** Its p50 delta is −0.1 / −0.5 / +0.2 µs across
the three sizes, against a `txn-rollback` control — the ending that paid the
invalidation on *both* binaries — of −0.3 / −0.4 / +0.4 µs. The two are
indistinguishable, which is exactly what "the commit now pays what the
rollback always paid, and that was never measurable" looks like.

| arm | rows | ops/side | p0 | p25 | p50 | p95 | p99 | Δp50 |
|---|---|---|---|---|---|---|---|---|
| `ddl-create` | 200 | 150 | 38.0 / 37.4 | 44.2 / 44.2 | 48.3 / 47.6 | 62.3 / 62.3 | 70.0 / 68.3 | −0.7 |
| `ddl-create` | 1000 | 150 | 36.7 / 37.4 | 44.4 / 44.1 | 48.2 / 47.9 | 63.0 / 58.4 | 68.7 / 74.6 | −0.3 |
| `ddl-create` | 10000 | 150 | 29.7 / 30.3 | 34.8 / 35.7 | 43.3 / 43.9 | 58.8 / 60.3 | 83.6 / 69.4 | +0.6 |
| `ddl-cidx` | 200 | 150 | 46.3 / 44.3 | 66.0 / 64.9 | 77.0 / 76.7 | 99.7 / 101.3 | 109.2 / 122.2 | −0.3 |
| `ddl-cidx` | 1000 | 150 | 43.9 / 44.2 | 64.8 / 65.5 | 76.1 / 79.2 | 99.3 / 100.9 | 109.0 / 117.6 | +3.1 |
| `ddl-cidx` | 10000 | 150 | 35.6 / 32.3 | 56.9 / 58.0 | 69.0 / 70.5 | 100.4 / 97.3 | 198.7 / 104.4 | +1.5 |
| `ddl-didx` | 200 | 150 | 31.3 / 26.8 | 34.7 / 34.4 | 36.3 / 36.6 | 40.3 / 41.0 | 50.3 / 47.7 | +0.3 |
| `ddl-didx` | 1000 | 150 | 29.0 / 26.8 | 34.4 / 33.9 | 36.5 / 36.4 | 40.4 / 43.2 | 50.9 / 54.5 | −0.1 |
| `ddl-didx` | 10000 | 150 | 24.0 / 23.8 | 26.3 / 26.7 | 27.6 / 28.0 | 31.6 / 32.1 | 41.9 / 44.7 | +0.4 |
| `txn-begin` | 200 | 150 | 22.7 / 27.6 | 30.1 / 30.3 | 31.2 / 31.0 | 40.4 / 33.7 | 59.4 / 47.4 | −0.2 |
| `txn-begin` | 1000 | 150 | 27.8 / 27.3 | 29.8 / 29.5 | 31.0 / 30.8 | 35.8 / 34.0 | 44.3 / 41.3 | −0.2 |
| `txn-begin` | 10000 | 150 | 21.1 / 21.2 | 27.3 / 25.2 | 29.8 / 29.8 | 35.6 / 41.1 | 60.7 / 53.2 | +0.0 |
| `txn-create` | 200 | 150 | 49.3 / 46.8 | 60.7 / 59.5 | 64.4 / 63.0 | 80.9 / 76.7 | 97.5 / 89.0 | −1.4 |
| `txn-create` | 1000 | 150 | 46.7 / 47.7 | 61.2 / 59.7 | 64.8 / 64.0 | 82.4 / 75.0 | 102.6 / 95.8 | −0.8 |
| `txn-create` | 10000 | 150 | 38.3 / 38.6 | 52.0 / 53.0 | 61.4 / 62.1 | 80.4 / 81.8 | 87.7 / 90.1 | +0.7 |
| **`txn-commit`** | 200 | 150 | 28.0 / 26.0 | 30.4 / 30.2 | 31.2 / 31.1 | 39.2 / 33.9 | 47.7 / 39.8 | **−0.1** |
| **`txn-commit`** | 1000 | 150 | 24.6 / 23.9 | 30.1 / 30.0 | 31.1 / 30.6 | 36.9 / 39.2 | 42.2 / 46.6 | **−0.5** |
| **`txn-commit`** | 10000 | 150 | 21.6 / 22.4 | 23.0 / 23.2 | 28.9 / 29.1 | 34.2 / 34.3 | 51.0 / 41.1 | **+0.2** |
| `txn-rollback` | 200 | 150 | 22.2 / 22.4 | 29.9 / 30.2 | 31.6 / 31.3 | 40.4 / 40.3 | 480.5 / 48.6 | −0.3 |
| `txn-rollback` | 1000 | 150 | 23.8 / 26.3 | 31.5 / 31.1 | 32.2 / 31.8 | 36.4 / 35.4 | 44.6 / 40.1 | −0.4 |
| `txn-rollback` | 10000 | 150 | 22.0 / 22.3 | 24.6 / 24.5 | 29.5 / 29.9 | 34.8 / 32.8 | 41.8 / 33.9 | +0.4 |
| `drop-txn` | 200 | 20 | 507.8 / 501.5 | 511.4 / 513.5 | 516.7 / 521.2 | 529.4 / 651.3 | 648.4 / 843.4 | +4.5 |
| `drop-txn` | 1000 | 20 | 508.8 / 506.7 | 515.7 / 511.9 | 525.0 / 519.2 | 538.5 / 548.5 | 654.4 / 762.4 | −5.8 |
| `drop-txn` | 10000 | 20 | 502.2 / 505.7 | 510.8 / 510.5 | 517.6 / 515.7 | 560.5 / 552.4 | 645.9 / 639.0 | −1.9 |
| `drop-commit` | 200 | 20 | 29.7 / 29.8 | 30.8 / 31.0 | 32.1 / 32.6 | 43.8 / 51.2 | 102.3 / 454.9 | +0.5 |
| `drop-commit` | 1000 | 20 | 24.4 / 30.9 | 31.8 / 31.5 | 32.6 / 32.9 | 36.1 / 39.7 | 44.0 / 40.0 | +0.3 |
| `drop-commit` | 10000 | 20 | 29.2 / 29.9 | 31.3 / 31.2 | 32.3 / 32.9 | 34.5 / 41.2 | 34.7 / 43.8 | +0.6 |

`ddl-cidx` creates its index on an **empty** relation on purpose, so the arm
prices the catalog path and not a backfill; that is why it is 77 µs and not a
function of `--rows`. `drop-txn` is 20 operations per side and its ±5 µs
swing is sample size, not signal — it is here because it is the statement
that *creates* the delete-marks the next section depends on, and because its
absolute cost, ~517 µs against `ddl-create`'s ~48 µs, is the single most
expensive catalog statement in the engine.

---

## The one place it does cost: a cold catalog with delete-marks and open transactions

`cold-pk-select` and `cold-ins-idx` put an untimed `ALTER TABLE … RENAME
COLUMN` in front of every timed statement. That is one `BumpVersion`, so the
timed statement resolves its relation from an empty cache and pays five
unfiltered `ScanAll`s — `FindTableOidByName`, `GetSysTableRow`,
`ScanSchemaFromColumns`, `ListCabins`/`ListForeignKeys` and `ListIndexes` —
over a catalog that at this point holds ~180 relations' rows, 600 of them
delete-marked by 100 transactional `DROP TABLE`s (one `sys.tables` row and
five `sys.columns` rows each). The `-live` variants repeat both arms with 32
idle `BEGIN`s parked on their own connections.

**With no transaction open, DT9 costs 0–3 µs on a cold catalog read: inside
the floor. With 32 open, it costs 8–11 µs at every percentile and every
row-set size: outside it, and consistently.**

`durability = relaxed`, `A / B`, microseconds:

| arm | rows | ops/side | p0 | p25 | p50 | p95 | p99 | Δp50 |
|---|---|---|---|---|---|---|---|---|
| `cold-pk-select` | 200 | 400 | 69.7 / 69.7 | 78.8 / 79.2 | 82.9 / 83.0 | 92.9 / 93.3 | 97.9 / 107.6 | +0.1 |
| `cold-pk-select` | 1000 | 400 | 68.3 / 69.5 | 72.8 / 75.7 | 77.1 / 78.6 | 93.3 / 95.1 | 509.3 / 604.4 | +1.5 |
| `cold-pk-select` | 10000 | 400 | 69.0 / 69.0 | 76.8 / 78.7 | 80.1 / 81.9 | 95.8 / 96.1 | 132.6 / 117.1 | +1.8 |
| `cold-ins-idx` | 200 | 400 | 72.2 / 72.0 | 76.2 / 76.7 | 79.7 / 80.0 | 90.4 / 91.4 | 106.9 / 113.0 | +0.3 |
| `cold-ins-idx` | 1000 | 400 | 67.1 / 70.8 | 74.6 / 77.6 | 77.9 / 80.7 | 90.3 / 93.4 | 129.5 / 128.7 | +2.8 |
| `cold-ins-idx` | 10000 | 400 | 68.0 / 68.1 | 76.3 / 76.5 | 79.6 / 79.8 | 93.4 / 91.9 | 112.0 / 116.2 | +0.2 |
| **`cold-pk-select-live`** | 200 | 400 | 74.8 / 78.4 | 78.8 / 87.4 | 83.2 / 91.2 | 93.8 / 105.5 | 110.2 / 125.1 | **+8.0** |
| **`cold-pk-select-live`** | 1000 | 400 | 74.7 / 84.9 | 78.8 / 89.6 | 82.6 / 93.0 | 94.4 / 103.2 | 107.6 / 113.1 | **+10.4** |
| **`cold-pk-select-live`** | 10000 | 400 | 69.9 / 77.4 | 79.8 / 88.5 | 82.5 / 91.9 | 94.9 / 103.7 | 105.1 / 131.9 | **+9.4** |
| **`cold-ins-idx-live`** | 200 | 400 | 68.2 / 78.0 | 76.6 / 84.4 | 80.3 / 88.4 | 91.6 / 99.7 | 103.9 / 106.2 | **+8.1** |
| **`cold-ins-idx-live`** | 1000 | 400 | 70.7 / 81.0 | 76.2 / 88.2 | 79.9 / 91.0 | 90.7 / 105.1 | 103.0 / 121.6 | **+11.1** |
| **`cold-ins-idx-live`** | 10000 | 400 | 68.8 / 75.3 | 76.9 / 82.9 | 79.3 / 86.9 | 94.1 / 99.9 | 131.1 / 112.3 | **+7.6** |

The delta is **flat in p0, p25, p50 and p95 alike** — +7 to +12 µs at all
four — which is the signature of a fixed amount of added work rather than a
tail effect or a scheduling artefact. It is also flat in `--rows`: the
catalog does not grow with the relation, so neither does the cost. And it is
present at both durability classes; the `group` matrix reads +7.8 / +9.4 /
+8.2 µs on `cold-pk-select-live`, within a microsecond of `relaxed`, because
a SELECT commits nothing and the fsync never enters this arm at all.

---

## Attribution: the cost is `delete-marked rows × live transactions`

If the +9 µs is `IsInFlight`, then removing either factor must remove it and
doubling either must double it. Both were varied against a fixed statement,
a fixed relation and a fixed catalog shape. `marks = 0` is the decisive
control: with no delete-marked row there is nothing for the new branch to
ask about, so a delta that survived it would not be DT9's.

`cold-pk-select-live`, `durability = relaxed`, `A / B` in microseconds:

| rows | delete-marked rows | live txns | ops/side | p0 | p25 | p50 | p95 | p99 | Δp50 |
|---|---|---|---|---|---|---|---|---|---|
| 200 | **0** | 32 | 500 | 38.0 / 38.9 | 42.0 / 42.4 | 46.5 / 46.4 | 55.3 / 54.1 | 65.4 / 62.9 | **−0.1** |
| 200 | 300 | 32 | 500 | 37.4 / 44.4 | 46.9 / 51.4 | 50.6 / 55.1 | 58.3 / 64.3 | 66.0 / 79.5 | +4.5 |
| 200 | 600 | 32 | 500 | 40.7 / 49.5 | 50.1 / 59.2 | 52.2 / 62.1 | 62.7 / 73.4 | 82.1 / 93.1 | +9.9 |
| 200 | 1200 | 32 | 500 | 52.2 / 74.5 | 60.4 / 79.3 | 64.3 / 82.6 | 76.8 / 95.2 | 275.0 / 101.0 | +18.3 |
| 200 | 600 | **8** | 500 | 46.3 / 43.1 | 50.1 / 52.0 | 51.9 / 54.8 | 62.9 / 67.5 | 71.6 / 89.1 | **+2.9** |
| 1000 | **0** | 32 | 500 | 32.6 / 32.7 | 42.8 / 43.2 | 46.0 / 46.9 | 56.2 / 57.1 | 66.9 / 76.1 | **+0.9** |
| 1000 | 300 | 32 | 500 | 38.0 / 43.0 | 48.0 / 52.1 | 51.8 / 55.7 | 60.0 / 65.6 | 90.3 / 83.9 | +3.9 |
| 1000 | 600 | 32 | 500 | 41.9 / 48.9 | 50.8 / 55.3 | 53.2 / 60.1 | 72.3 / 85.0 | 298.8 / 262.8 | +6.9 |
| 1000 | 1200 | 32 | 500 | 50.3 / 66.8 | 53.9 / 71.2 | 59.0 / 75.6 | 74.2 / 93.8 | 88.8 / 108.0 | +16.6 |
| 1000 | 600 | **8** | 500 | 40.2 / 42.9 | 43.3 / 46.0 | 48.2 / 51.1 | 62.2 / 62.5 | 76.2 / 87.4 | **+2.9** |
| 10000 | **0** | 32 | 500 | 31.5 / 31.9 | 40.8 / 41.5 | 42.7 / 44.8 | 57.9 / 56.1 | 69.0 / 75.9 | **+2.1** |
| 10000 | 300 | 32 | 500 | 43.5 / 46.8 | 47.6 / 52.6 | 51.7 / 56.7 | 60.9 / 65.0 | 75.9 / 84.5 | +5.0 |
| 10000 | 600 | 32 | 500 | 48.2 / 54.9 | 51.3 / 61.2 | 55.9 / 65.2 | 63.2 / 74.2 | 71.6 / 80.7 | +9.3 |
| 10000 | 1200 | 32 | 500 | 49.9 / 71.5 | 60.4 / 81.2 | 64.1 / 84.2 | 74.8 / 98.2 | 84.1 / 145.7 | +20.1 |
| 10000 | 600 | **8** | 500 | 41.3 / 42.2 | 43.7 / 45.1 | 48.9 / 50.4 | 62.2 / 65.5 | 86.2 / 83.3 | **+1.5** |

`cold-ins-idx-live` reproduces it independently on the write path: +0.4 /
−0.1 / +0.4 at zero marks, then +4.2 / +3.7 / +4.4 at 300, +10.5 / +7.5 /
+9.2 at 600 and +19.3 / +16.4 / +17.5 at 1,200 for the three sizes.

Read down either column and the shape is unmistakable. Doubling the marks
doubles the delta (300 → 600 → 1,200 gives ≈4.5 → 9.4 → 18.3 µs); dropping
the live count from 32 to 8 cuts it by about four (9.4 → 2.4 µs at 600
marks); setting the marks to zero erases it entirely, at all three row-set
sizes, including the size whose control cell the load gate refused the first
time and which was re-run for exactly this row.

Fitting `Δ = marks × (c₀ + c₁ × live)` on the two live counts — 14.9 ns per
mark at 32 live, 4.1 ns per mark at 8, each averaged over the three row-set
sizes — gives **c₁ ≈ 0.45 ns** per live transaction walked, one predictable
comparison behind a `std::unique_ptr` dereference, and **c₀ ≈ 0.4 ns** for
reaching `IsInFlight` and early-outing. At 1,200 marks and 32 live
transactions the model predicts 1,200 × 14.9 ns ≈ 17.9 µs against 16.6–20.1
measured, and at 300 marks 4.5 µs against 3.9–5.0. The model is the loop, and
nothing else is in it. The zero-live rows bound c₀ rather than measuring it:
`cold-pk-select` at 600 marks reads +0.1 / +1.5 / +1.8 µs, which is inside
the floor and consistent with anything below ~3 ns per mark.

---

## Where the time goes

A latency here is a sum. The arms were chosen so that the sum can be
decomposed by subtraction rather than guessed, and the numbers are side B's
p50 at 1,000 rows.

| wait | how it is isolated | relaxed | group |
|---|---|---|---|
| client + socket round trip | `ping` (`SHOW META`) — no relation resolved, no catalog read, no page touched | 29.4 µs | 30.0 µs |
| read statement, warm catalog | `pk-select` − `ping` — parse, compile, one clustered descent, render | +7.5 µs | +6.3 µs |
| secondary-index descent | `idx-probe` − `pk-select` — one `IndexProbe`, 7 entries scanned and resolved | +4.8 µs | +6.0 µs |
| write statement, no durability | `ins-idx` − `ping` at `relaxed` — heap append, index maintenance, WAL append without fsync | +3.9 µs | — |
| **durability / commit (fsync)** | `ins-idx` at `group` − `ins-idx` at `relaxed` | — | **+1030 µs** |
| catalog resolution, cold | `cold-pk-select` − `pk-select` — five unfiltered `ScanAll`s over ~180 relations' rows | +41.7 µs | +47.0 µs |
| **DT9's share of that** | `cold-pk-select-live` B − A at 600 marks / 32 live | **+10.4 µs** | **+9.4 µs** |
| lock or conflict wait | **does not apply**: one connection per side, no contended row, zero `TXN_CONFLICT` replies in any cell | — | — |

Two of those deserve naming. The **durability wait dominates every write**:
1,030 µs of a 1,063 µs indexed INSERT at `group` is the fsync, so a
per-statement engine change of even 20 µs is 2% of what a client waits for on
a write and is invisible without the `relaxed` matrix beside it. And the
**cold catalog resolution costs more than the statement it precedes** — 42 µs
of catalog walk in front of 7.5 µs of actual read work — which is the context
that makes DT9's 10 µs a quarter of an already-large number rather than a
quarter of a small one.

---

## Versus PostgreSQL

DT9's arms have no PostgreSQL twin and one would not be meaningful: they
price a KDS-internal catalog structure with no counterpart there, and
`pg_catalog` lookups are served from each backend's relcache with no route
to invalidate them per statement from SQL. **Stated rather than silently
omitted**, and named so it is not mistaken for an oversight: a
`tools/pg_catalog_read_ab_benchmark.py` would have to invalidate the relcache
per statement, which from a client means DDL on the relation being read, so
it would price PostgreSQL's DDL and not its catalog read. There is no honest
twin for this measurement and none should be built.

What *does* have a twin is every statement shape those arms carry, and the
baseline for them is `index_benchmark.py` against `pg_index_benchmark.py` —
the twin imports its schema, row generator and shape list from the ckdbs
driver, so the two cannot drift. Both sides were run at 200 / 1,000 / 10,000
rows, sequentially and never at the same time, against the port-15433
cluster at **PostgreSQL's own defaults** (nothing tuned, on purpose), on the
same B binary this document measures. `ckdbs / postgres` in microseconds;
the last column is PostgreSQL's p50 as a multiple of ckdbs's.

| shape | rows | ops | p0 | p25 | p50 | p95 | p99 | pg/ck |
|---|---|---|---|---|---|---|---|---|
| `ping` (round-trip floor) | 200 | 300 | 20.0 / 28.5 | 20.2 / 30.4 | 20.2 / 36.8 | 22.2 / 55.5 | 30.7 / 69.1 | 1.82× |
| `ping` | 1000 | 300 | 20.2 / 33.3 | 20.3 / 36.4 | 20.4 / 37.0 | 22.6 / 46.3 | 31.1 / 60.3 | 1.81× |
| `ping` | 10000 | 300 | 20.1 / 30.0 | 20.3 / 36.1 | 20.3 / 37.9 | 31.0 / 49.3 | 61.5 / 61.4 | 1.87× |
| pk point SELECT | 200 | 300 | 27.1 / 59.1 | 29.0 / 61.7 | 36.0 / 63.4 | 55.4 / 82.6 | 62.3 / 102.3 | 1.76× |
| pk point SELECT | 1000 | 300 | 26.9 / 57.9 | 28.5 / 65.3 | 30.6 / 68.4 | 43.1 / 86.3 | 55.5 / 102.6 | 2.24× |
| pk point SELECT | 10000 | 300 | 28.0 / 60.1 | 29.9 / 65.9 | 33.1 / 68.4 | 50.9 / 85.7 | 57.9 / 102.2 | 2.07× |
| indexed equality | 200 | 300 | 31.1 / 62.9 | 35.1 / 68.1 | 37.8 / 70.5 | 59.5 / 85.9 | 74.6 / 94.6 | 1.87× |
| indexed equality | 1000 | 300 | 32.4 / 70.3 | 35.6 / 77.0 | 37.2 / 80.3 | 45.7 / 98.2 | 57.1 / 114.0 | 2.16× |
| indexed equality | 10000 | 300 | 33.7 / 62.3 | 41.2 / 81.5 | 44.1 / 85.3 | 62.9 / 99.5 | 73.5 / 106.4 | 1.93× |
| indexed equality, repeat | 200 | 300 | 30.5 / 62.3 | 34.1 / 67.2 | 36.0 / 69.9 | 55.1 / 88.2 | 76.3 / 101.1 | 1.94× |
| indexed equality, repeat | 1000 | 300 | 31.5 / 69.1 | 34.6 / 75.3 | 35.8 / 78.2 | 40.3 / 94.1 | 49.4 / 108.8 | 2.18× |
| indexed equality, repeat | 10000 | 300 | 33.1 / 61.4 | 39.5 / 79.5 | 42.3 / 83.0 | 63.1 / 108.5 | 92.1 / 121.5 | 1.96× |
| unindexed equality | 200 | 300 | 39.4 / 60.1 | 41.2 / 64.6 | 43.2 / 67.5 | 62.5 / 86.9 | 74.7 / 97.2 | 1.56× |
| unindexed equality | 1000 | 300 | 81.7 / 95.3 | 83.5 / 102.8 | 84.4 / 106.4 | 97.0 / 124.2 | 101.2 / 139.9 | 1.26× |
| unindexed equality | 10000 | 300 | 560.4 / 400.4 | 569.4 / 411.1 | 577.7 / 418.6 | 623.4 / 438.4 | 727.6 / 450.4 | **0.72×** |
| INSERT, 0 indexes | 200 | 200 | 874.9 / 914.1 | 976.8 / 1014.1 | 1010.6 / 1225.6 | 1787.7 / 2095.4 | 2400.2 / 3047.1 | 1.21× |
| INSERT, 0 indexes | 1000 | 1000 | 865.7 / 891.2 | 971.9 / 988.1 | 1024.3 / 1021.0 | 1589.6 / 1228.3 | 2815.1 / 2028.2 | 1.00× |
| INSERT, 0 indexes | 10000 | 10000 | 830.7 / 864.0 | 928.5 / 1006.8 | 961.4 / 1068.9 | 1394.8 / 2145.8 | 2792.2 / 3326.5 | 1.11× |
| INSERT, 1 index | 200 | 200 | 909.7 / 911.5 | 970.3 / 1031.2 | 1012.2 / 1137.1 | 1502.3 / 2081.4 | 2922.6 / 3537.6 | 1.12× |
| INSERT, 1 index | 1000 | 1000 | 879.2 / 912.6 | 980.0 / 988.9 | 1030.9 / 1022.5 | 1422.8 / 1258.1 | 2329.7 / 1948.7 | 0.99× |
| INSERT, 1 index | 10000 | 10000 | 832.0 / 883.8 | 931.8 / 1008.6 | 966.5 / 1073.0 | 1418.8 / 2144.0 | 2656.1 / 3350.8 | 1.11× |
| INSERT, 2 indexes | 200 | 200 | 888.6 / 932.2 | 964.2 / 1033.7 | 1014.1 / 1186.0 | 2092.6 / 2144.0 | 3046.1 / 2617.7 | 1.17× |
| INSERT, 2 indexes | 1000 | 1000 | 873.3 / 898.6 | 980.7 / 992.4 | 1036.4 / 1027.2 | 1477.9 / 1271.3 | 3307.7 / 2114.1 | 0.99× |
| INSERT, 2 indexes | 10000 | 10000 | 833.9 / 890.6 | 934.4 / 1012.0 | 970.2 / 1076.4 | 1433.3 / 2127.7 | 2744.6 / 3120.3 | 1.11× |

PostgreSQL 16.14, `~/pg-bench`, port 15433, defaults. Cell metadata in
`~/bench-ddl9/out/idx-pg-*.json`.

The comparison's use here is calibration, not a scoreboard: **DT9's 10 µs is
a quarter of the 38 µs gap between the two engines' pk point lookups**, which
is what makes it a number worth reporting rather than a curiosity. Read
across the sizes and the familiar shape returns — ckdbs is 1.8–2.2× faster on
every indexed read and every point lookup — and the `ping` row says how much
of that is access path and how much is floor: PostgreSQL's own `SELECT 1`
already costs 37.0 µs against ckdbs's 20.4 µs, so 17 of the 38 µs gap on a
1,000-row pk lookup is protocol and backend overhead rather than the read
itself. It loses the *unindexed* scan at 10,000 rows (0.72×),
where a sequential scan through PostgreSQL's shared buffers beats walking the
clustered chain. Writes are a tie at every size, because both are one fsync
with a statement attached.

---

## What this says about the engine

**1. The catalog cache is doing the work the design credits it with, and DT9
rides entirely inside it.** Every hot arm is flat to within the floor
including `SHOW TABLES`, which does an unfiltered `ScanAll` per statement.
The only way to make DT9 cost anything at all was to defeat the cache
deliberately, once per statement, with a `BumpVersion` the workload does not
otherwise contain. `docs/spec/ddl-transactional.md`'s claim that a view is
minted only while some transaction holds uncommitted DDL, so the cache fast
path is untouched, is confirmed by measurement here for the first time.

**2. The cost that exists is a product of two unbounded quantities, and
that is the finding rather than the 10 µs.** Δ ≈ `marks × (0.4 ns + 0.45 ns ×
live)`. `live` is bounded — `kMaxTrackedLiveTxns` caps it at 64 active, so
the per-mark cost tops out near 29 ns. **`marks` is not bounded by anything
in the engine at the commit measured.** A delete-marked catalog row is never
purged (`docs/spec/txn.md`'s no-purge gap), so a database that has run
transactional `DROP TABLE` a thousand times carries 6,000 marks and pays,
by the fit above, ~180 µs on every cold catalog resolution at 64 live
transactions, and ~3 µs at zero. That is not a per-statement cost today
because resolution is cached; it becomes one for any workload that
invalidates the catalog often — a DDL-heavy setup phase, or a session
mixing DDL with reads. **The right lever is retiring the marks, not making
the walk faster**, and DT10 (`c535b39`, landed after this run and not
measured here) is exactly that lever applied at mount time. A mount-time
sweep bounds the accumulation across restarts; it does not bound it *within*
one long-lived process, and that gap is worth stating in the spec.

**3. `IsInFlight`'s linear walk is the right structure at the sizes that
exist.** `include/kds/txn/manager.hpp` justifies the linear scan over a hash
by the 64-entry bound, and the fit puts one live-list entry at ~0.5 ns —
about one L1-resident comparison. A hash lookup would not beat that at 32
entries. The scaling problem measured above is in the *outer* loop, over
delete-marks, and no change to the inner one addresses it.

**4. The commit-side invalidation `04ae010` added is free at the statement
level, and the comment that ships with it is honest.** `EndDdlScope` says a
cache clear a DDL transaction did not strictly need "costs nothing worth
measuring"; at 150 commits per side per size, `txn-commit` moves −0.1 /
−0.5 / +0.2 µs against a rollback control that moves −0.3 / −0.4 / +0.4 µs.
The claim is now measured rather than asserted. Note the scope of that: with
`cores = 1` the `on_invalidate_` hook's peer broadcast has no peer, so what
was priced is the cache clear and the catalog-page flush. **A multi-core
server has not been measured here** and the broadcast is the part that could
plausibly cost something.

**5. A transactional `DROP TABLE` is the most expensive catalog statement in
the engine, by an order of magnitude, and it is not DT9's doing.** 517 µs
against `CREATE TABLE`'s 48 µs and `DROP INDEX`'s 36 µs, identical on both
binaries. `Catalog::DropTable` runs five `ForFirstRow` sweeps that each
restart from the head of a catalog chain and loop until nothing matches, so
its cost is quadratic in the relation's own catalog rows and linear in the
whole catalog's. It is out of scope for this run and is recorded here because
this is the run that measured it.

---

## Correctness

The full suite was executed in **Release** from the snapshotted binary
`~/bench-ddl9/bin/kds_tests-5827973` (sha256 `e768c2f7…`), built at
`5827973` with `KDS_WITH_TLS=ON`:

```
[==========] 2390 tests from 237 test suites ran. (16939 ms total)
[  PASSED  ] 2390 tests.
```

**2390/2390 green.** That confirms rather than contradicts the 2348/2348
reported from a Debug build with `KDS_WITH_TLS=OFF`: the 42-test difference
is exactly the TLS-gated set — 13 in `tests/tls_channel_test.cpp`, 27 in
`tests/scram_test.cpp` and 2 inside `tcp_server_test.cpp`'s
`#if KDS_WITH_TLS` block — which that configuration excludes. 2390 − 42 =
2348.

No engine code was changed for this measurement. The driver
`tools/catalog_read_ab_benchmark.py` and this document are the only
additions, plus the driver's entry in `bench/docs/README.md`.
