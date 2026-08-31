# XD — the cross-owner commit, decomposed

**The ~3 ms cross-owner increment `bench/v2.7.0/results-scenario2-cores-v2.4.0-83-g57110cf.md`
found is a device cost, not a protocol-with-no-device-in-it cost — and the
proof is direct, not inferred.** Moving only the WAL segments to tmpfs (the
data file stays on ext4, durability semantics untouched) collapses the
increment by **51-62×** at both booker counts this order measured: from
~2.0 ms to ~40 µs at one booker, from ~3.7 ms to ~60-72 µs at eight. That is
the falsifier `instructions/v2.7.1/measurement-xd.md`'s H-XD1 named
("the increment survives tmpfs") failing to fire, and it means the parent
file's headline — *"3.0 ms of two-phase commit with no device in it"* —
read the wrong signal: `relaxed` removes the *ordinary* commit's wait, but
two of the protocol's three durability parks (the participant's prepare and
the coordinator's own decide) are **unconditional on durability class** —
confirmed here at exactly 2.00 syncs/booking under `relaxed` where a
one-owner commit takes 0 — so a relaxed cross-owner commit still pays most
of what a group one does. The device was in it the whole time; `relaxed`
just stopped being the experiment that would show that.

**Under load the increment does not track the sync count, and that is the
second confirmed prediction.** Concurrent bookings genuinely share syncs
under `group` — syncs-per-booking falls from a controlled 3.00 at one
booker to 2.45-2.98 as bookers rise to eight — but the *latency* of a
booking's own commit rises anyway, from a p50 of 2.93 ms at one booker to
5.01 ms at eight. The two move in opposite directions because they are not
the same claim: batching shares the device, not the queue behind it, and
core 0's own reactor occupancy (`sched_foreground_polled_us` share of wall
time) climbs monotonically with booker count — 2.6% → 3.1% → 3.7% → 5.6% at
b = 1/2/4/8 — exactly the signature H-XD4 predicted.

**And the strict/group split this order was asked to resolve turns out not
to be the interesting axis.** At one booker, `group`'s cross-owner ratio
(3.11-3.15×, this session's own measurement) and `strict`'s (3.08-3.13×,
median of three repeats) are indistinguishable — both driven by the same
three-sync chain XD0 exposed, which does not distinguish the two classes at
all. What *is* different, sharply, is `relaxed`, whose ratio (39.9-49.5×
against `f-c1-b8-relaxed`/`f-c8-b8-pl-relaxed`, the parent file's own
cells) is not a bigger overhead — it is a **denominator that collapsed**:
`relaxed`'s one-owner commit fell to ~70 µs while its cross-owner commit
did not move much at all, because two of its three legs are the same
mandatory waits `group` and `strict` pay.

Measured in the worktree `measure-v2.7.1` at `951a91a`
(`git describe --tags` → **`v2.7.0-2-g951a91a`**), branch
`xd-commit-decomposition`, executing
`instructions/v2.7.1/measurement-xd.md` rows XD2-XD5. XD6 is answered by
source read, confirmed in situ at this commit, not re-derived. XD7 (the
scenario-2 file's correction) is the operator's to write; this file states
what the numbers do to that sentence and stops there.

## 1. Provenance

| | |
|---|---|
| Date/time (UTC) | 2026-08-31, 06:04-06:43 (main matrix), 06:35-06:40 (the three "guaranteed cross-owner" reruns) |
| Worktree | `measure-v2.7.1` (`/home/ubuntu/ckdbs/.claude/worktrees/measure-v2.7.1`) |
| Branch | `xd-commit-decomposition` |
| Commit measured | **`951a91a`** (`git describe --tags` → **`v2.7.0-2-g951a91a`**). Work order lives under `instructions/v2.7.1/`; the operator has not named a `v2.7.1` version of record, so this file follows `v2.7.0`, the tag still current, per the operator's 2026-08-25 filing rule |
| Tree state | Clean at the start (`git status --short` empty). During this session the only changes are `bench/wal_sync_decomposition_probe.py` (new), `bench/docs/README.md` (an entry for it) and this results file plus its archive — no `src/`, no `include/`. HEAD did not move |
| Binary provenance | `build-release/kds_server`, mtime `2026-08-31 05:57:35.54Z`, built from a tree that reached `951a91a` at `06:00:48Z` (the commit came 3m13s *after* the build — normal build-then-commit order, not a stale binary). Confirmed by re-running `cmake --build build-release --target kds_server -j8` at the start of this session: **`Built target kds_server` with no recompilation**, so the binary already reflected the committed tree. Copied to `/home/ubuntu/bench-xd/kds_server-951a91a` before the first cell; `sha256` = `940708ce7414b6149420f28a4ba5f1d6febf6dd3b7132966e4fe9c57844f26f0`, re-checked against `build-release/kds_server` after the last cell and byte-identical — nothing rebuilt into the shared tree during this run |
| Build | `CMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`, `build-release/CMakeCache.txt`) |
| Test suite | **3,021 of 3,021 passing**, `build-release/tests/kds_tests`, run independently this session (35.5 s) in addition to the launching session's two prior runs at this commit. No engine code was touched, so a re-run before/after is not applicable — the suite is cited once, at the start |
| Device | Data files under `/home/ubuntu/bench-xd/cells/`, on `/dev/root`, **ext4** (`df -T`: 259 GB, 93 GB free, 63% used). XD3's ablation moves **only** the WAL directory (`wal_dir`, `kds.conf.sample`) to `/dev/shm/bench-xd-wal`, **tmpfs** (`df -T`: 8.0 GB, 33% used at peak) — the data file for every XD3 cell stays on the same ext4 path as every other cell's |
| Host | 8 logical CPUs = 4 physical cores, 2 threads/core, 1 socket, Intel Xeon Platinum 8488C, KVM guest, 15 GiB RAM, Ubuntu, Linux 7.0.0-1011-aws — the same host `results-scenario2-cores` and R6-B ran on |
| Host quiet | Every cell gated on `bench/wait_quiet.sh` (no `cc1plus`/`ld`/`kds_tests`, 1-minute load < 0.70) and sampled `/proc/loadavg` every 3 s for its own life. Peak 1-minute load across all 20 cells: 1.00 (`xd5-strict-c8pl-r3`); every other cell stayed ≤ 0.97. No cell discarded |
| Server config | `placement = creating` throughout (never varied — out of this order's scope); `durability`, `cores`, `peer_listeners` and `wal_dir` vary per cell and are named in each table; `range_size_ids` at its armed default (65,536) throughout — every relation in this schema is HEAP (`freights`, `charges`) or a `RangeEligible`-declining BTREE, so nothing splits regardless |
| Client | `bench/wal_sync_decomposition_probe.py` (new this session, documented at `bench/docs/README.md`) — orchestration only: schema, load, one booking attempt, and merging bookers' results are all `tools/scenario2_freight.py`'s own functions, called exactly as its `main()` calls them. What the driver adds is a per-core `SHOW META` snapshot taken the instant before the booker processes start and the instant after they all join, so a cell's `wal_syncs` delta covers *only* the booking phase, never DDL+load |
| Scale | 2,000 organizations, 200 ships, 2,000 operations, 20,000 cargos — identical to `results-scenario2-cores`'s own scale, so absolute numbers here are directly comparable to that file's |
| Work | `--bookings 5000` in every cell — equal work, not equal time (rule 7). No manifest reporter (off by default in this driver, so every cell's engine work is identical, on `results-scenario2-cores` arms A/B/D/E/F's own rule); `--verify 25` run on one representative cell per shape (both to sanity-check this session's own orchestration code and per the ck-tester rule that an unrun `--verify` is a measurement of nothing) |
| Raw output | `bench/v2.7.0/archive/xd-commit-decomposition-v2.7.0-2-g951a91a/` — every cell's `.json` and `.log`, the two run logs, and the orchestrating shell scripts under `harness/` |

**A note on what "cross-owner" required verifying, not assuming.** Every
relation this schema declares is created under `placement = creating`, so
every one is core 0's. A booker's own connection lands on whichever core
the kernel's `SO_REUSEPORT` picked; at one booker that is core 0 itself
with probability 1/8 per attempt, which would silently measure the
one-owner path under a cross-owner label. The driver's `--require-shipped`
(single booker) and `--require-shipped-rate 0.97` (several bookers) check
core 0's `shipped_executed` delta after the fact and exit 42 rather than
report a diluted cell; every `b=1` cell below needed 1-3 attempts (the
`-allx` reruns at `b=2/4/8` needed 1-2), and every number reported is
from an attempt that passed the check.

## 2. The sync-count model, confirmed in situ

The work order arrived with the per-class sync count already measured (2
cores, a hand-built counting probe, 20 transactions/arm): group and strict
take 3.00 syncs on a cross-owner commit against 1.00 for one-owner;
relaxed takes 2.00 against 0.00. At the scale this order runs — 5,000
committed bookings, 8 cores, the real scenario-2 workload rather than a
synthetic two-relation probe — the same model holds to three decimal
places at one booker, where `--require-shipped` guarantees every booking
is genuinely cross-owner:

| class | cores | booker | committed | total `wal_syncs` delta | syncs/booking |
|---|---|---:|---:|---:|---:|
| group, one-owner | 8, `peer_listeners=off` | 1 | 5,000 | 5,007 | **1.0014** |
| group, cross-owner | 8, `peer_listeners=on` | 1 | 5,000 | 15,044 | **3.0088** |
| strict, one-owner | 1 | 1 | 5,000 | 5,007 (median of 3) | **1.0014** |
| strict, cross-owner | 8, `peer_listeners=on` | 1 | 5,000 | 15,045-15,051 (3 repeats) | **3.0084-3.0102** |
| relaxed, cross-owner (tmpfs) | 8, `peer_listeners=on` | 8 | 5,000 | 9,667 | **1.9334** |

The chain the work order cited — participant prepare (sync #1,
`shipped_statement_executor.cpp:575` `RequestDurable`, `:598`
`co_await sched::WaitUntil{&durable}`), coordinator decide
(`command_dispatcher.cpp:453`/`:457`, unconditional on durability class),
participant's own commit-at-decide (`shipped_statement_executor.cpp:839-841`,
`StartDecision`'s `DispatchAsync(kCommit, ...)`, which rides the class) —
reads exactly this way at `951a91a`: unchanged from the work order's
citations, confirming XD1's source read is still current.

## 3. XD2 — sync accounting: batching shares the device, at a real cost curve

**Syncs-per-booking falls under load, and the fall is real rather than an
artifact of some bookers landing locally.** The naive worry — that a
`b=8` cell's apparent sharing is actually some of the 8 connections
landing on core 0 itself (fully local, contributing nothing to
"cross-owner" statistics) — was checked directly with
`--require-shipped-rate 0.97` reruns (labelled `-allx`, `shipped_executed`
within 0.3-0.5% of the fully-cross-owner expectation) and the fall
survives:

| cell | cores | listeners | booker | TPS | syncs/booking | which cores took them |
|---|---|---|---:|---:|---:|---|
| `xd2-nopl-c8-b1` | 8 | off | 1 | 707.1 | 1.0014 | core 0 only (5,007) |
| `xd2-nopl-c8-b8` | 8 | off | 8 | 1,029.5 | 0.8564 | core 0 only (4,282) — group batches concurrent *local* commits too, the same mechanism `results-scenario2-cores` §4 found from the outside |
| `xd2-pl-c8-b1` | 8 | on | 1 | 270.4 | **3.0088** | core 0 (10,013, participant's 2 legs) + one peer core (5,013, coordinator's 1 leg) |
| `xd2-pl-c8-b8` | 8 | on | 8 | 543.9 | 2.5046 | core 0 (7,407) + 7 peer cores (5,116 combined) |
| `xd2-pl-c8-b8-allx` (guaranteed cross-owner) | 8 | on | 8 | 658.3 | **2.4546** | core 0 (6,945) + 7 peer cores (5,328 combined) |

**H-XD2's falsifier does not fire.** It named two ways the hypothesis
could fail: syncs-per-booking ≈ 3 at b=8 (no sharing), or b=8 latency
materially below b=1's (sharing somehow shortening the chain). Neither
happened — 2.45-2.50 is well under 3.00, and the commit table below shows
latency *rising*, not falling, with b. What the two together say is
precisely H-XD2's claim: the three legs' `RequestDurable`s ride the same
system-group drain everything else does, so eight bookers' worth of
prepares and decides overlap into fewer than 8×3 physical syncs, while
each individual booking still serially waits its own three legs end to
end — sharing the device is not the same claim as shortening the chain.

**Commit latency, the companion table rule 6 asks for** (µs, ops=5,000
each):

| cell | p0 | p25 | p50 | p95 | p99 | mean |
|---|---:|---:|---:|---:|---:|---:|
| `xd2-nopl-c8-b1` (one-owner) | 648.0 | 896.4 | 941.8 | 1,102.2 | 1,216.1 | 949.8 |
| `xd2-nopl-c8-b8` (one-owner) | 697.5 | 1,009.6 | 1,087.2 | 2,107.9 | 2,394.2 | 1,319.1 |
| `xd2-pl-c8-b1` (cross-owner) | 1,996.5 | 2,786.0 | **2,930.5** | 3,354.9 | 3,570.9 | 2,993.8 |
| `xd2-pl-c8-b8` (cross-owner) | 2,060.8 | 3,894.9 | **4,831.1** | 7,503.9 | 8,493.0 | 5,079.3 |
| `xd2-pl-c8-b8-allx` | 2,396.9 | 4,167.6 | 5,006.1 | 7,322.0 | 8,517.7 | 5,228.1 |

The `-allx` and plain `b8` cells agree within ~4% on latency and within 2%
on syncs/booking — the "some bookers landed locally" confound the guard
was built to catch turned out to be small at this scale, and the guard
confirms rather than overturns the unguarded reading. The one place they
disagree more (TPS: 543.9 vs 658.3, ~21%) is a single, unrepeated
measurement each and is not read as a finding — §5's noise floor puts a
single `b=8` cross-owner cell's spread at up to ~7%, and this order did not
budget a third `b=8` repeat to resolve whether the rest of that gap is
signal.

## 4. XD3 — the tmpfs ablation: H-XD1 holds, and the parent file's sentence does not

**The device ablation changes device latency only — durability semantics
and code paths are untouched.** `wal_dir` moves the WAL segments; the data
file stays on the same ext4 path every other cell uses, and every commit
still goes through the identical `RequestDurable`/`WaitUntil{&durable}`
pair — what changes is how long `fdatasync` takes to return on the
directory those segments live in. Every number in this section is
therefore never quoted as an engine number, only as a probe of where the
increment's mass sits.

| cell | device | durability | booker | TPS | syncs/booking |
|---|---|---|---:|---:|---:|
| `xd2-nopl-c8-b1` | ext4 | group | 1 | 707.1 | 1.0014 |
| `xd2-nopl-c8-b8` | ext4 | group | 8 | 1,029.5 | 0.8564 |
| `xd2-pl-c8-b1` | ext4 | group | 1 | 270.4 | 3.0088 |
| `xd2-pl-c8-b8` | ext4 | group | 8 | 543.9 | 2.5046 |
| `xd3-tmpfs-nopl-c8-b1` | tmpfs | group | 1 | 2,297.4 | 1.0008 |
| `xd3-tmpfs-nopl-c8-b8` | tmpfs | group | 8 | 5,795.0 | 0.8710 |
| `xd3-tmpfs-pl-c8-b1` | tmpfs | group | 1 | 1,737.2 | 3.0028 |
| `xd3-tmpfs-pl-c8-b8` | tmpfs | group | 8 | 5,636.1 | 2.7134 |
| `xd3-tmpfs-pl-c8-b8-relaxed` (the bridge cell) | tmpfs | **relaxed** | 8 | 5,204.0 | 1.9334 |

The device ablation moves TPS by two to three orders of magnitude (270 →
1,737 at b=1; 544 → 5,636 at b=8) — the throughput reading confirms the
same thing the latency reading below does, from the opposite direction.

**Commit-latency distribution, all five percentiles** (µs, ops=5,000 each
— this is the rule-6 table, not a knob comparison):

| cell | p0 | p25 | p50 | p95 | p99 | mean |
|---|---:|---:|---:|---:|---:|---:|
| `xd2-nopl-c8-b1` | 648.0 | 896.4 | 941.8 | 1,102.2 | 1,216.1 | 949.8 |
| `xd2-nopl-c8-b8` | 697.5 | 1,009.6 | 1,087.2 | 2,107.9 | 2,394.2 | 1,319.1 |
| `xd2-pl-c8-b1` | 1,996.5 | 2,786.0 | 2,930.5 | 3,354.9 | 3,570.9 | 2,993.8 |
| `xd2-pl-c8-b8` | 2,060.8 | 3,894.9 | 4,831.1 | 7,503.9 | 8,493.0 | 5,079.3 |
| `xd3-tmpfs-nopl-c8-b1` | 12.8 | 22.3 | 23.1 | 33.4 | 44.6 | 23.9 |
| `xd3-tmpfs-nopl-c8-b8` | 13.6 | 87.7 | 101.6 | 141.8 | 161.2 | 102.8 |
| `xd3-tmpfs-pl-c8-b1` | 35.6 | 53.1 | 61.1 | 74.8 | 86.7 | 63.7 |
| `xd3-tmpfs-pl-c8-b8` | 48.6 | 126.1 | 162.0 | 267.0 | 315.0 | 174.3 |
| `xd3-tmpfs-pl-c8-b8-relaxed` | 43.2 | 130.0 | 168.2 | 290.4 | 368.5 | 184.6 |

**The increment — cross-owner commit minus the one-owner commit measured
on the same device and booker count — is what H-XD1 makes a claim about,
not the absolute cross-owner number:**

| booker | device | increment (p50) | increment (mean) |
|---|---|---:|---:|
| 1 | ext4 | 1,988.7 µs | 2,044.0 µs |
| 1 | tmpfs | **38.0 µs** | **39.8 µs** |
| 8 | ext4 | 3,743.9 µs | 3,760.2 µs |
| 8 | tmpfs | **60.4 µs** | **71.5 µs** |

Both tmpfs increments land far under H-XD1's 500 µs falsifier threshold —
the collapse is **51.4-52.6× at one booker and 52.6-62.0× at eight**,
computed both ways (p50 and mean) at both booker counts, all four readings
agreeing to within a factor of 1.2 of each other. **H-XD1 holds, clearly
rather than marginally.**

**What that means for the parent file's sentence.** `results-scenario2-cores`
§5 read `relaxed`'s near-zero *one-owner* commit (76.6 µs) against its
still-3,053.3-µs *cross-owner* commit and concluded "3.0 ms of two-phase
commit with no device in it." §2 here shows why that inference does not
follow: `relaxed`'s cross-owner path still pays 2.00 syncs, not 0 — the
participant's prepare and the coordinator's decide are durability-class-
independent by design (`command_dispatcher.cpp:453`'s comment states it
directly: "whatever the durability class"). `relaxed` only ever removed
the *third* leg's wait, the one that already rides the class on every
path, one-owner or not. The tmpfs ablation is the experiment that actually
answers "is it the device" by making the device's answer free regardless
of class, and it says yes: at one booker, moving the WAL off ext4 removes
**98.1%** of the group increment (2,004.2 of 2,044.0 µs) and leaves a
39.8 µs residue that is the hop/park cost H-XD1 predicted would remain.
That residue is close to what one independent local sync itself costs on
this device (925.9 µs = the ext4-minus-tmpfs difference of the one-owner
commit alone) times two, scaled down: **the two extra protocol syncs cost
2,004.2 µs together, ~1,002 µs each — almost exactly one local sync's own
device cost**, i.e. at one booker (no concurrent commit to share a drain
with) the three legs pay close to full, independent, additive sync
latency, with no batching discount. That is a *different* finding from
R6-B's ("the third leg costs less than a fresh sync") and §5 below returns
to why.

## 5. XD4 — the queueing curve: core 0's occupancy climbs with b, latency with it

**H-XD4 predicted the b=8-minus-b=1 increment is core 0 reactor occupancy
and should grow with b.** It does, monotonically, across all four points
this order asked for (the `b=2`/`b=4` cells rerun under
`--require-shipped-rate 0.97` for the same reason §3 used it — a diluted
cell would understate the true occupancy climb):

| booker | TPS | core 0 foreground-poll share of wall | core 0 idle-block share | core 0 unaccounted share |
|---|---:|---:|---:|---:|
| 1 | 270.4 | **2.57%** | 44.09% | 53.33% |
| 2 | 406.1 | **3.09%** | 19.79% | 77.12% |
| 4 | 493.5 | **3.70%** | 13.96% | 82.34% |
| 8 | 658.3 | **5.56%** | 6.03% | 88.42% |

**Commit-latency distribution, the companion rule-6 table** (µs,
ops=5,000; `b=2`/`b=4`/`b=8` are the guaranteed-cross-owner `-allx`
reruns, `b=1` is `xd2-pl-c8-b1`, itself guaranteed by `--require-shipped`):

| booker | p0 | p25 | p50 | p95 | p99 | mean |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 1,996.5 | 2,786.0 | 2,930.5 | 3,354.9 | 3,570.9 | 2,993.8 |
| 2 | 2,186.8 | 2,830.3 | 2,978.5 | 3,983.7 | 4,904.7 | 3,162.0 |
| 4 | 2,059.2 | 3,049.8 | 3,752.9 | 5,596.0 | 6,474.4 | 3,978.5 |
| 8 | 2,396.9 | 4,167.6 | 5,006.1 | 7,322.0 | 8,517.7 | 5,228.1 |

Foreground-poll occupancy more than doubles (2.57% → 5.56%) across the
sweep while idle-block share falls by a factor of 7 (44.1% → 6.0%) — core 0
goes from mostly idle between statements at one booker to almost never
idle at eight, and the freed time does not all move to foreground polling:
the remainder ("unaccounted" — wall time charged to no scheduling group,
`docs/inflight/in-progress/observability.md`'s own open question) rises
53.3% → 77.1% → 82.3% → 88.4%, consistent with more of core 0's wall clock
being spent *blocked inside `fdatasync`* as more concurrent commits queue
behind one another's syncs — the reactor thread cannot poll another task
while its own `fdatasync` call is inside the kernel.

**`shipped_wait_us_max` is a noisier, secondary signal and is reported as
such.** It read 0 at b=1/2/4 in every cell measured, and at b=8 it read 0
in `xd2-pl-c8-b8-allx` and 36,236 µs in the original unguarded
`xd2-pl-c8-b8`. A single nonzero reading out of two b=8 cells says a
queueing spike *can* happen at b=8 and did not always in this session's
sample — it is named because H-XD4 predicted it, not presented as a clean
curve the way the occupancy fractions above are.

**H-XD4 holds on its primary claim** (occupancy grows with b) and is
**inconclusive on the queueing-counter secondary signal**, reported as
such rather than rounded to a clean confirmation.

## 6. XD5 — the strict pair: 3.08-3.13× at one booker, matching group, not R6-B

Three repeats each, fresh server and data file per repeat, `durability = strict`,
one booker (no queueing to confound the protocol-only reading):

| repeat | `xd5-strict-c1` (one-owner) commit p50 | `xd5-strict-c8pl` (cross-owner) commit p50 |
|---|---:|---:|
| 1 | 947.5 | 3,046.0 |
| 2 | 949.1 | 2,920.4 |
| 3 | 951.9 | 2,846.2 |
| **median** | **949.1** | **2,920.4** |

**Noise floor, from these same six cells** (peak-to-peak / median):

| | one-owner (`c1`) | cross-owner (`c8pl`) |
|---|---:|---:|
| commit p50 | 0.46% | 6.84% |
| TPS | 4.14% | 7.01% |

The cross-owner path is a wider floor than the one-owner path's — expected,
since a three-leg chain compounds whatever jitter each leg's own device
sync carries, against one leg's jitter alone. Nothing in this section reads
a delta narrower than ~7% as a finding.

**The ratio, median of medians, against R6-B's 1.975× (p50) and the parent
file's relaxed comparison:**

| class | ratio (p50) | ratio (mean) | ratio (p99) | source |
|---|---:|---:|---:|---|
| strict | **3.077×** | 3.127× | 2.909× | this file, §6, median of 3 repeats |
| group | **3.111×** | 3.152× | (single measurement) | this file, §3, `xd2-pl-c8-b1`/`xd2-nopl-c8-b1` |
| group (R6-B's shape) | 1.975× | — | 1.867× | `bench/v2.5.0/results-r6b-cross-owner-cost-v2.4.0-28-g53f6aae.md` §5b, `xowner-2/local` |
| relaxed | 49.5× | 39.9× | — | `results-scenario2-cores` §5, `f-c8-b8-pl-relaxed`/`f-c1-b8-relaxed` |

**The relaxed row is at eight bookers, not one — named because it is not
apples-to-apples with the other three rows' booker count.** `strict` and
`group` above are both b=1, chosen precisely because one booker cannot
queue behind itself; the parent file's own relaxed cells were both driven
at b=8 (the smallest relaxed measurement that file made), and this order
did not re-measure a b=1 ext4 relaxed pair to complete the square. §4's
`xd3-tmpfs-pl-c8-b8-relaxed` bridges the durability class at b=8 on tmpfs,
not ext4 at b=1, so it does not close this gap either. The relaxed ratio's
*reading* (a collapsed denominator, not a larger cost) does not depend on
the booker count, but the number itself is not directly comparable in
magnitude to strict/group's b=1 figures for that reason.

**Group and strict agree with each other (3.08-3.15×) and disagree with
R6-B (1.975×), and the disagreement has a source-supported explanation
rather than being a loose end.** R6-B's `xowner-N` shape writes one row to
one relation per participant — its N=1 cell measured "1.815x local, in the
range M3 already found for shipped-statement overhead" (R6-B §6), i.e. it
priced the 2PC protocol close to alone. Scenario 2's booking ships **every
one of 6-8 statements individually** to the owner core, not just the
transaction boundary (`results-scenario2-cores` §5: `shipped_executed =
39,497` against ~40,000 statements) — so this order's ratio is
protocol-plus-per-statement-shipping, a strictly larger quantity than
R6-B's protocol-alone number, and 3.1× against 1.8-2.0× is the right
direction and a plausible size for that difference. **H-XD3's falsifier
("materially off 3x either way") does not fire** — 3.08-3.15× lands close
enough to the naive "3 syncs / 1 sync = 3×" additive prediction that,
combined with §4's finding that the two extra legs cost almost exactly one
independent local sync each at this booker count, the additive model is
this workload's *good* approximation at b=1, not merely an untested one.

**The relaxed ratio is not the same kind of number and should not be read
beside the other two as if it were.** 39.9-49.5× is not "relaxed pays a
bigger overhead" — §4's own numbers show `relaxed`'s cross-owner cost
(3,053.3 µs mean) sits *below* `group`'s (5,079.3 µs, `results-scenario2-cores`
value) and close to `strict`'s. What moved is the denominator: `relaxed`'s
one-owner commit fell to 76.6 µs because its one sync stopped being waited
on, while its cross-owner commit's two mandatory legs did not move. A
ratio built on a denominator that collapsed by two orders of magnitude is
answering "how much did the floor drop", not "how much did the ceiling
rise" — the two other ratios in this table are the ceiling question.

## 7. XD6 — the RO-participant reply, verdict from source, not re-derived

**H-XD5 holds: the read-only-participant reply cannot fire in scenario 2,
confirmed at `951a91a`, not newly measured.** `tools/scenario2_freight.py`'s
`book_once` (`:875` freight-insert, `:884` charge-insert, `:896`
operation-update, `:906` org-update) writes four statements to relations
every one of which is core-0-owned under `placement = creating` — every DDL
statement a peer session issues is shipped to core 0 to execute
(`src/server/command_dispatcher.cpp:926-945`, the CR5/CB4 ship route,
confirmed present at this commit). So the booking's sole
participant, whichever core owns `freights`/`charges`/`operations`/
`organizations` (always core 0 here), always receives at least one write —
there is no shape in this workload where a participant reads only.
`docs/spec/cross-owner-txn.md` §8 lists the optimisation itself as unbuilt
("a core that only read is prepared with a durable record like any other,"
§1a) — so there is also no counter this session's cells could show it
firing even if the workload admitted it.

**What the counters in this run confirm rather than contradict**: every
`--verify 25` cell in §§3-6 shows 75 of 100 checks answered and 0 failures
— the 25 unanswered are the known `freights JOIN cargos` refusal
(`results-scenario2-cores` §7, `commit d840a30`'s counted-not-skipped fix),
unrelated to XD6, and reproduced here at this commit as further confirmation
that fix still holds.

**The consequence for where the RO-participant lever is priced.** This
project's standing note that the read-only-participant reply is the
largest measured cross-owner cost is priced against
`bench/v2.5.0/results-rr-read-half-v2.4.0-32-g2a1cdcc.md`'s read-half
workload — a shape built specifically to have a participant that only
reads — **not** against scenario 2, whose booking cannot exercise it. Any
future citation of that "largest measured cost" note should name the
read-half workload, not scenario 2, as what it is priced against.

## 8. What this teaches about the engine

**The 2PC protocol's three syncs are not equally shareable.** XD2 shows
real batching (3.00 → 2.45-2.50 syncs/booking) but XD4 shows the shared
resource is the *device*, not the *chain* — every booking still walks all
three legs serially, so the queue behind the shared device is what a
higher booker count actually buys latency against. This is the same
distinction `docs/spec/sched.md`'s observability milestone drew between
"charged to a group" and "charged to nobody," now with a concrete case: at
b=8, 88% of core 0's wall clock is unaccounted, which on this evidence is
mostly sync-wait rather than idle or scheduling overhead — a number
`docs/inflight/in-progress/observability.md`'s per-leg-timer gap (named
again at R6-B §8) would resolve exactly rather than infer.

**The additive sync-cost model — three independent syncs cost three times
one — is not universally wrong, contrary to how R6-B's own finding might
be read.** R6-B found the third leg "costs less than a fresh sync" and
proposed two candidate explanations (idle-wake amortisation inside one
parked coroutine, or catching a sync cycle already in flight) without being
able to separate them. This order's tmpfs-based decomposition shows the
additive model holding almost exactly at b=1 (2,004.2 µs for two extra
legs against 925.9×2=1,851.8 µs predicted, 8.2% over) — the discount R6-B
found seems to depend on concurrent load sharing a drain cycle, which b=1
by construction has none of. Read together, the two results are
consistent rather than contradictory: no concurrent commits, no discount;
concurrent commits, a discount that grows with how many there are (XD2's
2.45-2.50 at b=8). Neither run separately proves this; together they are
the first evidence for *when* the discount R6-B measured actually applies.

**The scenario-2 booking's cross-owner cost is not "the 2PC protocol" —
it is the protocol plus per-statement shipping, and the two are
separable, now with a number for each.** At b=1, group's total
cross-owner increment against a local commit is ~2,044 µs; the tmpfs
ablation prices ~2,004 µs of that as device-sync (98.1%) and ~40 µs as
everything else — ring hops, shipping dispatch, park/resume. That 40 µs
residual is close to R6-B's own `xowner-1` finding (1.815× local, "in the
range M3 already found for shipped-statement overhead") once R6-B's ratio
is converted to an absolute number on a comparable host. The device, not
the shipping, is where this workload's cross-owner cost actually lives —
which was not obvious from the parent file's relaxed-durability test
alone, and is exactly the ambiguity a device ablation (rather than a
durability-class change) is suited to resolve.

**Open decisions this touches.** `docs/spec/crosscore.md`'s open list
still names "batch/credit/ring/extent sizing" and "the `ring_full` retry
protocol" as undecided; this order's finding that the device, not the
ring, dominates the b=1 cost suggests any near-term optimisation aimed at
this booking shape should target the sync chain (drain-sharing scope,
ack-timing per XD1's still-open question) before ring/credit tuning — a
statement about priority, not a decision, since this order built nothing.

## 9. What this order does not answer

No engine code changed this session (`git status --short` empty before and
after but for this file, the new probe and its README entry) — nothing here
is a performance result, only a decomposition of an existing one, per the
work order's own Improvement section. XD1 (the third leg's slack) is not
this order's row and stays open. The b=8 TPS gap between `xd2-pl-c8-b8` and
its `-allx` rerun (§3) is named, not explained — a third b=8 repeat would
be the next step if it matters. `shipped_wait_us_max`'s single nonzero
reading (§5) is named as inconclusive rather than forced into the clean
curve the occupancy fractions show.

**`cores = 8` is the host's own ceiling** (8 logical CPUs — a ninth core
has nowhere to run) and every cell in this file uses it wherever the row
calls for more than one core, per rule 4b. **`b = 8` is the work order's
own ceiling for this matrix, not an independently searched scaling
breakpoint** — XD4's queueing curve (§5) is still rising at b=8, not
flattening, so the booker count that "stops scaling" for this booking
shape is not yet reached and was not this order's question. `ck-tester`'s rule 9 asks every matrix to run at
200/1K/10K rows so a fixed cost can be told from a per-row one; this order
did not vary `--cargos`/`--organizations`/`--operations` because the
quantity every row here measures — a fixed eight-statement booking's
commit-protocol cost — is not a function of relation size at all. The
20,000-cargo scale is held constant throughout, matching
`results-scenario2-cores` and R6-B, so every ratio in this file is read
against those files' own numbers rather than against a size axis. Stated
rather than silently skipped, per that rule's own instruction for a shape
that does not scale with rows.
