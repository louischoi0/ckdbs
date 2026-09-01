# XG3's owed half: the answer edge under a real process death — 3/3, three passes

| | |
|---|---|
| Measured at | `v2.7.0-86-g6b18f69` (`git describe --tags`) |
| Build | `build-release` (Release), configured with `-DOPENSSL_ROOT_DIR=…` |
| Driver | `bench/shipped_answer_edge_kill_probe.py` (new) |
| Host | 8 cores, 15 GiB; no build overlapped any run |
| Raw | `bench/v2.8.0/archive/xg3-answer-edge-kill-pass3.json` (pass 3; passes 1 and 2 identical verdicts) |
| Tree state | This session's changes at the time of measurement are `bench/` only — the probe and this file. `src/`, `include/` and `tests/` were untouched between the release build and the last pass, and HEAD did not move |

**This repays the debt `workplan-shipped-read-typed.md` §8c names**, and it
is the gate on AH-T5 (operator decision of 2026-09-01, item 4(b): the
shipped-read matrix debt is repaid before new crash surface is added to
the 2PC path).

## 1. What was owed, and what this ran

XG3 placed four crash points on a typed client's shipped read and asserted
three *faults* against them — an out-of-order description chunk, a
description whole only when every chunk arrived, rows with no description
— each refused rather than decoded against a guess. It did not **kill the
process** at those points; §8c's table says "not run" three times.

| point | what the death interrupts | verdict |
|---|---|---|
| `shipped.answer_described_prerows` | the description is across, no row has left | **PASS** ×3 |
| `shipped.answer_batch_sent` | mid-stream, a batch has crossed | **PASS** ×3 |
| `shipped.answer_edge_closed_prereply` | every row and the EOF are away, the terminator is not | **PASS** ×3 |

A cell passes only when all four clauses hold: the crash line is in
stderr (**the point was reached**, not merely that a name compiles), the
process died `rc=-9`, the typed client received **no result** — it raised
rather than returning rows — and the restart mounted with the relation
readable, all 400 rows present, and no in-doubt residue.

**What the kill proves that the fault could not.** Both ends of the wire
are threads of one process, so the death takes the client's socket with
it: the taxonomy claim — *a failure reaches a typed client as a failure
and never as an empty or partial result set* — is tested against a real
death rather than an injected error return. In all nine cell-runs the
client's answer was `ConnectionError: server closed the connection`.
**Never a row count, never an empty result set.**

## 2. What is still not run, and it is not this file's to close

`workplan-shipped-read-typed.md` §8c lists two more that remain owed:
**credit exhaustion at owner death** and **the 10 s deadline on the typed
arm**. Neither is a process-kill cell — the first needs a client that
stops granting credit mid-stream, the second costs ten seconds a run — and
neither is in the gate AH-T5 names. They stay owed and §8c stays their
record.

## 3. Three harness findings, because each one produced a green-or-red
   flip on the same build

These are the reason this file exists rather than a one-line "ran it", and
each was found by a cell disagreeing with itself across runs.

1. **A session on the owner's core does not ship.** `peer_listeners = on`
   accepts on every core through `SO_REUSEPORT`, so a typed session lands
   wherever the kernel puts it — and one landing on the relation's owner
   executes the statement **locally**: nothing ships, no answer edge
   opens, the armed point is never reached. That is not flakiness, it is
   the cell silently not posing its question. The driver now picks a
   session off the owner core (`kwp_session_off`).
2. **`SHOW META` answers in the tag, not in rows.** The first draft of
   that picker read `rows` and found nothing, so every candidate looked
   wrong and the search exhausted itself.
3. **One port per cell, three phases, and `stop_proc` returning is not the
   port being free.** The armed instance could be started while the
   previous listener was winding down; `wait_up` then connected to the
   *dying* one, the read was answered by nobody, and `proc.wait()` timed
   out with "did not die at its point" while the crash point was never
   reached. `wait_port_free` closes it.

**Two more, about the engine rather than the harness**, both of which cost
a red run and both of which are true of any driver:

- **A first insert into a peer-owned relation meets a retryable refusal**
  — *"row-id lease for relation oid N is spent; retry after the refill
  grant lands"* — and a driver that treats a retryable as a failure scores
  a setup error. `txn_2pc_kill_matrix_probe` already retries; this one now
  does too.
- **The text arm's 992-byte reply cap is not the typed arm's** (XG1 §4a).
  Reading 400 rows back through the debug text port after the restart
  scored every cell "not readable" on a cap that has nothing to do with
  the crash. The read-back is a typed client now, the same surface the
  killed read used.

## 4. Verdict

**XG3's process-kill half: repaid, 3/3 across three passes.** The
`shipped-read matrix debt` the AH-T5 gate names is discharged for the kill
cells; the credit-exhaustion and 10 s-deadline cells were never part of
that gate and remain owed in §8c.
