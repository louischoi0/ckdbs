<!-- One proposal, one directory. The register, the seven fields every
     entry carries and the standing constraints are in ../README.md. -->

# OPT-003 — UPDATE and DELETE walk the relation with `kWrite` and dirty every page they read

**Hypothesis.** `command_dispatcher.cpp:8469` and `:9250` pass
`PageAccess::kWrite` to `VisitRelation`, which routes to
`heap_chain.cpp:296`'s `store.Get(page_id)` and marks **every frame it
touches dirty** (`device_page_store.cpp:494`). Neither body writes
through that view: `apply` re-fetches at `:8355` before
`OverwriteTuple`, `mark` at `:9187` before the delete-mark, and the
walk's own `page` is read-only. So a point UPDATE on a 2,000-row heap
relation dirties ~28 pages to change one, and the next checkpoint writes
all 28 back. Moving the walk to a read access should remove ~27/28 of
the write-back I/O for point UPDATE/DELETE on heap relations. No CPU
change is predicted — this is an **I/O** claim.

**Measurement.** `SHOW META`'s page-store write-back counters across a
fixed UPDATE workload (the counter is the instrument, and it should move
by an order of magnitude), with `tools/benchmark.py --update-ops
--sync` for the latency view and `tools/multicore_benchmark.py` for the
checkpoint-load view.

**Reason.** Two lines. It also lets the read-path machinery
(`eviction.md` §5's scan ring) apply to a walk it currently cannot see.

**Pros / cons.** Pro: smallest change on the list against a real I/O
amplification. Con: on a *leased* store, `Get` also runs `MayWrite`
(`device_page_store.cpp:479-489`), so the refusal a peer-writer lease
raises moves from the walk to the mutating re-fetch — later, but still
before anything is dirtied, so a `TxnConflict` is still raised
pre-write. That reordering is the cost, and the peer-writer refusal
tests are what price it.

**Consistency and sanity.** No hard invariant. It touches the
peer-writer lease contract (`workplan-peer-writer.md`) and eviction's
dirty-frame accounting. Proof: the peer-writer refusal tests must see
the same code at the same point, plus the full suite.

**Implementation.** Branch `opt-003-walk-read-access` — filled in with
its remote link, commits and suite result when the work lands.
