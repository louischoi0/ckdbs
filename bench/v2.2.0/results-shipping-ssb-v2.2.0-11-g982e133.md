# SS-B — statement shipping, measured against the memo that predicted it

`docs/inflight/known-gaps.md` recorded the engine's own position on this build:
**"Statement shipping is built and unmeasured."** The wire, the waiter, the
owner-side execution, the dispatch fork and the counters were in; none of
`docs/inflight/in-progress/memo-shipping-and-group-commit.md` §8's three claims had been judged.
This run judges them, under the order in
`instructions/v2.2.0/measurement-after-s5.md`.

**The thesis, stated first.** Shipping is very nearly free at load and costs
roughly 2× at idle, and the reason is neither the wire nor the waiter: **it
is that an idle reactor sleeps for a whole millisecond and nothing wakes it
when a ring message arrives.** That is measured, not inferred. A shipped
statement costs a flat **1,064–1,068 µs** more than a seated one — the same
constant with the device sync in the path and with it removed — and when the
one knob that sets a reactor's idle block is moved, **the shipped latency
follows it exactly**: 1.08 ms, 2.10 ms, 3.11 ms, 5.12 ms at blocks of 1, 2, 3
and 5 ms, with the seated control flat at 23 µs throughout (§4a). One
millisecond is the *floor*, because `IdleTimeoutMs` is a whole-millisecond
count handed to `epoll_wait` and rounds up (**source-read**,
`src/sched/scheduler.cpp:196-214`), and no wake path exists anywhere in the
tree.

Every cell below is that one fact seen from a different angle: at one session
per owner the shipped statement pays the sleep every time and runs at
**0.43×**; from four sessions upward the owner is never idle, the sleep never
happens, and shipping runs at **0.93–0.99×**, inside this run's noise floor.
The cost is a millisecond of sleep the ring cannot interrupt — a fixable
property of the scheduler rather than an intrinsic price of shipping.

---

## 1. The run

| | |
|---|---|
| `git describe --tags` | **`v2.2.0-11-g982e133`** |
| Commit | `982e133d0756c0f5777fd102ecd454f6b127ea0d` ("shipping: the fork's scope predicate was wrong in three places, and one of them answered") |
| Tree | **byte-identical to `origin/main` at `582d914`** (`git diff origin/main HEAD` empty); `582d914` is the commit the order was drafted against, so these are measurements of `main` |
| Worktree | `/home/ubuntu/ckdbs/.claude/worktrees/workplan-v2.2.0`, branch `ssb-measurement` |
| Tree state | clean at every cell (`git status --porcelain` empty except the `bench/` files this run adds) |
| Host | Intel Xeon Platinum 8488C (Sapphire Rapids), 1 socket, **4 physical cores, SMT on, 8 logical**. `cpu0-3` are four distinct physical cores; `cpu4-7` are their siblings (`thread_siblings_list` = `0,4 / 1,5 / 2,6 / 3,7`) |
| Core pinning | reactor core *N* is pinned to logical CPU *N* (`src/server/expeditor.cpp:1006` `PinToCore`, **source-read**), so `cpuN` in every table below **is** reactor core *N*. At `cores = 4` no two reactors share a physical core |
| Filesystem / device | **ext4 on `/dev/root`**, `--workdir` under `/home/ubuntu/ssb`. `df -T` checked at the start of the run; `/tmp` on this host is tmpfs and was not used. `--force` never passed |
| Build | `build-release`, `CMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG -march=x86-64-v3`, Ninja, GCC 15, `KDS_WITH_TLS=1` |
| Binary provenance | `build-release/kds_server` mtime **2026-08-26 09:35:35 UTC**, which *precedes* HEAD's commit timestamp (09:37:35) — `ninja -n kds_server` reported **"no work to do"**, so the binary is built from this tree and the commit object is simply younger than the compile. Copied to `~/ssb/bin/kds_server` before the first cell and every server started from the copy; `sha256sum` of both, identical: `8f31547a6fc7e5adb869761d32ae29e081f9cce45ad6600f98c24b4c82b16725` |
| Server config | `cores` 1/4/8 per cell, `placement = rotate`, `peer_listeners = on`, `durability = group` except where a cell names `relaxed`, `wal_drain_interval_us = 1000` except where a cell names otherwise. Fresh data file and fresh server per arm per rep |
| Date | 2026-08-26, 09:50–11:30 UTC |
| Reps | 5 per cell minimum; median with the full min..max spread reported for every ratio |
| Correctness | rows in = rows out per relation in **every** cell. Across every `single_relation_probe.py` cell in this document — the null cells, B1, B2, B4, B6, the row-set sweep, the durability control and the two attribution probes — **2,375,800 INSERTs attempted, 2,375,800 executed, 0 refused, 0 lost rows** (§8's `refusal_baseline_probe.py` cells are a different probe and are counted in §8a). Functional verification runs on its own track (the order's first line); no engine code was touched by this run and the correctness suite was **not executed** here |

**Contention.** This box is shared with other worktrees. Another agent's
`kds_tests` and `ld` ran during the first attempt at the main sweep, which
was discarded and re-run; `bench/run_ssb.py` now gates each arm on the
process list as well as the load average and **re-runs any arm a competitor
appeared beside**, recording the fact per rep. One arm (`b1-b-r4`) needed
three attempts; every kept rep records `"contended": []`.

---

## 2. The null cell, and the client ceiling

**The pretask run's ~10% ordering bias does not exist in this harness.** It
was a property of `tools/multicore_benchmark.py`, which runs both
configurations inside one process; SS-B runs each arm as its own process
against its own fresh server, and both null cells come back at 1.00.

| null cell | shape | arm A | arm B | ratio (median) | spread (raw — these two *are* the reference) |
|---|---|---|---|---|---|
| `null1` | `cores = 1` vs `cores = 1`, S = 4 — the order's cell verbatim | 1,772.6 ips | 1,757.2 ips | **0.991** | 0.840–1.038 |
| `null4` | `cores = 4`, both arms seated on the owner, S = 4 — the literal shape of every ratio below | 2,220.8 ips | 2,278.4 ips | **1.016** | 0.978–1.108 |

**Every A/B ratio in this document is divided by `null4` = 1.016.** `null4`
rather than `null1` because it is the same shape as the ratios it corrects —
four cores, rotation, peer listeners, two arms differing in nothing but the
seat — and because the correction is 1.6% either way, so the choice changes
no verdict. Both are reported so a reader can substitute.

**The noise floor is the null cells' own spread**, and it is wide: `null4`
returns 0.978–1.108 over five reps of a cell that is 12,000 statements per
arm. A corrected deficit smaller than about **10%** at that cell size is not
a finding, and this document says so where it applies. Larger cells are
tighter — `b4-k16` (240,000 statements) spreads 0.972–0.981 corrected — and the two
cells outside the floor (`b1`, `b4-k1`) are outside it by a factor of two,
not by a few percent.

**Client ceiling, re-checked on this host** — `bench/client_ceiling_probe.py
--threads 1,2,4,6,8,14`, three seconds per arm, `errors=0` throughout:

| threads | `ping` stmt/s | `SELECT` stmt/s | autocommit `INSERT` ips |
|---|---|---|---|
| 1 | 47,342 | 42,910 | 883 |
| 2 | **111,121** | 80,914 | 1,099 |
| 4 | 93,406 | **92,492** | 2,017 |
| 6 | 68,405 | 68,055 | 3,024 |
| 8 | 58,677 | 56,084 | 4,330 |
| 14 | 58,993 | 57,799 | 5,927 |

The pretask host's ~56k reproduces at the thread counts SS-B uses: the
CPython driver tops out at **56–59k stmt/s from 8 threads up**. The order's
rule is that a cell within 2× of the ceiling is the driver's number. The
threshold is therefore ~28k stmt/s, and **no `group`-durability cell in this
document comes within a factor of three of it** — the largest is `b4-k16`'s
8,773 ips. Two `relaxed` control cells do (§4), and they are labelled there.

`ping` at one thread also gives this run its own client-and-socket cost:
47,342/s = **21.1 µs** per round trip, which §11's wait accounting uses.

---

## 3. B1 — the R1 price: shipping costs half the throughput at one session per owner

**Measured** — `cores = 4`, one relation per writer core, one session each,
3,000 INSERTs per session, 5 reps:

```
bench/single_relation_probe.py --server ~/ssb/bin/kds_server \
    --workdir ~/ssb/run/b1-a-rN --port 17280 --arm multi --cores 4 \
    --sessions 3 --relations 3 --rows 3000 --seat owner
bench/single_relation_probe.py --server ~/ssb/bin/kds_server \
    --workdir ~/ssb/run/b1-b-rN --port 17284 --arm multi --cores 4 \
    --sessions 3 --relations 3 --rows 3000 --seat foreign
```

| arm | ips (median) | ips spread | attempted | executed | refused | rows in = rows out |
|---|---|---|---|---|---|---|
| seated on the owner | **2,665.7** | 2,631–2,777 | 45,000 | 45,000 | 0 | yes, all 3 relations, all 5 reps |
| shipped from a foreign core | **1,413.7** | 1,401–1,445 | 45,000 | 45,000 | 0 | yes |

**Ratio 0.534 raw, 0.526 divided by the null cell, corrected spread
0.506–0.533.** Five
reps inside 3% of each other, and the deficit is 47% — an order of magnitude
outside the noise floor. **Claim 2 holds, and by more than it predicted.**

**The seating, stated because it is not symmetric and the asymmetry matters.**
Rotation put the three relations on cores 3, 1 and 2 (read from `DESCRIBE`,
never assumed); the shipped arm's sessions landed on cores 0, 2 and 3. Every
owner core therefore executes exactly one session's statements in both arms —
the shipped arm adds a ring hop and a parked waiter, and nothing else. But
*which* cores get the waiter is not uniform: cores 2 and 3 own a relation
**and** host a waiter, so they never idle-block and their shipped statements
are picked up at once (§4a); core 1 owns a relation and hosts no waiter, so
it idles and pays the millisecond on every statement. **B1's 0.526 is
therefore a blend of the two regimes**, which is why B4's K = 1 cell (§7),
where one arrival core and one owner are cleanly separated, reports a
slightly worse 0.429. Both are far outside the floor and both say the same
thing; the isolate is B4's.

**The framing the order asks for, and it matters more than the number.**
R1's honest "before" is not a slower statement, it is a **refusal**: at
`582d914`'s parent behaviour a session on the wrong core could not write that
relation at all (`crosscore.md` CC3), which §8 measures at 80–92% of write
statements for a client that does not hunt for the owner. A 47% loss converts
a failure into work. Whether unconditional shipping (D6) is the right default
is `crosscore.md` §9's routing decision and the operator's; what this cell
supplies is its price.

---

## 4. Where the 47% goes — and it is not the wire

The memo's budget said *"a statement costs 21–23 µs; a sync costs ~0.9 ms;
the wire is ~1/40 of a sync"*, and concluded the round-trip objection to
shipping is arithmetically small. The first half is confirmed by this run.
The conclusion is not.

**The control: the same cell with the device sync removed from both arms.**
`durability = relaxed` takes the `fdatasync` out of the commit path on both
sides, so what remains is everything shipping costs that is *not* a sync.
Nothing measured under `relaxed` is a durability claim; the cell exists only
to attribute the gap.

```
bench/single_relation_probe.py --server ~/ssb/bin/kds_server \
    --workdir ~/ssb/runsync/sync-<d>-<a|b>-rN --arm multi --cores 4 \
    --sessions 1 --relations 1 --rows 3000 --seat <owner|foreign> \
    [--arrival-core -1] --durability <group|relaxed>
```

| durability | seated ips | seated p50 | shipped ips | shipped p50 | ratio (corrected) | corrected spread | shipped − seated p50 |
|---|---|---|---|---|---|---|---|
| `group` | 1,275.9 | 720.3 µs | 535.6 | 1,784.1 µs | **0.416** | 0.382–0.452 | **+1,063.8 µs** |
| `relaxed` | 36,138.0 | 23.7 µs | 860.4 | 1,091.4 µs | 0.023 | 0.0229–0.0239 | **+1,067.7 µs** |

**The shipped-minus-seated delta is 1,064 µs under `group` and 1,068 µs under
`relaxed` — the same number with and without the device.** Shipping does not
cost a sync and it does not cost 20 µs of wire. It costs a flat **~1.07 ms**,
and that constant is what every cell in this document is made of.

*(The `relaxed` seated arm at 36,138 ips is within 2× of this host's 56k
client ceiling and is reported as the driver's number, not the engine's. It
is used here only as the zero-point of a difference, where a ceiling on both
sides cancels; the shipped arm at 860 ips is nowhere near it.)*

### 4a. The millisecond **is** the owner's idle block — measured against the knob that sets it

`IdleTimeoutMs` decides how long a reactor with nothing runnable blocks in
`epoll_wait`, and it is capped by the next timer's deadline. The WAL drain is
the only timer at this cadence, so `wal_drain_interval_us` is a direct handle
on the block. If the shipped statement's ~1.09 ms is that block, the latency
must follow the knob.

**Measured** — `relaxed`, S = 1, one arrival core, 3 reps per point, both
arms at every point (2,000 statements per rep below 2000 µs, 600 above,
because a 5 ms cell at 200 ips is otherwise a very long rep):

| `wal_drain_interval_us` | `epoll_wait` timeout it produces | **shipped p50** | shipped ips | shipped p25 | shipped p95 | seated p50 (control) |
|---|---|---|---|---|---|---|
| 50 | 1 ms | **1,083–1,086 µs** | 857–865 | 1,052–1,061 | 1,771–1,800 | 23.3–23.6 µs |
| 200 | 1 ms | **1,083–1,093** | 856–865 | 1,053–1,064 | 1,749–1,825 | 23.1–23.6 |
| 500 | 1 ms | **1,083–1,093** | 853–869 | 1,061–1,062 | 1,739–1,827 | 23.3–23.8 |
| 1000 (default) | 1 ms | **1,091–1,094** | 858–860 | 1,059–1,065 | 1,791–1,814 | 22.8–23.6 |
| **2000** | **2 ms** | **2,097–2,108** | 499–501 | 1,852–1,861 | 2,833–2,878 | 23.3–23.6 |
| **3000** | **3 ms** | **3,104–3,116** | 333 | 2,226–2,576 | 3,839–3,913 | 13.4–23.4 |
| **5000** | **5 ms** | **5,108–5,126** | 200 | 4,168–4,199 | 5,899–5,978 | 23.0–23.5 |

**The shipped statement's p50 equals the idle block plus ~100 µs, over a
fivefold range of the knob, while the seated control does not move at all.**
Throughput is the reciprocal to three figures — 500, 333, 200 ips at 2, 3 and
5 ms. This is not an inference from a constant that happened to look like a
millisecond; it is a functional dependence on the knob that sets the block,
with a control that stays flat.

**The flat region below 1 ms is the same fact, and it is why the first four
rows look like a failed experiment.** `IdleTimeoutMs` **rounds up to whole
milliseconds**, and the value goes to `epoll_wait`, which takes milliseconds:

```cpp
// src/sched/scheduler.cpp:196-214, source-read
int Scheduler::IdleTimeoutMs() const noexcept {
    if (HasReadyTask()) return 0;
    int timeout = config_.max_idle_block_ms;      // 10, scheduler.hpp:79
    ...
    const MonoTimeNs remaining_ms = (deadline - now + 999'999) / 1'000'000;
```

A timer 50 µs away yields `remaining_ms == 1` (`src/sched/scheduler.cpp:319`
→ `src/sched/epoll_io_backend.cpp:88`). **One millisecond is the smallest
non-zero block this reactor can take**, whatever the timer period is, so
50/200/500/1000 µs are all the same block and all give the same latency. The
first attempt at this sweep stopped at 1000 µs and read as a rejected
hypothesis; extending it upward is what turned it into the answer, and the
lower half is kept because it is the evidence for the rounding.

**Why the block is on the path at all: nothing wakes a core when a ring
message arrives for it.** **source-read** — `grep -rn "eventfd\|EFD_\|Wake"`
over `src/` and `include/` finds no mechanism; the ring is discovered only by
`Scheduler::DrainInbox()` (`src/sched/scheduler.cpp:62`, called from the run
loop at `:353`), once per iteration. `src/server/expeditor.cpp:1712` says the
same thing about the commit path in its own words: *"without it the parked
statement has nothing to wake it until the timer below fires"*. A shipped
request that arrives at an idle owner therefore waits for that owner's next
`epoll_wait` return.

**This is the finding of the run**, and it is not about shipping: any
cross-core message to an idle core pays it. Shipping is simply the first
feature that puts one on a client's critical path twice per statement. It is
handed to `docs/spec/sched.md` §4's owner; a wake on the ring send, or a
sub-millisecond block expressed in nanoseconds, would remove most of §3's
47%.

### 4b. The millisecond is a latency, not a serialization — and that is why it disappears

If ~1.07 ms were a cost each statement paid in series, throughput under
`relaxed` would be pinned near 900 ips however many sessions were offered.
It is not.

**Measured** — `relaxed`, one relation, every session on **one** arrival
core, 1,500 statements each:

| S (shipped sessions, one arrival core) | ips | ips / S | p0 | p25 | **p50** | p99 | arrival cpu | owner cpu |
|---|---|---|---|---|---|---|---|---|
| 1 | 857.9 | 858 | 355.5 | 1,052.2 | **1,090.9** | 2,025.1 | 0.812 | 0.024 |
| 2 | 1,720.7 | 860 | 236.8 | 1,055.3 | **1,082.9** | 2,060.3 | 0.897 | 0.041 |
| 4 | 3,548.4 | 887 | 23.3 | 1,051.5 | **1,080.5** | 2,083.5 | 0.941 | 0.061 |
| 8 | 7,108.0 | 889 | 28.2 | 1,036.0 | **1,079.9** | 2,205.3 | 0.946 | 0.079 |

**Throughput is linear in S at ≈ 880 × S while p50 sits pinned at 1,080 µs.**
The millisecond is a *shared* wait, exactly like the group commit's sync
trip: the owner sleeps, wakes once, drains the whole batch of pending ring
requests, executes them (S × ~20 µs), replies to all of them, and goes back
to sleep. That is the model the numbers fit rather than a trace of the
reactor, but the occupancy column is the same fact from the other side —
eight sessions leave the owner **92% idle**, and the whole batch costs it
~150 µs of a 1.13 ms cycle.

That is why the deficit vanishes at load under `group` durability: from four
sessions upward the owner always has a commit staged or a statement running,
`HasReadyTask()` is true, `IdleTimeoutMs` returns 0, and the sleep never
happens.

---

## 5. B2 — the R2 curve: shipping tracks the seated curve within the noise floor

**Measured** — `cores = 4`, S sessions on foreign arrival cores (dealt
round-robin over the three non-owner cores, core 0 included), all writing
**one** owner's relation, autocommit, 3,000 INSERTs per session, 5 reps.
Paired arm identical but seated on the owner (T1b's shape re-run on this
tree).

```
bench/single_relation_probe.py --server ~/ssb/bin/kds_server \
    --workdir ~/ssb/run/b2-sS-<a|b>-rN --arm multi --cores 4 \
    --sessions S --relations 1 --rows 3000 --seat <owner|foreign>
```

| S | seated ips | shipped ips | raw ratio | **corrected** | corrected spread | attempted / executed / refused (per arm) |
|---|---|---|---|---|---|---|
| 2 | 1,156.9 | 1,094.0 | 0.955 | **0.940** | 0.918–0.978 | 30,000 / 30,000 / 0 |
| 4 | 2,301.4 | 2,173.2 | 0.944 | **0.929** | 0.862–0.989 | 60,000 / 60,000 / 0 |
| 8 | 4,389.3 | 4,322.2 | 1.005 | **0.989** | 0.946–1.001 | 120,000 / 120,000 / 0 |
| 14 | 7,643.7 | 7,290.3 | 0.962 | **0.947** | 0.852–0.981 | 210,000 / 210,000 / 0 |

Rows in = rows out in every rep of every cell.

**Verdict 1 — does throughput track ≈ 590 × S?** Yes, on both arms:

| S | seated ips / S | shipped ips / S | shipped, as a fraction of 590 × S |
|---|---|---|---|
| 2 | 578 | 547 | 0.93 |
| 4 | 575 | 543 | 0.92 |
| 8 | 549 | 540 | 0.92 |
| 14 | 546 | 521 | 0.88 |

T1b fitted the seated peer curve at ≈ 590 × S on this host with 1,000 rows
per session; this run re-fits it at **546–578 × S with three times the rows**,
and the shipped arm at **521–547 × S**. The law survives the wire. The 5%
sublinearity from S = 2 to S = 14 is present on **both** arms equally, so it
is the relation's growing btree, not shipping.

**Verdict 2 — is per-statement latency near two syncs?** Yes, and flat in S:

| S | seated p0 | p25 | p50 | p95 | p99 | shipped p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 672.5 | 1,319.0 | 1,975.4 | 2,321.5 | 2,455.5 | 680.7 | 1,591.1 | 1,771.2 | 2,261.4 | 2,887.9 |
| 4 | 608.4 | 1,227.4 | 1,918.0 | 2,344.0 | 2,481.3 | 513.4 | 1,599.6 | 1,863.8 | 2,347.8 | 3,047.5 |
| 8 | 605.7 | 1,355.9 | 1,985.0 | 2,405.7 | 2,566.2 | 516.3 | 1,378.5 | 1,828.0 | 2,932.4 | 4,471.1 |
| 14 | 611.5 | 1,259.5 | 1,977.4 | 2,498.1 | 2,678.9 | 493.5 | 1,244.8 | 1,877.5 | 3,295.3 | 4,574.2 |

Microseconds; medians over 5 reps. p50 is 1.77–1.99 ms on both arms and does
not move with S — the pretasks' 1.7–2.0 ms band, reproduced with the wire in
the path. **The shipped p50 is consistently *lower* than the seated one**
(by 100–160 µs) while its p99 is 400–1,900 µs higher: shipping moves work off
the owner's reactor into a ring drain and lengthens the tail, which is what
the p95/p99 columns say and what a mean would have hidden.

**Verdict 3 — the local-vs-shipped gap is the wire + waiter cost at scale.**
It is **1–7%, and at every S it is inside the ±10% noise floor §2 measures.**
The memo's case — that ~20 µs against ~0.9 ms should make the wire nearly
invisible — is upheld at the level of the *conclusion*. §4 records that it is
upheld for the wrong reason: what disappears at load is not a small cost, it
is a millisecond that stops being paid.

**Claim 3 — the owner does not saturate, so it is unproven rather than
disproven.** At the top of the curve:

| cell | owner core | owner cpu (busy fraction) | owner `sched_foreground_polls` | owner `polled_us` as % of `sched_wall_us` | reactor time charged to no group |
|---|---|---|---|---|---|
| S = 14 seated | 3 | 0.147 | 84,022 | 7.4% | **92.5%** |
| S = 14 shipped | 3 | 0.241 | 84,014 | 3.2% | **96.2%** |
| S = 16 seated (`b4-k16`) | 3 | 0.155 | 96,024 | 7.7% | 92.2% |
| S = 16 shipped (`b4-k16`) | 3 | 0.112 | 96,023 | 3.1% | 96.4% |

The owner runs at **11–24% of one core** at the busiest cell this harness can
build, and 92–96% of its reactor wall clock is charged to no scheduling group
— the `fdatasync` and the idle block. Its execution ceiling is nowhere near.
**Claim 3 is unproven**, exactly as the order anticipated: the pretask run
already judged that a single reactor's execution ceiling is not reachable
with this harness, and that judgement stands. What *is* new is that a
resource **does** saturate under shipping, and it is not the owner — it is
the arrival core (§6).

**And a result that cuts against claim 3 from the other side: a shipped
statement costs the owner *less* than a seated one.** The owner's foreground
group is polled exactly **2.00 times per statement in both arms** — the same
statements, the same count — but the cost of a poll more than halves when the
statement arrives over the ring:

| cell | arm | owner `sched_foreground_polls` | polls per statement | `polled_us` | **µs per poll** |
|---|---|---|---|---|---|
| S = 14 | seated | 84,022 | 2.00 | 409,377 | **4.87** |
| S = 14 | shipped | 84,014 | 2.00 | 185,315 | **2.21** |
| K = 16 | seated | 96,024 | 2.00 | 424,546 | **4.42** |
| K = 16 | shipped | 96,023 | 2.00 | 173,364 | **1.81** |

Seated, the owner's foreground coroutine is the whole dispatcher path —
socket read, parse, execute, render, socket write. Shipped, the socket and
the render happen on the *arrival* core and the owner does parse, execute and
stage only. **Shipping moves 2.6 µs per statement of the owner's own CPU onto
the arrival core**, which is also why §5's shipped p50 sits consistently
below the seated one. Whatever the owner's execution ceiling is, shipping
moves it further away rather than closer.

---

## 6. B3 — not run, and why

The order's condition is explicit: *"**Measure.** Only if B2's curve departs
from ≈ 590 × S."* §5's fit table is the test, and the curve does not depart —
the shipped arm holds 521–547 × S across a sevenfold range of S, 88–93% of
the stated law, with the shortfall present on the seated arm too and
therefore attributable to the btree rather than to a tail page. There is no
departure to separate into a per-page component.

**B3 is therefore not run, and the stride question does not return on this
run's evidence.** That is a statement about this cell only: nothing here says
a tail page never serializes, only that at `cores = 4` with up to fourteen
sessions on one ascending btree tail there is no residual for it to explain.
`docs/inflight/in-progress/workplan-stride-forest.md` keeps its own SF-V0 premise probe, which
this host still cannot run at `cores > 4` without SMT siblings sharing
physical cores.

---

## 7. B4 — the waiter population: the spin is real, and it costs a whole core from K = 1

This is the memo's falsifier 2, the one still open, and the answer is
unambiguous. **Measured** — `cores = 4`, one relation, K sessions all on
**one** arrival core (the lowest peer that is not the owner, so the block is
not core 0's listener and catalog work), 3,000 INSERTs each, 5 reps:

```
bench/single_relation_probe.py --server ~/ssb/bin/kds_server \
    --workdir ~/ssb/run/b4-kK-<a|b>-rN --arm multi --cores 4 \
    --sessions K --relations 1 --rows 3000 --seat <owner|foreign> \
    [--arrival-core -1]
```

| K | seated ips | shipped ips | corrected ratio | corrected spread | arrival core **polls/s** | `polled_us` | **µs per poll** | polled as % of wall | arrival **cpu** |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1,235.4 | 536.1 | **0.429** | 0.407–0.449 | **3,121,376** | 1,028,098 | **0.059** | 18.3% | **0.893** |
| 4 | 2,213.9 | 2,133.3 | **0.951** | 0.891–0.980 | **6,350,706** | 2,150,489 | **0.060** | 38.0% | **0.925** |
| 16 | 8,772.6 | 8,703.8 | **0.976** | 0.972–0.981 | **7,860,133** | 2,996,077 | **0.068** | 53.6% | **0.929** |

Attempted / executed / refused: 15,000/15,000/0, 60,000/60,000/0,
240,000/240,000/0. Rows in = rows out throughout.

**The spin signature the memo named is present exactly as it described it:
polls climbing while polled stays flat.** From K = 1 to K = 16 the poll rate
rises 2.5× and the *cost of a poll* does not move at all — 0.059, 0.060,
0.068 µs. The rising `polled_us` percentage is the poll *count* rising, not
any poll doing more work. The mechanism is `IdleTimeoutMs` returning 0 while
a `WaitUntil` predicate task sits in a ready queue
(`src/sched/scheduler.cpp:196-199` and `include/kds/sched/coro.hpp:425`'s
`WaitUntil`, **source-read**), which is the same line §4a reads from the
other side.

**The finding the memo could not anticipate: one parked waiter already burns
the core.** At K = 1 the arrival core is **89% busy** doing nothing but
asking a predicate three million times a second whether a reply has arrived.
K = 16 takes it to 93%. The population size barely matters; the *existence*
of a parked waiter is what costs a core. In `b2-s14`'s shipped arm two
arrival cores sat at **0.921 and 0.921** busy against **0.058 and 0.051** in
the seated arm — two entire CPUs converted into spin, with no throughput
consequence on this box because two CPUs happened to be free.

**Either answer closes pretasks §8c honestly, and this is the answer.** The
waiters do not cost *throughput* here; they cost CPU, and they will cost
throughput on any host where the arrival cores are not idle. This is handed
to `docs/spec/sched.md` §4's owner together with §14's other scheduler findings;
this run fixes nothing.

The seated arm at K = 16 (8,773 ips) is the highest `group` cell in this
document and is still 6.4× below the client ceiling.

---

## 8. B5 — demand conversion: 80–92% refused becomes 0% refused, and the residue is a distribution

### 8a. Conversion

**Measured** — `bench/refusal_baseline_probe.py` **unchanged**, an unrouted
client (sessions taken as the kernel gives them, writes round-robin over
every relation including the ones the session's core does not own), the four
cells `bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md` §9b
measured, `--tables 6 --rows 200`, 5 reps each:

```
bench/refusal_baseline_probe.py --server ~/ssb/bin/kds_server \
    --workdir ~/ssb/b5work --cores <4|8> --sessions <4|8> --tables 6 --rows 200
```

| cores | sessions | attempts | accepted (median) | refused (median) | **refusal rate** | CC3 class, all 5 reps | lease-refill class, all 5 reps | `cross_core_write_refusals` (every core, every rep) |
|---|---|---|---|---|---|---|---|---|
| 4 | 4 | 800 | 776 | 24 | **0.030** (0.013–0.030) | **0** | 104 | **0** |
| 4 | 8 | 1,600 | 1,563 | 37 | **0.023** (0.011–0.029) | **0** | 179 | **0** |
| 8 | 4 | 800 | 776 | 24 | **0.030** (0.026–0.030) | **0** | 117 | **0** |
| 8 | 8 | 1,600 | 1,553 | 47 | **0.029** (0.012–0.031) | **0** | 208 | **0** |

The pretask reading of the same four cells was **0.841, 0.799, 0.924, 0.842**
with 667, 1,266, 734 and 1,334 CC3 refusals. **Every one of those is gone.**
Not one CC3 refusal survives in 4,800 write attempts across twenty runs, and
the engine's own counter agrees from every core in every run: `0`.

What remains is a class shipping was never going to remove: the retryable
lease refill (PW1b), 1.2–3.1% of attempts. It is *larger* than the pretask
run's 0.4–0.8% for the obvious reason — the pretask run's writes were mostly
refused before they could draw a lease, and 776 accepted writes per cell draw
far more row-id and extent leases than 127 did. This probe deliberately does
not retry, so these count as refusals; a client that follows the `retryable=1`
bit clears them, which is what every other driver in `bench/` does.

**`docs/inflight/known-gaps.md`'s 80–92% entry closes with this number: 0.0% CC3, 0
counted by the engine, four cells out of four.**

### 8b. The residue, by class — the 2PC evidence base's first reading

Because the unchanged probe's workload is entirely inside D1's scope, its
residue is empty, and an empty residue tells a 2PC designer nothing. So the
probe gains a `--residue` phase that exercises the shapes shipping declines
by construction, and reports what each one meets. **Measured** — same server,
8 unrouted sessions, 5 reps of each shape per session:

```
bench/refusal_baseline_probe.py --server ~/ssb/bin/kds_server \
    --workdir ~/ssb/b5res --cores 4 --sessions 8 --tables 6 --rows 200 \
    --residue --residue-reps 5 --residue-limit 200
```

| shape | what puts it outside D1 | outcome | n | wire status |
|---|---|---|---|---|
| `autocommit_write` | nothing — the control | **accepted** | 40/40 | — |
| `autocommit_read` | nothing — the control (`LIMIT 2`) | **accepted** | 40/40 | — |
| `in_explicit_txn` | `BEGIN` then a foreign write; `MayShip` refuses on `in_explicit_txn()` | `cross_core_cc3` | 40/40 | `TXN_CONFLICT retryable=1` |
| `subquery_write` | `UPDATE … WHERE … IN (SELECT … FROM <other>)`; the fork skips a subquery predicate whatever the owners are | `cross_core_cc3` | 40/40 | `TXN_CONFLICT retryable=1` |
| `two_owner_read` | a join whose two relations have two owners; `SoleForeignOwner` declines | `cross_core_read` | **35**/40 | `Unsupported retryable=0` |
| `two_owner_read` | *the same statement from a session on core 0* | **accepted, with the right rows** | **5**/40 | — |
| `overlong_read` | in scope and shipped; the answer does not fit the ring | `shipped_reply_overlong` | 40/40 | `UNKNOWN_OUTCOME retryable=0` |

Verbatim, so the spellings are on record:

```
ERR TXN_CONFLICT retryable=1 this transaction's writes are bound to core 3 and relation
'r1' is owned by core 1; a transaction may write on one core only until two-phase commit exists

ERR relation 'r1' is owned by core 1 and this statement is running on core 3;
cross-core reads need the step pipeline, which is not built

ERR UNKNOWN_OUTCOME retryable=0 statement shipping: the statement executed on its owner
but its reply is 2170 bytes, past the 992 a reply carries; the statement's effect stands
and its answer is lost
```

**The two-owner read splits by arrival core, and that is not a defect.**
Chased down with a dedicated reproduction
(`bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133/two_owner_repro2.py`, archived
beside this file with its first form): a session on **core 0** answers `SELECT a.balance FROM a JOIN b ON
b.id = a.id` with the correct four rows where `a` is core 3's and `b` is core
1's, because P4d's two-step pipeline is reachable from core 0's dispatcher
(`src/server/command_dispatcher.cpp:5130-5148`) and runs *below* the affinity
check. A session on a peer has no such pipeline and meets the refusal. So the
multi-owner **read** population is already partly converted by machinery that
predates shipping; the multi-owner **write** population is not converted at
all, and it is the whole of what R6 must be designed for.

**Reported as a distribution, as the order asks:** of the six out-of-scope
shapes, two (`in_explicit_txn`, `subquery_write`) are the write population
2PC would have to carry, at 100% refusal with a retryable bit that will never
clear; one (`two_owner_read`) is 87.5% refused and 12.5% already served by
P4d; one (`overlong_read`) is a shipping-internal bound, not a 2PC question,
and §14's finding 7 hands it over. `docs/inflight/known-gaps.md`'s R6 entry stays open and should
now point here.

### 8c. Where the 992-byte cap actually bites

The order estimates "roughly 40 wide rows". **Measured**, for a narrow row
(`id int64, owner varchar, balance int64`), on a peer-owned relation read
from a foreign session:

```
bench/shipped_reply_cap_probe.py --server ~/ssb/bin/kds_server \
    --workdir ~/ssb/cap --cores 4 --rows 200
```

| | |
|---|---|
| Largest shipped `SELECT * … LIMIT k` answered | **k = 99**, reply 987 bytes |
| First refused | **k = 100**, reply would be 998 bytes |
| Refusal | `ERR UNKNOWN_OUTCOME retryable=0 … its reply is 998 bytes, past the 992 a reply carries; the statement's effect stands and its answer is lost` |
| The same statement seated on the owner | answered, at every k up to 200 |

So the cap is 992 bytes exactly (`kCoreRingPayloadBytes` 1,024 −
`kShippedStatementReplyFixedBytes` 32, `include/kds/sched/ring_transport.hpp:133`
and `include/kds/server/statement_ship_service.hpp`, **source-read**) and, for
this row shape, that is 99 rows — not 40. The number scales inversely with
row width, so "40 wide rows" and "99 narrow rows" are the same bound; a
results file quoting a row count must say which shape it measured.

---

## 9. B6 — the abandoned transaction, priced from both sides

The dispatch fork sits after `BeginWrite`
(`src/server/command_dispatcher.cpp:5615-5623`, `:3646`, `:6352`,
**source-read**), so every shipped write opens a transaction on the arrival
core and abandons it. The order asks for the trade as a comparison.

**Measured** — one session, one arrival core, **25,000** shipped statements
(6.1 trx-id lease blocks; `kTrxIdBlockSize = 4096`,
`include/kds/txn/trx_id.hpp:74`), 5 reps, the full per-statement latency
series captured in arrival order:

```
bench/single_relation_probe.py --server ~/ssb/bin/kds_server \
    --workdir ~/ssb/run6/b6-<a|b>-rN --arm multi --cores 4 --sessions 1 \
    --relations 1 --rows 25000 --seat <owner|foreign> [--arrival-core -1] \
    --trace-latencies
```

| arm | ips | p0 | p25 | p50 | p95 | p99 | attempted / executed / refused | rows in = rows out |
|---|---|---|---|---|---|---|---|---|
| seated | 876.6 (868–897) | 437.3 | 1,074.0 | 1,160.6 | 1,348.1 | 1,444.4 | 125,000 / 125,000 / 0 | yes |
| shipped | 472.5 (469–474) | 869.8 | 2,003.5 | 2,127.9 | 2,756.7 | 3,158.5 | 125,000 / 125,000 / 0 | yes |

Corrected ratio **0.531** (corrected spread 0.517–0.537) — the same S = 1 price §3 and §4
measured, unchanged over 25,000 statements. Nothing degrades with sustained
load.

**Refill frequency.** PW7's triple, read from every core:

| arm | core | lease | requests | wait_max | submit lag | to-grant lag | resume lag |
|---|---|---|---|---|---|---|---|
| seated | 1 (idle peer) | trx-id | **1** | 20.43 ms | 0.00 | 20.43 | 0.00 |
| seated | 3 (owner) | trx-id | 7 | 20.43 ms | 0.02 | 20.43 | 0.00 |
| shipped | **1 (arrival)** | trx-id | **7** | **9.44 ms** | **0.00** | **9.44** | **0.00** |
| shipped | 3 (owner) | trx-id | 7 | 9.19 ms | 0.01 | 9.19 | 0.00 |

**The abandoned transaction costs the arrival core one extra trx-id refill
per 4,096 shipped statements** — 6 extra over 25,000, exactly the block
arithmetic. Each costs at most 9.44 ms, and the triple says all of it is core
0's turnaround (`to-grant`), none of it the arrival core's own reactor
(`submit` and `resume` are 0.00 ms) — PW7's two scheduling floors holding.
Amortised: 6 × 9.44 ms over 25,000 statements = **2.3 µs per statement**.

**No latency step at a block boundary.** Aggregated over all 5 reps, the
statements within ±W of every 4,096 boundary against every other statement of
the same run:

| arm | W = 0 | W = 2 | W = 8 |
|---|---|---|---|
| seated | 1.015 | 1.031 | 1.029 |
| shipped | 1.079 | 1.027 | 1.022 |

The seated arm — which opens no abandoned transaction on any arrival core —
shows the same 1.02–1.03×, so the effect is the run's own variability. The
ten statements per rep that exceed 3× the median land at block offsets 234,
764, 904, 1,423, 1,987, 2,546, 2,704, 3,116, 3,213, 3,725 — uniformly across
the block, not at its edge. **A refill is visible as ~8–10 ms on fewer than 1
statement in 2,500, and p99 does not move.**

**The cost the order did not name, and it is the larger one: WAL volume.**
Segments are preallocated, so `ls` and `du` say 64 MiB whatever was written;
the written extent is the offset of the last non-zero byte. **Measured**, one
25,000-statement run per arm with the data directory kept:

| arm | core 0 | core 1 | core 2 | **core 3 (owner)** |
|---|---|---|---|---|
| seated (session on core 3) | 7,578 B | **4,650 B** | 4,650 B | 12,363,859 B |
| shipped (session on core 1) | 7,994 B | **1,605,243 B** | 5,050 B | 12,364,723 B |

The owner's stream is unchanged (+864 bytes over 25,000 statements). The
**arrival core's stream grows from 4,650 bytes to 1,605,243** — 1,600,593
bytes over 25,000 statements = **64.02 bytes per shipped statement**, into a
log that would otherwise carry nothing. `kRecordHeaderSize = 32`
(`include/kds/wal/record.hpp:236`) and TXN_BEGIN/TXN_ABORT carry no payload
(`include/kds/wal/payload.hpp:40`, both **source-read**): two 32-byte
envelopes, exactly. That is a **13% increase in the instance's total WAL
volume**, all of it transactions that did nothing.

Source-read confirms the one thing that would have been worse is not
happening: `EndWrite`'s abort arm
(`src/server/command_dispatcher.cpp:6262-6269`) does not set
`pending_commit_lsn_`, so the abandoned transaction is appended but never
waited on. It costs bytes and a lease slot, not a sync.

**The other side of the trade.** Moving the fork above `BeginWrite` costs a
second parse and catalog resolve on every **local** write. This run did not
measure that directly; the closest measurement in the tree is
`bench/results-ddl-catalog-read-ab.md`'s decomposition, which prices *"parse,
compile, one clustered descent, render"* at **+6.3 to +7.5 µs** — an upper
bound on a parse-and-resolve, since it includes a btree descent and a render
that a resolve does not need.

| | shipped path, today's fork (measured here) | local path, fork moved above `BeginWrite` (bounded from the tree) |
|---|---|---|
| per statement | 2.3 µs of refill + 64 B of WAL | ≤ 7.5 µs of parse + resolve |
| paid by | every shipped write | every **local** write |

On this evidence the fork is where it is more cheaply than the alternative,
and by a wide margin once the population sizes are considered — but that
comparison rests on a bound rather than a measurement of the alternative, and
**whether the fork moves is the operator's call**, as the order says.

---

## 10. The row-set sweep — shipping's cost is fixed, the btree's is not

`ck-tester` rule 9: a measurement at one cardinality cannot tell a fixed cost
from a per-row one. **Measured** — S = 4, `cores = 4`, the relation taking
`sessions × rows + 1` rows, 5 reps per cell:

| relation rows | per-session rows | seated ips | shipped ips | raw ratio | **corrected** | corrected spread | attempted / executed / refused |
|---|---|---|---|---|---|---|---|
| ~200 | 50 | 3,255.8 | 3,104.7 | 0.952 | **0.937** | 0.599–1.015 | 1,000 / 1,000 / 0 |
| ~1,000 | 250 | 3,304.9 | 2,716.9 | 0.775 | **0.762** | 0.583–1.072 | 5,000 / 5,000 / 0 |
| ~10,000 | 2,500 | 2,281.8 | 2,218.6 | 0.972 | **0.957** | 0.910–0.976 | 50,000 / 50,000 / 0 |
| ~12,000 (§5's S = 4 cell) | 3,000 | 2,301.4 | 2,173.2 | 0.944 | **0.929** | 0.862–0.989 | 60,000 / 60,000 / 0 |

**The 200- and 1,000-row cells are not findings and are printed to say so.**
At 1,000 and 5,000 statements per arm the corrected ratio spreads 0.58–1.07, three
times the null cell's own spread; the 0.762 median at 1,000 rows sits inside
that and must not be read as a dip. Only the two ≥10,000-row cells are
resolvable, and they agree with each other and with §5.

**What the sweep does say, twice over.** First, **the shipped/seated ratio
does not depend on relation size**: 0.937, 0.957 and 0.929 at 200, 10,000 and
12,000 rows. Shipping's cost is a fixed per-statement latency, exactly as §4
found, and not something that grows with the tree. Second, the **absolute**
rate does depend on it — the seated arm falls from 3,305 ips at 1,000 rows to
2,282 at 10,000, a 31% loss to btree depth and page splits, present equally
on both arms. A run that reported only one cardinality would have attributed
some of that to shipping.

---

## 11. Where a shipped statement's time goes

Rule 3's decomposition, built entirely from this run's own cells, for one
autocommit INSERT at S = 1, `cores = 4`, `durability = group`:

| wait | how it is isolated here | seated | shipped |
|---|---|---|---|
| client + socket round trip | `ping` at 1 thread, §2's ceiling probe (47,342/s) | 21.1 µs | 21.1 µs |
| write statement, no durability | `sync-relaxed` seated p50 (23.7) − the round trip | 2.6 µs | 2.6 µs |
| **durability / commit (`fdatasync`)** | `sync-group` seated p50 (720.3) − `sync-relaxed` seated p50 (23.7) | **696.6 µs** | 696.6 µs |
| **ship round trip — ring out, owner's idle block, ring back, waiter resume** | `sync-relaxed` shipped p50 (1,091.4) − seated p50 (23.7) | — | **1,067.7 µs** |
| lock or conflict wait | **does not apply**: 0 retries and 0 `TXN_CONFLICT` replies in every measured window of every cell | — | — |
| read wait | **does not apply**: the measured statement is an INSERT | — | — |
| **sum** | | **720.3 µs** | **1,788.0 µs** |
| **measured p50** | | **720.3 µs** | **1,784.1 µs** |

The residual is **−3.9 µs, 0.2%** — the decomposition is complete. Two
readings follow directly:

- **The ship round trip is 1.5× the sync it is supposed to be a rounding
  error beside.** The memo's ~1/40 estimate was of the *wire*, and the wire
  really is that cheap; what it did not account for is the sleep on the far
  side of the wire.
- **At S ≥ 4 the ship row goes to zero rather than shrinking**, because the
  owner stops being idle. That is why §5's gap is 1–7% rather than 150%.

---

## 12. Versus PostgreSQL

**No twin exists for this workload and none could be run on this host, and
this section exists so that absence is stated rather than silent.**

Two reasons, in order of weight. First, the A/B this document is built on has
no PostgreSQL analogue at all: PostgreSQL has no core-ownership model, no
per-core WAL stream and no notion of a statement arriving on the wrong core,
so "shipped versus seated" is not a knob that exists there. What *would* be
comparable is the absolute law — N sessions committing autocommit INSERTs
into one table — against §5's 546–578 × S seated and 521–547 × S shipped.

Second, and decisively for this run: **PostgreSQL is not installed on this
host.** `tools/pg_setup.sh` expects to `initdb` a scratch cluster on port
15433 and there are no `postgresql` packages present (`/usr/lib/postgresql`
does not exist, no `initdb`/`pg_ctl`/`psql` on `PATH`). Installing them
mid-run would have perturbed a box already shared with other worktrees.

**The task that would build the baseline**, named as rule 4 requires: install
the PostgreSQL server packages, `tools/pg_setup.sh init` (defaults only — a
tuned baseline is not a baseline), and add a `tools/pg_single_relation.py`
twin of `bench/single_relation_probe.py`'s seated arm: one table, N sessions,
autocommit single-row `INSERT`s with a server-issued key, S = 1, 2, 4, 8, 14,
reporting ips and p0/p25/p50/p95/p99. That gives §5's curve a second engine's
constant to sit beside, which is the number that would say whether ≈ 550 × S
is good.

---

## 13. The memo's three claims

| # | claim | verdict | judged by |
|---|---|---|---|
| 1 | Shipping is **throughput-positive** where more than one session targets an owner, and the margin grows with the session count | **Upheld on the order's test, missed on the literal wording** | §5 (B2) |
| 2 | Shipping is **throughput-negative** at or below one session per owner core | **Upheld — and by more than predicted** | §3 (B1), §7 (B4, K = 1), §9 (B6) |
| 3 | The binding constraint under shipping is the owner's **execution capacity**, not its sync rate | **Unproven, not disproven** | §5's owner-CPU block (B2 at S = 14) |

**Claim 1.** The order's test is *"does throughput track ≈ 590 × S"*, and it
does: 521–547 × S over S = 2…14, with the local-vs-shipped gap 1–7% and
inside the noise floor at every point. Read literally — *positive*, with a
*growing* margin — the claim is missed: shipping is never faster than the
seated arm at any S, and the corrected ratios (0.940, 0.929, 0.989, 0.947)
show no trend in S. The distinction matters, so both readings are recorded.
The claim's own framing rescues the literal form: the memo's stated "before"
for these sessions is a **refusal** (§8a measures it at 80–92%, now 0%), and
against zero throughput every one of these cells is positive.

**Claim 2.** Upheld unambiguously and in three independent cells: 0.526 at
one session per owner over three owner cores (B1), 0.429 at one session on
one arrival core (B4 K = 1), 0.531 sustained over 25,000 statements (B6).
Five reps each, corrected spreads inside ±5% of their own median, an order
of magnitude outside the noise floor. The memo predicted "a small net loss" from "a round trip and a
waiter"; the measured loss is **a factor of two**, and §4 shows it is neither
the round trip nor the waiter but a millisecond of owner sleep. **D6's
unconditional shipping is therefore paying a 2× penalty in the R1 regime
today**, which is the number `crosscore.md` §9's routing decision inherits.

**Claim 3.** Unproven. The owner core runs at 11–24% busy at the top of the
curve this harness can build, with 92–96% of its reactor wall clock charged
to no scheduling group; nothing about its execution capacity is being tested.
The order anticipated this outcome and asked that it be called unproven
rather than disproven, and that is the honest word.

The run adds two things the memo could not have. First, **a resource does
saturate under shipping and it is not the owner**: the arrival core, at
89–95% busy from K = 1 (§7). Second, **shipping moves the owner's ceiling
further away rather than nearer** — a shipped statement costs the owner's
foreground group 1.8–2.2 µs per poll against 4.4–4.9 µs seated, at the same
2.00 polls per statement, because the socket and the render happen on the
arrival core (§5). If claim 3's ceiling is ever reached, shipping will be
what postponed it.

---

## 14. Findings

Each tagged **measured** or **source-read**, each with its site.

1. **The owner reactor's idle block is the whole cost of shipping, and it is
   at least one millisecond.** A shipped statement's p50 tracks the block
   over a fivefold range — 1.08 / 2.10 / 3.11 / 5.12 ms at
   `wal_drain_interval_us` 1000 / 2000 / 3000 / 5000, throughput the exact
   reciprocal, seated control flat at 23 µs — and is unmoved below 1 ms
   (50/200/500/1000 µs all give 1.08 ms) because `IdleTimeoutMs` rounds up to
   whole milliseconds. §4a. **measured.** The floor and the absence of any
   wake path are **source-read**: `src/sched/scheduler.cpp:196-214` and
   `:319` → `src/sched/epoll_io_backend.cpp:88` for the rounding; no
   `eventfd`/`Wake` anywhere in `src/` or `include/`, the ring seen only by
   `Scheduler::DrainInbox()` at `src/sched/scheduler.cpp:62`. **This is not a
   shipping defect** — any cross-core message to an idle core pays it;
   shipping is the first feature to put one on a client's critical path.
   *Owner: `docs/spec/sched.md` §4. This run fixes nothing.*
   **Answered 2026-08-26/27** by that owner (`instructions/v2.3.0-reactor-wake.md`,
   worktree `v2.3.0-rwc1`): an eventfd per reactor, woken only when the
   destination is actually asleep. The cell above re-reads **0.416 → 0.989**
   (`bench/v2.3.0/results-reactor-wake-r1-v2.2.1-10-g01da467.md`) and this
   sweep's functional dependence is gone — 43.2 µs flat across the same four
   knob values, and 42.4 µs at 50,000
   (`bench/v2.3.0/results-knob-sweep-cell2-v2.2.1-14-g13c6d4d.md`). The
   rounding floor was left unbuilt: with nothing waiting for the timer, the
   residual is 20 µs of wire rather than a millisecond of block.
2. **The millisecond is shared, not serialized.** Under `relaxed`, shipped
   throughput is linear at ≈ 880 × S while p50 holds at 1,080 µs and the
   owner sits at 2–8% busy (§4b). **measured.** It is therefore invisible
   whenever the owner has work, which is why §5's gap is 1–7%.
3. **A single parked waiter costs a whole arrival core.** 89% busy at K = 1,
   93% at K = 16; 3.1M → 7.9M polls/s while the cost of a poll holds at
   0.059–0.068 µs — the memo's falsifier-2 spin signature exactly (§7).
   **measured**, mechanism **source-read** at
   `src/sched/scheduler.cpp:196-199` and `include/kds/sched/coro.hpp:425`.
   *Owner: `docs/spec/sched.md` §4; this closes pretasks §8c.*
   **Answered 2026-08-26** by the same order's RW3 ("parked is not ready"):
   the arrival core reads **0.032** at K = 1 and 0.028 at K = 4
   (`bench/v2.3.0/results-parked-is-not-ready-v2.2.1-12-g12c0ebb.md`,
   `bench/v2.3.0/results-hot-path-cell4-v2.2.1-14-g13c6d4d.md`). The trade
   is stated there rather than netted off: at K = 1 on an idle box it costs
   0.900× throughput and +31.5 µs p50, and by K = 4 that is gone.
4. **The 80–92% cross-core write refusal is gone.** 0 CC3 refusals in 4,800
   unrouted write attempts over 20 runs at `cores` 4 and 8, engine counter 0
   from every core in every run (§8a). **measured.** *Closes
   `docs/inflight/known-gaps.md`'s 80–92% entry with its number.*
5. **The R6 residue is two write shapes, and one read shape that is already
   half-converted.** `in_explicit_txn` and `subquery_write` refuse 100% with
   a retryable bit that never clears; `two_owner_read` refuses 87.5% and is
   answered correctly for the other 12.5% by P4d's pipeline when the session
   lands on core 0 (`src/server/command_dispatcher.cpp:5130-5148`,
   **source-read**; the split **measured** in §8b and reproduced separately).
   *`docs/inflight/known-gaps.md`'s R6 entry stays open and should point here.*
6. **The abandoned transaction's real cost is WAL volume, not the lease.**
   64.02 bytes per shipped statement into an otherwise-idle stream — a 13%
   rise in instance WAL — against 2.3 µs/statement of amortised refill and no
   latency step at any 4,096 boundary (§9). **measured**; record sizes
   **source-read** at `include/kds/wal/record.hpp:236` and
   `include/kds/wal/payload.hpp:40`; the abort's non-durability
   **source-read** at `src/server/command_dispatcher.cpp:6262-6269`.
7. **The 992-byte reply cap is 99 rows for a three-column narrow row**, not
   40, and past it a *read* that already executed answers
   `UNKNOWN_OUTCOME retryable=0` (§8c). **measured**; the bound
   **source-read** at `include/kds/sched/ring_transport.hpp:133` and
   `include/kds/server/statement_ship_service.hpp`. *Owner:
   `statement_ship_service.hpp` rule 1 — the row count belongs beside the
   byte count there, since a caller reasons in rows.*
8. **Shipping's cost does not scale with the relation.** 0.937 / 0.957 /
   0.929 at 200 / 10,000 / 12,000 rows while the absolute rate falls 31%
   from btree depth on both arms (§10). **measured.**
9. **The dedup and identity machinery is quiet under every load this run
   applied.** Across 360 per-core `SHOW META` readings:
   `shipped_identity_mismatches`, `shipped_early_evictions`,
   `shipped_unanswerable`, `shipped_deduped`, `shipped_late_executed`,
   `shipped_waiting` and `shipped_running` are **all 0**, and
   `shipped_statements` equals `shipped_replies` equals `shipped_executed` in
   every cell. **measured.** `shipped_refusals` is non-zero (2–6 per rep) and
   is fully explained: it equals the warm-up INSERT's retryable lease-refill
   refusals, which the driver retried and cleared — the counter includes
   refusals a client recovered from, which is worth knowing before it is read
   as an error count.
10. **The pretask harness's ~1.099 ordering bias is not a property of this
    box.** Two null cells return 0.991 and 1.016 (§2). **measured.** It was
    `tools/multicore_benchmark.py`'s two-configurations-in-one-process shape;
    a driver that runs each arm as its own process does not carry it.
11. **A shipped statement costs the owner less CPU than a seated one.**
    Exactly 2.00 owner foreground polls per statement in both arms, at
    **4.87 → 2.21 µs per poll** (S = 14) and **4.42 → 1.81 µs** (K = 16):
    the socket read, the render and the socket write move to the arrival
    core (§5). **measured.** It is why the shipped p50 sits below the seated
    one at every S in §5's latency table, and it pushes claim 3's ceiling
    further away rather than nearer.
12. **94–98% unaccounted reactor time reproduces**, at 92.2–99.9% across
    every core in every cell here (§5's table). **measured.** Unchanged, not
    this run's to fix; still owed to `docs/spec/sched.md` §4.

---

## 15. What this run does not measure

- **Explicit-transaction shipping and multi-owner statements.** Out of scope
  by design (D1). §8b measures their *refusal* distribution; it measures
  nothing about how they would perform if carried.
- **Reads beyond the 992-byte cap.** §8c locates the cap and shows the
  refusal; no read larger than it was benchmarked, because past the cap there
  is no answer to time.
- **Shipped reads as a throughput workload.** D1 includes reads and §8b
  confirms a small one converts (40/40 accepted where a peer session would
  otherwise be refused), but no read cell was swept for throughput or
  latency. A read twin of §5's curve is the obvious next cell.
- **Any cell within 2× of the client ceiling.** §4's two `relaxed` *seated*
  arms (36,138 and 74,960–77,795 ips) are at or through the 56–59k ceiling
  and are used only as the zero-point of a difference. No `group` cell is
  within a factor of three of it.
- **`cores = 8` for the A/B cells.** Only §8a's conversion cells ran at
  `cores = 8`. Above `cores = 4` this host's reactors share physical cores
  through SMT, so a throughput ratio there would confound shipping with
  hyperthreading.
- **B3, the tail page.** Not run: §6 states the order's condition and why it
  is not met.
- **Recovery across the wire** (the order's §5-6 item): no kill −9 mid-burst
  was performed here. Correctness runs on its own track.
- **The alternative fork position.** §9's right-hand column is a bound taken
  from `bench/results-ddl-catalog-read-ab.md`, not a measurement of a moved
  fork.
- **PostgreSQL.** §12: no twin exists and the packages are not on this host;
  the task that would build one is named there.
- **The correctness suite.** Not executed by this run. No engine code was
  changed; only `bench/` drivers.

---

## Reproducing

Every cell, in the order it ran. `build-release` only, `--workdir` on ext4,
binary copied out of the build tree first.

```
cp build-release/kds_server ~/ssb/bin/kds_server      # sha256 8f31547a…

# gates
bench/client_ceiling_probe.py --server ~/ssb/bin/kds_server \
    --workdir ~/ssb/ceiling --threads 1,2,4,6,8,14

# null cells, B1, B2, B4  (5 reps each, arms in fixed order a then b)
bench/run_ssb.py --server ~/ssb/bin/kds_server --workdir ~/ssb/run \
    --archive bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133 \
    --cells null1,null4,b1,b2,b4 --reps 5 --rows 3000 --port 17200

# the row-set sweep
bench/run_ssb.py --server ~/ssb/bin/kds_server --workdir ~/ssb/runsz \
    --archive bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133/sz \
    --cells sz --reps 5 --port 20200

# the durability control that attributes the S = 1 gap
bench/run_ssb.py --server ~/ssb/bin/kds_server --workdir ~/ssb/runsync \
    --archive bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133/sync \
    --cells sync --reps 5 --rows 3000 --port 21000

# B6
bench/run_ssb.py --server ~/ssb/bin/kds_server --workdir ~/ssb/run6 \
    --archive bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133/b6 \
    --cells b6 --reps 5 --port 18400

# B5 conversion (four cells x 5 reps) and the residue
bash bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133/run_b5.sh
bench/refusal_baseline_probe.py --server ~/ssb/bin/kds_server \
    --workdir ~/ssb/b5res --cores 4 --sessions 8 --tables 6 --rows 200 \
    --residue --residue-reps 5 --residue-limit 200

# the three attribution probes: the drain sweep below 1 ms (the flat half,
# which is the rounding), above it (the half that discriminates), and the
# relaxed session curve
bash bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133/drain_sweep.sh
bash bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133/drain_sweep2.sh
bash bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133/relaxed_curve.sh

# the reply cap
bench/shipped_reply_cap_probe.py --server ~/ssb/bin/kds_server \
    --workdir ~/ssb/cap --cores 4 --rows 200

# the WAL-volume pair (data directory kept, then the written extent scanned)
bench/single_relation_probe.py --server ~/ssb/bin/kds_server --workdir ~/ssb/wal \
    --arm multi --cores 4 --sessions 1 --relations 1 --rows 25000 --seat owner
bench/single_relation_probe.py --server ~/ssb/bin/kds_server --workdir ~/ssb/wal \
    --arm multi --cores 4 --sessions 1 --relations 1 --rows 25000 --seat foreign \
    --arrival-core -1
python3 bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133/wal_extent.py \
    ~/ssb/wal/multi-c4-s1-r1-*/probe.db.wal/wal-*.log
```

The drivers are documented in `bench/docs/README.md`. Raw per-rep JSON, the
orchestrator summaries and every driver's stdout are archived beside this
file under `bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133/`; no data file and
no WAL segment is archived. B6's per-rep files carry the full 25,000-entry
latency series that §9's boundary table is computed from; the same series was
stripped from the duplicate copies inside `summary.json`.
