# Operator input against `cross-owner-protocol.md` — the ratification record's raw material

This file is operator input, not CLA prose: it holds the operator's own
words as they arrived, for RP0 to write into
`docs/inflight/in-progress/workplan-cross-owner-txn.md` §2. RP0 is the row
that produces the ratification record; this is only what it has to work
from, so **nothing here is a decision until RP0 records it**, and RP0 still
stops on anything not covered below.

## Received 2026-08-27, in session, workflow mode active

Two of the work order §2's items arrived unprompted, each restating the
question and naming the proposal:

- **D3's `[OPEN]` — the watermark under READ COMMITTED.** Operator's
  words: *"READ COMMITTED cross-owner 트랜잭션이 watermark를 건너뛰는가.
  제안은 「그렇다」."* Read as: the proposal stands — a READ COMMITTED
  cross-owner transaction **skips the watermark entirely**.

- **D5's `[OPEN]` — what an in-doubt participant does to a writer of the
  same rows.** Operator's words: *"in-doubt participant가 같은 행 writer를
  막는가, retryable하게 거절하는가. 제안은 「막되 상한을 두고 named
  refusal로 끝낸다」."* Read as: the proposal stands — the participant
  **blocks**, with a bounded ceiling that ends in a **named refusal**,
  rather than refusing retryably up front.

**The reading is CLA's, and it is the one thing here that could be wrong.**
Each message states the question and its proposal without a verdict word;
they are read as ratifications because the operator sent them, unprompted,
at the moment workflow mode opened on the order whose §2 gates on exactly
these two. If either reading is wrong, RP0 is where it is corrected, and
correcting it costs nothing until RP1 builds on it.

## Still unratified — RP0 stops on these *(superseded 2026-08-28 by the section below; kept because it is what RP0 reported against, and a record that erases its own gate cannot be audited)*

**D1–D7 themselves.** `workplan-cross-owner-txn.md`'s header states the
position: *"D1–D7 are not ratified — only Finding 1's option is."* The two
`[OPEN]`s above are items *inside* D3 and D5, not the D-rows. Until the
operator ratifies, amends or refuses D1–D7, RP0's own gate holds and the
order stops there by its own instruction — RP1 implements D4 directly and
reads D3's watermark rule and D5's in-doubt rule, so it cannot start.

## Ratified 2026-08-28 — D1–D7, by the operator's "follow CLA proposal"

The operator supplied the full ratification packet (*"Ratification packet —
R6 design decisions D1–D7"*, prepared against `main` at `ec5f993`) and
ratified it with the words **"follow CLA proposal"**. That settles every row
at the packet's own summary table, CLA's reading being the ratified wording
in each case:

| # | Ratified as |
|---|---|
| **D1** | As written. The **arrival core coordinates**; participants are relation owners **discovered as the transaction runs**; a one-owner transaction takes the single-core path unchanged. Forecloses data-chosen coordinators. |
| **D2** | As written **with its rejection reason amended**. Per-participant local trx ids from each core's own lease, the coordinator's `(session_id, transaction_id)` recorded beside them. The shared-id alternative is rejected **on mount validation** — `CoreRuntime::Open` refuses a mount whose peer stream names an id above the superblock's ceiling (`trx_id.hpp:110-113`), so a shared id puts foreign ids in every participant's stream — **not** on the "global atomic counter or cross-stream ordering" grounds the parent order gave, which are false at the source: the trx-id domain is already global (`trx_id.hpp:92-93`, `trx_id_lease.hpp:11-18`). Outcome unchanged; the reason is corrected so a later reader does not find it false and reopen a settled decision. |
| **D3** | As written. A per-participant **watermark**, giving a cross-owner RR transaction a **consistent-per-core** snapshot, not a globally consistent one — and that weakening is a product property for `client-manual.md`, not only a spec line. |
| **D3's `[OPEN]`** | **Yes — READ COMMITTED skips the watermark entirely**; watermarks are carried for REPEATABLE READ only. Obliges one client-manual sentence distinguishing the two levels' cross-core promises, since RC is the default and a reader of D3's RR wording would otherwise assume it covers RC. |
| **D4** | As written. Two phases over the existing ring; a participant that replied prepared may not unilaterally abort; **the `COMMIT` in the coordinator's own stream is the decision, and it lives in exactly one stream**. Forecloses one-phase commit and presumed-commit/presumed-abort. |
| **D5** | As written. An in-doubt participant may neither abort nor commit, holds its locks and waits; resolution is a retry in Finding 1's sense, so a coordinator no longer holding the record answers `UnknownOutcome`; after `UNKNOWN_OUTCOME` the remedy is to **read the data**, never to retry — in the words shipped statements already use. |
| **D5's `[OPEN]`** | **Block**, with a **bounded ceiling ending in a named refusal**. Three obligations ride with it: the ceiling is a **named constant reached through one function** and config-swept; the refusal at the ceiling is **retryable and named**, and is **not** `UnknownOutcome`; and **R6-5 declares and sizes the in-doubt ask's wire form**, which R6-1's sizing answer does not cover. |
| **D5's ceiling value** | **CLA's to propose and measure** — the packet offered that branch and "follow CLA proposal" takes it. Proposed from the sync cost M3 and `bench/v2.1.0` §3a already measured, then swept. |
| **D6** | **Confirmed discharged** by R6-1's measurement at `c97f5ca` (24 bytes per request leg, 256 for the participant reply, against a 1,024-byte slot; assertions written against `kCoreRingPayloadBytes`, not the literal). **R6-5's in-doubt ask is still owed its own sizing.** |
| **D7** | Ratified **as a pre-registered prediction**, not a decision: two syncs deep, up to four participants wide. **B1 reports p50 and p99 rather than a single ratio** — M3 found shipping's cost in the tail (+11% p50 against +76% p99), so a 2× median alone would record confirmation while an unpredicted tail passed unnoticed. |

**The gate is open.** RP0's remaining work is to write the above into
`docs/inflight/in-progress/workplan-cross-owner-txn.md` §2 as the
ratification record; RP1 (R6-3) opens behind it, and RP2–RP8 return to the
queue in their planned order.

**One path note, not a correction.** The packet cites the order as
`instructions/v2.4.0/cross-owner-protocol.md`; the file is at
`instructions/v2.5.0/cross-owner-protocol.md`, and the order's own §0 cites
its parent as `instructions/v2.5.0/2pc.md` when the parent is at
`instructions/v2.4.0/2pc.md`. The two citations are inverted versions of the
same v2.4.0/v2.5.0 confusion, already tracked as cws issue
`cross-owner-protocol-parent-cite` (id 10). Operator input is not edited
here.
