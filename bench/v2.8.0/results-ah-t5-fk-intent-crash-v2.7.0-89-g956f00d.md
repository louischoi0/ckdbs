# AH-T5: the reference intent's crash window — 3/3, three passes, and the gate that had to move first

| | |
|---|---|
| Measured at | `v2.7.0-89-g956f00d` (`git describe --tags`) |
| Build | `build-release` (Release), configured with `-DOPENSSL_ROOT_DIR=…`; `kds_server` relinked from this commit's tree |
| Driver | `bench/fk_intent_crash_probe.py` (written at AH-T5, run here for the first time) |
| Host | 8 cores, AMD EPYC 9V74; no build overlapped any pass |
| Raw | `bench/v2.8.0/archive/ah-t5-fk-intent-crash-passes.json` (all three passes) |
| Tree state | The measured binary is this commit's tree. The commit's `src/`/`tests/` change is the peer-writer FK arm and its converse cell; nothing else moved between the release build and the last pass |

**Invocation:** `python3 bench/fk_intent_crash_probe.py --json …`
(defaults: `--cores 3 --port 15494 --passes 3 --build-dir build-release`).

## 1. What was owed

`foreign-keys.md` §2a states AH-R5 as a testable claim rather than an
argument:

> a participant that restarts after granting an intent and before its
> prepare leg forces the coordinator's transaction to fail

A reference intent is memory-resident **by design** — under
`cross-owner-txn.md` §1a an intent-only participant writes no
`TXN_PREPARE` record, so there is nothing for an intent to ride in. A
window in which the coordinator can still commit is therefore a defect of
AH, not a documented limitation. AH-T5 is the cell that tries to open one.

## 2. The gate that had to move first, and what it cost

**The probe was written on 2026-09-01 and could not arm itself.** Its
setup — `INSERT INTO parent VALUES (7)` against a peer-owned relation —
was refused:

```
ERR NOT_IMPLEMENTED retryable=0 an FK-linked relation cannot take writes
on core 2: validation reads the linked relation, which this core may not
fault (workplan-peer-writer.md §4)
```

That is `CheckWriteAffinity`'s peer-writer funding gate: `funded_shape`
required both fkey lists empty, so **a relation carrying a foreign key in
either direction took no write on any core but 0** — and with no peer
write there is no forward check, no park, no probe. The crossing AH built
was unreachable in a running instance.

Its stated reason is the one §2a removed. The operator directed the
narrowing on 2026-09-01; it landed at `956f00d`, this file's commit, with
the FK arm struck and the cabined and `CannotEnforce` arms untouched.
**The measurement is what proves the narrowing was the whole gate**: the
same probe, unchanged, goes from `ERROR: seed parent` on every pass to
`PASS` on every pass.

| | before the narrowing (`v2.7.0-88-g546ddc8` binary) | after (`v2.7.0-89-g956f00d`) |
|---|---|---|
| pass 1 | `ERROR: seed parent: ERR NOT_IMPLEMENTED …` | **PASS** |
| pass 2 | `ERROR: seed parent: ERR NOT_IMPLEMENTED …` | **PASS** |
| pass 3 | `ERROR: seed parent: ERR NOT_IMPLEMENTED …` | **PASS** |

## 3. The cell, and the four clauses it passes on

Placement is the cell's first requirement and it is checked, not assumed:
the probe reads `owner_core` back from `DESCRIBE` and calls the run
**vacuous** unless parent and child landed on two different peer cores.
All three passes report `parent: 2, child: 1` — a real crossing, with core
0 holding neither relation.

The armed write runs **inside an explicit transaction**, so the parent's
owner is *enrolled* as a participant rather than merely asked: that is
what makes the window a 2PC window instead of an autocommit statement's.

| clause | what it rules out | all three passes |
|---|---|---|
| `crash_line` in stderr | a point that compiles but is never reached | `true` |
| `rc == -9` | an orderly shutdown wearing a crash's name | `-9` |
| no `tag='ahT5'` child row after restart | **the defect: a coordinator that committed anyway** | 0 rows |
| `txn_in_doubt_unresolved` clean | a transaction nothing at runtime can finish | absent |

The client's own answer in every pass was `ConnectionError: server closed
the connection` — never a reply, never a row count.

**On the fourth clause, because absence is easy to misread as a skipped
check.** `SHOW META` prints the in-doubt block *only where something has
been in doubt* (`command_dispatcher.cpp`, the "absent rather than zeroed"
rule the recovery block follows). After the restart the block is absent,
which is the zero: nothing on the restarted instance is holding rows for a
transaction it cannot decide. The probe accepts absent and `0` alike, and
this is why.

## 4. What this does not prove, stated before anyone quotes it

**Both cores are threads of one process, so the death takes the
coordinator with the participant.** This cell cannot stage a participant
dying *under a surviving coordinator*, which is the shape AH-R5's sentence
literally describes. What it does falsify — and the falsifiable half is
the point — is that **no child row survives** a transaction whose
participant lost its enrolment mid-flight. The surviving-coordinator half
needs two processes and stays **owed**, and it is owed in the same fixture
AH-T6 already needs for the end-to-end park cell.

Nor is this a latency number: H-AH1 and H-AH2 (the probe round's cost, and
the claim that FK cost is a function of distinct owners rather than row
count) are AH-T6's matrix and are **not run** here.

## 5. Verdict

**AH-R5's invariant held, three passes out of three, at the one window
that could break it on a single process.** The AH-T5 gate is discharged
for the half a single process can reach; the other half moves to AH-T6
beside the end-to-end cell, and neither is closed by this file.
