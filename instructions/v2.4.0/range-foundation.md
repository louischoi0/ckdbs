# Work order R3-A — the range foundation that needs no decision

Drafted 2026-08-27 against `main` at `b0b6e8a` (`v2.2.1-56-gb0b6e8a`).

Companion to `instructions/v2.4.0/2pc.md`, which owns R6. This order owns
the front half of R3 and deliberately stops where R6 needs the tree.

## 0. Why this milestone exists, and why it is first

R3 (`docs/inflight/in-progress/workplan-range-directory.md`) and R6
(`instructions/v2.4.0/2pc.md`) are both open, both under `v2.4.0`, and both
route through one function. `CommandDispatcher::CheckWriteAffinity`
(`src/server/command_dispatcher.cpp:3599`) refuses a write whose
`access.owner_core` is not this core. R6-8 replaces that refusal with an
enrolment. RD4 reuses the same function's live-id `cabin_ids` test
(`:3631-3633`) and RD6 eventually makes `owner_core` stop being the answer
at all. Two branches rewriting one decision point in two directions is the
collision, and everything below is arranged so it does not happen.

**The sequencing this order assumes**, stated so a disagreement lands on
the reason:

1. **R3-A** — this order. Everything in R3 that no unratified decision
   blocks: RD0's doc half, RD1, RD4.
2. **A decision session** — R6's D1–D7 and its two `[OPEN]`s; R3's D1, D2,
   D4, D6. Not a build session.
3. **R6-3 … R6-9**, at relation granularity.
4. **RD2 … RD9**, against a `CheckWriteAffinity` R6-8 has already settled.

R6 before R3's back half, and not the reverse, because R6 is already
granularity-agnostic where it counts: R6-2 keys a participant's enrolment
on `(src_core, session_id)`, never on a relation
(`include/kds/server/shipped_statement_executor.hpp`, built at `93b0ba4`).
Under ranges a participant is still a core. The one row that changes is
R6-8, which would call RD3's resolver instead of reading a field. Building
R6 on ranges first would instead re-open D1's participant discovery on a
substrate that does not exist yet.

**The format epoch is why R3-A comes before the decision session rather
than after it.** RD1 moves `kSuperBlockVersion` 15 → 16
(`include/kds/server/superblock.hpp:176`) and every existing data file
stops mounting. R6-4 (recovery) and R6-6 (prepared state across restart)
build recovery fixtures. A bump landed after them re-baselines all of it; a
bump landed now costs nothing, because RD1's product is an **empty**
relation that no path reads.

## 1. Scope

**In.**

- RD0's remaining half: the three sentences `workplan-range-directory.md`
  §0 says the operator's direction reverses, amended in place.
- RD1: `sys.ranges` exists as a bootstrap catalog relation and is empty.
- RD4: `RangeEligible(access)` — the §6a gates, built and tested before
  anything can allocate a second range.
- The baseline capture §7 M3 defines.

**Out, stated so nothing assumes otherwise.**

- **RD2** (the directory row's fields) — `[D2]`, and D2 is
  `crosscore.md` CC9's, not this order's.
- **RD5, RD6** (allocation, per-range chains). RD6 carries the review's
  blocking finding about `desc_page_id` as every insert path's head; it is
  the largest row in R3 and it needs D6's size answered first.
- **RD7** (the pipeline over ranges) — `[D4]`, and it collides with R6-3's
  fan-out over N participants. One of the two designs the ring's fan-in
  shape, and it should be the one that lands first, not both at once.
- **Every R6 row.** This order must not touch
  `shipped_statement_executor.*`, `statement_ship_service.*`, or
  `CheckWriteAffinity`'s cross-core arm.
- **Range policy of any kind** — split triggers, merge, migration. CC10
  gives those to the physical optimizer's Part III, unwritten.
- **User-facing range DDL.** §2 below is where that is retracted.

## 2. What has already happened — RD0(a) is not owed

`workplan-range-directory.md` §7 lists RD0 as *"Probe and record, before
building"*, with a reachability probe on `kStepBatchTargetBytes` (32 KiB,
`include/kds/server/step_pipeline.hpp:210`) against the production ring
slot (1,024 bytes, `include/kds/sched/ring_transport.hpp`). **That probe
ran, reproduced, and was fixed on `main` before this order was drafted.**

- `7148343` — reproduces at 42 rows, fixes it in four parts: the transport
  answers `max_payload()`, the simulated transport takes the production cap
  (which is why no simulation could have found it), the seal becomes
  predictive, and the two step senders refuse an oversize payload instead
  of returning an OK they had not earned. Two further defects fell out of
  building it, both recorded in that commit's message.
- `beec260`, `f448e1f`, `44bdd2f` — the exact seal, the single step-send
  seam, and the review's findings applied.
- `dcdc5e5` — the bug report retired, per `docs/inflight/bugs/README.md`'s
  rule that a report is deleted when the fix lands with its test.

So RD0's framing question — bug report or `known-gaps.md` entry — was
answered as a bug report, and R3-A inherits the finding rather than
re-running it. **State this in the workplan rather than leaving RD0 open**;
an open task row that is done reads as remaining work to whoever picks this
up next.

**RD0's doc half is genuinely undone.** All three sentences §0 says are
reversed still stand as written:

| Site | Still says | Must say |
|---|---|---|
| `docs/spec/crosscore.md:379` | `SPLIT RANGE` (working name) refused carrying the offending token's byte position | The gate lives; the DDL does not. The refusal moves to the allocator's admission check, and a decline has no token and no byte |
| `docs/spec/crosscore.md:516-518` | §8 test 13 — `SPLIT RANGE` on a gated relation refused with the token's byte position | The allocator declines, names the gate, the relation stays one range |
| `blueprint-range-ownership.md:157` | R3's row — *"manual `SPLIT RANGE` DDL"* | Reversed: there is no user-facing range DDL, in this phase or any later one |

## 3. The conclusions this milestone must produce

A milestone that only lands code hands the decision session nothing. These
are the four answers R3-A owes, and each names how it is reached rather
than being a topic.

**C1 — that the format epoch is necessary at all.** RD1's bump is
irreversible for every existing file, so the alternative must be refused on
a reason and not by omission. The alternative is a non-bootstrap directory:
`sys.ranges` as an ordinary user-invisible relation created on demand, or
range rows folded into an existing catalog relation. Reach it by writing
what each would cost at **mount**, when the routing cache is built before
any statement runs and the directory must already be readable. If a
non-bootstrap shape survives that, the bump is not necessary and this order
is revised rather than built around.

**C2 — that `RangeEligible`'s field set is complete.** The four gates are
`SchemaCanSpill` (`src/catalog/schema.cpp:29`), `indexes.empty()`
(`include/kds/catalog/schema.hpp:371`), the live-id `cabin_ids` test
(`command_dispatcher.cpp:3631-3633`), and
`fkeys_out.empty() && fkeys_in.empty()` (`schema.hpp:311-312`), plus D1's
btree decline. Reach the conclusion by **enumerating every per-relation
auxiliary structure the engine builds** — index, Bound Cabin, var-heap
root, Waystone trail, assertion entry pages, pattern rows, the anchor page
— and stating for each whether a gate covers it, whether it is
split-invariant, or whether it is a fifth gate this order missed. The
enumeration is the product; a list of four fields restated is not.

**C3 — where a decline is read.** §0 makes a decline *"a logged engine
decision naming the gate"*. A log line nothing surfaces is not observable,
and `docs/inflight/in-progress/observability.md` is proposal-only. Decide
and state: a counter, a `SHOW` field, or a log line and nothing else —
with the reason. The "absent rather than zeroed" rule the shipping block
keeps (`workplan-cross-owner-txn.md` R6-0's second deferral) applies here
too: a field that reads structurally 0 until RD5 exists should not be added
at RD4.

**C4 — sharpened input to D2 and D6, not a restatement of them.** For D2,
the anchor page's `{u64 key, u32 root}` capacity is 679
(`include/kds/catalog/anchor_page.hpp:42`) and its `index_oid == 0`
sentinel collides with a range at `lo = 0` (`src/catalog/catalog.cpp:1106-1111`)
— price whether that collision is avoidable by a `lo` offset, so the
operator chooses between CC9's row and the anchor on cost rather than on a
blocker. For D6, the two candidate units are **id space**
(`kRowIdLeasePerGrant = 4096`,
`include/kds/server/row_id_lease_service.hpp:27`) and **page space**
(`kDefaultExtentPages = 64`); invariant 13 makes row size a schema constant
(`RowLayout::Build`), so compute the rows-per-range each unit yields at the
three row widths the scenario benches actually use, and hand the operator
that table.

## 4. Hypotheses — each with its falsifier

Written before the work so a surprise is legible as one.

**H1 — RD1 is inert.** A v16 instance carrying an empty `sys.ranges`
behaves identically to v15 on every existing path; the only observable
difference is that a v15 file refuses to mount.
*Falsifier*: any suite delta beyond the version pins RA2 edits, or any
mount-time cost outside M1's noise band.

**H2 — RD4 costs zero because it has no caller.** `RangeEligible` is built
and tested at RA3 with exactly one caller — its test. Nothing on a
statement path calls it until RD5.
*Falsifier*: a caller appears in `CheckWriteAffinity`, `InsertOneRow`, or
any plan path. If one is needed, this order stops and reports, because that
is RD5's shape leaking into RD4 and it changes what §2b must decide.

**H3 — the four gates plus D1 are complete.** No auxiliary structure the
engine builds per relation is both split-unsound and unrepresented in
`TableAccess`.
*Falsifier*: C2's enumeration finds one. This is the hypothesis most likely
to fail, and failing is the useful outcome — the alternative is discovering
it at RD5 when a relation has already been split.

**H4 — the catalog ceiling cost is immaterial.** Each new bootstrap root
shrinks the catalog overflow range by one page, roughly 68 `sys.columns`
rows off the instance ceiling
(`include/kds/catalog/well_known.hpp:313-325`).
*Falsifier*: M2's number read against the widest schema the scenario benches
declare. If a real relation comes within an order of magnitude of the
ceiling, the bump is priced differently and C1 re-opens.

**H5 — the pre-range baseline is capturable now and not later.** RD3's
zero-cost invariant (*"a one-range relation on its owner core must add zero
instructions over today"*) and RD9(a) both compare against a "today" that
stops existing the moment RD5 lands. Capturing it at the format epoch is
the only honest before.
*Falsifier*: none — this one is a plan, not a claim. It is listed here so
that skipping M3 is a visible omission rather than an invisible one.

## 5. Task series — RA rows

| # | Task | Gate |
|---|---|---|
| **RA0** | **Retract RD0(a) in the workplan.** Mark it closed with `7148343`/`dcdc5e5` and the finding it produced, so the row stops reading as owed work. Record the two incidental defects that commit's message names — the `AttachTransport` read-before-assignment, and the moved-from batch at the queue head — where the subsystem owns them, not only in a commit message | none |
| **RA1** | **RD0(b)(c): the three reversed sentences**, amended in place at `crosscore.md:379`, `:516-518` and `blueprint:157` per §2's table. Plus §0's `[OPEN]` closure: `crosscore.md:447` and `:537` leave interleaved blocks *"default or opt-in"* — opt-in is a spelling the user would have to write, so under the operator's direction it reads as **default**, recorded as CLA's reading and correctable | none |
| **RA2** | **RD1: `sys.ranges` exists and is empty.** Oid **133** (free: table oids 100, 110-116, 130-132; column-oid bases 120-123, 140-145), fixed root page **15**, `kCatalogOverflowFirst` → 16, `kSuperBlockVersion` → **16** with a ledger entry quoting the 12 → 13 precedent verbatim. Joins all five exhaustive lists: `kAllWellKnownOids` (`well_known.hpp:215-230`, compile-gated), `kAllCatalogPages` (`:291-296`), the `static_assert` at `:333`, `Bootstrap()`'s `kSysTables` (`src/catalog/catalog.cpp:532-557` — the hard-coded `std::array<…, 9>` widens to 10), and the `DropTable` sweep chain (`:1704-1736`, or rows outlive the relation). Fixed-offset row per `SysCabinRow`'s template, every field fixed-width. **`tests/assertion_catalog_test.cpp:109` is `EXPECT_EQ(kCatalogOverflowFirst, 15u)` and this row breaks it** — that file's own comment at `:102-106` argues an exact pin is the wrong shape, which is why `:107` is `>=`; apply the same reasoning to `:109` rather than bumping the literal. It was missed once already at 13 → 14 | **C1** |
| **RA3** | **RD4: `RangeEligible(access)`**, the four fields plus D1's btree decline, per §6a. Built and tested **before** anything can allocate. A decline is a logged engine decision naming the gate — no token, no byte (§0). One caller: its test (H2) | RA2, **C2** |
| **RA4** | **C3's observability decision, applied or explicitly deferred.** If deferred, say where it lands and why a field added now would read structurally 0 | RA3 |
| **RA5** | **C4's two tables** — the anchor-page collision priced, and the range-size unit table at the scenario benches' real row widths. Handed to the decision session as its input, not decided here | RA2 |

## 6. Correctness — the gate, ahead of any number

- **Full suite green** at each RA row, `build-release`. Baseline read at
  `b0b6e8a` before RA2 and again after, in one sitting.
- **`scripts/sim.sh`** likewise, both runs in one sitting — the corpus
  varies day to day, so a comparison split across days is void.
- **A v15 file refuses to mount and says so.** Not a crash, not a silent
  reinterpretation. This is the whole point of the bump and it is the one
  behaviour RA2 adds.
- **A v16 file mounts, and `sys.ranges` reads as empty** through every
  route that enumerates catalog relations, including `DropTable`'s sweep.
- **`cores = 1` unchanged** (guideline 2).
- **No new caller of `RangeEligible`** — assert H2 by grep, in the report.

## 7. Measurement — M cells

Thin by design; this milestone is mostly structural. `build-release`,
interleaved arms in one sitting, fresh server and data file per invocation,
per-rep spreads before any median. Overhead A/B remains suspended per the
2026-08-24 operator amendment, so anything not run is reported as **not
run**, never implied.

| cell | measures | reads against |
|---|---|---|
| **M1** | **Mount cost of the tenth bootstrap relation.** Boot-to-listener time at v15 (`b0b6e8a`) against v16, fresh file both arms | H1. A delta outside noise means `sys.ranges` is not inert and RA2's shape is wrong |
| **M2** | **The catalog ceiling delta.** `sys.columns` rows available before and after, computed from `well_known.hpp:313-325`, read against the widest schema the four scenario benches declare | H4, and C1's second input |
| **M3** | **The pre-range baseline.** The statement mix RD9(a) will re-run — single-relation read, single-relation insert, the shipped-statement path — captured at v16 with `sys.ranges` empty. **This is the "today" RD3's zero-cost invariant is measured against**, and it stops being capturable the moment RD5 opens a second range | H5. Its product is a results file, not a finding |

Results to `bench/<version>/`, named by `git describe --tags`, in the
v2.3.0 cells' format: host with physical-versus-logical cores and SMT
state, filesystem, build flags, binary provenance; per-rep tables; findings
tagged **measured** or **source-read** with sites; and what the run does
not measure.

## 8. Improvement — what this milestone leaves better than it found

Three, and each is a thing a later reader would otherwise have to
rediscover.

- **RD0's open row closed with its history.** The step-batch defect was a
  documented invariant nothing enforced (`step_pipeline.hpp:153-154`
  declared the rule for the pipeline's whole life). RA0 records the
  *shape* — a declared invariant with no enforcement, and a null guard
  that turned a crash into a quiet wrong answer — where the subsystem owns
  it, because that shape is not unique to this site.
- **The exact-pin lesson applied rather than repeated.**
  `assertion_catalog_test.cpp:109` is the third instance of a pin that
  breaks on every later bump for no reason, and the file already carries
  the argument against it at `:102-106`. RA2 fixes the pin's shape, not
  its literal.
- **The decision session gets inputs, not topics.** C1–C4 are what turn
  D1/D2/D4/D6 and R6's D1–D7 from a list of open questions into a session
  with numbers on the table. That is the milestone's real output; the code
  is the smaller half.

## 9. Deliverable

- The three amended sentences (§2's table) landed in `crosscore.md` and the
  blueprint, with the `[OPEN]` closure recorded as CLA's reading.
- `sys.ranges` at v16, empty, joining all five exhaustive lists, with its
  superblock ledger entry.
- `RangeEligible` built, tested, and uncalled.
- C1–C4 written into `workplan-range-directory.md` — C1 and C2 as
  conclusions with their reasoning, C3 as a decision, C4 as two tables for
  the operator.
- M1–M3 results under `bench/<version>/`, `git describe --tags` in the
  filename.
- Every task row landing with its worktree name and short commit **inside
  the sentence that makes the claim**, per the standing rule.

Open items handed on rather than fixed: R3's D1 (the shared-structure
access mechanism, which removes the btree half entirely), D2, D4, D6; R6's
D1–D7 and its two `[OPEN]`s; RD6's `desc_page_id` finding; the 992-byte
reply cap; and the unattributed ~11 ms periodic stall.
