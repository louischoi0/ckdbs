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
