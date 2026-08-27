# Work order R6-P — the cross-owner commit protocol

Drafted 2026-08-27 against `main` at `ec5f993` (`v2.2.1-79-gec5f993`).

The second half of `instructions/v2.5.0/2pc.md`. That order owns R6 whole;
this one takes rows R6-3 … R6-8 as a milestone and states what building
them must conclude, predict and measure. **It does not restate D1–D7** —
they live in the parent order and this milestone's first row is the gate
that records their ratification.

## 0. Why this milestone, and why now

R3-A closed 2026-08-27 (`ec5f993`): `sys.ranges` exists empty at superblock
v16, `RangeEligible` is built with one caller, C1–C4 are written, M1–M3 are
captured. `workplan-range-directory.md` §8 ends *"the build resumes at RD2
after the decision session."* R6 has been parked at `b7b2bfc` since 08:36
that morning with R6-0/1/2 built and R6-3 unstarted.

So the tree has two resumable fronts and one shared decision point. This
order takes R6 rather than RD2, for three reasons, each falsifiable:

1. **R6-8 rewrites `CheckWriteAffinity`; RD5/RD6 rewrite what it reads.**
   The RA3 review already recorded this — S1 moved the live-id cabin
   predicate to `TableAccess::AnyCabin()` and left `CheckWriteAffinity`
   untouched *"per the order's out-list; that site flips whenever
   RD5/R6-8 rewrites it."* One of the two should flip it, and the other
   should find it already flipped.

2. **R6 is granularity-agnostic where it counts.** R6-2 keys enrolment on
   `(src_core, session_id)`, never on a relation. Under ranges a
   participant is still a core; only R6-8's discovery changes, from a
   field read to RD3's resolver. Building R6 on ranges first re-opens D1's
   participant discovery on a substrate that does not exist.

3. **R3's back half currently has nothing to measure.** §10b's last caveat
   is the finding: all three named bulk bench relations — `daily_stats`,
   `trades`, `freights` — are declared `BTREE` by their drivers, and §4's
   D1 decline means **no btree relation can split until the
   shared-structure mechanism lands**. RD9(b)'s size sweep and RD9(c)'s
   k-range read would have to invent a heap-clustered relation no scenario
   uses. RD2–RD8 would build correctly and measure nothing real. That is
   an argument for taking R3's D1 seriously at the decision session, not
   for building past it.

## 1. Scope

**In.** R6-3 (prepare and decide), R6-4 (recovery), R6-5 (in-doubt), R6-6
(PW3b extension), R6-7 (PL-A revisit), R6-8 (dispatch); the parent order's
§5 correctness gate in full; its §6 B-cells.

**Out.** R6-9 (the spec under `docs/spec/`, and the `crosscore.md`/`wal.md`/
`client-manual.md` rewrites) — a closing pass, because `CLAUDE.md` makes
`docs/spec/` what is confirmed *and implemented*, and until RP7's gate
passes this is neither. Every RD row. Range policy. `cabin_optimizer`
jurisdiction.

**Explicitly not touched**, so a diff outside these is a finding:
`RangeEligible` and its three files (H2 holds — `RangeEligible`'s TU does
not enter `kds_server`'s link, confirmed at the linker at `fd8e4da`);
`sys.ranges`; the free-map; the reactor wake path.

## 2. The gate this milestone opens on

**Nothing below may start until D1–D7 and their two `[OPEN]`s are
ratified, amended or refused by the operator.**
`workplan-cross-owner-txn.md`'s header states the current position without
softening it: *"D1–D7 are not ratified — only Finding 1's option is."*

R6-0/1/2 were buildable under that because they survive any D-choice — a
wire bit, a sizing `static_assert`, and an enrolment keyed on a pair of
ids. **R6-3 is not.** It implements D4 directly and reads D3's watermark
rule and D5's in-doubt rule. Building it against unratified proposals means
writing the protocol twice.

Two `[OPEN]`s in particular have no default:

- **D3's**: whether READ COMMITTED cross-owner transactions skip the
  watermark entirely. The parent order proposes yes.
- **D5's**: whether an in-doubt participant **blocks** writers of the same
  rows (correct, can stall) or **refuses them retryably** (available,
  surfaces engine-internal state to the user). The parent order proposes
  block, with a bounded wait ending in a named refusal.

RA4's precedent applies to neither: that decision was CLA's to take because
it was reversible and had no caller. These two are neither — D5's choice is
a client-visible contract, and D3's determines what a participant's read
does on the hot path.

## 3. The conclusions this milestone must produce

**CP1 — recovery across a core-count change.** `wal.md` §3's second
`[OPEN]`, which R6-4 is named as the place that answers it. A `PREPARE`
record sits in a stream whose core no longer exists, or exists at a
different index. Reach the conclusion by enumerating the three shapes —
fewer cores, more cores, same count re-indexed — and stating for each what
mount does. **A refusal to mount is an acceptable answer** and is recorded
as one; a hang is not. The falsifier that matters: if the only sound
resolution requires comparing LSNs across streams, guideline 3 refuses it
and the answer becomes refuse-to-mount by construction, which is a stronger
conclusion than a mechanism.

**CP2 — the fast path is free at the instruction level, not at the
median.** The parent order's D1 makes the one-owner path *"pay nothing"*
and §5 asserts it by counting prepare messages (zero). That is necessary
and not sufficient — a zero count is compatible with a branch on every
commit. M3's precedent is the standard to meet: H2 was confirmed by
sha256-identical binaries, not by a benchmark. Reach CP2 the same way where
possible — name what a single-owner commit executes that it did not before,
and if the answer is "one predictable branch on a field already in cache",
say so with the site.

**CP3 — the third era of `cross_core_write_refusals`.** R6-8 converts one
class of refusal into enrolment and leaves every other class spelled
exactly as it was. The conclusion is the **list of what still refuses**:
DDL, hop limit, the shape gate's tail, and whatever else survives. This is
B4's product and it is what names the roadmap's remaining debt, so it is a
conclusion and not a counter reading.

**CP4 — PL-A's revisit verdict.** `page-lsn-cross-stream.md` reserved a
revisit clause armed by exactly this milestone. Execute it: state what 2PC
changes about cross-stream page handoff, **or state that it changes nothing
and why**. The second is a real outcome and the more likely one — a
block-aligned participant writes its own pages from its own lease — but it
must be argued, not assumed.

## 4. Hypotheses — each with its falsifier

**HP1 — the one-owner fast path costs zero.** A transaction touching one
owner takes the pre-R6 path, entering no protocol and paying no measurable
per-commit cost.
*Falsifier*: B3 shows a loss outside the noise band, **or** CP2 finds work
on the single-owner path that is not one cached-field branch. This is D1's
gate failing, and the parent order says so.

**HP2 — two syncs deep, up to four wide.** D7 predicts a two-owner
transaction at roughly 2× a one-owner one, with participants' prepare syncs
overlapping — `bench/v2.1.0` §3a measured this device overlapping four
streams at 3.37× before declining.
*Falsifier*: B1 lands materially off 2×, or B2 shows width cost growing
before four participants. Either means the protocol, not the device, is the
limit — which is the more useful outcome and the reason D7 was written
before measuring.

**HP3 — no hang is reachable.** Every wait in the protocol has a bounded
ceiling ending in a named refusal (D5), so the kill −9 matrix produces
errors and refusals, never silence.
*Falsifier*: any protocol point at which a participant or coordinator waits
unbounded. The parent order calls a hang a blocking finding; this states it
as a prediction so its failure is legible.

**HP4 — every other refusal keeps its exact spelling and wire bit.** R6-8
changes one class and leaves the rest byte-identical.
*Falsifier*: any existing refusal test needs its expected text or status
edited. If one does, that is a wire-contract change and it stops the row.

**HP5 — recovery needs no new cross-stream comparison.** Resolving a
prepared-but-undecided transaction reads the coordinator's stream for the
outcome and the participant's for what to redo, which are two independent
reads and not a comparison.
*Falsifier*: R6-4's implementation orders two streams' records against each
other. Guideline 3 refuses it, and CP1 turns into refuse-to-mount.

## 5. Task series — RP rows

| # | Task | Gate |
|---|---|---|
| **RP0** | **The ratification record.** D1–D7 and both `[OPEN]`s written into `workplan-cross-owner-txn.md` §2 as ratified, amended or refused, each with the operator's wording where it differs from the proposal. **If any is unratified, this order stops here and reports** — that is the row's real function | operator |
| **RP1** | **R6-3 — prepare and decide** (D4). The coordinator's parked waiter over N participants; the participant's durable prepare in its own stream naming the coordinator's `(session_id, transaction_id)`; the decision record in the coordinator's stream and nowhere else. The one-participant short-circuit fires **before** any of it (D1) | RP0 |
| **RP2** | **R6-4 — recovery** (D4), and **CP1**. Analysis learns each participant's prepared-but-undecided transactions and resolves each against the coordinator's stream. The core-count-change answer per CP1, recorded whichever way it lands | RP1 |
| **RP3** | **R6-5 — in-doubt** (D5). The participant's wait, its bounded ceiling, the resolution ask carrying R6-0's retry bit, and the client-facing `UNKNOWN_OUTCOME` contract in the words shipped statements already use | RP1 |
| **RP4** | **R6-6 — PW3b extension.** A prepared-but-undecided transaction survives shutdown and restart. Re-read PW3b's open review item (`known-gaps.md:250`) in this light and say what it now means | RP1 |
| **RP5** | **R6-7 — PL-A revisit**, and **CP4**. Analysis and doc; no engine code expected, and if it needs some, that is the finding | RP1 |
| **RP6** | **R6-8 — dispatch**, and **CP3**. `CheckWriteAffinity` stops refusing the cross-owner explicit-transaction shape and enrols a participant instead. **Every other refusal keeps its exact spelling and wire bit** (HP4). Note for the RD5 author: this is the row that flips the site the RA3 review left untouched | RP1, RP3 |
| **RP7** | **The correctness gate** — the parent order's §5 in full. Kill −9 at each protocol point on each side: before prepare, between prepare and its durability, after prepare before decide, after decide before participants learn. All-or-nothing verified by count after every one. In-doubt resolution exercised deliberately, including the coordinator's record gone and `UnknownOutcome` the honest answer. One-owner fast path asserted by prepare-message count = 0. `cores = 1` unchanged. Full suite and `scripts/sim.sh`, baseline and re-read **in one sitting** | RP2…RP6 |
| **RP8** | **R6-B cells B1–B5** (parent §6). B5 is captured but **does not gate** — the parent order says so | RP7 |

## 6. Correctness — ahead of any number

The parent order's §5 is the gate and RP7 runs it; it is not restated here.
Three additions this milestone owes:

- **A hang is a blocking finding, and HP3 makes it a predicted
  impossibility.** Two-phase commit's characteristic failure is not a slow
  transaction but one that never answers. Any test that needs a timeout to
  finish is reported, not tuned.
- **No diff outside §1's in-list.** Asserted by reviewing the changed-file
  set at RP7, not by intention.
- **The sitting rule.** The sim corpus varies day to day, so a baseline and
  its re-read that straddle two days is void, and RP7 must say which
  sitting it ran in.

## 7. Measurement — B cells

Parent §6's five cells, unchanged. `build-release`, interleaved arms in one
sitting, per-arm processes, fresh server and data file per invocation,
per-rep spreads before any median, rows in = rows out. The overhead A/B
suspension (2026-08-24) still stands, so anything not run is reported as
**not run**.

| cell | measures | reads against |
|---|---|---|
| **B1** | Two-owner transaction against the same work as two separate one-owner transactions | HP2's ~2× |
| **B2** | Width: 2, 3, 4 participants, and past four where the device's overlap curve declines | HP2's depth-2-width-4 shape |
| **B3** | One-owner transactions on an R6 build against the pre-R6 tag | **HP1**, and D1's gate |
| **B4** | What `cross_core_write_refusals` still counts | **CP3** — the product is the class list, not the number |
| **B5** | Scenario benches: what fraction of a realistic workload takes the two-phase path | Sizes future work. Captured, **non-gating** |

Two standing constraints inherited rather than invented:

- **M3's re-read contract binds only RD9(a)**, not these cells. But its
  reason binds everything: absolute numbers are sitting-bound (M1's
  finding), so B3's before arm is a **same-sitting rebuild of the pre-R6
  tag**, never a subtraction from a stored file.
- **Results to `bench/v2.5.0/`**, named by `git describe --tags`, in the
  v2.3.0 format: host with physical-versus-logical cores and SMT state,
  filesystem, build flags, binary provenance, per-rep tables, findings
  tagged **measured** or **source-read** with sites, and what the run does
  not measure.

## 8. Improvement — what this leaves better than it found

- **`CheckWriteAffinity` settled once.** The function has been the
  collision point between two open milestones since both opened. RP6 flips
  it, and RD5's author inherits a settled site instead of a contested one.
- **Two `[OPEN]`s closed with their reasons**, not by omission: `wal.md`
  §3's core-count case (CP1) and `page-lsn-cross-stream.md`'s revisit
  clause (CP4). Both have been armed and waiting on this milestone by
  name.
- **The refusal counter's third era named** (CP3). `cross_core_write_refusals`
  has meant three different things across three eras; B4's product is the
  list that says what the third one is, which is what the roadmap's
  remaining debt actually consists of.
- **A protocol whose failure mode was predicted before it was tested**
  (HP3). The kill −9 matrix either confirms the ceiling or finds the point
  that has none, and either way the finding is legible as one.

## 9. Deliverable

- D1–D7 and both `[OPEN]`s recorded as ratified, amended or refused (RP0).
- R6-3 … R6-8 built, each row landing with its worktree name and short
  commit **inside the sentence that makes the claim**, and each with its
  `critics-developer` pass, findings applied or rejected by name.
- CP1–CP4 written into `workplan-cross-owner-txn.md`.
- The §5 gate run and reported, including which sitting.
- B1–B5 under `bench/v2.5.0/`.
- **Not delivered here**: the `docs/spec/` spec (R6-9). It follows once
  this milestone's gate passes, because a spec is what is confirmed and
  implemented.

Open items handed on rather than fixed: R3's D1 (shared-structure access —
and §0's third reason makes it the pivotal one), D2, D4, D6; RD6's
`desc_page_id` finding; the 992-byte reply cap against rule 1; the
unattributed ~11 ms periodic stall; RW-C1's C-state experiment; and the two
cws issues `range-merge-open-tension` and
`scenario2-freight-stale-ceiling-comment`.
