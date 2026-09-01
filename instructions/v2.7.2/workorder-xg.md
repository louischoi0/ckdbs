# Work order XG — the typed shipped read lands, the portal stops leaking, and scenario 2 crosses owners

Drafted 2026-09-01 by CLA against `main` at `1beda80`
(`v2.7.0-27-g1beda80`). Successor to work order XF
(`instructions/v2.7.1/workorder-xf.md`): XF3 and XF4 closed there; this
order carries XF0's and XF5's answers into rulings and builds what they
gated. Source of record: `instructions/v2.7.1/ratification-xf0.md`,
`instructions/v2.7.1/ratification-xf5.md`,
`docs/inflight/in-progress/workplan-shipped-read-typed.md` (the XF1 plan,
§8), and `bench/v2.7.0/results-xe-ack-at-append-v2.7.0-17-ge310f8e.md`
§4.1 (the measurement this order exists to un-block).

## Rulings — XG-R1..XG-R8

Operator direction 2026-09-01: **CLA's recommendations are followed;
where XF0/XF5 carried no recommendation, CLA's proposal below is
accepted under the same instruction.** Each ruling names which kind it
is. No constant is decided anywhere in this order; the two places a
number threatened (Q3's description bound, Q5's target) are answered
structurally instead.

**XG-R1 (= Q1, recommended).** The rows cross on **an answer edge over
the existing step wire**: the arrival core mints the `PipelineTag` and
registers the receiver; the owner installs a batch sink on the shipped
session and streams `STEP_BATCH` under the existing credit protocol; the
ship reply POD becomes the terminator carrying status and watermark.
Codec, batch builder, credit grant, cancel and ceiling are reused
unchanged. Recorded consequence, stated at ratification so nobody
re-litigates it at review: the cap **moves from the whole reply to the
widest row** — a row wider than `StepBatchCeiling` at the shipped slot
stays refused `ResourceExhausted`; thirty narrow rows go from refused to
served.

**XG-R2 (= Q2, CLA proposal, accepted).** **A read is exempt from the
dedup record; a duplicate re-executes.** Grounds: D4's "guessing is the
one thing forbidden" is written about a write against an engine-issued
pk; a read has no side effect to guess about, and a re-executed read
under READ COMMITTED may answer different rows exactly as two successive
RC statements already may. This also bounds the record's width by
construction — the alternative that preserves today's behaviour needs a
cap, and a cap is a constant. `Remember` keeps running for every write,
untouched.

**XG-R3 (= Q3, recommended; sub-question answered by proposal).** The
result description crosses as **its own message on the answer edge,
before the first batch**. The column-count bound is answered by
**chunking**: a description larger than one ring message crosses as an
ordered sequence of description chunks, reassembled on the arrival core
before the receiver is armed; no ceiling is named and no new constant
exists. The chunk framing reuses the edge's existing sequencing — a
description chunk is to the description what a batch is to the result.

**XG-R4 (= Q4, recommended).** The text arm keeps its 992-byte
whole-reply cap, unchanged. The debug surface's limits are a debug
surface's (KW-D6); the newline byte-identity suite stays out of the
blast radius, which is precisely what makes it usable as this order's
regression proof.

**XG-R5 (= Q5, recommended).** The answer edge is governed by
`kStepBatchTargetBytes` (32 KiB) and `StepBatchCeiling`, both reused
unchanged. The order's earlier citation of the 64 KiB target is
corrected in the spec text: KW-D2's number is a socket-side quantity and
bounds nothing on a ring.

**XG-R6 (= Q6, CLA proposal, accepted).** **The shrink is accepted**:
`kShippedStatementTextMax` 992 → 976 bytes, the 16-byte `PipelineTag`
carried on the request POD. Grounds: option (b)'s 8-byte derived tag is
a second tag shape in an engine whose no-second-name rule is load-
bearing, and option (c) is `crosscore.md` §9's sizing decision, not
XF1's. The bound's move is client-visible and is therefore stated in
`client-manual.md` with this order, not discovered.

**XG-R7 (= Q7, recommended list confirmed).** What stays refused after
the edge lands, by name, in `crosscore.md` §4: (1) a row wider than
`StepBatchCeiling`; (2) a result that misses the 10 s deadline —
`UnknownOutcome`, unchanged; (3) `ANALYZE` of a foreign relation; (4) a
join over a spread relation (R5's). The operator adds none.

**XG-R8 (= XF5, CLA proposal, accepted).** **Option (b): a statement
error auto-closes the portal it was executing** — the one-line erase at
`OnStatementComplete`'s error arm, where the portal is already in hand.
The portal lifecycle change is written into `protocol.md`'s portal
section: a portal ceases to exist when its statement fails, so a later
`C_DESCRIBE`/`C_CONTINUE` answers "no such portal", and a later
`C_CLOSE` is the no-op it already is on an absent name. The sibling
question resolves by (iii)+(ii): with the leak fixed, reaching 64
portals means a client genuinely holds 64, which is a client defect and
correctly non-retryable; `IsRetryable` stays one code wide and the bit
stays 0 — the refusal text at `:551-554` is already accurate and
actionable. `tools/kwp.py`'s workaround is reverted and its ~11-12 µs
tax re-baselined in its own results file (XF's requirement, kept).

## Background

Three facts carried from XF, restated once:

1. Under KWP/1 every session carries a result sink, and `ShipStatement`
   refuses a shipped read to a typed client
   (`command_dispatcher.cpp:4257`) — so a typed client cannot read
   foreign data inside a transaction, and scenario 2's booking, which
   opens with exactly that read, cannot run under `peer_listeners = on`
   (XE §4.1, confirmed on three binaries).
2. The portal leak (`kwp_session.cpp:389-391` discarding a batched
   `C_CLOSE` during skip-to-sync) was worked around in the client at
   +11-12 µs per statement on every success path, quarantining XE's
   absolutes from XD's.
3. XF4's timers made the commit chain leg-addressable (three coordinator
   legs are the chain, +0.15% unaccounted), so the measurement this
   order un-blocks (XG4) can be reported per leg, not per total.

## Conclusions (standing)

1. **Spec precedes code** (XF conclusion 2, inherited): XG0 lands the
   amended spec text before XG1 compiles anything. The `cores = 1` path
   and the text protocol's bytes are untouched end to end — Guideline
   2's zero-overhead reading applies to the sink seam, and the newline
   suite must pass byte-identical on every commit of this order.
2. XE1 stays settled (XF3's confirming repeat); nothing here revisits
   ack timing. XG4 *uses* it.
3. SA proceeds independently; the one seam is stated in XG4 (the booking
   read's enrolment now takes SA-T0's no-prepare path, and the cell
   reports it rather than discovering it).

## Hypotheses

- **H-XG1.** The four refused shapes (projected foreign read, read
  inside a cross-owner transaction, sort/quota/sub-chain-bearing reads
  the remote-step edge declines) are served typed over the answer edge,
  with the XG-R7 residue the only remaining refusals.
- **H-XG2.** Reverting the client workaround recovers the measured
  11-12 µs per statement against XD's client, reconnecting XE's
  absolutes to XD's baselines.
- **H-XG3.** Scenario 2 runs whole under `peer_listeners = on` at
  cores ∈ {1, 2, 4}, with the kill matrix green — the refusal at `:4257`
  narrowed, not widened, and no new `UnknownOutcome` class.
- **H-XG4.** A real booking's cross-owner increment, measured at last,
  decomposes on XF4's legs with the prepare leg dominant (~46% was the
  pure-write shape's figure; the read-bearing shape's figure is the
  cell's answer, not assumed), and SA-T0 removes the read-only
  participants' prepare syncs from it.

## Tasks

**XG0 — the rulings land as text.** Amend, in one commit, before any
code: `crosscore.md` §4 (the residue list, XG-R7; the row-cap move,
XG-R1), the shipped-statement service spec (answer edge, description
chunks, terminator role, XG-R1/R3/R5), `cross-owner-txn.md` D4's note
(read exemption, XG-R2), `client-manual.md` (976, XG-R6),
`protocol.md`'s portal section (XG-R8), and `known-gaps.md` (the
shipped-read entries rewritten to point here). Move
`workplan-shipped-read-typed.md` from `blocked/` to `in-progress/`.
Gate: none.

**XG1 — the build, XF1-a through XF1-e as written.** The workplan's §8
rows execute under the rulings: (a) the answer edge and receiver
registration; (b) the owner-side batch sink on the shipped session; (c)
the description message and its chunking; (d) the terminator reply and
`ShipStatement`'s refusal narrowed to the XG-R7 residue; (e) the dedup
arm per XG-R2. The routing gates at `:7274-7277`/`:7362` are untouched —
which statements ship does not change; what a shipped read answers does.
Gate: XG0.

**XG2 — the portal fix and the tax refund.** XG-R8's one-line erase;
the spec sentence already landed in XG0; revert `tools/kwp.py`'s
always-own-frame `C_CLOSE`; one results file re-baselining the
per-statement cost against XD's client on the same machine discipline
(interleaved arms, ≥3 runs, spread). The golden sessions and newline
suite must not move. Gate: XG0; independent of XG1.

**XG3 — the kill matrix extends to the answer edge.** Crash injection
at: receiver registered/no batch yet; mid-stream between batches;
description partially chunked; terminator sent/not yet consumed; cancel
racing the first batch; credit exhausted at the moment of owner death.
Each cell answers with the existing taxonomy (error, `UnknownOutcome`,
clean close) — no new outcome class is admitted. The 10 s deadline cell
re-run on the typed arm. Gate: XG1.

**XG4 — the measurement XE §4.1 left open.** Scenario 2 whole, typed
client, `peer_listeners = on`, cores ∈ {1, 2, 4}, b ∈ {1, 8},
against the text-arm and local baselines, reported per leg on XF4's
timers; the read-only-participant share reported explicitly (SA-T0's
seam; this cell subsumes what SA-M0 would have measured on a synthetic
shape, and is named in SA's file as doing so). Result files under
`bench/v2.7.2/`, named by `git describe --tags`, arms interleaved, ≥3
runs with spread. Gate: XG1, XG2 (the tax must be gone first — its
removal is what makes these absolutes comparable), XG3 (a cell run
before the matrix is green risks measuring a defect).

**XG5 — closure.** `known-gaps.md`'s shipped-read and portal entries
closed or narrowed to the XG-R7 residue; XE's quarantine note lifted
(its absolutes now reconnect through XG2's re-baseline); the workplan
deleted per the workflow rule; a short verdict section in the XG4
results file stating which of H-XG1..4 held, refused, or split. Gate:
XG4.

## Measurement discipline

`build-release` only; `git describe --tags` naming; ≥3 runs with
min/max/stddev; interleaved arms for every A/B; per-statement fixed
costs as server CPU; no comparison against any newline-protocol-era
absolute; the XG2 re-baseline is the bridge that makes XE-era and
XG-era absolutes comparable, and no other bridge is claimed.

## What this order does not claim

It does not touch ack timing (XE1 settled), the ring slot size
(`crosscore.md` §9 stays open), fragmented batches for the
wider-than-ceiling row (named residue, XG-R7(1)), the remote-step
edge's own refusals (deliberate, `:7355-7362`), SA's tasks T1-T9, or
any constant. The sibling retryability question is closed by XG-R8's
(iii)+(ii) and reopens only if the leak's fix is ever reverted. If any
XG1 sub-task finds the workplan's source citations stale against a
moved line, it re-verifies at HEAD and amends the workplan rather than
building against a memory.

---

## Row status (CLA, appended as rows land)

| row | status |
|---|---|
| XG0 | **Done 2026-09-01.** `crosscore.md` gains **§4a** (the answer edge, the description's chunked own message kind, the sizing correction, the four-item residue) and its stale "the D5 encoder does not exist yet" claim is retracted; `client-manual.md` carries the per-row bound and the 976-byte statement cap; `protocol.md` §7 carries the portal lifecycle; `cross-owner-txn.md` §1a carries the read's dedup exemption and what it bounds; `known-gaps.md`'s three entries point here; the workplan moved to `in-progress/` |
| XG1 | **Done 2026-09-01.** The wire (`form` + tag, 976-byte statements), the receiver, the owner's sink and answer edge, `kShippedRowDesc` chunked, XG-R2's dedup arm, and the arrival core's forward - **option 2**, a `ResultSink::AcceptsEncodedRows` predicate, false by default, so the edge's bytes reach a wire sink unchanged and a text sink is refused rather than fed the wrong encoding. Row boundaries from `wire::DecodeRowExtents`, in the file that owns the format. Two corrections landed in `crosscore.md` §4a: the owner **buffers and sends under credit** rather than streaming (a `ResultSink` has no suspension point), so the terminator can outrun the rows and the arrival core waits for the EOF too; and XG-R2 is narrower than its letter because the owner cannot identify a text-arm read. One real bug caught in review: the re-framing stripped the u16 row count that `EncodeStepBatch` keeps, which would have shifted every field by two bytes and decoded as plausible values |
| XG2 | **Done 2026-09-01.** XG-R8's one-line erase at `OnStatementComplete`'s error arm, two unit cells, `tools/kwp.py` reverted to the batched `C_CLOSE`, and an end-to-end check that 100 failing statements no longer wedge a session. Re-baselined in `bench/v2.7.2/results-xg2-portal-close-tax-v2.7.0-30-g9a4dd76.md`: the refund is **29.8 µs p50** on this host against a 0.47% floor. **H-XG2 splits** - the mechanism reproduces (one round trip, flat across the distribution) and the magnitude does not transfer from XE's host, so **the bridge XG asked this cell to be does not exist** and XE's quarantine note must not be lifted on it |
| XG3 | **Partly done 2026-09-01, and the unfinished half is named.** Four crash points placed on the answer edge (`shipped.answer_desc_chunk`, `.answer_described_prerows`, `.answer_batch_sent`, `.answer_edge_closed_prereply`) and three faults asserted without a kill: an out-of-order description chunk, a description that is whole only when every chunk has arrived, and rows arriving with no description at all - each refused rather than decoded against a guess, with the sink left undescribed and empty. No new outcome class was admitted. **The process-kill half is owed and has not been run**, along with the credit-exhaustion cell and the 10 s deadline on the typed arm; the cell-by-cell table is `workplan-shipped-read-typed.md` §8c |
| XG4 | **Done 2026-09-01** - `bench/v2.7.2/results-xg4-scenario2-crossowner-v2.7.0-44-g08c5592.md`. 30 cells, 0 failures. Scenario 2 crosses owners at last: **0.47x local TPS, 2.16x p50, 3.009 syncs per booking** at one booker, the same at 2 and 4 cores because `creating` gives one participant either way. **The three legs are the chain on a real booking** (+/-1.4%) and **`prepare` is the largest at 35-39%**, both as XF4 predicted - but its transport share is **2.2% at one booker against XF4's 26% at eight**, so "half a cross-owner commit is not the device" is a **load** property, not a protocol one. **H-XG4's SA-T0 clause is refused**: `shipped_readonly_prepares` is 0 in every cell, because a booking writes to its participant (XD6) and so never has a read-only one. Two engine bugs and one harness trap found on the way, all in §2-§3 |
| XG5 | not started |

**One thing XG0 settled that the order left to the builder**: a
description chunk is **its own message kind with its own sequence**, not
a `STEP_BATCH` with a flag. `StepBatchHeader::seq` is per-edge and
asserted contiguous — "a receiver that sees a gap has lost a batch …
asserted, not handled" — so folding a differently-shaped payload into
that sequence would either break the assertion or force description
chunks to be counted as batches. Two kinds, two sequences, one tag.
Recorded here and in `crosscore.md` §4a.
