<!-- One proposal, one directory. The register, the seven fields every
     entry carries and the standing constraints are in ../README.md. -->

# OPT-005 — `BtreeLookup` drops the leaf pin and every caller re-fetches the same page

**Hypothesis.** `src/storage/btree/btree.cpp:758` builds a `Descent`
that holds the leaf `PageRef` (`:46-57`), returns only
`Location{page_id, slot}`, and lets the pin die at return. Four callers
then re-fetch that exact page: `step_vm.cpp:590` (point step),
`:1334` (index-range phase 2, **once per resolved row**), `:1452` and
`:1472` (Cabin heal, two per healed entry), and
`command_dispatcher.cpp:8452` (point UPDATE). Each redundant
`GetForRead` costs a `std::map` region lookup plus three `frames_`
hash lookups plus three virtual dispatches
(`device_page_store.cpp:366`, `:491`, `:1352`, `:1371`). Carrying the
leaf ref out removes one of ~4 page fetches per point lookup and per
index-resolved row: **3-8% on `join-probe` and index-range shapes**,
nothing on scans. Note `LocateByPk`'s comment at `:6743` already claims
this is done — the comment is stale, and this entry makes it true.

**Measurement.** `ANALYZE SELECT ...` — `pages=` must drop by exactly
one per resolved row (`plan_printer.cpp:427`), which makes this the one
entry on the list that is **provable from a counter** rather than
inferred from a timer; latency via `tools/index_benchmark.py` and
`tools/benchmark.py --join-ops`.

**Reason.** A pin the descent already holds is thrown away and
immediately re-acquired, four times over, on the engine's most repeated
lookup.

**Pros / cons.** Pro: the win is counter-visible, so an A/B cannot be
argued with. Con: it holds one more live pin per active point step,
which pushes on `eviction.md` §3's `kPinCeiling`
(`device_page_store.cpp:1361`) — that ceiling's audited bound (MG04) has
to be re-checked, and a change that raises pin pressure to save a hash
lookup is only worth it if the ceiling has room.

**Consistency and sanity.** Touches R1 in `step_vm.hpp` (decode before
descending) and eviction's pin accounting. A pin held across
`AcceptTupleAt` is safe — pins are what make it safe — but
`PageSpanGuard` must still see zero live spans when the descent
fetches. Proof: the index and eviction contract suites, plus an
8-frame-budget run of the full suite, which is how eviction was armed in
the first place.

**Implementation.** Branch `opt-005-btree-leaf-ref` at `c578e29`,
carried on to `1495016` (`v2.7.0-47-g1495016`) by the review.
`BtreeLookupHeld` moves the descent's leaf `PageRef` out; `BtreeLookup`
remains, implemented over it, for callers wanting an address rather than
bytes. Converted: the point step, the index-range resolve (once per
resolved row), and `fk_check.cpp`'s parent descent (once per FK-checked
row) — **the third instance, which this entry's own survey missed** and
the review found. Left unconverted, each verified to record a location
without reading the tuple: the Cabin heal, `cabin_optimizer_exec.cpp`,
and `LocateByPk`, whose caller is a write path that must re-fetch a
writable frame anyway. Release suite **3091/3091**, sim **228/0**.

**Two claims in this entry were false, and both are retracted.**

- *"`ANALYZE`'s `pages=` must drop by exactly one per resolved row,
  which makes the change provable from a counter."* It does not:
  `pages_fetched` is incremented explicitly beside the descent and the
  removed `GetForRead` was **never counted**. The A/B confirmed it
  empirically — `pages=` and `index_resolved=` byte-identical across all
  32 arm/shape/size comparisons.
- *"one more live pin, pressing on `eviction.md` §3's ceiling."* Also
  wrong: both converted sites already held a `PageRef` across the
  identical read, so peak pins is unchanged at 1 and MG03's audited
  claim stands. The invented cost would have argued against the very
  conversion this entry wanted.

**Measured — and unresolvable at this scale.**
`results-opt005-btree-leaf-ref-v2.7.0-45-gc578e29.md`, beside this file.
Two pairs (`31bc482` vs `c578e29`; `c578e29` vs `1495016` isolating the
fk_check site), six shapes, three sizes. **Every delta sits inside a
~0-3% floor, and none reproduces in the predicted direction across
sizes.** The reason is arithmetic rather than mysterious: the saving is
a few hundred nanoseconds against a statement costing 80-800 us. The
change is real by construction — a fetch that no longer happens — and
invisible to this harness. That is the honest verdict, and it is not a
win.

Correctness: 96 full-table hash comparisons, zero mismatches, zero
statement errors. One cell showed a uniform 20-30% slowdown across
*every* shape including untouched controls; the tester attributed it to
host interference, discarded it, and re-measured clean rather than
reporting it.
