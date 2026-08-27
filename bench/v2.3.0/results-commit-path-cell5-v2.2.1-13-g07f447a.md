# RW-B cell 5 — the commit path, which D5 said this version could break

**Measured 2026-08-26/27** in worktree `v2.3.0-reactor-wake`, all arms on the
same 8-CPU host (`/dev/root`, ext4 — the data files are under `$HOME`, never
tmpfs), every server started from a **copied, hashed** binary per
`bench/docs/README.md`:

| arm | commit | what it has |
|---|---|---|
| **prewake** | `bce12d0` (`v2.2.1-3-gbce12d0`) | neither the wake path nor the park rule |
| **pre** | `8048b5d` (`v2.2.1-11-g8048b5d`) | the wake path; the spin intact |
| **post** | `07f447a` (`v2.2.1-13-g07f447a`) | the wake path **and** "parked is not ready" (RW3) |

Order: `instructions/v2.3.0-reactor-wake.md` §5 cell 5, which names two
cells — *PW6's four-writer cell* and *the point-SELECT beside a committing
session* — and requires that "**neither may regress at any drain
interval**". Both are here (§2, §3), plus a third the order does not ask for
and D5 does (§1).

**Verdict: neither regresses.** RW3 is neutral to slightly favourable on
every stable statistic measured, and the one cell that moves consistently
moves the right way.

---

## 1. The D5 hazard, tested directly (an addition, not one of the two)

D5 said a rule that let the reactor block between a commit's staging and the
post-task hook's sync would put the WAL drain interval on **every commit**.
The signature would be unmistakable — seated commit p50 tracking
`wal_drain_interval_us`, the same functional dependence SS-B §4a used to
convict the idle block of shipping's millisecond.

**Measured** — `bench/single_relation_probe.py --seat owner`, one session,
1,500 rows, three drain intervals, pre against post, interleaved, 3 reps:

| `wal_drain_interval_us` | pre p50 | post p50 | pre ips | post ips | post/pre ips |
|---|---|---|---|---|---|
| 1000 | 715.8 µs | 694.2 µs | 1,374.2 | 1,398.7 | 1.018 |
| 2000 | 808.5 µs | 731.9 µs | 1,235.6 | 1,349.8 | 1.092 |
| 5000 | 858.2 µs | 827.0 µs | 1,139.8 | 1,196.1 | 1.049 |

**The commit p50 does not track the knob on either arm** — 716 → 858 µs
across a **5×** change, a 20% drift, against the 1:1 tracking (1.08 / 2.10 /
3.11 / 5.12 ms) that convicted the idle block. Every post p50 is *lower*
than its pre. **D5's hazard did not materialize**, and the post-task hook's
`bool` answer does in the engine what
`TheHooksWorkIsProgressSoACommitDoesNotWaitOutABlock` says it does in a unit
test.

## 2. PW6's four-writer cell

**Measured** — `bench/run_pw6.py --cell C-rotate-t4`, which is PW6's own
shape and pins its multi-core config at `cores = 2` with four relations and
four writer sessions, 2,000 rows, each invocation carrying its own
single-core control. Three reps per arm, interleaved:

| arm | multi ÷ single, per rep | median | multi-core insert p50, per rep |
|---|---|---|---|
| pre | 1.054 / 1.110 / 1.081 | **1.081** | 1,489 / 1,515 / 1,506 µs |
| post | 1.098 / 1.052 / 1.156 | **1.098** | **1,385 / 1,433 / 1,377 µs** |

**No regression.** The ratios overlap and post's median is the higher of the
two. The insert latency is the firmer signal: **every post rep is faster
than every pre rep** (post max 1,433 < pre min 1,489), a consistent ~7%
improvement in the four-writer cell's own write path. Rows in = rows out and
`err=0` in every phase of every run.

Both arms sit above PW6's published **1.030×**, which was measured on the
2-CPU host that refused `cores = 3`; the comparison across hosts is context,
not a control, and the control is the interleaved arm beside it.

## 3. The point-SELECT beside a committing session

This is the cell the order names as *1,088/1,083 µs against 37/35 alone*, and
it needed a script first: PW6 measured it by hand and left none, which is
why it could not be re-checked when the idle policy changed underneath it.
`bench/reader_beside_committer_probe.py` is that script — three windows on
one server (reader alone, reader beside a committer, committer alone), both
sessions seated on the relation's owner so nothing is shipped.

**Measured** — three arms, **seven reps each**, 800 INSERTs in the busy
window, interleaved:

| arm | reader alone p50 | reader beside, p95 | p99 | committer p50 | committer p99 |
|---|---|---|---|---|---|
| prewake | 23.9 µs | 796.5 | 894.2 | 687.0 | 998.8 |
| pre | 23.9 µs | 696.6 | 830.3 | 689.4 | 976.2 |
| post | 23.8 µs | 747.7 | 870.3 | 672.7 | 969.4 |

**No arm separates on anything stable.** The reader's floor is identical to
a tenth of a microsecond; the stall's magnitude in the tail is within the
same band for all three, prewake's being the *largest*.

**But the median is bimodal, and that is the finding.** Per-rep
reader-beside-committer p50:

| arm | seven reps (µs) | reps in the blocked mode |
|---|---|---|
| prewake | 687.7, 23.8, 464.0, 641.6, 28.0, 26.3, 688.3 | 4/7 |
| pre | 26.7, 47.4, 20.0, 28.2, 698.7, 26.5, 17.8 | 1/7 |
| post | 43.1, 44.2, 502.4, 44.6, 696.3, 26.7, 40.6 | 2/7 |

Every rep lands in one of two modes — the reader mostly free (~20–45 µs) or
mostly behind the sync (~460–700 µs) — with nothing in between. Four-of-seven
against one-of-seven is **not** separable at this n, and reporting a
median-of-seven as the arm's number would have manufactured a 20× difference
out of a coin flip. (The first three-rep pass did exactly that in both
directions on successive days.)

**So PW6's 29× median stall does not reproduce here as a stable property.**
What reproduces is the stall itself, in the tail: a p95/p99 of 700–900 µs —
one commit's worth of wait — in **every arm, including the one with no wake
path at all**. The reader is waiting for a reactor that is inside
`fdatasync`, exactly as PW6 said; what has changed since is how often a
given run's reads land inside those windows.

**Corroborated independently by §2's cell**, which measures a point-SELECT
phase beside four committing writers with a different driver: p50 42–47 µs
with p95 swinging between 87 µs and 1,123 µs across reps, on both arms. Two
unrelated instruments, the same bimodality.

**One limitation, named because it is the leading suspect.** The probe runs
its reader and its committer as two threads in **one Python process**, so
the driver's own GIL scheduling can decide whether the reader's round trips
interleave with the committer's. A mode that flips per run and not per arm
is what that would look like. Settling it means one process per session, and
that is what the next revision of this probe owes.

## 4. What this cell answers, and what it does not

- **Answers**: RW3 does not regress the commit path — not at any drain
  interval measured (§1), not in PW6's four-writer shape (§2), and not in
  the reader-beside-committer shape (§3). The order's bar for cell 5 is met.
- **Answers, unasked**: the four-writer cell's insert p50 improves ~7% under
  RW3, consistently across every rep.
- **Does not answer**: whether the wake path improved the reader cell. §5
  invited that finding — *"if the wake improves the second one, say so with
  the number"* — and the number is not available at this n, because the
  statistic it would be made of is bimodal. Stated as unresolved rather than
  reported in whichever direction three reps happened to fall.

## 5. Gates and provenance

Binaries, sha256 of the copies actually run:

```
d3f5543...  kds_server-prewake-bce12d0
3e7a5d4...  kds_server-pre-8048b5d
7fcdbaf...  kds_server-post-07f447a
```

Raw driver output for all three sub-cells is archived beside this file in
`bench/v2.3.0/archive/cell5-commit-path/`, including the PW6 runner's own
log with the per-config latency tables the ratios were computed from.

**On the null cell.** §5 asks every A/B to be divided by one. None was run:
SS-B finding 10 established that the ~1.099 ordering bias was
`tools/multicore_benchmark.py`'s two-configurations-in-one-process shape and
that a per-arm process does not carry it (two null cells at 0.991 and
1.016), and every arm here is a separate process with a fresh server and a
fresh data file. That is an argument standing in for a measurement, and it
is recorded as such rather than as a cell that passed.
