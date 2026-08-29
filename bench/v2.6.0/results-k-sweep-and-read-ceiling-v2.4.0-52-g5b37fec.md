# R4-M — the k sweep, the read ceiling, and the surface a spread relation keeps

Work order `instructions/v2.6.0/r4-k-sweep.md`, measured in worktree
`v2.6.0-ksweep`. Every table names the commit it was measured at.

## 1. The headline, before any table

**§3's read ceiling is real and this file measures it, but it is not the
binding constraint — and the constraint that binds was not in the order.**

The order carried `workplan-insert-spreading.md` §3's arithmetic — *a
spread relation is readable up to 64 × `range_size_ids` rows and refuses
every `SELECT *` after that; at 4,096 that is 262,144 rows* — and asked
for it as a measured number. It is measurable and §6 measures it. But two
harder limits sit in front of it, and a spread relation meets both at its
**second** range rather than at its sixty-fifth:

- **Under `placement = creating` a spread relation is unreadable from
  every core, in every shape.** That is the arrangement spreading exists
  to produce, the one IS7 configured, and the one this order's own k sweep
  configures.
- **Where it is readable at all, the surface is one shape**: `SELECT *`,
  optionally with a `WHERE`, optionally with a free `ORDER BY <pk> ASC`,
  from a **core-0 session only**. A projection, an aggregate, or a `LIMIT`
  is refused, and so is every shape from every peer.

Measured at **395 rows** on relations copied from this repository's own
scenario benchmarks (§5), not at 262,144. So the order's aggregate cell —
gated on CK4 exactly so that this would be established before it ran —
**is reported as not run**, with the refusal that stops it.

**Where the ceiling *is* reachable, §3's arithmetic is right** (§6): the
refusal fires at 65-72 stages and at 32,025 / 64,730 / 234,776 rows for
block sizes 512 / 1,024 / 4,096, against an arithmetic of 32,768 / 65,536 /
262,144. And it has a precondition §3 did not state — **two or more peers
must contend**. At one peer, IS5's suppression holds the relation at two
ranges and the ceiling was not reached after two million rows.

**The one number that changes a decision** is §6d's: `range_size_ids` below
4,096 does not merely lower the ceiling, it **costs throughput** — the
relaxed arm runs at 0.434× at 256 and 0.809× at 1,024 against ~1.0 from
4,096 up, because a small block is spent as fast as it is granted. §3
framed the knob as ceiling against burn; there is a third axis, and below
4,096 it dominates both.

## 2. Provenance

**Host.** Intel Xeon Platinum 8488C, **8 logical CPUs on 4 physical
cores**, SMT **active** (`/sys/devices/system/cpu/smt/active` = 1; sibling
pairs 0/4, 1/5, 2/6, 3/7), 1 socket, 1 NUMA node, KVM guest, 15 GiB RAM.
Linux 7.0.0-1011-aws, Ubuntu 26.04 LTS. Data files under `/home/ubuntu`,
**ext4 on `/dev/root`** (`df -T`) — a real device, not tmpfs.

**The host changed since IS7 and no number here is compared to one of
its.** IS7's 1.132× was taken on a 2-CPU box off a Debug build; the order
says plainly that it is not this file's baseline and is not subtracted
from. It is quoted in §4 only to say what it did *not* settle.

**A note on the neighbouring file's version string**, because the rule it
follows exists to stop exactly this. `bench/v2.6.0/results-insert-spreading-v2.2.1-127.md`
names itself `v2.2.1-127`, and `git describe --tags a135a59` — the commit
it documents — returns **`v2.4.0-48-ga135a59`**. `v2.4.0` is a reachable
tag and `v2.2.1` is two tags behind it, so that filename dates the run to
a build it was not taken on, which is the failure the "every measurement
names its version, and `git describe --tags` is how" rule names. Recorded
here rather than corrected in place: renaming another run's file breaks
citations to it, and the correction is the operator's to make.

**Build.** `cmake -DCMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG -std=c++20
-Wall -Wextra`, `-DKDS_WITH_TLS=1`. Every server started from a **copy**
of the binary at `/home/ubuntu/ksweep/kds_server`
(`sha256 b595837edc1090ee047745a9479132ec662be020400abb4f71d108d5fb3b5348`),
not from `build-release/kds_server`, per `bench/docs/README.md` — the
build tree is shared and a rebuild mid-matrix would swap the engine under
a run with nothing in the output to show it.

**Reactors are pinned one per CPU index** (`expeditor.cpp`,
`CPU_SET(core_id)` → `pthread_setaffinity_np`). With this host's sibling
map that makes the k sweep have a **structural boundary between k = 4 and
k = 5**: k ≤ 4 puts every reactor on its own physical core (CPUs 0-3),
and k ≥ 5 starts pairing each new reactor with an existing one's physical
core. §4 reads the curve against that boundary rather than around it.

**And the driver competes for the same eight CPUs.** The sweep runs k
engine reactors plus k client processes on one 8-CPU host, so at k = 8
every CPU carries a pinned reactor and the clients run wherever the
scheduler finds room. k ≤ 4 is the region where an engine number is not
also a scheduling number; k ≥ 5 is measured and reported, with that stated
beside it rather than in a footnote.

## 3. What was cleared first (IM0), because the order made it a row

Three debts, each recorded as closed rather than assumed. All at
`a135a59` plus this file's own test changes, in worktree `v2.6.0-ksweep`.

**The suite.** `ctest --test-dir build-release` — **3017 of 3017 passed, 0
failed**, 53.4 s wall. IS7's commit recorded 3008/3008 at `1d9ccbd` and
nothing since; the tree is now verified at the commit the measurements
were taken on. (3017 rather than 3008 because IM0's own review items added
nine tests; §3a.)

**The simulation corpus.** `scripts/sim.sh` — **171 runs, 0 failures**,
against `build-release/ckdbs-sim`. Same sitting as the suite, before the
first cell.

**`build-release` built and used throughout.** No number in this file
comes from a Debug build, and the order's §2 says why that matters: RD9(a)
found a sequential full sweep reading 14-18% slower in Debug and a
rep-interleaved re-check of the identical shape **reversing the sign**.

### 3a. RB5's two review items, and what closing the second one found

RB5 (`e5ab4f9`) landed at the operator's instruction with its review
incomplete, leaving two questions.

**Item 1 — does every shippable shape have an equivalence test?** No; it
had eight. Nine more are added, each a route that reaches the per-range
walk differently from the eight: a projection, `LIMIT`/`OFFSET` (three
cases — stopping short of the cut, on it, and across it), `ORDER BY` under
`LIMIT` (the top-N heap rather than the sort), a `GROUP BY` whose **group
straddles the boundary** (the merge that ungrouped aggregates never
exercise), `COUNT(DISTINCT)` (a set carried across the cut rather than a
scalar), joins with the split relation on either side plus a pk-keyed one,
five subquery shapes, the remaining comparison operators, and the
unpredicated writes. Three of these are places where a range-blind
implementation returns a **plausible short answer** rather than an error,
which is the defect class every review in this milestone caught.

`ExpectSame`'s relation substitution was rewritten from "replace the first
`" t"`" to a token-level identifier rewrite, because RB5's review had to
check each call site by hand to confirm the single replacement hit the
right occurrence — and a qualified name (`SELECT t.id FROM t`) hits the
wrong one, which is what had kept joins and subqueries out of the suite.
The reply's header is renamed back before comparison, bounded to the
header, so a qualified projection compares rows and not the substitution.

**Item 2 — does `TheEquivalenceRestsOnInsertionOrderMatchingRangeOrder`
assert the interesting half of its claim?** It asserted only the
`ORDER BY` half, which a neighbouring test already covered. Writing the
falsifier its comment described — *insert one row out of order, watch the
byte-identity break while the row set holds* — found something better than
a weak assertion: **the statement the comment describes is refused.** A
heap relation refuses a named key below its high-water mark
(`catalog.cpp`'s `AdmitExplicitRowId`; `heap-and-tuple.md` §3.1b/§4.1), so
the old test's two inserts both failed and it was vacuous in a second way
the review had not suspected.

That is now asserted rather than deleted, because it says what the
equivalence claim rests on: not a convention the fixture follows, **an
ascent the engine enforces**. And the falsifier moved to where R4 makes it
reachable — `core_runtime_test.cpp`'s `TwoPeersEachInsertIntoTheirOwnRangesTail`
gains a third round that **interleaves** two peer writers and asserts the
issued id sequence is *not* globally ascending while uniqueness (K1) and
the per-range ascent both hold. That is invariant 11's §4.1a amendment as
an assertion instead of a sentence, and it is unconstructible on one core:
two leased blocks are what get past the mark rule.

## 4. The k sweep — CK1, CK2, HK1, HK2, HK5

**Method.** One relation, written concurrently from k cores' sessions,
4,000 rows per core. Arms differ in one config key — `range_size_ids = 0`
(concentrated: peers ship to the owner) against `4096` (spread) — crossed
with `durability` ∈ {`group`, `relaxed`}, at k = 1…8. `placement =
creating` and `peer_listeners = on`, which is the arrangement that spreads
widest: the relation is core 0's, so every peer is a foreign writer that
takes a range of its own. Fresh server and data file per cell; **three
reps, arms interleaved** — one rep touches every (k, durability, arm) cell
once, so drift arriving mid-run lands on both arms rather than on whichever
ran second. Medians across reps.

**`peer_listeners` is omitted at k = 1**, where the engine refuses it
(*"has no peer to listen; the only effect would be losing the exclusive
bind on the one socket"*). So the k = 1 cell runs today's single-core
configuration exactly, which is what HK5 asks of it — the unmoved
baseline, not a one-core copy of the spread arm.

**The driver was rewritten for this order and the two changes are
load-bearing.** IS7 ran one k, one rep, arms in sequence, writers as
CPython **threads**. Threads are what had to go: the loop is one
synchronous send-and-read per row, so a thread holds the GIL for
everything between the two socket calls, and at k = 8 the driver would
bind before the engine did. Writers are now one process per core. §4b is
the control that says, per k, whether a number is the engine's.

### 4a. The numbers

Measured on `v2.6.0-ksweep` at `03b815b`, binary
`sha256 e493bc09…c886`. Medians of three interleaved reps; **every cell
placed all `4000 × k` rows, no cell reported a writer error, and no cell
came up short** (the driver prints `SHORT:` and `ERRORS:` lines and there
are none).

| k | group C | group S | **group S/C** | relaxed C | relaxed S | **relaxed S/C** |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 890 | 873 | **0.981** | 40,234 | 40,315 | **1.002** |
| 2 | 892 | 926 | 1.038 | 30,966 | 28,658 | 0.925 |
| 3 | 988 | 1,381 | 1.398 | 43,686 | 49,010 | 1.122 |
| 4 | 1,361 | 1,986 | 1.460 | 60,398 | 60,365 | 0.999 |
| 5 | 1,727 | 2,606 | **1.509** | 69,859 | 71,246 | 1.020 |
| 6 | 2,094 | 2,734 | 1.306 | 72,577 | 78,844 | 1.086 |
| 7 | 2,465 | 2,783 | 1.129 | 75,883 | 86,360 | 1.138 |
| 8 | 2,880 | 2,784 | 0.967 | 78,073 | 93,371 | **1.196** |

*(inserts/s; C = concentrated, `range_size_ids = 0`; S = spread, 4096.)*

**HK5 holds — `cores = 1` is unmoved.** 0.981 and 1.002, the eleventh
consecutive order. At one core there is no peer, no lease and no range:
`ids_burnt` is 0 on both arms, so the spread arm is not a configuration so
much as the same configuration under a different name.

**CK2, and §8's prediction is refuted in the direction that matters.**
§8 predicted the group arm flat-to-slightly-down, on v2.1.0's finding that
spreading divides the commit batch. It is **not flat and not down**: the
concentrated group arm itself rises 890 → 2,880 across k, and spreading
adds up to **1.51×** on top of that at k = 5. Whatever divides the batch,
it does not cost more than spreading buys anywhere below k = 8. v2.1.0's
result is not contradicted — it was measured on rotation of *independent*
sessions, and this is one relation — but the inference drawn from it in
§8, that a durably-committed insert workload cannot gain from spreading,
does not survive.

**And the two arms do not track each other**, which is the second half of
CK2 and directly corrects IS7's headline. IS7 found `group` and `relaxed`
agreeing to 0.2% at k = 2 and called that the finding. Across the sweep
they diverge sharply and in both directions — at k = 4, group is 1.460
where relaxed is 0.999; at k = 8, group is 0.967 where relaxed is 1.196.
**The 0.2% agreement was a coincidence of k = 2**, and the order was right
to refuse to build on it.

**CK1 — the shape, and what binds.** Two different curves:

- **The group arm peaks and declines**: 1.038, 1.398, 1.460, **1.509**,
  1.306, 1.129, 0.967. A clear maximum at k = 5 and a return to parity by
  k = 8. This is the decline `bench/v2.1.0` §3a warned was live — that
  device overlapped four streams at 3.37× before declining — arriving here
  one core later.
- **The relaxed arm rises, late, and monotonically past k = 5**: 1.020,
  1.086, 1.138, **1.196** at k = 6, 7, 8, with no peak inside the sweep.

The two arms disagreeing about *where* spreading helps is what names the
binding constraint, because the arms differ in exactly one thing: whether
`fdatasync` is on the critical path.

- Under `group`, spreading's benefit is that k committers batch k
  independent streams instead of one core serialising every write; it
  peaks at k = 5 and falls away as the reactors start sharing physical
  cores (§2: k ≥ 5 pairs each new reactor with an existing one's physical
  core), so the **device-plus-SMT boundary** binds the group arm.
- Under `relaxed` there is no sync, so the concentrated arm is bound by
  **core 0 serialising every shipped statement** — visible in §4b's
  control, where the same shape plateaus at ~80 k — and spreading lifts
  exactly that: 93,371 against 78,073 at k = 8, which is the plateau being
  exceeded.

So **CK1's answer names two constraints, not one**: the group arm is bound
by the device and the host's SMT pairing, and the relaxed arm by the
single owner's serialisation, which is the one spreading exists to remove.
The fan-in stage count — the third candidate the order listed — **binds
neither**, for the reason §6a gives: it is not reached before the read
surface closes.

**HK1 is supported with a correction.** Throughput does rise with k, and
the device does bind — but only on the arm that touches it. The falsifier
the order named for HK1 (that the stage count binds first) is not what
happened: the stage count binds *reads*, and this cell is writes.

**The one anomaly, stated rather than smoothed.** k = 2 relaxed is 30,966
concentrated against 43,686 at k = 3 and 40,234 at k = 1 — a dip below
both neighbours on both arms. Three reps agree, so it is not a single bad
cell. Unexplained; the most likely reading is that at k = 2 the one peer's
shipped statements and core 0's own work land on two physical cores that
share an L3 slice while k = 1 pays no wire at all, but nothing here
measures that and it is left as an anomaly rather than dressed as a
finding.

### 4b. The harness's own ceiling, per k — and it is not close

`bench/spread_client_ceiling.py`, k processes each on its own core's
session against a k-core server, 3 s per arm. `ping` is answered before any
parsing, so it is socket round trip plus CPython — the harness proper. The
other two run against a **btree** relation core 0 owns, so peers ship: that
is deliberately the *concentrated* shape, and its plateau is a second
reading of the same limit §4a measures.

| k | `ping` ops/s | pk `SELECT` ops/s | relaxed `INSERT` ops/s |
|---:|---:|---:|---:|
| 1 | 54,452 | 44,535 | 36,535 |
| 2 | 100,730 | 61,827 | 53,114 |
| 3 | 135,237 | 79,294 | 64,324 |
| 4 | 176,570 | 92,907 | 72,856 |
| 5 | 274,209 | 94,372 | 77,790 |
| 6 | 447,886 | 94,197 | 79,965 |
| 7 | 534,781 | 95,392 | 80,243 |
| 8 | 595,627 | 95,895 | 80,042 |

**The harness never saturates**: `ping` rises monotonically to 596 k ops/s,
six to seven times the engine's plateau, so no cell in §4a sits on the
driver's limit and none is reported as unresolved on that ground. The
process-per-writer rewrite is what bought that; a thread-based driver is
the reason the question had to be asked.

**And the two engine arms plateau at k ≈ 4-5**, at ~95 k reads/s and
~80 k inserts/s, which is one core-0-owned relation saturating its single
owner. That plateau is the number the spread arm has to beat.

## 5. CK4 — what a scenario relation does when spreading is armed

Measured with `bench/scenario_range_eligibility.py`, k = 4,
`range_size_ids = 64`, `placement = creating`, 400 peer inserts per
relation round-robined across the three peers. The twenty-four relations
are the schemas of `tools/scenario0_stockmarket.py`,
`scenario1_backtest.py`, `scenario2_freight.py` and `scenario3_library.py`,
copied verbatim.

| scenario | relation | clustered | rows | peer lease grants | spread | whole scan afterwards |
|---|---|---|---|---|---|---|
| s0 stockmarket | accounts | BTREE | 400 | 0 | no | ok |
| s0 stockmarket | users | BTREE | 400 | 0 | no | ok |
| s0 stockmarket | assets | BTREE | 400 | 0 | no | ok |
| s0 stockmarket | **trades** | HEAP | 395 | 9 | **yes** | **REFUSED** |
| s0 stockmarket | **user_periodic_profit** | HEAP | 396 | 9 | **yes** | **REFUSED** |
| s1 backtest | exchanges | BTREE | 400 | 0 | no | ok |
| s1 backtest | symbols | BTREE | 400 | 0 | no | ok |
| s1 backtest | sessions | BTREE | 400 | 0 | no | ok |
| s1 backtest | daily_bars | BTREE | 400 | 0 | no | ok |
| s1 backtest | **daily_stats** | HEAP | 394 | 9 | **yes** | **REFUSED** |
| s1 backtest | models | BTREE | 400 | 0 | no | ok |
| s1 backtest | **model_results** | HEAP | 395 | 9 | **yes** | **REFUSED** |
| s2 freight | organizations … recipes (6) | BTREE | 400 | 0 | no | ok |
| s2 freight | **freights** | HEAP | 395 | 9 | **yes** | **REFUSED** |
| s2 freight | **charges** | HEAP | 395 | 9 | **yes** | **REFUSED** |
| s3 library | users, books, reservations, loans | BTREE | 400 | 0 | no | ok |

**Six of twenty-four relations spread, and all six lost all five read
shapes** — `SELECT *`, `SELECT * WHERE id = 1`, `SELECT COUNT(*)`,
`SELECT id`, `SELECT * LIMIT 10` — at **≈395 rows**, three orders of
magnitude below §3's 262,144.

`range_size_ids = 64` here, chosen so 400 rows is enough to spread; the
**conclusion does not depend on it**, because what refuses these reads is
the relation having a second *owner*, not having many ranges (§6a). A
production block size delays the refusal by exactly one block and does not
remove it.

The eighteen that did not spread are the btree ones, and the reason is
structural rather than a gate firing: **the pump is heap-only.**
`command_dispatcher.cpp`'s `heap_omitting_pk` requires
`clustered_type == kHeap`, so a btree relation never records row-id
demand, never causes a refill, and `RangeEligible` is never asked about it
(**source-read**, `command_dispatcher.cpp` §"R4/IS3: which core this
INSERT belongs on"). Confirmed by measurement: 300 peer inserts into a
btree relation leave `rowid_refill_requests=0` where 300 into a heap twin
leave `requests=5 grants=5`.

**So CK4's answer is that the aggregate cell cannot run**, and not because
any relation is too large. Each scenario's append-only ledgers — the
relations spreading is *for* — become unreadable at the second range, and
every one of the four drivers reads them.

### 5a. An instrument that cannot see its most important entry

`SHOW META`'s `range_split_decline_detail` exists as RD5's C3 — *"which
gate declines how often on which relation is the evidence for which owning
decision to lift first"* (`command_dispatcher.cpp`). It cannot report D1.

A btree relation never reaches `RangeEligible`, per the paragraph above,
so `kBtree` — the gate that declines eighteen of these twenty-four
relations — is never recorded. The counter is not wrong; the decline it
would report never happens, because the question is never asked. But an
operator reading `range_split_decline_detail` to decide which gate to lift
first sees every gate except the one blocking the most relations.
**Measured**: the census's first form used that counter and reported all
twenty-four relations eligible, btrees included. This file's census reads
behaviour instead.

## 6. CK3, HK3, HK4 — the ceiling as a measured number

`bench/spread_ceiling_probe.py`, **`placement = rotate` from a core-0
session** (§6a: the only readable arrangement), `durability = relaxed`,
writers on every core, polled on rows rather than on the clock and densely
from 85% of the arithmetic. Measured at `5b37fec`
(`v2.4.0-52-g5b37fec`), binary `sha256 0f8eccdb…a070`. **Every cell
reported all its writers and none errored** (`writers_unreported` empty,
`writer_errors` empty in all fifteen).

| k | `range_size_ids` | last readable | refused at | **stages** | 64 × size | ids/stage | ids burnt |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 512 | 1,202,162 | **not reached** | — | 32,768 | — | 526 |
| 2 | 1024 | 1,442,846 | **not reached** | — | 65,536 | — | 994 |
| 2 | 4096 | 2,000,662 | **not reached** | — | 262,144 | — | 2,282 |
| 3 | 512 | 33,040 | 33,393 | 65 | 32,768 | 528 | 899 |
| 3 | 1024 | 75,122 | 75,520 | 65 | 65,536 | 1,229 | 3,327 |
| 3 | 4096 | 275,138 | 276,410 | 65 | 262,144 | 5,104 | 55,321 |
| 4 | 512 | 30,805 | 32,025 | 66 | 32,768 | 512 | 1,484 |
| 4 | 1024 | 63,236 | 64,730 | 65 | 65,536 | 1,056 | 3,814 |
| 4 | 4096 | 218,390 | 234,776 | 68 | 262,144 | 4,277 | 56,018 |
| 6 | 512 | 28,367 | 30,964 | 67 | 32,768 | 527 | 2,444 |
| 6 | 1024 | 50,424 | 63,432 | 68 | 65,536 | 1,039 | 5,912 |
| 6 | 4096 | 209,464 | 225,988 | 67 | 262,144 | 4,157 | 52,512 |
| 8 | 512 | 2,248 | 20,316 | 72 | 32,768 | 533 | 3,678 |
| 8 | 1024 | 33,955 | 61,413 | 67 | 65,536 | 1,055 | 7,031 |
| 8 | 4096 | 198,075 | 222,608 | 71 | 262,144 | 4,038 | 61,317 |

**HK3 is supported: the ceiling is 64 × `range_size_ids`, measured.** The
refusal fires at 65-72 stages — 65 is the first count above
`kMaxFanInUpstreams`, and the excess is the poll bracket, not the engine —
and the row counts land on the arithmetic: 32,025 against 32,768 at 512,
64,730 against 65,536 at 1024, 234,776 against 262,144 at 4096. Neither of
HK3's falsifiers fired at k ≥ 4. **§3's arithmetic was right**, and this
is the number it predicted.

**HK4 is refuted in its strong form, and the refutation is the operator's
most useful line here.** HK4 said IS5's suppression *does not* bound a
contended relation. At k = 2 it bounds it completely: the ceiling is **not
reached after two million rows** at any size, and `ids/stage` cannot even
be computed because there is no second stage. The reason is source-read
and then confirmed: `range_alloc.cpp` suppresses on **top-owner
identity** — if the core asking already owns the relation's top range, no
boundary opens — and under `rotate` at k = 2 exactly one peer ever asks,
so every carve after the first is suppressed and the relation settles at
two ranges forever.

At k ≥ 4 suppression is essentially absent: `ids/stage` is 512, 1,056,
4,277 against block sizes of 512, 1,024, 4,096 — **stages equal blocks to
within 4%**. k = 3 is the transition, where two peers alternate imperfectly
and `ids/stage` runs 3-25% above the block size.

**So the ceiling is a function of how many peers contend, not just of the
block size**, and that was in neither §3 nor the order:

| peers that take ranges | ranges | ceiling |
|---|---|---|
| 1 (k = 2 under `rotate`) | 2, forever | **none** |
| ≥ 2 (k ≥ 3) | ≈ one per block | 64 × `range_size_ids` |

### 6b. K-e — the burn, which is the other side of D6

`ids burnt` above is `next_id - 1 - rows placed`: the 40-bit space charged
for and not occupied by a row. It is dominated by the **unissued remainder
of every live lease block**, so its floor is set by how many cores hold one.

At 512 and 1,024 it reads as almost exactly that: 3,678 at k = 8 / 512 is
7.2 blocks against seven peers holding one each, and 7,031 at k = 8 / 1,024
is 6.9. **At 4,096 it is about twice that and nearly independent of k** —
55,321 / 56,018 / 52,512 / 61,317 at k = 3, 4, 6, 8, or roughly thirteen
blocks in every case. Consistent with a refill landing before its
predecessor is spent, so a core holds two blocks rather than one; **not
attributed further, because no instrument here separates them.**

The trade for the operator, in one line: burn is **one to two blocks per
contending core per mount**, so it scales with `range_size_ids` while the
ceiling does too — 3,678 ids burnt buys a 32k-row ceiling, and 61,317 buys
a 262k-row one.

**`burnt/mount` is 0 in every cell**, and that corrects a claim rather than
confirming one. §3 says *"a restart burns every live block"*. The remount
does not move `next_id`: those blocks' remainders were already charged to
the mark **when they were carved**, so they are counted in the column
above and a restart adds nothing to it. What a restart costs is the
*re-carving* after it, which is the same one-to-two blocks per core again.

### 6c. K-g — read cost against range count (RD9(c))

Timed at geometric checkpoints inside the same cells, whole-relation
`SELECT *` from the core-0 session.

**The x-axis is `rows / range_size_ids`, which is the true stage count only
where suppression does not fire** — so at k ≥ 4 it is the stage count (§6's
`ids/stage`), and at **k = 2 it is not**: the relation has two ranges
however many blocks it took, so the k = 2 rows below are read cost against
*size*, with the stage count pinned at 2. Labelled rather than corrected,
because it is the honest pair of readings.

| k | size | series (`rows/block` : read ms / reply KiB) |
|---|---|---|
| 2 | 4096 | 1:2/21 · 4:11/207 · 18:31/907 · 72:142/4034 · 289:743/17253 |
| 4 | 4096 | 1:1/23 · 5:15/244 · 10:12/522 · 21:25/1092 · 43:58/2314 |
| 8 | 1024 | 2:2/21 · 5:65/82 · 16:98/200 · 33:146/422 |
| 8 | 4096 | 1:10/39 · 6:43/349 · 12:59/614 · 24:46/1227 · 49:90/2572 |

**Reply size is the dominant term, not the stage count.** The k = 2 series
— two stages throughout — costs 743 ms for 17 MiB, and the k = 4 series at
43 genuine stages costs 58 ms for 2.3 MiB; per KiB those are 0.043 and
0.025 ms, so *more* stages came out cheaper per byte. Where a stage cost is
visible it is at **small** replies on many cores: k = 8 / 1024 runs
0.095 ms/KiB at 2 stages and 0.346 at 33, a 3.6× per-byte rise across a
16× rise in stages.

So RD9(c)'s answer is that **the fan-in's per-stage cost is real but
second-order against the bytes it carries**, and it is measurable only
where the reply is small enough not to swamp it. That is a weaker
conclusion than the cell was designed for, and it is the honest one: the
run does not separate stage count from reply size, because on this
mechanism they rise together by construction.

### 6a. Why the ceiling is measurable in exactly one configuration

**Source-read**, then confirmed by measurement in both directions.

The fan-in route in `HandleSelect` (`command_dispatcher.cpp`) is guarded
by, among other shape tests, two facts about *who is asking*:

```
remote_reads_ != nullptr                       // this core has a client
&& owner_access->owner_core != core_id_        // and does not own the relation
```

- **`remote_reads_` exists on core 0 alone.** `expeditor.cpp` constructs
  it as `remote_reads_.emplace(/*core_id=*/0, step_seam.send, …)` and calls
  `SetRemoteReads` on that one dispatcher. `CoreRuntime` — every peer core
  — has `remote_steps_`, the **server** half, and no client member at all.
  So every peer can *serve* a stage and no peer can *open* one.
- **The owner test excludes `placement = creating` entirely**, because
  under it every relation's `owner_core` is 0.

A statement that fails the route then reaches `CheckReadAffinity`, whose
`WhollyOwnedBy` arm refuses it — *"has ranges on another core and this
shape cannot fan in over them; reading it here would answer short"*. That
refusal is deliberate and documented: `workplan-range-directory.md` §15d
records it as *"an honest refusal; widening the fan-in gate to cover it
needs a self-directed stage, which is a design question and not this
row's."* What is **not** recorded anywhere is that the design question
gates the whole read surface of a spread relation.

**Measured, 4 cores, `range_size_ids = 512`, 1,200 rows spread over the
boundary, every shape asked from every core's session:**

| shape | core 0, `creating` | core 0, `rotate` | any peer, either |
|---|---|---|---|
| `SELECT * FROM t` | refused | **ok** (10,853 B) | refused |
| `SELECT * FROM t WHERE id = 1` | refused | **ok** | refused |
| `SELECT * FROM t WHERE v = 1` | refused | **ok** | refused |
| `SELECT * FROM t ORDER BY id ASC` | refused | **ok** | refused |
| `SELECT id FROM t` | refused | refused | refused |
| `SELECT COUNT(*) FROM t` | refused | refused | refused |
| `SELECT * FROM t LIMIT 2` | refused | refused | refused |

The four that answer under `rotate` are the ones the route admits:
`chain.star()`, not aggregated, not sorted (`ORDER BY <pk> ASC` on an
ascending-key relation is dropped by the compiler and costs nothing), no
`LIMIT`/`OFFSET`, no sub-chain. The 10,853-byte reply also settles a
neighbouring question in passing: **the 992-byte reply cap does not bound
a plain fan-in read** — it is the cross-owner-transaction read's cap, and
this is not one.

So §6's ceiling below is measured under `placement = rotate`, from a
core-0 session, polling `SELECT *`. That is not a convenience: it is the
only configuration in which a spread relation can be read at all, and it
is *not* the configuration the k sweep or IS7 measured writes in.

## 6d. K-f — D6's value, with a sweep behind it and the ceiling bounding one side

RD9(b) re-run with the large end reachable, which two cores could not do.
k = 4, both arms, both durability classes, two interleaved reps each,
`placement = creating` (the throughput arrangement, as §4). The ceiling
column is §6's measured number at k = 4 where it was measured and the
arithmetic where it was not.

| `range_size_ids` | group S/C | **relaxed S/C** | read ceiling | ids burnt at k=4 |
|---:|---:|---:|---:|---:|
| 256 | 1.333 | **0.434** | ~16,384 † | — |
| 1,024 | 1.401 | **0.809** | 64,730 (measured) | 3,814 |
| 4,096 | **1.470** | 1.025 | 234,776 (measured) | 56,018 |
| 16,384 | 1.296 | 1.001 | ~1,048,576 † | — |
| 65,536 | 1.339 | 1.031 | ~4,194,304 † | — |

† arithmetic; §6 measured 512/1024/4096 and the three landed on it.

**The finding is at the small end, and it is new.** A small block does not
merely lower the ceiling — it **costs throughput outright**: the relaxed
arm runs at **0.434×** at 256 and 0.809× at 1,024, against ~1.0 from 4,096
up. That is the refill rate showing through: a block of 256 is spent in
milliseconds, and every exhaustion is a round trip to core 0 plus a client
retry. §3's trade was stated as *ceiling against burn*; **there is a third
axis, and below 4,096 it dominates both**.

The group arm peaks at 4,096 (1.470) and is flat-ish either side, because
its cost is the commit path rather than the refill.

**So D6's value, handed to the operator with the sweep behind it:**

- **Below 4,096 is a loss on every axis** — lower ceiling, worse
  throughput. 256 and 1,024 are not candidates.
- **4,096 is the throughput optimum on both arms** and buys a 234,776-row
  ceiling for 56,018 burnt ids per mount at k = 4.
- **Above 4,096 costs a little throughput and buys a lot of ceiling**:
  16,384 gives up 12% of the group arm's gain (1.296 against 1.470) for
  4× the ceiling; 65,536 gives up 9% for 16×.
- **The ceiling only exists at all where two or more peers contend** (§6's
  HK4 result). A relation written by one peer has none, at any size.

CLA does not pick the value; the shape above is what the pick is made on,
and the one thing this order can say flatly is that **the sweep's centre
was not obviously wrong and its bottom end was**.

## 7. Versus PostgreSQL

**No twin exists, and RP8 §10's reasoning is why — cited, not
re-derived.** `bench/v2.5.0/results-r6b-cross-owner-cost-v2.4.0-28-g53f6aae.md`
§10 establishes that PostgreSQL has no concept of a relation *owned* by
one of several backend-partitioned execution units the way this engine's
cores are, so a comparison shaped around ownership has no honest mapping
onto a single-process PostgreSQL instance. Every cell in this file is
shaped around exactly that: which core owns which range of one relation,
what a foreign writer costs, and which core may read the result.
PostgreSQL's own partitioning is declared by the user in DDL and served by
one process pool, so the nearest twin would answer a different question.

## 8. The three ways out of §3, re-priced against what was measured

§3 named three, each with an owner, and the order asks for them re-priced
against the measured number. The re-pricing is not about the number:
**none of the three addresses the constraint that actually binds.**

| way out | owner | what it moves | what it does not |
|---|---|---|---|
| raise `range_size_ids` | operator, on this order's numbers | the ceiling, linearly; at 2^20 it is 67 M rows. Costs burn — §6's measured `ids_burnt` | nothing about the two refusals in §6a. A relation with **two** owners is still unreadable from every core under `creating` and from every peer under `rotate` |
| raise `kMaxFanInUpstreams` 64 → 255 | `crosscore.md` §9 | the ceiling, 4× | the same. §3 already said 4× does not change the shape; it now also does not change the binding limit |
| per-core **stripe** of the id space | operator (reverses D6) | ranges stop accumulating: `cores` per relation, forever, so the ceiling disappears | still leaves both refusals. A striped relation has k owners, which is exactly the state a peer cannot read and the owner core cannot fan in over |

**What would move the binding limit is neither a constant nor D6.** It is
the two items §6a names from the source:

- **A self-directed stage**, so a core owning some of a relation's ranges
  can fan in over the rest and walk its own locally.
  `workplan-range-directory.md` §15d already identifies this exact
  mechanism and defers it — *"a design question and not this row's."* It
  is what makes `placement = creating` readable, and `creating` is the
  default and the arrangement spreading produces.
- **A fan-in client on every core**, not core 0 alone. Every core already
  has the server half (`CoreRuntime`'s `remote_steps_`); none but core 0
  has the client. Until then, a session's ability to read a spread
  relation depends on which core the kernel's `SO_REUSEPORT` accept put it
  on, which is not a property a client can choose.

Both are **wider than a config value and narrower than reversing D6**, and
neither has an owner named anywhere. That is this order's largest
hand-off.

## 9. What this run does not measure, and what was not run

- **The aggregate cell (A / IM6) was not run**, and CK4 is why: §5
  establishes that every scenario relation spreading would spread — six of
  twenty-four, the append-only ledgers — becomes unreadable at its second
  range, and all four drivers read them. A run would produce refusals in
  the load phase rather than a `cores = 1` against `cores = N` comparison,
  which is the reason the order gated the cell rather than interpreting
  it. **Not run, reported as not run.** What it would take to run it is
  §8's self-directed stage, not a smaller workload.
- **The range count below the ceiling is derived, not read.** Nothing
  reports a relation's range count: `sys.ranges` has no column definitions
  (`SELECT * FROM ranges` → `ERR no columns for this rel_id`, measured) and
  `SHOW META` carries only decline counters. The fan-in refusal names its
  stage count, so the count is *measured* at the ceiling and *estimated*
  (`ids issued / range_size_ids`) below it — §6 and §6c mark which is
  which, and §6c's k = 2 rows are exactly where the estimate is wrong. A
  `SHOW META` field would close this; adding one is an engine diff and the
  order forbids it, so it is a finding rather than a cell.
- **The ceiling above `range_size_ids = 4096` was not reached.** 16,384
  and 65,536 put it at ~1 M and ~4.2 M rows, minutes of pure ingest per
  cell; §6d marks those two rows as arithmetic. The three that *were*
  measured landed on the arithmetic, which is a reason to trust the other
  two rather than a substitute for measuring them.
- **K-g does not separate stage count from reply size**, because on this
  mechanism they rise together by construction. §6c states what it can and
  stops there.
- **Why `ids burnt` at 4,096 is about twice the live-block bound**, and
  nearly independent of k. Measured (§6b), consistent with a refill landing
  before its predecessor is spent, and left unattributed — no instrument
  here separates one core's blocks from another's.
- **Per-statement overhead A/B.** Suspended for v2-stage development by
  the operator amendment of 2026-08-24. Stated as a fact, not implied as a
  pass.
- **The write path under `rotate`.** §6's ceiling cells run `rotate`
  because it is the only readable arrangement; the k sweep runs `creating`
  because it is the only arrangement that spreads over every peer
  (`known-gaps.md`: core 0 never takes a range of a relation it does not
  own, so a `rotate` relation is (k-1)-way). The two cells therefore
  differ in placement as well as in subject, and no number is carried
  between them.
- **Any comparison with IS7's 1.132×.** Different host, Debug build. The
  order forbids it and this file does not do it.

