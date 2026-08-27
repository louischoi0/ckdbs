# H1 and H2 at the woken reactor — and the first per-arm-process reading of them

**Measured 2026-08-27** in worktree `rwc1-h1h2-per-arm`, an interleaved A/B of
two binaries run alternately in one sitting on the same 8-CPU host:

| arm | commit | `git describe --tags` | what it has |
|---|---|---|---|
| **prewake** | `bce12d0` | `v2.2.1-3-gbce12d0` | neither the wake path nor the park rule |
| **post** | `158d6b5` | `v2.2.1-16-g158d6b5` | the wake path, "parked is not ready", and RW5's counters |

Not part of `instructions/v2.3.0-reactor-wake.md` — that order's RW-B cells
are 1–6 and all six are filed. This is an **operator-requested addition**:
does the v2.3.0 reactor work move `bench/v2.1.0/results-multicore-writers-v2.1.0.md`'s
H1 and H2, the multi-relation multi-writer scaling cells that no RW-B cell
touched? Run per-arm-process at the operator's choice, so the ratio is
unbiased in the ordering sense and **not** directly comparable with the
published 1.051 and 1.024.

> **Verdict: the wake path is neutral on this workload, and H2 says so
> cleanly.** H2 reads **1.055 prewake against 1.050 post** with per-arm
> spreads of 1.2–1.3% — a difference of −0.005, five times smaller than
> either arm's own spread. **H1 cannot answer**: its arms differ by 0.025
> against a null cell whose spread is ±6%. Insert p50 is identical between
> the arms to within 3 µs at every point.

---

## 1. What was run, and what "per-arm processes" changed

H1 is `--cores 4 --tables 6 --rows 2000 --placement rotate
--peer-listeners`: six relations, six writer sessions dealt over **cores
[1, 2, 3]**, a five-phase workload (insert, point-select, update, delete,
scan) against a `cores = 1` baseline running the same work on core 0. H2 is
H1 at `--rows 20000`. The reported number is multi-core ÷ single-core
throughput.

Every published cell in `bench/v2.1.0/` was taken with
`tools/multicore_benchmark.py` running **both configurations in one
process** — single first, multi second. SS-B finding 10 measured what that
shape costs: a `cores = 1` against `cores = 1` null cell returns **1.099**,
because the second arm always runs later. This run uses the `--only` flag
added for it (`059ea98`), one configuration per process, the ratio computed
outside:

```
tools/multicore_benchmark.py --server <arm binary> --cores 4 --tables 6 \
    --rows <2000|20000> --placement rotate --peer-listeners \
    --only <single|multi> --workdir <fresh> --port <n> --json <out>
```

Order within every rep is `single-post, single-prewake, multi-post,
multi-prewake`, so each arm's numerator and denominator sit exactly **two
slots apart** and inherit the same drift. H1 5 reps, H2 3 reps, plus a
5-pair null cell. Fresh server and fresh data file per invocation.

**Provenance.** sha256 of the copies actually run:

```
6f2096e3706789358373b018a730c4da8c96655a12ee9f8d45a2b58f47ec50cf  kds_server-post-158d6b5
d3f55437cc166a4d70a2e8a35d4532c73acbe7bda8e7bdf1f4e3923c94f69205  kds_server-prewake-bce12d0
```

The post hash differs from RW-B cells 2 and 4's `7fcdbaf…` because RW5's
counters landed between `13c6d4d` and `158d6b5`. Filesystem **ext4 on
`/dev/root`**, workdirs under `/home/ubuntu/rwc1`.

---

## 2. H2 — the cell that can answer

420,007 statements per run, ~116–123 s of wall each.

| arm | config | throughput, per rep (stmt/s) | median | insert p50, per rep |
|---|---|---|---|---|
| prewake | single | 3,453.8 / 3,422.7 / 3,433.0 | 3,433 | 2,312 / 2,352 / 2,324 µs |
| prewake | multi | 3,630.0 / 3,637.9 / 3,621.1 | 3,630 | 2,170 / 2,176 / 2,177 |
| **post** | single | 3,446.6 / 3,422.9 / 3,450.5 | **3,447** | 2,324 / 2,359 / 2,327 |
| **post** | multi | 3,619.4 / 3,628.9 / 3,613.2 | **3,619** | 2,174 / 2,177 / 2,173 |

| | ratios per rep | **median** | spread |
|---|---|---|---|
| H2 prewake | 1.051 / 1.063 / 1.055 | **1.055** | 1.051–1.063 |
| H2 post | 1.050 / 1.060 / 1.047 | **1.050** | 1.047–1.060 |

**The arms are indistinguishable, and this cell is precise enough for that
to mean something.** Each arm's three ratios span 1.2–1.3%; the medians
differ by 0.5%, less than half of one arm's own spread, and the ranges
overlap across nearly their whole length. The four throughput columns tell
the same story directly — post's single arm is 0.4% *faster* than prewake's
and its multi arm 0.3% slower, both inside the reps' own scatter.

**Latency does not move either.** Insert p50 is 2,324 against 2,327 µs
single and 2,176 against 2,174 µs multi — a 3 µs difference on a 2.3 ms
statement, which is the clearest statement in the file that nothing on this
path changed.

## 3. H1 — cannot answer, and the null cell is why

| | ratios per rep | median | spread |
|---|---|---|---|
| H1 prewake | 1.043 / 1.018 / 1.032 / 0.991 / 1.050 | 1.032 | 0.991–1.050 |
| H1 post | 1.159 / 1.124 / 1.050 / 1.024 / 1.057 | 1.057 | 1.024–1.159 |
| **null cell** (two single-only processes, same engine, same rep) | 1.072 / 0.978 / 0.952 / 0.979 / 0.993 | **0.979** | **0.952–1.072** |

The arms differ by **0.025**. The null cell — the same binary running the
same configuration twice, in two processes — spans **0.952 to 1.072**. The
instrument's error on an identical-versus-identical comparison is ±6%, and
the effect being looked for is 2.4% of it. **Not separable**, and no amount
of reading the medians changes that.

**The post arm's ratios fall monotonically across the sweep** — 1.159,
1.124, 1.050, 1.024, 1.057 — and its `cores = 1` baseline climbs at the same
time (3,293 → 3,411 → 3,553 → 3,620 → 3,529 stmt/s). The box warms; later
runs are faster; the first rep of a cold sweep is its highest ratio. Anyone
quoting a two-rep H1 number is quoting that warming, which is exactly what
happened once in the course of this run before the reps completed.

**H2 is tight where H1 is not because of its row count, not its shape.**
Ten times the rows makes each run a 2-minute average instead of a 12-second
one, and the per-statement fixed costs and the warm-up both amortise away.
That was already visible in `bench/v2.1.0/`, where H2 was called *"the
tightest cell in the matrix"* at a 1.1% spread; this run reproduces that
property at 1.2–1.3% while H1 stays loose.

## 4. Why neutral is the expected answer, stated so it is falsifiable

The wake path pays **only when a core is asleep as a message arrives for
it**, and the park rule pays only when a core's queue holds nothing but
parked tasks. This workload arranges for neither:

- **Every statement is seated on its relation's owner.** Nothing ships, so
  no arrival core parks on a peer's reply — the population RW-B cells 1–4
  measured does not exist here.
- **Every writer core is saturated.** Six sessions across three cores drive
  a five-phase workload back to back; a reactor with work to run passes
  `timeout_ms == 0` and never reaches the block the wake exists to end.
- **The only cross-core traffic is lease refills**, and their wait did not
  move: trx-id to-grant `wait_max` reads **32.4 ms prewake against 33.5 ms
  post** on H1 and **33.6 against 33.8** on H2. Whatever that 33 ms is, it
  is not the idle block, and it is the same on both arms. Recorded as
  observed and **not chased** — it is outside this run's question, and PW7's
  own cell reads 2.7–3.3 ms for the same leg under a different shape.

So this cell is consistent with RW-B rather than in tension with it: cell 1
and cell 2 measured what the wake is worth when the target **is** idle
(0.416 → 0.989, and a shipped p50 that stops tracking `wal_drain_interval_us`
over a 50× range), cell 4 measured that it costs nothing where the target is
**never** idle, and this cell is a workload of the second kind. A result
other than neutral here would have contradicted cell 4.

**What would falsify the reading**: a host where the three writer cores are
*not* saturated — fewer sessions per core, or more cores than relations — so
that a peer goes idle between statements. That cell is not run here and is
the one place this workload could still show the wake path.

## 5. On comparability with the published H1 and H2

**These numbers must not be quoted against the published 1.051 and 1.024.**
Those were taken in-process, carrying the ordering bias SS-B measured at up
to 1.099; these are taken per-arm-process, whose own null cell reads 0.979.
The two shapes answer the same question with different instruments.

One observation, without a mechanism attached: the correction is **not a
single factor**. Published H1 1.051 sits above this run's 1.032 prewake,
while published H2 1.024 sits *below* this run's 1.055 prewake. If the
in-process bias were one multiplicative constant, both would move the same
way. They do not, so "divide by the null cell" is a per-cell operation and
never a number carried between cells — which is what SS-B prescribed and
what this run's own null cell exists to supply.

## 6. Gates

- **42 runs, 6,300,294 statements, 0 errors**, and
  `verify: survivors as expected` in every one of the 42 driver logs.
- **The correctness suite was not executed by this run.** It is green at
  `1a305ab` — 2743/2743, `bench/v2.3.0/results-correctness-cell6-v2.2.1-15-g1a305ab.md` —
  and no engine code changed between that commit and the post binary here.
- The only code this run touched is the driver (`--only`, `--json`,
  `059ea98`); `--only both` remains the default, so every existing
  invocation and every published cell is unaffected.
- Raw driver output — 42 JSON summaries and their logs — is archived beside
  this file in `bench/v2.3.0/archive/h1h2-per-arm/`.

## 7. What this does not answer

- **H3, H4a, H4b, C1, C2, P7.** Only H1 and H2 were asked for and only H1 and
  H2 were run. H3 (3 relations, 1 session per core, published 1.927) is the
  cell where the writer cores are least saturated and is therefore the most
  likely of the six to show something.
- **The 33 ms trx-id refill wait** seen on both arms (§4).
- **Anything about shipping**, which this workload does not do.
