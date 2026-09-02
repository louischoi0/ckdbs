# AI-T3: what the foreign key's crossing costs, and what the gate's lift did not

| | |
|---|---|
| Measured at | `v2.7.0-95-g341e9be` (`git describe --tags`) |
| Pre-lift arm | `v2.7.0-88-g546ddc8`, built from a clean `git archive` of that commit into its own tree |
| Build | `build-release` (Release) both arms, configured with `-DOPENSSL_ROOT_DIR=…` |
| Driver | `bench/fk_crossing_cost_probe.py` (new) |
| Host | 8 cores, AMD EPYC 9V74. No build, suite or second measurement overlapped any run; the three runs are sequential in one script |
| Raw | `bench/v2.8.0/archive/ai-t3-crossing-group.json`, `…-crossing-relaxed.json`, `…-gate-ab-relaxed.json` |
| Tree state | The measured binary is `341e9be`'s tree. The driver changed after that commit (`bench/` only — retry handling, the durability knob, an issued rather than named seed key); nothing under `src/`, `include/` or `tests/` moved between the build and the last run |

**Invocations**, all three from the repository root:

```
bench/fk_crossing_cost_probe.py --mode crossing --rows 400 --warm 40 --blocks 3 --port 15496
bench/fk_crossing_cost_probe.py --mode crossing --rows 400 --warm 40 --blocks 3 --port 15496 \
    --durability relaxed
bench/fk_crossing_cost_probe.py --mode gate-ab --rows 400 --warm 40 --blocks 3 --port 15496 \
    --durability relaxed --server-a <546ddc8>/kds_server --server-b build-release/kds_server
```

Every arm is alternated block by block on one host — never one arm's three
blocks then the other's — so a drift in the machine hits both.

## 1. The shape is proved, not assumed

Both measurements turn on relations landing where the driver says they do,
and on a crossing actually crossing. Neither is inferred:

- **Placement** is read back from `DESCRIBE`. Every run reports
  `pa: 2, spacer: 1, cc: 2, cx: 1` — the parent and the colocated child on
  core 2, the crossing child on core 1, nothing on the system core. A run
  where the colocated pair is not colocated, or the crossing pair does not
  cross, is refused rather than reported.
- **The crossing** is counted through the `SHOW META` block AI-T3 added
  (`fk_probes_sent`, `fk_intents_granted`, `fk_intents_released`,
  `fk_intents_live`), read on the child's owner and the parent's owner at
  every block boundary. Across all six crossing blocks: **400 probe rounds
  per 400 statements**, 400 intents granted, and `fk_intents_live = 0` at
  every boundary. Across all six colocated blocks: **0 probes**. The driver
  fails the run on any other reading.

One round per statement, never per row, is AH-R2 holding at 400 statements
rather than at one; and `fk_intents_live` returning to 0 after every block
is work order AI's F1 fix observed at scale rather than in a unit cell.

## 2. M1 — the peer-writer gate's lift costs nothing, and the numbers say so

`CheckWriteAffinity`'s `funded_shape` lost two `empty()` tests when AI
narrowed its foreign-key arm. Every peer write runs that predicate,
including the very large majority carrying no foreign key, so the arm
under test is a **peer write to a relation with no foreign key at all** —
the population that gained nothing from the lift. `durability = relaxed`,
because that is the arm where a microsecond is visible.

| arm | p0 | p25 | p50 | p90 | p99 | p100 | mean |
|---|---|---|---|---|---|---|---|
| `546ddc8` pre-lift | 83.0 | 92.7 | **95.7** | 106.9 | 172.4 | 6573.2 | 117.8 |
| `341e9be` post-lift | 69.5 | 86.6 | **91.3** | 100.9 | 172.6 | 1747.7 | 105.6 |

µs per statement, 1200 samples per arm (3 blocks × 400).

| arm | per-block p50 | min | max | sd |
|---|---|---|---|---|
| pre-lift | 92.9, 99.6, 94.9 | 92.9 | 99.6 | 2.8 |
| post-lift | 91.3, 86.0, 92.7 | 86.0 | 92.7 | 2.9 |

**Verdict: no cost, and the direction is a coincidence rather than a
finding.** The post-lift arm is 4.3 µs (4.5%) *faster* at p50. Two
`std::vector::empty()` calls are nanoseconds; nothing about the change can
buy 4.3 µs, so what the delta measures is the host. The evidence for that
is in the third table: each arm's own block-to-block spread — 6.7 µs
pre-lift, 6.7 µs post-lift — is larger than the difference between the
arms' pooled medians. **The honest statement is "inside the run's own
noise floor", and it is stated with the floor rather than asserted.**

## 3. M2 — what a crossing costs, twice, because the device answers first

Same statement, same parent relation, same parent **row**; the only
difference is whether the parent lives on the child's core.

### Under the shipped default (`durability = group`)

| shape | p0 | p25 | p50 | p90 | p99 | p100 | mean |
|---|---|---|---|---|---|---|---|
| colocated | 1031.4 | 1154.4 | **1199.3** | 1796.4 | 2558.9 | 12300.0 | 1332.2 |
| crossing | 1104.8 | 1219.2 | **1268.9** | 1946.2 | 2500.4 | 5295.0 | 1400.3 |
| **delta** | | | **+69.5 µs (+5.8%)** | | | | +68.1 µs (+5.1%) |

Per-block p50 sd: 10.2 µs colocated, 3.4 µs crossing.

### With the device out of it (`durability = relaxed`)

| shape | p0 | p25 | p50 | p90 | p99 | p100 | mean |
|---|---|---|---|---|---|---|---|
| colocated | 74.2 | 78.0 | **79.4** | 91.8 | 129.0 | 1700.4 | 96.3 |
| crossing | 119.7 | 131.3 | **136.3** | 163.7 | 1679.2 | 8390.4 | 180.4 |
| **delta** | | | **+56.8 µs (+71.5%)** | | | | +84.1 µs (+87.3%) |

Per-block p50 sd: 0.9 µs colocated, 4.1 µs crossing.

**The two rounds cost about 57 µs, and a user pays 6% of a write for
them.** That is the whole finding: the crossing's cost is a *ring* cost —
one probe round out and back at the dispatch fork, then one decide round
to release the reference intent — and under the class this engine ships
the write is already waiting on a device, so 57 µs of ring lands on top of
1.2 ms of fsync and reads as 5.8%.

**Against this engine's own nearest number**, which is what makes the
figure mean something: XD priced a cross-owner *commit* at **3.077×** a
one-owner commit (`bench/v2.7.0/results-xd-commit-decomposition-*`),
because it added two device syncs. The foreign key's crossing adds **two
ring round trips and no sync at all** — the intent-only participant writes
no `TXN_PREPARE` (`cross-owner-txn.md` §1a) and, since AI's F4, is not
asked to prepare — and that is exactly why it costs 5.8% where the commit
cost 208%. *A round trip counted is not a round trip a user feels, when
the device is in the path.* The `group` run's `fk_probes_sent = 400` per
block is what lets this file say the round happened at all while the
number stays that small.

## 4. The finding nobody asked for: the crossing's tail is not its median

Under `relaxed` the crossing arm's **p99 is 1679 µs against the colocated
arm's 129 µs** — 13× — while its p50 is only 1.7×. `p100` is 8390 µs. The
retry counter is **0** in every block, so this is not a lease refill
surfacing as latency; it is a small population of statements whose probe or
decide round waited far longer than the median one. Nothing here diagnoses
it, and nothing should: it is named because a median that improved while a
tail like this existed would be the wrong thing to celebrate, and because
**AH-T6's leg attribution is the measurement that can say which round it
is.** Recorded as owed to AH-T6 rather than answered here.

## 5. What this file does not measure

- **Which of the two rounds costs what.** The delta prices probe and decide
  together. Splitting them is XD-style leg attribution and belongs to
  AH-T6, which needs the same fixtures for its own matrix.
- **The transaction path.** Every number here is autocommit. Inside an
  explicit transaction the decide rides the commit's own leg instead of
  being the statement's, so the shape is different and is AH-T6's too.
- **`durability = strict`**, and more than one distinct owner per statement
  (AH-R2's cost is a function of that count, and this measures it at one).
- **Concurrency.** One client, one statement at a time. XD's finding that a
  leg's cost *reverses* under load (25.9% at eight coordinators where there
  was nothing to save serially) is the reason to say so out loud rather
  than let a serial number stand for the engine.
