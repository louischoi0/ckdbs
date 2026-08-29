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

*(K-a and K-b. Filled from the run below.)*

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

*(K-c, K-d, K-e. Filled from the run below.)*

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
  (`ids issued / range_size_ids`) below it — §6 marks which is which. A
  `SHOW META` field would close this; adding one is an engine diff and the
  order forbids it, so it is a finding rather than a cell.
- **`range_size_ids` above 16,384 was not swept to the ceiling.** At
  65,536 the ceiling is ~4.2 M ids and the run is minutes of pure ingest
  per cell; the sweep's large end is bounded by wall clock, not by
  principle, and §6 says which sizes were reached.
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

