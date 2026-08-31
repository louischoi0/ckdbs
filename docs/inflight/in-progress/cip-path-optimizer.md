# CIP — Change Improvement Proposals (path-optimizer)

Open workplan. Each entry is a **numbered change improvement proposal**
(`OPT-nnn`) against a measured hot path, cut and **built** on the
`path-optimizer` worktree. The role that owns this file does not stop at
the proposal: it implements the proposal on its own branch, measures it,
pushes that branch to `origin`, and **links the branch back into the
entry** — so a proposal here is readable as code, not only as prose.

Upstream of every entry: `CLAUDE.md`'s hard invariants and the owning
spec. A proposal that needs an Open Decision is not cut; it is reported
as blocked and named.

## What an entry must carry

Seven fields, in this order. An entry missing one is not ready to build.

1. **Number and title** — `OPT-nnn`, one line saying what changes.
2. **Hypothesis** — what specifically costs, and the *predicted
   direction and magnitude* of the win. A prediction with no number
   cannot be wrong, so it is not a hypothesis.
3. **Measurement** — the existing driver, probe or counter that decides
   it, named by path, plus the A/B shape. `build-release`, interleaved
   A/B, per `CLAUDE.md`'s measurement rule; a Debug number is not a
   result. Every number carries `git describe --tags`.
4. **Reason** — why it is worth doing now, in terms of a statement shape
   the engine actually serves.
5. **Pros / cons** — both, where both exist. An entry with no cons is
   under-examined; where the cost is genuinely nil, say so plainly.
6. **Consistency and sanity** — which numbered hard invariant and which
   spec rule the change touches, what could break, and what proves it did
   not (the suite, a contract test, a `sim/` cell).
7. **Implementation** — the branch, its remote link, the commit ids, and
   what the suite said. Written back into the entry as the work lands,
   never left as an intention.

## Branch and push convention

- One branch per proposal, cut from the worktree branch:
  `opt-<nnn>-<slug>` (e.g. `opt-001-tuple-decode`).
- Pushed to `origin` as a **working branch**. `main` is never pushed from
  this role.
- The entry carries the link:
  `https://github.com/louischoi0/ckdbs/tree/<branch>`, plus the commit id
  every claim in the entry was true of.
- A proposal that is measured and *rejected* keeps its branch and its
  entry — a refuted hypothesis is a result, and deleting it invites the
  next run to re-propose it.

## Standing constraints on every entry

- **No on-disk format change**, and nothing that would need a
  `kFingerprintVersion` bump.
- **Nothing in `CLAUDE.md`'s Open Decisions** — those wait on the
  operator, and an entry here must not silently pick one.
- **No new subsystem.** A local, provable change beats a redesign.
- **A refusal stays a refusal**: an optimization may not widen what the
  engine admits. Same answers, same errors, fewer cycles.
- Advisory structures (Waystone) stay advisory: invariants 8 and 9 hold
  whatever the speedup.

## Status

| # | Title | State | Branch | Commit |
|---|---|---|---|---|
| OPT-001 | UPDATE/DELETE decode every scanned row before testing the WHERE | proposed | — | — |
| OPT-002 | Every decoded string costs a malloc+free the codec's own header says it should not | proposed | — | — |
| OPT-003 | UPDATE/DELETE walk the relation with `kWrite` and dirty every page they read | proposed | — | — |
| OPT-004 | `DecodeRowInto` still pays the Status-constructing preconditions AP02 removed | proposed | — | — |
| OPT-005 | `BtreeLookup` drops the leaf pin and every caller re-fetches the same page | proposed | — | — |
| OPT-006 | A correlated sub-chain rebuilds its whole runner per outer row | proposed | — | — |

Survey that produced OPT-001..OPT-006: `worktree-path-optimizer` at
`1beda80` (`v2.7.0-27-g1beda80`). Ordering is by (expected win) /
(risk x size), which is why the two smallest entries are built first: a
change that cannot regress is worth landing before a change that could.

---

## OPT-001 — UPDATE and DELETE decode every scanned row before testing the WHERE

**Hypothesis.** `apply()` in `src/server/command_dispatcher.cpp:8146`
decodes the whole row *twice* (`:8168` `DecodeRow` into a fresh
`std::vector<AstValue>`, `:8175` `DecodeRowInto` the frame slots),
resolves spills twice (`:8191`, `:8192`), and only then evaluates the
predicate (`:8196`). DELETE has the same shape with one decode
(`:9109` / `:9121` / `:9125`). Because `LocateByPk` returns `kScan` for
every heap relation (`:6765`), `UPDATE t SET c=x WHERE id=n` runs that
body on **every slot of the chain**: at `tools/benchmark.py`'s defaults
(heap, 2,000 rows) that is 2,000 full-row decodes of which 1,999 are
discarded. Reordering to *masked decode of the WHERE columns → predicate
→ full decode only on a match* should cut per-rejected-row work roughly
5-10x on a five-column row, and the **UPDATE phase's server CPU 2-4x**,
more on wide relations. This is the same defect AP01 measured at 75% of
the scan on the read path (`bench/results-scenario1-vs-pg.md`), left
standing on the write path.

**Measurement.** `tools/benchmark.py --clustered heap --update-ops 1000
--json`, plus server CPU from `/proc/<pid>/stat` fields 13-14,
interleaved A/B per `workplan-aggregate-perf.md`'s "How to measure here";
`tools/catalog_read_ab_benchmark.py` is the existing interleaved harness
to copy. There is no `ANALYZE` for UPDATE, so **server CPU is the
instrument**, not latency alone.

**Reason.** A point UPDATE by pk on a heap relation is the shape the
engine exists for, and it currently pays a full decode per row of the
relation. The masked decode this needs already exists — AP01 built it
for SELECT; `step_compiler.cpp:1303` simply hands UPDATE
`Step::kAllColumns`, whose comment ("they need every column of a
matching row anyway") is true of a matching row and false of a rejected
one.

**Pros / cons.** Pro: the largest measured win on the list, with the
machinery already built and proven on the read path. Con: it is the
entry with the most ordering to preserve — MVCC classify → masked decode
→ predicate → conflict check → full decode → FK → write — and a wrong
mask yields a *stale slot value and a wrong row*, not a crash. That is
the cost: a real bug class in exchange for the biggest win.

**Consistency and sanity.** No numbered hard invariant. It touches R1 in
`row_codec.hpp` (spills resolved with no page span live) and the
pre-SET image the `previous` copy must carry (`:8232-8236`).
`CompileWhere` must emit a real mask via `FilterColumnsOf`
(`step_compiler.cpp:1318`) rather than `kAllColumns`. Proof: the full
suite, the MVCC and FK contract tests, and a `sim/` cell whose oracle
would see any wrong-row write.

**Implementation.** Branch `opt-001-update-decode-order` — filled in
with its remote link, commits and suite result when the work lands.

---

## OPT-002 — Every decoded string costs a malloc+free the codec's own header says it should not

**Hypothesis.** `src/exec/row_codec.cpp:532` builds a fresh
`std::string`, fills it byte-at-a-time, and *move-assigns* it into the
slot; the `char(N)` arm (`:487-497`) and `ResolveSpills` (`:1062-1067`)
do the same. The move frees the slot's existing buffer and adopts a new
one, which **defeats the buffer reuse the file's own header claims at
`:430-433`**. For any value past libstdc++'s 15-byte SSO that is one
malloc + one free per string column per decoded row —
`tools/benchmark.py`'s `TEXT_LEN = 16` sits deliberately just past it.
Replacing both with `assign(ptr, n)` (one `memcpy`, capacity reused)
should remove ~40-80 ns per row per string column: **5-15% on scan
shapes that project text**, against the 0.05-0.12 us/row fitted in
`bench/results-scenario1-vs-pg.md` §6, and it compounds with OPT-001,
which decodes strings twice per rejected row.

**Measurement.** `tools/benchmark.py --json`, `full-scan` and `update`
phases, interleaved A/B in `build-release`. `ANALYZE SELECT * FROM t`
must show `examined=` and every other counter **unchanged** — this
change may not move a single counter, which is what makes an A/B the
only instrument.

**Reason.** It is fifteen lines against a cost paid per row per string
column on every shape that projects text, and the file already documents
the behaviour it fails to deliver.

**Pros / cons.** Pro: no design question, no counter change, cannot
regress a result. Con: none worth the name — the only thing at stake is
the `char(N)` arm's read-back-to-first-NUL contract
(`row_codec.cpp:182-197`), which the rewrite must keep with an explicit
`memchr` rather than inherit by accident.

**Consistency and sanity.** No hard invariant. Invariant 13's
fixed-length cell rule is untouched: this changes how bytes reach an
`AstValue`, never how many bytes a cell has. Proof: the types contract
suite (which compares configurations byte-for-byte) and the full suite.

**Implementation.** Branch `opt-002-string-slot-assign` — filled in with
its remote link, commits and suite result when the work lands.

---

## OPT-003 — UPDATE and DELETE walk the relation with `kWrite` and dirty every page they read

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

---

## OPT-004 — `DecodeRowInto` still pays the Status-constructing preconditions AP02 removed

**Hypothesis.** `row_codec.cpp:979-980` calls `CheckKeystoneColumn` (an
out-of-line, cross-TU call returning a `Status` that carries a
`std::string` by value, `src/catalog/schema.cpp:79`) and
`CheckLayoutMatches` **per decoded row**. The sibling
`DecodeColumnsInto` was already fixed by AP02 via `CheckDecodeInputs`
(`:868-900`), whose comment states the rule — *the predicate first, the
Status only on failure* — and `DecodeRowInto` never got it. Hot callers:
UPDATE `:8175`, DELETE `:9109`, and every step compiled `kAllColumns`
including every step of a chain carrying a sub-chain
(`step_compiler.cpp:1565-1567`). AP02 measured 19-35% on this class;
here expect **low single digits**, but on ~5 lines that cannot regress.

**Measurement.** The same interleaved server-CPU A/B in
`build-release`; `tools/aggregate_benchmark.py` for a
sub-chain-carrying shape.

**Reason.** It is the identical fix already ratified and measured
elsewhere in the same file, left on one function.

**Pros / cons.** Pro: identical checks, identical messages, no
behavioural surface. Con: none — the win is small, and the entry says so
rather than inflating it.

**Consistency and sanity.** No hard invariant. `CheckDecodeInputs`
already performs exactly these checks, so the refusal text a client sees
must be byte-identical; the decode-error tests are the proof.

**Implementation.** Branch `opt-004-decoderowinto-preconditions` —
filled in with its remote link, commits and suite result when the work
lands.

---

## OPT-005 — `BtreeLookup` drops the leaf pin and every caller re-fetches the same page

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

---

## OPT-006 — A correlated sub-chain rebuilds its whole runner per outer row

**Hypothesis.** `EvaluateSubChain` (`src/exec/step_vm.cpp:313`) runs
once per accepted outer row (`:2312`) and per call constructs a
`RowSink` `std::function` capturing 7+ locals (`:344`, past the inline
buffer, so one malloc/free), a whole fresh `ChainRunner` (`:420`) with
its `bound_`, `schemas_`, `frame_`, `spills_`, `seen_pks_` and
scratch buffers, a `Bind()` + `frame_.Open()` (`:288`-`:289`, three more
allocations plus N `AstValue` constructions), and a second capture-heavy
`std::function` at `:1783`. That is ~6 malloc/free pairs per outer row
paid **before a single inner row is read**. Invisible when the sub-chain
walks 10,000 inner rows; **dominant once JB4/JB6's build turns the walk
into a bucket probe**, which is exactly the shape
`docs/spec/join-inner-build.md` optimises — and `InnerBuildStore` was
already lifted to statement scope for this reason (`:236-248`), while
the runner around it was not.

**Measurement.** `tools/benchmark.py --join-ops` / `--join-scan-ops`
(`join-probe`, `join-semi`) and `tools/join_benchmark.py` /
`tools/join_ksweep.py` at a large `--join-rows`. `ANALYZE` must show
identical `sub_chain_runs`, `build_probes` and `examined=` — the win is
invisible to every counter, so the interleaved A/B is the only
instrument.

**Reason.** The build made the inner walk cheap; what is left around it
is now the cost.

**Pros / cons.** Pro: the win grows exactly where the engine's own join
work has been heading. Con — stated plainly because it is the reason
this entry is last: a cached runner with an explicit reset is **less
simple** than a fresh one, and `record_through_stops_`, `stopped_`,
`recording_`, `building_` and `depth_` must each be reset per outer row
or a stale flag silently changes what a walk records (the C1 break at
`WalkAndRecord:794-829`). This is the highest-risk entry on the list,
and it trades simplicity for speed.

**Consistency and sanity.** No numbered hard invariant, but it sits
against invariants 8 and 9: what a walk *records* into a Waystone trail
must not change, and a stale `recording_` flag is precisely how it
would. The parent frame `&outer` is one object for the whole outer step,
so caching itself is sound. Proof: the waystone contract suite
byte-for-byte, the join tests, and `ANALYZE`'s counters unchanged.

**Implementation.** Branch `opt-006-subchain-runner-reuse` — filled in
with its remote link, commits and suite result when the work lands.

---

## Not cut, and why

- **Heap/btree INSERT's duplicate scan is O(slots per page)**
  (`heap_chain.cpp:109-122`, `btree.cpp:632-645`): removable in
  principle, since invariant 11's 2026-08-25 amendment says an omitted
  key or a named key at or above the high-water mark needs **no page
  read** — but it means threading a "uniqueness proven" flag from the id
  issuer, and a wrong flag admits a duplicate pk silently. A
  backwards-scan short-circuit would lean on within-page id ordering,
  which **invariant 4 explicitly disclaims**. Needs the owner's
  decision, not a patch.
- **A last-frame memo in `GetForRead`**: real, but per *page* rather than
  per row, and OPT-005 removes a whole fetch on the same shapes for less
  risk.
- **`stats::MakeValueKey` copies a `std::string` per bucketed row and
  per probe** (`cabin_store.hpp:132-139`): only for string join keys,
  and a `string_view`-keyed form is a wider `CabinKey` refactor.
- **AP05** is already owned by `workplan-aggregate-perf.md`; it is not
  re-reported here as a discovery.
- **A compiled-plan cache** is excluded as a new subsystem, and it
  collides with the catalog-invalidation soundness gap in
  `catalog_version()`.
