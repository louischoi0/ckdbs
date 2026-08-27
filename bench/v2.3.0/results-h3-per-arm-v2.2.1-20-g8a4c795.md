# H3 at the woken reactor — the falsifier, run, and it does not fire

**Measured 2026-08-27** in worktree `rwc1-h1h2-per-arm`, the same two hashed
binaries and the same per-arm-process shape as
`bench/v2.3.0/results-h1h2-per-arm-v2.2.1-19-gd6b0280.md`:

| arm | commit | `git describe --tags` | what it has |
|---|---|---|---|
| **prewake** | `bce12d0` | `v2.2.1-3-gbce12d0` | neither the wake path nor the park rule |
| **post** | `158d6b5` | `v2.2.1-16-g158d6b5` | the wake path, "parked is not ready", RW5's counters |

**Why this cell and not another.** The H1/H2 file closed by naming what
would falsify its neutral result: *"a host where the three writer cores are
not saturated … that cell is not run here and is the one place this workload
could still show the wake path"*, with H3 named as the nearest published
shape. H3 is `--cores 4 --tables 3`: three relations, three writer cores,
**one session per core** rather than two, published at **1.927** in
`bench/v2.1.0/results-multicore-writers-v2.1.0.md`. This file runs it.

> **Verdict: the falsifier does not fire.** H3 reads **1.865 prewake against
> 1.924 post**, and at ten times the rows **1.857 against 1.876** — a 1.0%
> difference against per-arm spreads of 5–8% and a null cell spanning 3.6%.
> Not separable, in the same direction and for the same reason as H1 and H2.
> **All three published cells this version has been asked about are neutral.**

---

## 1. What was run

```
tools/multicore_benchmark.py --server <arm binary> --cores 4 --tables 3 \
    --rows <2000|20000> --placement rotate --peer-listeners \
    --only <single|multi> --workdir <fresh> --port <n> --json <out>
```

Three sub-cells, arms interleaved `post, prewake` at every point, fresh
server and fresh data file per invocation:

- **`h3`** — H3 as published, 2,000 rows, 5 reps.
- **`null3`** — three pairs of single-only runs at H3's own shape. H3 gets
  its own divisor because the H1/H2 run established that a null cell is a
  per-cell number and never carried between cells.
- **`h3long`** — H3 at 20,000 rows, 3 reps. What H2 is to H1, added because
  H2's 1.2% spread against H1's ±6% was entirely the row count.

Binaries as hashed in the H1/H2 file (`6f2096e3…` post, `d3f5543…`
prewake); ext4 on `/dev/root`, workdirs under `/home/ubuntu/rwc1`.

## 1a. The first attempt was void, and how that was caught

**Stated because it is the reason this file exists at all rather than one
written half an hour earlier.** The first H3 sweep fired its runs back to
back. H3's runs are ~8 seconds each on four cores, so the box's 1-minute
load average never decayed, and once it crossed the driver's own host guard —
`load1 > 0.5 * cores`, `tools/multicore_benchmark.py:120` — **24 of 50
invocations were refused and wrote no JSON**. Every `null3` and every
`h3long` run was in that 24.

The runner reported `exit=0` for all fifty. `echo "$(date …) exit=$?"`
expands `$?` *after* the command substitution, so it captured `date`'s
status and never the driver's. **What caught it was cross-checking the JSON
count against the expected run count**, not the exit codes — a habit worth
keeping, because the failure mode of a refused benchmark is silence.

The re-run gates each invocation on no live `kds_server` and a 1-minute load
below 2.5, captures the exit code into a variable before anything else runs,
and fails loudly if the JSON is absent: **38 runs, 0 failures, 0 settle
timeouts**. Everything below is from that re-run. (The earlier partial
figures — 1.825 against 1.889 — are void and are recorded here only so they
are not quoted from a transcript.)

---

## 2. The result

| sub-cell | arm | ratios per rep | **median** | spread |
|---|---|---|---|---|
| `h3` (2,000 rows) | prewake | 1.928 / 1.939 / 1.762 / 1.865 / 1.784 | **1.865** | 1.762–1.939 |
| `h3` | **post** | 1.924 / 1.965 / 1.914 / 2.018 / 1.805 | **1.924** | 1.805–2.018 |
| `h3long` (20,000 rows) | prewake | 1.871 / 1.784 / 1.857 | **1.857** | 1.784–1.871 |
| `h3long` | **post** | 2.004 / 1.876 / 1.853 | **1.876** | 1.853–2.004 |
| **`null3`** (identical vs identical) | — | 0.991 / 0.955 / 0.970 | **0.970** | 0.955–0.991 |

Throughput behind those ratios, medians in stmt/s:

| sub-cell | arm | single | multi |
|---|---|---|---|
| `h3` | prewake | 1,919 | 3,497 |
| `h3` | post | 1,888 | 3,408 |
| `h3long` | prewake | 1,773 | 3,292 |
| `h3long` | post | 1,767 | 3,416 |

## 3. Judged

**Not separable, and the margin is not close.** At the published row count
the arms differ by 0.059 while each spans ~10% on its own. At ten times the
rows they differ by **0.019 — 1.0%** — against a prewake spread of 4.9%, a
post spread of 8.2%, and a null cell that spans 3.6% on an
identical-versus-identical comparison. Every per-arm range overlaps the
other's across most of its length.

**H3 reproduces on this host**, which is what makes the comparison worth
anything: the published cell is 1.927 (1.886–1.947) and these arms sit at
1.86–1.92. The 1.9× scaling this cell is known for is intact on both
binaries; what the wake path does to it is nothing measurable.

**`h3long` did not tighten the way H2 did, and that is worth saying.** The
row count bought H2 a 1.2% spread; here it buys 4.9% and 8.2%. The
difference is the ratio's own size — H3 scales at 1.87 against H2's 1.05, so
the same relative noise in either arm moves the ratio nearly twice as far —
compounded by three reps against H2's three at four times the wall clock per
run. A cell that needs to resolve 1% at a 1.9 ratio needs more reps than
this run gave it, and that is a statement about what would have to be spent,
not a hedge on the result: **nothing in the data points at a real
difference in either direction.**

## 4. What it means for the wake path

The H1/H2 file predicted this outcome from the mechanism and named the
condition that would break it. The condition is now tested and the
prediction holds:

- The wake pays **only when a core is asleep as a message arrives**. H3
  halves the sessions per writer core relative to H1, but each of the three
  writer cores still runs a five-phase workload back to back against its own
  relation. One session per core is not an idle core; it is a core with a
  continuous serial workload, which is why the cell scales at 1.9× in the
  first place.
- **Nothing ships.** Every statement is seated on its relation's owner, so
  no arrival core parks on a peer's reply — the population RW-B cells 1–4
  measured is absent here as it was there.
- **Latency does not move.** `h3long` insert p50 is 2,400 µs prewake against
  2,309 post on the single arm, 1,183 against 1,150 on the multi arm — post
  a couple of percent ahead in both, inside the same scatter as everything
  else.

So the three published cells this version has been asked about — H1, H2, H3
— are all neutral, and the reason is one sentence: **this workload never
lets a core go idle with a message waiting for it, which is the only
condition the v2.3.0 work changes.** That is consistent with RW-B cell 4,
which measured directly that the wake costs nothing where the target is
never asleep, and with cells 1 and 2, which measured what it is worth when
the target *is*.

**What would still falsify it**, now that H3 has not: a cell with more cores
than relations, so that a writer core has genuinely nothing to do between
statements and its peer's message finds it asleep. No published cell in
`bench/v2.1.0/` has that shape — H4a and H4b both hold relations ≥ writer
cores — so it would be a new cell rather than a re-run, and it is not run
here.

## 5. An observation not chased

The trx-id refill's to-grant wait is **30.0 ms prewake against 38.7 ms post**
on `h3long` (n = 9 each), while on `h3` the same leg reads 36.2 against 37.3
and on H1/H2 it read 32.4 against 33.5 and 33.6 against 33.8. One sub-cell
out of four shows a gap, in the direction of post being *worse*, and the
other three are flat. **Recorded as observed and not attributed** — three
reps cannot separate it from the same scatter everything else in this file
carries, and the leg itself is outside this run's question. What is
consistent across all four sub-cells is that this wait is tens of
milliseconds, where PW7's own cell reads 2.7–3.3 ms; that gap is a property
of this workload's shape, not of either binary, and belongs to whoever next
opens `docs/inflight/in-progress/workplan-peer-writer.md`.

## 6. Gates

- **38 runs, 3,066,152 statements, 0 errors**, `verify: survivors as
  expected` in every driver log, 0 settle timeouts.
- The correctness suite was not executed by this run; it is green at
  `8a4c795`, where the pre-push gate ran it at 2743/2743, and no engine code
  changed between that commit and either binary here.
- Raw driver output — 38 JSON summaries and their logs — is archived beside
  this file in `bench/v2.3.0/archive/h3-per-arm/`. The void first attempt is
  **not** archived: 24 of its runs have no data and the rest were taken on a
  loading box, so keeping it would only invite someone to read it.
