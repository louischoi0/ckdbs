# A cross-core read of 42 rows or more answers zero rows, silently

## 1. The symptom

A `SELECT` whose relation another core owns, executed through the P4d
step pipeline, returns **the heading and no rows** once its answer reaches
42 rows. There is no `ERR`, no `retryable` bit, no log line, and the
statement completes normally. The owner's own answer to the same
statement, on its own core, is the full result set.

Measured on the `ForeignIndexRig` two-core rig with a two-column `int64`
relation and `SELECT * FROM t WHERE v > 1000 AND v <= 1000+n`:

| n | Session's reply | Owner's local reply |
|---|---|---|
| ≤ 41 | byte-identical to local | n rows |
| **≥ 42** | **`id,v` — heading, zero rows** | n rows |

Verified at n = 42, 43, 44, 48, 64, 100, 120. **The loss is total, not
partial**: every batch of the statement is dropped, so the answer is
always heading-only.

`shipped_statements()->executed()` stays 0 throughout — this is the
pipeline path, not statement shipping, and the test asserts it.

## 2. Where it reproduces

Found in worktree `v2.4.0-r3` at `29593ac` (`v2.2.1-28-g29593ac`), whose
code tree is `acb2540`'s — the intervening commit is documentation only.
Scoped from the R3 workplan's §6 and confirmed by probe.
`CoreRuntimeTest.*` runs **76 passed / 1 failed**, the failure being the
new test alone.

## 3. The mechanism

Six links, each source-read.

1. **The batch target is 32× the slot it must fit.**
   `kStepBatchTargetBytes` is 32 KiB (`include/kds/server/step_pipeline.hpp:157`);
   the production ring slot payload is **1,024 bytes**
   (`sched::kCoreRingPayloadBytes`, `include/kds/sched/ring_transport.hpp:204`,
   its one caller `src/server/expeditor.cpp:1210-1211`). Nothing clamps
   one against the other — **though `step_pipeline.hpp:153-154` declares
   the rule**: *"must stay at or below the ring's max message payload
   minus the header"*. It is an unenforced documented invariant, which is
   why this is a defect and not a design gap.
2. **The final seal is unconditional**, so the target never protects
   anything: the streaming producer seals whatever it holds at
   `src/server/remote_step_service.cpp:899`
   (`if (writer.row_count() > 0) SealAndDrain(tag, writer);`), with the
   target-crossing seal at `:880`. `RowBatchWriter::full()` caps only on a
   u16 row count (`src/wire/row_codec.cpp:269-271`).
3. **42 rows crosses 1,024 bytes.** Payload = 24 (`StepBatchHeader`) + 2
   (`kBatchHeaderSize`, `src/wire/row_codec.cpp:82`) + 24/row → 1,010 at
   n = 41, **1,034 at n = 42**.
4. **`TrySend` refuses an oversize payload** with `InvalidArgument`
   (`include/kds/sched/spsc_ring.hpp:76-86`) — *"a programming error
   rather than a runtime condition"*, which is exactly right and exactly
   what nothing acts on.
5. **The send is fire-and-forget.** The step send lambdas submit a
   `MakeSendRetryTask` with **no `on_done`** and return `Status::OK()`
   synchronously (`src/server/expeditor.cpp:1363-1373`,
   `src/server/core_runtime.cpp:455-467`). `SendRetryTask::Poll` retries
   only `kResourceExhausted`; every other status calls a null `on_done_`
   and returns `kDone` — the payload is discarded
   (`include/kds/sched/send_retry.hpp:79-88`). `Drain` does check `send_`'s
   status (`src/server/remote_step_service.cpp:936-942`) and is told OK.
6. **The `STEP_EOF` still arrives**, so the session's read completes
   normally (`src/server/session_step_client.cpp:102-108`).

**The link that would have made this loud does not exist.**
`step_pipeline.hpp:51-53` says of the per-edge `seq` that *"a receiver
that sees a gap has lost a batch … asserted, not handled"*. It is neither:
`SessionStepClient::OnStepBatch` (`src/server/session_step_client.cpp:80-100`)
reads `tag` and `row_count` and never looks at `seq`.

## 4. The reproduction

`CoreRuntimeTest.AStepBatchWiderThanTheRingSlotStillDeliversEveryRow`,
`tests/core_runtime_test.cpp:3147` — **uncommitted, working tree only, and
it fails**. It builds a real `RealRingTransport` at
`sched::kCoreRingPayloadBytes`, a real peer `CoreRuntime` with production
`kStepBatchTargetBytes` and the `MakeSendRetryTask` sender, and a core-0
`SessionStepClient` wired as `Expeditor::Serve` wires it
(`src/server/expeditor.cpp:1362-1373`, `:1418-1443`).

**The control is what makes it decisive**: raising *only* the rig's ring
slot to 64 KiB makes the identical test pass at every n through 120.
Nothing else — credit, budget, MVCC, catalog coherence — is involved.

## 5. Why no existing test caught it

Every `RealRingTransport` in `tests/` other than `ForeignIndexRig`'s is
built with a 64- or 128-byte payload for unrelated reasons. The real
reason, though, is the P4e equivalence rig
(`tests/core_runtime_test.cpp:1399`, `:1490-1520`): it delivers each
message by calling the far side's handler **directly** from its `deliver`
lambda, with `batch_target_bytes = 1`. No batch it builds ever meets a
ring slot, and its target is 32,768× smaller than production's. The
equivalence suite is byte-exact about *content* and blind to *transport*.

## 6. What it is not

- **Not the shipped-reply cap** (`docs/inflight/known-gaps.md:1027-1034`).
  Measured on the same rig: a shipped reply past
  `kShippedStatementReplyTextMax` (992) answers
  `ERR UNKNOWN_OUTCOME retryable=0 … the statement's effect stands and its
  answer is lost` (`src/server/statement_ship_service.cpp:32-38`). That
  entry is accurate, and **the two paths differ in kind — shipping
  refuses and names the loss; the pipeline returns a wrong answer
  silently.** The pipeline is the worse of the two.
- **Not ring backpressure.** `kResourceExhausted` is retried correctly and
  is not this path.
- **Not R3's.** R3 multiplies edges rather than batch size, so it neither
  causes nor worsens this; the workplan
  (`docs/inflight/in-progress/workplan-range-directory.md` §6) is where it
  was found, not where it belongs.

## 7. What a fix owes

Two independent defects, and fixing only the first leaves a silent
wrong answer reachable by anything else that oversizes a message:

- **The sizing.** `batch_target_` must be clamped against the
  transport's `max_payload()` minus `sizeof(StepBatchHeader)` — the rule
  `step_pipeline.hpp:153-154` already states and nothing enforces.
- **The silence.** A send that fails for anything other than
  backpressure must reach the statement. The `on_done` seam already
  exists on `MakeSendRetryTask` and is passed null at both step senders;
  a `STEP_ERROR` to the session is the honest answer, and it is what
  turns a wrong result into a refusal.

Two corrections to the analysis this report was scoped from, recorded so
the numbers are not re-derived wrongly: the unconditional seal on the
`CoreRuntime` path is the streaming producer's at
`remote_step_service.cpp:899`, not the collect-then-stream fallback's at
`:385`; and the row-batch header is 2 bytes, not 4, which is why the
threshold is 42 rows rather than "roughly 20-50".
