# The operator's words of 2026-10-31

## 1. CJ written: an assertion reservation's commit settle, removed

| | |
|---|---|
| **Prompt** | The operator proposed Oracle's delayed block cleanout for KDS (W0). CLA read the tree at `18264195` and reported three things. No tuple is visited at commit and no reader writes a page to learn one. The assertion reservation's settle (`AssertionEnforcer::CommitTxn`) is the one commit-time step whose page work scales with the writes. And the `kEntryReserved` bit it clears has no reader. CLA offered (a) keep the settle, (b) remove it, (c) give the entry a writer id so the bit can be resolved lazily |
| **Word** | W1: *"Tell me about the assertion reservation settle."* W2: *"Write an exploratory, verifying milestone document on (b)."* |
| **Mark** | **CJ written** (`workorder-cj-assertion-commit-settle.md`), **not opened**. CJ-Q0..Q7 carry CLA's proposals, and none is marked. The independent read of the order found that a stranded reservation fails **open** when it is a departure or a negative `SUM` arrival. Stranded here means kept by a crash loser (no mount path compensates it, §1.8) or by a failed settle (§1.6): the group's total is left too low, and the upper bound admits writes it forbids, `[quiet-wrong]`. Filed as `docs/inflight/bugs/an-assertion-keeps-the-reservations-of-a-crash-loser-and-a-failed-settle.md`, found by reading and not reproduced. CJ removes the failed-settle path. The crash path waits for an order of its own (CJ-Q5) |
| **Does not settle** | CJ's opening; every CJ-Q; a push of anything but documents; a tag, a version |
| **Recorded at** | this file; the order's header; the CJ and 2026-10-31 rows in `index.md`; the bug entry |
