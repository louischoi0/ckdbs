# The operator's words of 2026-10-31

## 1. CK written: an assertion reservation's commit settle, removed

| | |
|---|---|
| **Prompt** | The operator proposed Oracle's delayed block cleanout for KDS (W0). CLA read the tree at `18264195` and reported three things. No tuple is visited at commit and no reader writes a page to learn one. The assertion reservation's settle (`AssertionEnforcer::CommitTxn`) is the one commit-time step whose page work scales with the writes. And the `kEntryReserved` bit it clears has no reader. CLA offered (a) keep the settle, (b) remove it, (c) give the entry a writer id so the bit can be resolved lazily |
| **Word** | W1: *"Tell me about the assertion reservation settle."* W2: *"Write an exploratory, verifying milestone document on (b)."* |
| **Mark** | **CK written** (`workorder-ck-assertion-commit-settle.md`), **not opened**. CK-Q0..Q7 carry CLA's proposals, and none is marked. The independent read of the order found that a stranded reservation fails **open** when it is a departure or a negative `SUM` arrival. Stranded here means kept by a crash loser (no mount path compensates it, §1.8) or by a failed settle (§1.6): the group's total is left too low, and the upper bound admits writes it forbids, `[quiet-wrong]`. Filed as `docs/inflight/bugs/an-assertion-keeps-the-reservations-of-a-crash-loser-and-a-failed-settle.md`, found by reading and not reproduced. CK removes the failed-settle path. The crash path waits for an order of its own (CK-Q5) |
| **Does not settle** | CK's opening; every CK-Q; a push of anything but documents; a tag, a version |
| **Recorded at** | this file; the order's header; the CK and 2026-10-31 rows in `index.md`; the bug entry |

## 2. The order relettered CK

| | |
|---|---|
| **Word** | W3: *"CJ is already in use"*, after §1's order was pushed as CJ at `c0814a6b` |
| **Mark** | **Relettered CK** (`workorder-ck-assertion-commit-settle.md`). The file is renamed, and every `CJ-Q`/`CJ-R`/`CJ-S` token is now `CK-`, in the order, this file, `index.md` and the bug entry. §1 above reads as relettered. The letter CJ belongs to the session using it. No `CK` stood on `origin` at `c0814a6b` |
| **Does not settle** | anything §1 left open |
| **Recorded at** | this file; the order's "The letter"; the CK row in `index.md` |

## 3. CA closed

| | |
|---|---|
| **Word** | *"close CA"* |
| **Mark** | **CA closed** (`workorder-ca-background-advisory-optimizer.md`, header and §7). Its premise was refuted at CA-S0 (the advisory work is 0.2-0.6 % of sampled server CPU on scenarios 1-4), so CA-S1..S3 were never built; the kept candidates are C1 `txn::VisibilityMemo`, C3 `ExecStats::For` inline and C4 `storage::InlineCellBytes`, all on `main`. Its close was measured at `v3.2.0-159-g954abc20` (`bench/v3.2.0/results-ca-close-v3.2.0-159-g954abc20.md`): scenarios 1 and 4 improved, scenarios 2 and 3 unchanged within noise. What it leaves is §8 of the order and `docs/inflight/known-gaps.md`'s "The advisory optimizer, in the background (CA)". No suite was run for this record, which adds only `instructions/` |
| **Does not settle** | Scenario 2's commit-protocol change; the advisory structures CA-R1..R6 describe; a push; a tag, a version |
| **Recorded at** | this file; the CA order's header; the CA row and this file's row in `instructions/v3.2.0/index.md` |

## 4. CK opened on the standing go-ahead

| | |
|---|---|
| **Word** | *"go ck"*, then the standing go-ahead (`go-ahead-achieving-milestone`): follow CLA's proposal where a decision is needed |
| **Mark** | **CK opened** in worktree `ck-assertion-commit-settle` at `8d183e9b`. CK-Q0..Q7 **adopted as CLA proposed, by CLA, on the operator's standing go-ahead** and not as per-item marks: Q0 (a), Q1 (a), Q2 (a), Q3 (a) if S0 confirms H1 for every shipped tag (else (b)), Q4 (a), Q5 (a), Q6 (a), Q7 (a). CK-Q0 (a) reserves S0's verdict (build, narrow or drop) to the operator, so the run ends at S0's close |
| **Does not settle** | S0's verdict; CK-S1 onward; the crash-loser order (CK-Q5); a push; a tag, a version |
| **Recorded at** | this file; the order's header and §4 and §7; the CK row in `index.md` |
