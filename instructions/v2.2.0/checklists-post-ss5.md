# v2.2.0 verification order — statement shipping, post-SS5

Drafted 2026-08-26, assuming SS1–SS5 landed on the `v2.2.0` line. Companion
to `v2.2.0-work-order` rev 2 (which specified the build) and to
`docs/memo-shipping-and-group-commit.md` (which stated what the build must
be checked against). This order specifies **acceptance**: what must be true
before the tag is minted and before any shipping number is quoted.

Discipline unchanged: every claim **measured** (with its invocation) or
**source-read** (`path:line` + commit); `build-release` for numbers; every
A/B divided by a null cell (pretasks §7a: the harness carries ~10% ordering
bias); results named by `git describe --tags`. Verification tasks decide no
constant and no policy — findings are reported, decisions are the operator's.

**Order is not negotiable: Part A before Part B.** Shipping created the most
dangerous surface in this engine — remote execution, retry, and commit on a
foreign core. A throughput number taken over a correctness defect is
worthless, and two of the pretask run's findings (§8d-1, §8d-2) were found
*by* driving the shipped path, not by reading it.

---

## Part A — Correctness. Every item blocking.

### A1. Outcome integrity under adversarial delivery (D4)

The highest-risk surface in the version. Engine-issued pks make a blind
retry a double-insert, so the dedup record is load-bearing and must be
attacked, not merely exercised.

- **Duplicate delivery**: the same (session, sequence) arriving twice must
  answer the recorded outcome, never re-execute. Verify by row count, not
  by return status alone.
- **Reply lost after commit**: kill the reply path (drop the message, or
  −9 the arrival core) after the owner has committed. The retry must find
  the record. The row must exist exactly once.
- **Dedup record evicted**: force the bounded record past its bound, then
  retry a statement whose entry is gone. The arrival core must surface
  **unknown outcome** as a distinct non-retryable status — not a guess,
  not a silent re-execute, not a generic error the client will retry.
- **Session id reuse after reconnect**: a client disconnects and
  reconnects; if session ids can repeat, a stale sequence from the prior
  session must not match the new one. This is the quietest way the scheme
  breaks; construct it deliberately.

Exit: each case has a test in the suite, each asserting the row count.

### A2. Client disconnect while a statement is in flight

The owner commits; nobody is listening. Verify no row is lost or
duplicated, no waiter leaks (count parked waiters before and after), and
the arrival core reclaims its slot. Then verify the same for **timeout**:
a deadline expiry must be distinguishable by the client from a refusal,
and must not leave the owner executing a statement whose waiter is gone.

Exit: tests merged; the resulting semantics stated in
`docs/client-manual.md` — a client that disconnects mid-statement may find
the statement applied, and that is the documented contract.

### A3. Ring saturation and backpressure

**The item rev 2 did not carry, and the one most likely to bite.** Shipped
DDL was rare; shipped DML is the high-volume path, so the rings will fill
in normal operation rather than in a corner case. G2's lesson was that a
*conforming* retry loop can destroy an instance in 30 seconds.

Under sustained shipped load at ring capacity, verify:
- no message is dropped (the B2 class — silent loss is the worst outcome);
- no spin (the §8a `polls`/`polled_us` block is the instrument: polls
  climbing while polled stays flat);
- a full ring answers retryably **only if retry can actually succeed** —
  an honest non-retryable status is better than a loop that cannot win;
- nothing allocates on the refusal path (D5), audited on every refusal
  site the shipped path can reach, and proven by the G2 storm adapted to
  DML: map delta = 0.

Exit: a saturation test in the suite; the storm test's DML form merged.

### A4. Per-session statement ordering

A session issuing S1 then S2 to the same owner must execute them in that
order; the visible failure is an `INSERT` followed by a `SELECT` that does
not see its own write. The ring is per-edge FIFO, but retry paths have
historically been where that ordering is lost — verify explicitly rather
than inheriting the assumption.

Exit: a read-your-own-write test across the ship boundary, including one
where the first statement is retried.

### A5. Shape gate preservation

Every refusal that existed before shipping must survive it: FK-linked,
cabined, assertion-covered relations, explicit-transaction statements,
multi-relation statements spanning owners. Shipping must not become a
path that routes *around* a gate. Audit the fork site (SS2) and assert
each refusal keeps its exact spelling and wire bit.

Exit: one test per gate class, asserting the refusal, not just an error.

### A6. The G1 defect class at DML volume

The permanent unwritability (`ERR page id not found`, non-retryable)
appeared after ~58 shipped **DDL** builds. DML runs the same wiring
thousands of times more often. Run sustained shipped DML churn at
`cores = 4` and verify the relation remains writable throughout and after
restart.

Exit: a sustained-churn cell with a stated iteration count, clean.

### A7. Recovery and the single-core floor

- Kill −9 mid-burst on **both** sides of the wire; restart; every
  relation's count = accepted acks; no shipped statement half-alive. A
  shipped statement's redo lives wholly in the owner's stream — prove it
  rather than assert it (SS3's claim).
- `cores = 1` short-circuits before the fork (SS2): the single-core
  benchmark must not move. Guideline 2's no-regression test, run against
  the pre-shipping tag.

Exit: both green; the `cores = 1` delta reported as a number, not a
judgement.

---

## Part B — The memo's trial. Only after Part A is green.

`docs/memo-shipping-and-group-commit.md` §8 states three claims for exactly
this purpose. Each cell below marks one right or wrong; state misses
plainly, since the memo exists to be checked against.

| Cell | Judges | Shape |
|---|---|---|
| B1 | **Claim 2** (negative at ≤1 session/owner) and D6's price | One session per owner, shipped from a foreign arrival core, against the same session seated on the owner. The predicted small loss becomes the number `crosscore.md` §9's routing decision inherits. Note the honest "before" in R1 is a *refusal*, not a slower statement |
| B2 | **Claims 1 and 3** | S = 2, 4, 8, 14 foreign-arrival sessions targeting one owner's relation. Prediction: throughput tracks T1b's ≈ 590 × S, latency near two syncs, ceiling is execution not sync. Compare against the same S seated locally — that gap **is** the wire + waiter cost at scale |
| B3 | **Falsifier 3** — and the stride question | Every shipped INSERT in B2 lands on one btree's ascending tail; commits batch, the tail page does not. If the curve departs from ≈ 590 × S, isolate the per-page component before naming a cause: same S over 1, 2, 4 relations on the same owner. **This cell, not this version, decides whether stride returns** |
| B4 | **Falsifier 2** — the one open falsifier | K concurrent shipped statements are K parked waiters per arrival core: the population T4 could not fake. §8a's `polls`/`polled_us` on arrival cores at K = 1, 4, 16. The spin signature appears or it does not; either answer closes pretasks §8c honestly |
| B5 | **Demand conversion** | `refusal_baseline_probe.py` unchanged: 80–92% refused must become ~0 shipped-and-executed for autocommit, with `cross_core_write_refusals` flat at zero in the same run — the two-era counter reading its second era |

Every cell reports rows in = rows out per relation. A cell that loses a row
reports a blocking finding and stops, regardless of its throughput number.

---

## Part C — Closing the version

1. **Mint `v2.2.0`** and re-name the results file by it. Shipping is an
   architectural fork; every future comparison will baseline here, and the
   current interim names (`v2.1.0-10-g82a2749`) should not be what the
   baseline is remembered by.
2. **`docs/known-gaps.md`**: the 80–92% entry closes **with its number**;
   the R6 residue entry stays open and points at the counter, now reading
   its second era.
3. **The memo gains a pointer** to the results file that judged its three
   claims, with each marked upheld or missed.
4. **`crosscore.md` §9** receives B1's number as input to the routing
   decision — as input, not as a decision.
5. **Findings that belong to other owners** are recorded and handed over,
   not fixed here: §8b's 94–98% unaccounted reactor time (`docs/sched.md`
   §4), and anything B3 surfaces about tail-page serialization (the stride
   file).

---

## What this order deliberately does not do

Decide routing policy; start explicit-transaction shipping or any prepare
record (R6); reopen stride (B3 reports, the operator decides); change the
scheduler; or touch the free-map track, which proceeds on its own worktree
under its own discipline.

If a check cannot be performed as specified, report what blocked it rather
than substituting a different shape.
