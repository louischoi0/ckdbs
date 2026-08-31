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
