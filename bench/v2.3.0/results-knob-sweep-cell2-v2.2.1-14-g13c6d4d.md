# RW-B cell 2 — the knob sweep, and what is left after the block is gone

**Measured 2026-08-27** in worktree `v2.3.0-rwc1`, an interleaved A/B of two
binaries run alternately in one sitting on the same 8-CPU host:

| arm | commit | `git describe --tags` | what it has |
|---|---|---|---|
| **prewake** | `bce12d0` | `v2.2.1-3-gbce12d0` | neither the wake path nor the park rule |
| **post** | `13c6d4d` | `v2.2.1-14-g13c6d4d` | the wake path (RW1–RW2) **and** "parked is not ready" (RW3) |

The version is `2.3.0` by the operator's naming; **no `v2.3.0` tag exists**,
so both arms name themselves off the `v2.2.1` line, per the rule that
nothing is back-filled.

Order: `instructions/v2.3.0-reactor-wake.md` §5 cell 2 — claim 1's
**falsifier**, and RW6's gate. It answers one question: *does the shipped
statement's p50 stop depending on `wal_drain_interval_us`?*

> **Verdict: it stops, completely.** Over a **50×** range of the knob the
> shipped p50 moves by 1.1 µs. The prewake arm on the same box tracks the
> knob 1:1 and reproduces SS-B to three figures. **RW6 does not open**: the
> residual over a seated statement is **20.0 µs**, which is the wire, two
> orders of magnitude below the 1 ms rounding floor D6 would have gone after.

---

## 1. What was run

```
bench/single_relation_probe.py --server <arm binary> --workdir <fresh> \
    --arm multi --cores 4 --sessions 1 --rows <5000|2000|600> \
    --seat <owner|foreign> [--arrival-core -1] --durability relaxed \
    --wal-drain-interval-us <1000|2000|3000|5000> --json <out>
```

Shape copied from `bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md`
§4a so the two tables can be read against each other: **`relaxed`**
durability (the device sync out of both arms, which is what makes the seated
control a flat 23 µs), S = 1, one arrival core (`--arrival-core -1`, the
lowest peer that does not own the relation), 3 reps per point, **both arms
at every point**, fresh server and fresh data file per invocation. Rows per
rep follow SS-B's own rule — 5,000 on the post arm and at `d = 1000` on the
prewake one, 600 where a rep would otherwise take minutes.

Run order interleaves the binaries at every point
(`post, prewake, post, prewake, …` within each knob within each rep), which
is the only defence against a box that moves between sittings — the lesson
cell 3 paid for.

**Provenance.** `sha256sum` of the copies actually run, per
`bench/docs/README.md`'s rule that a measurement never starts
`build-release/kds_server` itself:

```
7fcdbaf29cfd86a26b7f6567544b79e154c1f73b5a2cf45fd56087cd3ab1c198  kds_server-post-13c6d4d
d3f55437cc166a4d70a2e8a35d4532c73acbe7bda8e7bdf1f4e3923c94f69205  kds_server-prewake-bce12d0
```

The post hash is **byte-identical to cell 5's `kds_server-post-07f447a`**,
which is the arithmetic the two commits promise: `07f447a..13c6d4d` touches
`bench/` only. Filesystem **ext4 on `/dev/root`**, workdirs under
`/home/ubuntu/rw-b/cell2`; `/tmp` is tmpfs on this host and was not used.
Host idled before the sweep began.

---

## 2. The result

Medians of three reps. `shipped − seated` is the p50 delta at the same knob,
which is the statistic SS-B convicted the idle block with.

| `wal_drain_interval_us` | arm | seated p50 | **shipped p50** | **shipped − seated** | shipped ips | seated ips |
|---|---|---|---|---|---|---|
| 1000 | prewake | 23.3 µs | **1,082.9 µs** | **+1,059.6** | 843 | 36,077 |
| 1000 | **post** | 23.2 | **43.2** | **+20.0** | 17,766 | 36,323 |
| 2000 | prewake | 23.2 | **2,109.4** | **+2,086.2** | 498 | 33,947 |
| 2000 | **post** | 23.4 | **43.2** | **+19.8** | 18,081 | 34,970 |
| 3000 | prewake | 23.5 | **3,118.6** | **+3,095.1** | 333 | 33,427 |
| 3000 | **post** | 23.2 | **43.2** | **+20.0** | 17,022 | 35,793 |
| 5000 | prewake | 23.2 | **5,118.4** | **+5,095.2** | 200 | 35,527 |
| 5000 | **post** | 23.1 | **43.5** | **+20.4** | 8,414 | 35,302 |

Per-rep p50 spreads on the post arm are 43.2/43.6/43.2, 42.9/43.4/43.2,
43.5/43.2/43.2 and 44.0/42.0/43.5 — the arm's whole variation across twelve
runs and four knobs is **2.0 µs**.

**The prewake arm reproduces SS-B on this box**, which is what makes the
post arm worth reading: SS-B published 1,091 / 2,097–2,108 / 3,104–3,116 /
5,108–5,126 µs at the same four points on a different host and a different
tree, and this run reads 1,082.9 / 2,109.4 / 3,118.6 / 5,118.4. Throughput
is the reciprocal to three figures on that arm — 843 / 498 / 333 / 200 ips —
exactly as it was.

---

## 3. Claim 1's falsifier, judged

The order states the falsifier as: *"the p50 still tracks the knob → the
wake is not on the path and the diagnosis was wrong."*

**It does not fire.** On the post arm the shipped p50 is 43.2, 43.2, 43.2,
43.5 µs across a fivefold change in the knob, and the shipped-minus-seated
delta is 20.0, 19.8, 20.0, 20.4 µs — **flat to 0.6 µs**. On the prewake arm
the same delta is the knob itself. The functional dependence SS-B measured
is gone, and it is gone because the reactor no longer waits for the timer
that the knob sets.

### 3a. The 50× point, which the order did not ask for and which settles it

A fivefold range can be argued with. `max_idle_block_ms` is 10
(`include/kds/sched/scheduler.hpp:79`), so a knob **above** 10 ms lets the
ceiling — not the drain timer — decide the block, and the two arms then
predict different things. Measured on `v2.3.0-rwc1` at `13c6d4d`, same
shape, `wal_drain_interval_us = 50000`, 2 reps:

| arm | shipped p50 | shipped p25 | shipped p99 | ips |
|---|---|---|---|---|
| prewake `bce12d0` | **11,010 / 10,982 µs** | 10,871 / 10,760 | 11,343 / 11,413 | 100.0 / 100.0 |
| **post** `13c6d4d` | **42.4 / 42.8 µs** | 41.0 / 41.9 | 87.9 / 891.6 | 19,136 / 13,235 |

The prewake arm stops at **11 ms, not 50** — the 10 ms ceiling plus a
statement — which is D4's *"a block always has a ceiling"* read off the
outside of a binary that predates the decision. The post arm does not move
at all: **42.4 µs at a 50 ms drain interval**, a 257× separation, and the
knob-independence claim now stands over a 50× range rather than a 5× one.

---

## 4. The residual — measured against the 1 ms floor, which is RW6's gate

The order: *"If it is independent but a flat residual remains, measure the
residual against the 1 ms rounding floor: that residual, and only that,
opens RW6."*

**The residual is 20.0 µs and it is not the rounding.** Three reasons, in
order of how much they settle:

1. **It is 50× smaller than the floor it would have to be made of.**
   `IdleTimeoutMs` rounds up to whole milliseconds; a residual the rounding
   explains is ≥ 1,000 µs and this one is 20.
2. **It does not move with the knob** — 20.0 / 19.8 / 20.0 / 20.4 across the
   sweep, and **19.6 µs at a 50 ms drain interval** (shipped 42.9 against
   seated 23.3 in §5's discriminator pair, the only point where both seats
   were run that far out). A rounding artifact is a function of the timer
   deadline; this is a constant.
3. **It is the size SS-B priced the wire at** — *"the wire itself is ~20 µs
   against a ~0.9 ms sync"* — and a shipped statement crosses it twice
   (request and reply) on a path that also parses and binds at the owner.

**So RW6 stays shut, and D6's own reasoning is what closes it**: *"after
D2/D3 a ring message no longer waits for a timer at all, so the floor may
cease to matter; building it first would be fixing a premise the fix itself
removes."* That is now measured rather than predicted. `epoll_pwait2` would
be a change to how long the reactor sleeps when nothing has arrived, and
nothing on this path is waiting for that any more.

### 4a. What the 20 µs costs depends entirely on what the statement costs

Under `relaxed` the shipped/seated **throughput** ratio in the table above
is 0.489 / 0.517 / 0.476 / 0.238 — nothing like cell 1's 0.989. Both are
right, and the difference is the point:

| durability | seated p50 | the wire | shipped/seated |
|---|---|---|---|
| `group` (cell 1) | 705.7 µs | +20 µs | **0.989** |
| `relaxed` (here) | 23.2 µs | +20 µs | **0.49** |

`relaxed` takes the device out of the statement, so a statement costs 23 µs
and a 20 µs wire nearly doubles it. **After this version, shipping's cost is
a fixed ~20 µs per statement**, and whether that reads as 1% or 51% is a
property of what the statement was doing seated, not of shipping. Before
this version the cost was ~1,060 µs and that reading did not depend on the
statement at all — which is why `crosscore.md` §9's routing decision
inherits a *constant* now rather than a ratio.

### 4b. The CPU the block was costing, in a second shape

Cell 3 measured the arrival core's spin directly. The same fact falls out of
this sweep's `cpu_busy`, at 21× the throughput:

| arm, `d = 1000` | arrival core | owner core | ips | **CPU-seconds per 1,000 statements** |
|---|---|---|---|---|
| prewake | **0.879** | 0.026 | 843 | **1.07** |
| post | **0.280** | 0.320 | 17,766 | **0.034** |

The owner rises from 0.026 to 0.320 because it is now executing statements
instead of sleeping through them, and the pair together costs **32× less CPU
per statement**.

---

## 5. What is left in the tail, and the three things it is not

The post arm's p50 is flat but its p99 is not: 80–890 µs at `d = 1000`, and
at `d = 5000` two of three reps show p95 445–486 µs with throughput halved
(8,414 and 7,778 ips against 17,159 in the third). That is a real residual
and it is **not** the one RW6 was written for.

**Measured** — `--trace-latencies`, every statement's latency in arrival
order, post arm, 5,000 rows:

| trace | over 200 µs | structure |
|---|---|---|
| `d = 1000` shipped | 24 of 5,000 (0.48%) | one stall of **~2.0 ms every 11.0 ms**, period stable to ±0.4 ms |
| `d = 1000` seated | 15 of 5,000 (0.30%) | one stall of ~1.2 ms every **11.5 ms** |
| `d = 5000` seated | 13 of 5,000 (0.26%) | one stall of ~1.3 ms every **10.7 ms** |
| `d = 5000` shipped | 458 of 5,000 (9.2%) | the same period, in bursts of up to 32 consecutive statements |

**It is on the seated arm at the same period and the same cost.** A seated
statement never touches a ring, never parks on a peer's reply and never
waits for an idle block, so whatever this is, it is not the wake path, not
the park rule and not the block. That single observation is what takes it
out of this order's scope; the rest is recorded so the next reader does not
re-derive it.

Two hypotheses were tested and **rejected**, both by direct measurement on
`v2.3.0-rwc1` at `13c6d4d`:

- **`relaxed`'s own loss-window sync** (`relaxed_flush_interval_ns`, 10 ms,
  and `include/kds/server/expeditor.hpp:415-421` predicts exactly this
  signature — *"one ~2.2 ms statement every 12 ms … the sync that enforces
  it runs on the reactor thread"*). Rejected: with
  `relaxed_flush_interval_us = 0`, which disables the sync outright, the
  stalls are **unchanged** — 44 bursts at 163/s, median cost 1,008 µs,
  against 36 bursts at 134/s and 1,087 µs with it at its default.
- **The WAL drain timer.** Rejected: at `wal_drain_interval_us = 50000`
  *and* the flush disabled, the stalls persist at 177/s (shipped) and 136/s
  (seated) with a median cost of 959 and 1,252 µs.

So it survives both WAL timing knobs and appears identically with and
without the wire. What is left is a ~1 ms stall arriving at ~100–170/s on
the reactor thread on a path that still writes to the device even when
nothing is asked to sync — kernel writeback against the WAL file is the
obvious suspect and is **not** established here. **Handed on, unattributed
and unfixed**, with the driver flag this cell added
(`--relaxed-flush-interval-us`) left in place, because it is what made two
of the three candidates falsifiable in one invocation each.

**What it means for the numbers above**: the p50 statistics in §2 are
untouched by it (the stalls are 0.3–1.0% of statements at `d ≤ 3000`), and
the `d = 5000` shipped **ips** column is depressed by it rather than by the
knob — the p50 at that point is 43.5 µs, the same as everywhere else.

---

## 6. Gates

- **Rows in = rows out** in every run: the 48-run sweep is **142,800
  INSERTs attempted, 142,800 executed, 0 refused**, and across all 66 runs
  archived here — the sweep, the four traces, the six discriminators and the
  four ceiling runs — **223,600 attempted, 223,600 executed, 0 refused**,
  `verify = rows as expected` in every one.
- **Correctness suite**: not executed by this cell — it is cell 6's, run at
  `1a305ab` and recorded in
  `bench/v2.3.0/results-correctness-cell6-v2.2.1-15-g1a305ab.md`.
- **The null cell**: not run. Every arm here is a separate process with a
  fresh server and a fresh data file, which is the shape SS-B finding 10
  showed does not carry the harness's ~10% ordering bias (null cells 0.991
  and 1.016); and the headline statistic is a **latency that changes by 250×**,
  not a ratio inside a noise floor. Recorded as an argument standing in for
  a measurement.
- Raw driver output — 48 sweep JSONs, 4 traces, 6 discriminator runs, 4
  ceiling runs, with their logs — is archived beside this file in
  `bench/v2.3.0/archive/cell2-knob-sweep/`.

## 7. What this cell does not answer

- **The hot-path cost of the wake at load** is cell 4's, running separately.
  Nothing here says the wake is free where the target was never asleep.
- **Anything under `group` durability.** This cell runs `relaxed` on
  purpose, to keep the seated control at 23 µs; cell 1 is the `group`
  reading and cell 5 the commit path's.
- **The tail's identity** (§5). Characterized, twice-falsified, not named.
