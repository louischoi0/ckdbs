# Keystone id allocation, measured

Measured 2026-08-03 for K0 (`docs/keystoneid-k0-findings.md`), against
`docs/keystoneid-invariant.md` §2's proposed high-water-mark allocator.

One question, asked directly: **can issue-once make inserts dramatically
slower?** The answer is yes, by one specific implementation choice, and no
otherwise — and the gap between the two is a factor of about 2,600.

---

## Method

`bench/keystone_alloc_bench.cpp`, in-process, Release build, no server and no
socket — the numbers below are engine cost with the client removed.

Sections 1–3 run on an `InMemoryPageStore`, so no device appears in them.
Section 4 runs on a `FilePageDevice` where `Sync()` is a real `fsync`, and
**the file's location decides whether that section means anything**: on this
machine `/tmp` is tmpfs, where `fsync` costs about 4 µs and answers the
durability question wrongly. The bench defaults its data file to the working
directory (xfs on NVMe here) and takes `KDS_BENCH_DIR` to override.

Everything was run three times. Spread across runs is under 15% on the
memory sections and under 2% on the fsync-bound ones; single figures below
are the median.

## 1. The allocator's share of an INSERT

| | throughput | per op |
|---|---|---|
| `Catalog::AllocateRowId` (today) | 2.6 M/s | 0.38 µs |
| INSERT end to end, unlogged | 126 k/s | 7.9 µs |

**The allocator is 4.3–4.9% of an unlogged INSERT.** That is the ceiling on
what any allocator change can win, and it is the first number that should
have been asked for: no redesign of this component can move insert
throughput by more than about 5%, in either direction, as long as it stays
non-durable.

Measured against an *unlogged* insert deliberately — the harsher denominator.
Against a logged one the allocator's share can only be smaller.

## 2. The sys.tables scan, and a ceiling nobody mentioned

`AllocateRowId` scans `sys.tables` for the matching row, so its cost is
O(relations). It does not matter, for a reason worth writing down:

| user relations | per op |
|---|---|
| 1 | 0.33 µs |
| 10 | 0.32 µs |
| 30 | 0.32 µs |
| 31 | 0.32 µs — **`CREATE TABLE` refused past here** (2-column tables) |

**The catalog cannot hold more than ~62 columns in total, across every user
relation.** The fixed catalog pages are single pages that do not chain, and
`sys.columns` is the one that fills first — one row per column, ~117 bytes
with tuple overhead, ~64 to a page. `CREATE TABLE` then fails with
`heap page has no room for this tuple`. Measured directly:

| columns per table | tables created before refusal |
|---|---|
| 2 | 31 |
| 4 | 15 |
| 8 | 7 |

The product is flat at ~62 columns, which is what identifies `sys.columns`
as the binding page rather than `sys.tables` or `sys.objects`.

So the scaling cliff this section was written to find does not exist — the
scan is bounded by a limit that binds long before the scan does. That is not
a reassuring finding. It is a much more serious limitation than the one being
looked for, in a product aimed at financial systems, and it belongs to the
catalog rather than to this milestone.

## 3. Chunked bump-ahead, in CPU

§2's allocator, prototyped in the bench, on the memory store:

| | throughput | per op |
|---|---|---|
| N=1 (today's cadence, the control) | 3.3 M/s | 0.31 µs |
| N=64 | 107 M/s | 0.009 µs |
| N=4096 | 291 M/s | 0.003 µs |

N=1 matching today's `AllocateRowId` is what makes the rest of the column
believable: the prototype does the same work through the same page path, and
only the cadence differs.

In CPU terms bump-ahead is ~100× cheaper per id at N=4096. Against §1's 4.3%
share that is worth about 4% of an insert — real, and not why the design
exists.

## 4. The number the question was actually about

Crash-safe issue-once means the sequence has to reach the platter before the
ids it covers are handed out. On a real file:

| | throughput | per op | vs today |
|---|---|---|---|
| `AllocateRowId`, no durability (today) | 2.5 M/s | 0.40 µs | 1× |
| ...forced durable **per id** | **949/s** | **1054 µs** | **2629× slower** |
| bump-ahead N=64, durable per chunk | 58 k/s | 17.2 µs | 43× slower |
| bump-ahead N=4096, durable per chunk | 2.0 M/s | 0.48 µs | **1.24×** |

Three things fall out of this table.

**Per-id durability is catastrophic and would be the natural thing to
write.** 949 ids/s caps INSERT at 949/s whatever else the engine does, which
lines up with the 802/s already measured for strict-durability inserts
(CLAUDE.md) — the same fsync, counted twice. This is the "dramatic slowdown"
the question was about, and it is reachable by implementing K1 the obvious
way rather than §2's way.

**N=4096 is not a tuning suggestion, it is the design.** At 0.48 µs/id,
crash-safe bump-ahead costs 1.24× today's *non*-durable allocator — about
0.05 µs on a 7.9 µs insert, or 0.6%. It is strictly better than what runs
today: cheaper than per-insert catalog writes would be at this cadence, and
correct across a crash, which today's is not.

**N=64 is not enough, and the gap between 64 and 4096 is where the design
lives.** 17.2 µs per id against a 7.9 µs insert is a **3× INSERT
regression** — one fsync per 64 rows is still one fsync every 64 rows.
§2 lists N as `[PROPOSED: 4096]` without saying why; this is why, and any
future move to make N tunable has to carry a floor rather than a default.

---

## What this says about K-M2

- The allocator is not a bottleneck today (§1) and K-M2 should not be sold as
  a performance change. It is a **correctness** change that happens to be
  slightly cheaper.
- The `[PROPOSED]` chunk size of 4096 is **confirmed by measurement**, and
  the reason is the fsync cadence, not CPU. Small N is the trap.
- Durability, not allocation, is the whole cost. Which means K-M2's real
  dependency is the one the workplan does not list: **catalog writes must be
  logged** before "persist the ceiling" means anything, since today's catalog
  writes reach the platter only at a checkpoint
  (`docs/keystoneid-k0-findings.md` §3).
