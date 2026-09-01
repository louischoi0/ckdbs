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

**Implementation.** Branch `opt-005-btree-leaf-ref` — filled in with its
remote link, commits and suite result when the work lands.
