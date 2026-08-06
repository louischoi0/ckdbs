# Where a business transaction's time goes

Measured 2026-08-05 with `bench/txn_layers_bench.cpp` (`kds_txn_bench`),
driving the four statements `tools/stress_business.py` counts TPS for:

```
INSERT INTO trades ...    buy leg   (HEAP, logged)
INSERT INTO trades ...    sell leg
UPDATE accounts SET ...   buyer     (BTREE, logged)
UPDATE accounts SET ...   seller
```

Release build, in-process (no socket), 200 accounts, 200 transactions per
configuration, single connection. Host: AMD EPYC 7571, 2 cores, **xfs on an
EBS volume** — `KDS_BENCH_DIR=/home/ec2-user/kds-bench-scratch`.

**The first run of this benchmark was wrong and is worth recording as a
method note.** With `KDS_BENCH_DIR` unset the scratch directory lands in the
system temp directory, which on this host is **tmpfs** — where `fsync` costs
0.35 µs and all four durability classes measure the same. Every number below
is from the real device; the binary now prints its scratch path for exactly
this reason. `df -T <path>` is the check.

Two instruments, deliberately not one. The **in-situ** table comes from the
dispatcher's own per-phase marks (a temporary `phaseprof` block in
`command_dispatcher.cpp`), so its rows are a partition of the statement's
server-side time. The **direct-call** table calls each component in its own
loop and is a lower bound on what removing that component would save.

**§§3-4 cannot be reproduced by re-running the binary today**: that
instrumentation was temporary and has since been removed from the
dispatcher. The numbers stand as measured; reproducing them means
re-inserting the marks. §§1-2 and §6 need no instrumentation and re-run as
they are.

---

## 1. The headline: one durability point, or four

| | p50 | p99 |
|---|---|---|
| four autocommit statements | **4,712 µs** | 8,238 µs |
| the same four in one `BEGIN … COMMIT` | **998 µs** | 4,552 µs |

**4.7×, for a change no engine work is needed for.** Every autocommit
statement is its own transaction and therefore its own `fsync`; wrapping the
four makes one. The stress tool does not wrap them — deliberately, so its
numbers stay comparable to results recorded before transactions existed
(`tools/stress_business.py`'s header says so) — but that means its TPS is
measuring four durability points per business transaction, and any client
that groups them gets this straight away.

## 2. Per statement, per durability class

| | INSERT p50 | UPDATE p50 | whole txn p50 |
|---|---|---|---|
| unlogged (no WAL manager) | 8.4 µs | 19.0 µs | 56 µs |
| wal **relaxed** | 6.9 µs | 11.5 µs | 38 µs |
| wal **group** (shipped default) | 938 µs | 946 µs | 4,642 µs |
| wal **strict** | 944 µs | 950 µs | 4,766 µs |

**Group and strict cost the same here, and the counters say why**:
`mean_group_batch_size = 1.0`. With one connection there is nothing to batch
with, so a group commit is a strict commit that took a different code path.
This confirms the earlier finding in `results-business-stress.md` ("a batch
of one is a batch") and localizes it: `CommandDispatcher::LogInsert` syncs
inline on the committing statement's own stack (`DrainOnce` +
`EnsureDurable`), so a batch can only form from commits that were *already*
in the ring, never from commits that arrive while this one waits.

## 3. In situ, phase by phase (`durability = group`, as shipped)

| phase | INSERT p50 | UPDATE p50 |
|---|---|---|
| scope (BeginWrite, read view) | 1.98 µs | 2.53 µs |
| **parse** | 5.95 µs | 8.78 µs |
| catalog (+ CompileWhere on UPDATE) | 0.70 µs | 3.29 µs |
| AllocateRowId | 1.47 µs | — |
| encode | 1.30 µs | — |
| store (heap append / btree descent + overwrite) | 3.55 µs | 7.38 µs |
| wal append (+ undo write on UPDATE) | 0.95 µs | 5.38 µs |
| **commit (fsync)** | **933.69 µs** | **929.85 µs** |
| **total** | **951 µs** | **962 µs** |

**98 % of a logged statement is one `fsync`.** Nothing else on the list is
worth optimizing until that changes — a 2× improvement in the parser moves a
logged INSERT by 0.3 %.

## 4. In situ, unlogged — what the engine costs when durability is free

| phase | INSERT p50 | UPDATE p50 |
|---|---|---|
| scope | 0.69 µs | 1.10 µs |
| **parse** | **2.74 µs (38 %)** | **3.82 µs (32 %)** |
| catalog (+ CompileWhere) | 0.16 µs | 1.03 µs |
| AllocateRowId | 0.62 µs | — |
| encode | 0.46 µs | — |
| store | 1.65 µs | 2.86 µs |
| wal / undo | 0.06 µs | 2.14 µs |
| commit | 0.78 µs | 0.82 µs |
| **total** | **7.21 µs** | **11.82 µs** |

The in-situ totals run ~0.3 µs above the end-to-end p50s in §2 because the
profiler itself reads the clock nine times per statement. That is noise at
`group` and about 4 % here; it is stated rather than subtracted.

Direct-call prices for the same components, as a cross-check: parse INSERT
2.06 µs, parse UPDATE 2.81 µs, catalog resolve (cached) 0.11 µs,
`AllocateRowId` 0.49 µs, `EncodeRow` 0.27 µs, `CompileWhere` 0.26 µs,
`FingerprintOf` standalone 1.26 µs.

## 5. What foreign keys cost

Not separable at `group`: 4,711 µs against 4,642 µs without them is inside
the run-to-run spread of a workload whose p50 is one `fsync`. On the tmpfs
configuration, where the check is visible, a checked INSERT was **10.2 µs
against 8.7 µs** — about **1.5 µs**, which is one btree descent into the
parent plus the visibility test, as designed. The UPDATE side does not move,
because the SET lists in this workload touch no fk column and the check is
resolved once per statement rather than per row.

---

## 6. The tail: three causes, not one

p95 and p99 run 2–6x the median per statement, and the four-statement
transaction's p99 is ~3x its p50. `MeasureTails` in the same binary times
every statement and tags it with what the engine did underneath - pages
allocated (read from `DevicePageStore::allocated_pages()`), WAL bytes
written, syncs performed - so the causes can be separated by measurement.

**Measured (400 transactions, 800 statements of each kind, load average 3.2
on a 2-core host):**

| | p50 | p95 | p99 | max |
|---|---|---|---|---|
| INSERT, wal group | 951 µs | 2,095 µs (2.2x) | 4,998 µs (5.3x) | 11,463 µs |
| UPDATE, wal group | 965 µs | 2,147 µs (2.2x) | 5,906 µs (6.1x) | 11,094 µs |
| INSERT, unlogged | 10.1 µs | 13.7 µs (1.4x) | 33.0 µs (3.3x) | 14,366 µs |
| UPDATE, unlogged | 15.2 µs | 19.6 µs (1.3x) | 38.0 µs (2.5x) | 2,310 µs |

**1. The logged tail is the device's own tail.** A standalone `fsync` loop on
this volume measures p50 2,020 µs and p99 4,640 µs - **2.3x**, on the device,
with no engine involved. A logged statement is 98% `fsync` (§3), so it
inherits that distribution and nothing else explains it. Confirmation from
the other direction: **zero of 800 logged statements exceeded 20x the
median**, so the logged distribution is a smooth heavy tail, not a
population with occasional spikes.

**2. Relation growth is real, bounded, and not the tail.** Statements that
allocated a page cost **2,053 µs against 951 µs** logged, and **16.5 µs
against 10.1 µs** unlogged - a 1.6-2.2x penalty. It happens to ~7 of 800
statements (a heap page holds ~100 of these rows), and only 2 of the slowest
40 logged statements allocated anything. The penalty is the FPI: chain
growth logs a full 8 KB page image of the old tail, which shows in the
counters as the slowest 5% averaging 581 B of WAL against 222 B for the
rest. Every 64th allocation also extends the file, since `EnsureCapacity`
rounds to a whole extent (`kDefaultExtentPages`). **Worth fixing for log
volume - it is what `docs/wal.md`'s `HEAP_CHAIN_LINK` record type would
retire - but it is not where the tail comes from.**

**3. The 10-15 ms maxima are the host, not the engine.** In the *unlogged*
configuration - where a statement touches no device at all - 6 of 800
statements ran beyond 20x the median, up to 14.4 ms, and **every one of them
allocated no page and wrote no WAL byte**. There is no engine work that
takes 14 ms and leaves no trace in either counter. The host has 2 cores and
was at load average 3.2; a 10 µs statement preempted by a scheduler slice
produces exactly this. Their positions also move between runs, where a
structural event's would not.

So: **the tail an operator sees is the storage device**, the growth penalty
is a second-order effect worth about 2x on 1% of statements, and the extreme
maxima in these numbers are the measurement host. The first statement of a
run is frequently in the slowest 5% - cold caches - which is worth
discarding rather than diagnosing.

One consequence for the four-statement transaction: it is the **sum of four
independent draws** from the per-statement distribution, so it is slow
whenever *any* of the four is. That is why its p99/p50 (~3x) is worse than
any single statement's ratio, and it is a second reason to group the four
into one transaction (§7 item 1) - one draw instead of four.

---

## 7. What to do about it, in order

1. **Group the four statements into one transaction — 4.7×, measured.** No
   engine change. It also drives the tool's `torn` counter to zero, which is
   the other thing that has been waiting on it.

2. **Make group commit actually batch.** Today a committing statement syncs
   on its own stack, so `mean_batch` is 1.0 and D2 is D1 with extra steps.
   The prerequisite that used to be missing now exists: `DispatchAsync` is a
   coroutine and `WaitFor` is the suspension shape (`sched/coro.hpp`), so a
   commit can park until a drain syncs and N concurrent connections can
   share one `fsync`. On a device where `fsync` is 0.9–2 ms, that is the
   difference between a ~1,000 TPS ceiling and N × that. This is the largest
   engine-side win available and the one the coroutine work was for.

3. **Do not bother with `fdatasync`.** Measured on this volume: `fsync` p50
   2,020 µs, `fdatasync` p50 2,030 µs, over 200 preallocated-file syncs. The
   segment files are preallocated so there is no metadata to skip, and the
   device does not care. Recorded so nobody spends a day on it.
   (`FileLogDevice::Sync()` syncing *every* segment rather than the tail is
   still worth fixing, but for a different reason: it is O(segments) and
   there is one segment in this run.)

4. **Prepared statements.** Parse is 32–38 % of the unlogged path and the
   protocol already specifies the fix (`docs/protocol.md`'s PARSE/BIND over
   server-side statement handles). It buys nothing while `fsync` dominates —
   which is precisely why it should be sequenced *after* item 2.

5. **UPDATE decodes each row twice.** `UpdateInner` calls `DecodeRow` for an
   owned copy and `DecodeRowInto` for the evaluation frame, over the same
   payload. That is most of the gap between the two `store` phases (2.86 µs
   against 1.65 µs). One decode plus a copy of the frame's values would do.

---

## 7. A stale result this invalidates

`bench/results-business-stress.md` (2026-08-03) uses `UPDATE` as its control
— "unlogged, and it does not move (18 → 21 µs)". **`UPDATE` has been logged
since the transaction work landed**: it writes an `UNDO_WRITE` and a
`HEAP_OVERWRITE`, and in autocommit it therefore takes a durability point.
Measured here at **946 µs** under the shipped default, against the 21 µs that
document records.

So its 166.8 TPS was measured when a business transaction cost two
durability points; today the same four statements cost four. That document's
conclusions about the reporting job's contention still stand — it was
measuring a different axis — but its absolute TPS and its `UPDATE` control
row should not be quoted against a current build without re-running it.
