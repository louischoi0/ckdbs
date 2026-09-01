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

**Implementation.** Branch `opt-003-walk-read-access` at `31bc482`
(`v2.7.0-41-g31bc482`), cut from `40c5e86` and pushed to
`https://github.com/louischoi0/ckdbs/tree/opt-003-walk-read-access`. Two
`PageAccess::kWrite` → `kRead` substitutions and the comments saying
why. No ring fetcher is in play — `VisitRelation` passes none, so `kRead`
takes the plain `GetForRead` branch and the walk's page lifetime is what
it always was.

**Sanity.** Release suite **3091/3091 green**; `scripts/sim.sh` **228
runs, 0 failures** — the corpus matters here specifically because this
change alters which pages are marked dirty, which is what a crash leaves
behind.

**Measured** — `results-opt003-walk-read-access-v2.7.0-41-g31bc482.md`,
beside this file, driver and raw traces under `archive/`. Arm A
`40c5e86` against arm B `31bc482`, both from clean `git archive` exports,
`durability = group` and `checkpoint_interval_ms = 0` so every writeback
is attributable to the driver's own `SYNC` rather than to a background
timer racing the measurement.

| Rows | Data-file bytes written, A ÷ B |
|---|---|
| 200 | 2.0-2.5x |
| 1,000 | 7.5x |
| 10,000 | **67-68x** |

**Arm B's write cost is flat at about one heap page whatever N is**,
while arm A's grows linearly with the relation — which is this entry's
own arithmetic reproduced rather than merely a favourable number, and it
is deterministic (`stdev = 0` almost everywhere). The SELECT control
wrote byte-identical counts on both arms across all 150 reps.

**The instrument is the other half of the result.** `SHOW META` was read
end to end and **carries no page-store write-back counter at all** — the
WAL sync counters are there, `DevicePageStore`'s dirty/writeback
bookkeeping is not; this entry's own "Measurement" section above named a
counter that does not exist, and that is now a known gap rather than a
plan. `/proc/<pid>/io`'s `write_bytes` was tried second and **rejected on
evidence**: the WAL's per-record `pwrite`+`fdatasync` on a separate fd
swamps the page-store signal by two orders of magnitude. What worked was
`strace` summing `pwrite64` to the data file's own fd.

**Latency did what a non-CPU claim should.** Raw per-statement latency
shows **nothing** for select or update at any size, inside the floor —
correct, since nothing here changes the work a statement does. The cost
surfaces where the writes are paid: checkpoint/`SYNC` latency, 2.6-2.8x
at 10,000 rows, clearing its floor by a wide margin. Reported plainly
rather than buried: DELETE's raw-latency mean sits slightly outside its
floor at the two larger sizes with no OPT-003 mechanism behind it, and
is called a thin-sample artifact rather than a win.

**The deferred lease refusal was tested, not argued.** This entry's own
"cons" said the peer-writer refusal moves from the walk to the mutating
re-fetch. A scratch-only test (never committed) confirms the two halves:
`GetForRead` admits a fault-granted, write-*un*granted page, while `Get`
still refuses it with a retryable `TxnConflict` **before anything is
dirtied**, and admits once the write grant lands.

**Correctness:** 18 hash comparisons across six independent sessions
(plain and strace x three sizes), all matched, zero error replies.
