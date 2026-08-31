# Ratification DA — D6's value, placement policy, and the fan-in width

Ratified by the operator 2026-08-31 against `main` at `c8e3d31`
(`v2.2.1-153-gc8e3d31`). Recorded by CLA.

Three items from `CLAUDE.md`'s Open Decisions whose measurement was
complete and which needed only a value. None of them required new work
to decide; all three require work to enact.

## DA1 — `range_size_ids` = 65,536

D6's value, settled. Sweep behind it:
`bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md` §6a,
k = 4, `placement = creating`.

| `range_size_ids` | group S/C | relaxed S/C | read ceiling |
|---:|---:|---:|---:|
| 256 | 1.333 | **0.434** | ~16,384 |
| 1,024 | 1.401 | **0.809** | 64,730 (measured) |
| 4,096 | **1.470** | 1.025 | 234,776 (measured) |
| 16,384 | 1.296 | 1.001 | ~1,048,576 |
| **65,536** | 1.339 | **1.031** | ~4,194,304 |

What the value buys and gives up, stated plainly: 65,536 gives up **9%
of the group arm's gain** against 4,096's optimum (1.339 against 1.470)
and takes **16× the ceiling**. The relaxed arm prefers it — 1.031 is the
sweep's high — because that arm's cost is refill rate, and a block this
size is not spent fast enough to show it.

**The operator's stated ground was "the same unit as a lease grant".
CLA records a discrepancy rather than resolving it silently**:
`kRowIdLeasePerGrant` is **4,096** (`row_id_lease_service.hpp:30`), not
65,536, and its comment carries its own justification — K-M2's measured
floor for bump-ahead allocation, below which the durable bump stops
amortizing. So the value ratified and the ground given point at
different numbers. This ratification takes **the value**, on the reading
that 65,536 was chosen deliberately as the sweep's top; if the ground
was the intent, the value is 4,096 and DA1 is wrong. Correct it here
rather than in the code.

**Burn.** A core that stops inserting burns the remainder of its block,
and a restart burns every live block — so the cost is up to 65,535 ids
per (relation, core, mount) against a 40-bit space. The sweep measured
56,018 burnt at k = 4 with 4,096; the same arrangement at 65,536 burns
proportionally more and is still four orders below the space.

**What does not change.** The ceiling exists only where two or more
peers contend (§6's HK4). A relation written by one peer has none at any
size.

## DA2 — initial placement policy = `creating`

`crosscore.md` §9's item, settled as `creating` — a relation's first
range is owned by the core that created it.

The measured input the decision was made on
(`bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md` §6):
rotation's crossover is a **step at the first core to take a second
session**, and past it rotation is **negative at seven writer cores
(0.51×)**. `creating` is also what the sweep above was run under, so
DA1's numbers are numbers for the policy now ratified rather than for
the other one.

**What this does not settle.** `rotate` is not deleted — it stays a
configurable placement, and §6a's gates are unchanged. What is settled
is the default.

## DA3 — `kMaxFanInUpstreams` 64 → 255

Accepted. The wire carries the upstream index in one byte
(`remote_step_service.cpp:149`), so 255 is reachable **without a format
change**; the cost is per-stage state on the session core.

**R4-M's assessment of this option needs restating under DA1, because it
was made against a different range size.** §3's verdict was that raising
the constant was *"not taken because 4× does not change the shape of the
problem"* — reasoning about a 4,096-id range, where 255 stages buy
~1.04 M rows. Under DA1 the arithmetic is different:

| | 64 stages | 255 stages |
|---|---:|---:|
| 4,096 ids | 262,144 | 1,044,480 |
| **65,536 ids** | 4,194,304 | **16,711,680** |

DA1 and DA3 together take the ceiling from 262,144 rows to roughly
**16.7 M**, a factor of 64. Whether that changes the shape of the
problem is a question the two decisions answer jointly and neither
answered alone.

## What none of these close

**The per-core id-space stripe stays open.** It is the standing
alternative to D6 — each core's ranges become one interval per relation,
so range count stays at `cores` forever and the ceiling disappears
entirely. R4-M calls it *"the real answer"*. It reverses D6 and retracts
invariant 11's wording, so it is a larger decision than DA1, and DA1
does not pre-empt it: raising the ceiling by 64× buys time for that
decision rather than replacing it.

**D1 stays open**, and it is still what keeps every btree relation
unsplittable — so DA1's ceiling applies to heap relations, and the
eighteen scenario relations D1 blocks never reach it.

**The btree top-of-tree hop's access mechanism stays open**
(`crosscore.md` §9, narrowed there after CC11), with
`instructions/v2.6.0/v2.6.0-per-range-trees.md` as the candidate that
removes the structure instead.

## The work these three require

Not zero. DA1 and DA2 are defaults with existing readers; DA3 is a
constant with per-stage state behind it.

**DA-a. Set the values and say where the number came from.**
`range_size_ids`'s default moves off `kRangeSizeOff`
(`range_alloc.hpp:89`, whose header states the on-value deliberately had
no constant until an order raised the default — this is that order).
`kMaxFanInUpstreams` moves to 255. Each gets the citation of the
measurement it rests on, as every constant in this tree does.

**DA-b. Establish that 255 stages are affordable in state, not just on
the wire.** The one-byte index is what makes 255 *representable*; what
makes it *usable* is the per-stage state on the session core. Measure
it — a fan-in at 200+ stages was never run, and §6's ceiling tests
stopped at the refusal boundary of 65-72. **If the state cost is
superlinear, report and stop**; a wire that admits 255 and a session
that thrashes at 120 is worse than a clean refusal at 64.

**DA-c. Re-run the read-ceiling cell under both new values.** §6
measured 512/1,024/4,096 against 64 stages and the arithmetic held. The
arithmetic for 65,536 × 255 is ~16.7 M rows and is **not measured**;
DA1's table marks its own top two rows †. Confirm the refusal boundary
moves where predicted, or find where the prediction breaks.

**DA-d. `creating` is already the default the sweep ran under** — verify
that in source rather than assuming it, and record whether anything
still reads `rotate` as its default.

---

## CLA's enactment record (2026-08-31)

Appended by CLA below the operator's text rather than edited into it. The
sections above are the ratification; this is what enacting it found.

### DA1's recorded discrepancy resolves in the value's favour

The order records that the stated ground — *"the same unit as a lease
grant"* — points at `kRowIdLeasePerGrant` = 4,096 rather than at 65,536,
and asks that the discrepancy be corrected here if the ground was the
intent. **It was not a discrepancy, and the value stands with the ground
intact.** `core_runtime.cpp`'s refill reads

```
const std::uint64_t count = ranges_on ? config_.range_size_ids : kRowIdLeasePerGrant;
```

so **where ranges are armed the row-id lease grant *is* `range_size_ids`**
— D6's "one quantity, not two", built. `kRowIdLeasePerGrant` is the value
the grant takes only where ranges are **off**, and its own justification is
K-M2's measured **floor** below which the durable bump stops amortizing.
65,536 clears that floor rather than contradicting it. Range size and
grant size therefore remain the same unit and the same number, which is
exactly the ground the operator gave; nothing needs correcting and DA1 is
enacted at 65,536.

### DA-a — done

`kRangeSizeIdsDefault = 65536` (`server/range_alloc.hpp`) with the sweep
cited at the constant, and `Expeditor::Config::range_size_ids` takes it.
`kMaxFanInUpstreams = 255` (`server/remote_step_service.hpp`) with DA3's
argument and R4-M's superseded one both cited. `kRangeSizeOff` stays the
off-switch.

Two consequences of the constant that the order did not name, recorded
because they change how the code reads and not only what it does:

- **At 255 the STEP_OPEN upstream byte is exhaustive.** Every count the
  wire can spell is at or below the ceiling, so the decoder's
  `upstream_count > kMaxFanInUpstreams` refusal can no longer fire. It is
  kept under `if constexpr (kMaxFanInUpstreams < 255)` — compiled in
  exactly where a lower ceiling would make a count spellable above it —
  rather than left as a check that guards nothing. The producer-side gate
  is unchanged and is the real one: the dispatcher refuses a statement
  needing more stages than the ceiling, and refuses rather than truncates.
- **`CoreRuntime::Config::range_size_ids` deliberately stays
  `kRangeSizeOff`.** It is a transport field the expeditor always writes
  from `Expeditor::Config`, the same idiom `in_doubt_ceiling_ns` already
  uses, so a hand-built `CoreRuntime` — every unit test — states its own
  arrangement instead of inheriting an instance-wide default it is not
  modelling.

### DA-d — verified in source, and the prose it found was stale

`creating` is the default in all three places that carry one, and
**nothing reads `rotate` as a default**: `PlacementPolicy`'s first
enumerator (`catalog/core_placement.hpp`), `Catalog::placement_`
(`catalog/catalog.hpp`), and `Expeditor::Config::placement`
(`server/expeditor.hpp`). Every `kRotate` in the tree is a test setting it
explicitly.

What the check did find is that **both comments justifying the default
were arguing from an unfinished pipeline rather than from a measurement**
— `core_placement.hpp` said `kRotate` *"becomes the real policy when P4d
wires multi-step pipelines and converts the executor"*, and
`expeditor.hpp` said a rotated relation's statements are refused until
cross-core dispatch exists. Both premises are spent: P4d landed,
statement shipping carries an autocommit single-relation statement to its
owner, and R4-R/RS gave a spread relation a read surface from every core.
Rotation is no longer one shape wide, so the reason it is not the default
is DA2's measurement and nothing else. Both comments now say that.

### DA-b and DA-c — what this host can and cannot answer

Reported rather than attempted quietly. **A relation on this machine
cannot exceed two ranges**, so neither a 255-stage fan-in nor the
16.7 M-row refusal boundary is reachable here:

- The host has **2 CPUs**, and `cores` is refused above
  `hardware_concurrency()` (`expeditor.cpp`) — reactors are pinned and
  never block.
- Only cores **1..n-1** get a `CoreRuntime`, so only they hold a row-id
  lease and only they ask core 0 to open a range. At `cores = 2` that is
  one peer.
- `OpenRangeOnSystemCore`'s IS5 suppression declines whenever the asking
  core already owns the top range. One peer therefore opens exactly one
  boundary and every later carve is suppressed — which is not inference:
  R4-M measured it as HK4's refutation, *"at k = 2 the ceiling is not
  reached after two million rows at any size"*.

This is the same bound IS7 already stated for the k sweep. It was then
**confirmed empirically rather than left as a derivation**: 100,000
inserts through the one peer at `range_size_ids = 256` - 390 lease
blocks' worth - left `SHOW META`'s `split_relation_detail` reading
`4000:2@2` on both of two independent runs.

**DA-b is answered, in isolation from that limit.** A pipeline tag is
minted once per `Open()` regardless of how many ranges the stages
address, so `bench/session_step_state_bench.cpp` builds the same
`SessionStepClient::reads_` vector a 255-stage fan-in would and prices
the two candidates directly. Both **are** superlinear - the scaling
ratios land on the quadratic prediction (4.11 against 4.00 at N 64 ->
128; 3.53 against 4.00 at 32 -> 64) - and both are small: **10.9 us per
park-predicate poll and 268 us for one teardown at N = 255**, two to
three orders below this engine's own ~20 us wire hop and ~0.94 ms
`fdatasync`. The order's instruction was *"if the state cost is
superlinear, report and stop"*; it is, and what stops here is the
inference from superlinear to unaffordable. Session-side bookkeeping is
not what would make 255 stages unusable.

**What DA-b still does not cover** is the order's own third mechanism in
disguise: credit (`kInitialCreditsPerEdge` = 4) against ring capacity
(`kCoreRingSlots` = 256), which could self-throttle a fan-in near 64
stages per peer for reasons unrelated to state size. That needs real
batches over a real ring between distinct owner cores, so it needs the
two-range limit not to hold. **Not measured, not measurable here.**

**DA-c is not run**, for the reason above, and nothing estimates it.

**What was runnable was run**: `cores = 1` is unmoved by the armed
default across 200/1K/10K rows, with `ids_burnt = 0` on both arms - so
spreading structurally never activated, not merely measured equal; the
read surface at `cores = 2` under `range_size_ids = 65,536` is 11 of 16
shapes, identical to `3446666` shape for shape; `scripts/sim.sh` is
190/190 and the Release suite 3018/3018. All of it, with its provenance
and its noise floor, is in
`bench/v2.7.0/results-ratification-da-v2.2.1-155-g1f04418.md`. Nothing
unrun is reported as a pass.

### One consequence of arming the default that DA1 did not name

Found by the enactment's review, recorded in `docs/inflight/known-gaps.md`
rather than only here. Arming `range_size_ids` turns ten paths into
default refusals on a multi-core instance - every one of them already
existed, and what changed is that a range now opens on **workload**
instead of on configuration. **The one that does not recover** is
`RefuseAuxiliaryOnSplitRelation`: on a relation with two or more ranges,
`CREATE INDEX`, `CREATE CABIN` (**including the Cabin optimizer's
automatic path**), `CREATE ASSERTION` and an FK naming it are all
`Unsupported`, and nothing merges ranges until R5. So **order decides and
the decision is permanent** - index-then-write keeps the relation
unsplittable through `RangeEligible`'s gate, write-then-index is refused
for the life of the relation, and under DA1 the second order is what an
ordinary session produces. `cores = 1` meets none of it.

### Amended 2026-08-31: that consequence is decided, and it is AX

The paragraph above was written the day DA1 was enacted and is kept as
written, because it is what the enactment's review found. The operator
then ruled the non-recovering refusal a **defect rather than a
constraint** and ratified the fix the same day:
`instructions/v2.7.0/ratification-AX.md` (AX-D1 through AX-D6, AX-D12),
built to `instructions/v2.7.0/wirkorder-AX.md` and
specified as `docs/spec/crosscore.md` §6c.

So the two documents cite each other rather than disagreeing: DA1 armed
spreading and the arming had this cost; AX removes it, by coalescing the
relation back to one range synchronously when an **explicit** auxiliary
DDL names it. What survives of the paragraph above is that the Cabin
optimizer's automatic path still meets the refusal (AX-D12), and that a
coalesced relation forgoes spreading's gain for as long as its auxiliary
lives (AX-D6) - so **DA1's 1.51× and an auxiliary are mutually exclusive
on one relation until R5**, which is a real limit on what DA1 buys and is
stated here rather than left in AX's file alone.
