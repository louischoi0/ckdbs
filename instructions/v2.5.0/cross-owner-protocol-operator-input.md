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

## Still unratified — RP0 stops on these

**D1–D7 themselves.** `workplan-cross-owner-txn.md`'s header states the
position: *"D1–D7 are not ratified — only Finding 1's option is."* The two
`[OPEN]`s above are items *inside* D3 and D5, not the D-rows. Until the
operator ratifies, amends or refuses D1–D7, RP0's own gate holds and the
order stops there by its own instruction — RP1 implements D4 directly and
reads D3's watermark rule and D5's in-doubt rule, so it cannot start.
