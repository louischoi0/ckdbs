# RW-B cell 4 — the hot-path cost of the wake, where nothing was ever asleep

**Measured 2026-08-27** in worktree `v2.3.0-rwc1`, an interleaved A/B of two
binaries run alternately in one sitting on the same 8-CPU host:

| arm | commit | `git describe --tags` | what it has |
|---|---|---|---|
| **prewake** | `bce12d0` | `v2.2.1-3-gbce12d0` | neither the wake path nor the park rule |
| **post** | `13c6d4d` | `v2.2.1-14-g13c6d4d` | the wake path (RW1–RW2) **and** "parked is not ready" (RW3) |

Order: `instructions/v2.3.0-reactor-wake.md` §5 cell 4 — **claim 2**, *"the
wake costs nothing at load"*, whose falsifier is: *"a per-send syscall on the
hot path shows up as a loss anywhere the target was never asleep — which is
what D3 exists to prevent and what this cell is the check on."*

> **Verdict: the falsifier does not fire, in any of the three sub-cells.**
> Nothing regresses; two of the three improve. The single most useful number
> is **K = 4**, where the shipped/seated ratio goes 0.912 → **0.999** while
> the arrival core's CPU goes 0.923 → **0.028**: a whole core given back at
> no throughput cost at all.

---

## 1. What was run

84 invocations, three sub-cells × 3 reps, arms alternating within every
point, one fresh server and one fresh data file per invocation:

```
bench/single_relation_probe.py --server <arm binary> --workdir <fresh> \
    --arm single --cores 1 --sessions <1|4> --rows <4000|2000>          # (a)
    --arm multi --cores 4 --sessions <4|16> --relations 1 --rows 1000 \
        --seat <owner|foreign> [--arrival-core -1]                      # (b)
    --arm multi --cores 4 --sessions <2|4|8|14> --relations 1 \
        --rows 3000 --seat <owner|foreign>                              # (c)
```

`group` durability and `wal_drain_interval_us = 1000` throughout — the
default, and the regime every SS-B cell this compares against was taken in.
Binaries are the same two copies cell 2 used, hashed there
(`7fcdbaf…` post, `d3f5543…` prewake); ext4 on `/dev/root`, workdirs under
`/home/ubuntu/rw-b/cell4`.

---

## 2. (a) `cores = 1` — the build that arms nothing

D3's whole argument is that a sender reads the destination's `sleeping` flag
before writing an eventfd, and a single-core build has no transport, no
waker and no flag (`Scheduler::wake_armed()` is false). If that argument
were wrong, this is where it would show.

| shape | arm | ips (per rep) | ips median | p50 median | **post/prewake** |
|---|---|---|---|---|---|
| S = 1, 4,000 rows | prewake | 885.2 / 872.6 / 923.0 | 885.2 | 1,114.1 µs | |
| | **post** | 846.9 / 911.7 / 916.7 | **911.7** | 1,092.2 µs | **1.030** |
| S = 4, 2,000 rows | prewake | 1,969.1 / 1,974.6 / 1,973.7 | 1,973.7 | 2,022.3 µs | |
| | **post** | 1,985.8 / 2,006.9 / 2,161.7 | **2,006.9** | 1,999.1 µs | **1.017** |

**No cost, and the S = 4 arm says so tightly**: the prewake reps span 1,969
–1,975 (0.3%), so a 1.017 is outside that arm's own spread in the *upward*
direction. The S = 1 arms overlap and are worth one figure. Nothing here is
a claim that the wake made a single-core build faster; the claim is that a
syscall per send does not exist on it, which is what D3 said and what these
two rows fail to contradict.

---

## 3. (b) B4 — K waiters on one arrival core

SS-B's B4 is the cell that found the spin: K sessions all seated on **one**
foreign arrival core, against the same K seated on the owner. It published
0.951 at K = 4 and 0.976 at K = 16 with the arrival core at 0.925/0.929
busy.

| K | arm | seated ips | shipped ips | **ratio** | ratio per rep | **arrival cpu** |
|---|---|---|---|---|---|---|
| 4 | prewake | 2,267.5 | 2,067.0 | **0.912** | 0.912 / 0.906 / 0.936 | **0.923** |
| 4 | **post** | 2,284.7 | 2,282.5 | **0.999** | 0.956 / 0.951 / 1.127 | **0.028** |
| 16 | prewake | 9,361.5 | 8,963.5 | **0.957** | 0.941 / 0.973 / 0.937 | **0.929** |
| 16 | **post** | 8,897.1 | 9,744.2 | **1.095** | 1.095 / 1.101 / 1.057 | **0.146** |

**The prewake arm reproduces SS-B** (0.912 and 0.957 here against 0.951 and
0.976 there, different host, different tree), which is the control that
makes the post row readable.

**Nothing regressed and the CPU came back**: 33× less arrival-core CPU at
K = 4, 6× at K = 16, while the ratio *rose* on both. Cell 3's finding —
that RW3 costs ~10% of throughput at K = 1 on an idle box — is bounded by
this: **the cost is gone by K = 4**, which is what cell 3 predicted from a
single K = 4 point and what three interleaved reps now say.

**Why the K = 16 ratio exceeds 1, stated rather than banked.** A seated
session puts its socket, its render and its execution on the owner's one
reactor; a shipped one puts socket and render on the arrival core and only
the execution on the owner. At K = 16 that split is worth more than the
wire costs — shipped p50 **1,546.5 µs against seated 1,755.6** — and SS-B
saw the same sign in its §5 (*"the shipped p50 is consistently lower than
the seated one"*). It is not the wake making anything faster; it is a
two-core split that the idle block used to cancel out. The p99 is the price
and does not move much either way (2,781.6 shipped against 2,719.3 prewake).

---

## 4. (c) The R2 curve — SS-B §5's 521–547 × S

| S | arm | seated ips | shipped ips | **ratio** | ratio per rep | shipped ips / S |
|---|---|---|---|---|---|---|
| 2 | prewake | 1,080.5 | 1,054.2 | 0.976 | 0.924 / 1.004 / 0.970 | 527.1 |
| 2 | **post** | 1,075.1 | 1,111.9 | **1.034** | 0.920 / 1.072 / 1.040 | **556.0** |
| 4 | prewake | 2,225.8 | 2,225.9 | 1.000 | 0.937 / 1.010 / 0.948 | 556.5 |
| 4 | **post** | 2,204.1 | 2,062.7 | **0.936** | 0.935 / 0.936 / 0.953 | **515.7** |
| 8 | prewake | 4,397.2 | 4,193.0 | 0.954 | 0.940 / 0.974 / 0.967 | 524.1 |
| 8 | **post** | 4,292.3 | 4,113.9 | **0.958** | 0.972 / 0.921 / 0.959 | **514.2** |
| 14 | prewake | 7,661.0 | 6,631.4 | 0.866 | 0.876 / 0.947 / 0.840 | 473.7 |
| 14 | **post** | 7,667.0 | 7,278.0 | **0.949** | 0.938 / 0.970 / 0.941 | **519.9** |

**The law survives**: the post arm's shipped throughput is 514–556 × S
against SS-B's published 521–547 × S, over the same four points. Three of
the four ratios are inside the ±10% floor §2 of SS-B measured and the fourth
(S = 14) is the one point where the arms separate — **0.866 → 0.949**, and
that is prewake being *worse*, not post.

At S = 4 the post arm reads 0.936 against prewake's 1.000. Its three reps
(0.935 / 0.936 / 0.953) are tighter than prewake's (0.937 / 1.010 / 0.948),
whose median is carried by one rep at 1.010; the two arms' spreads overlap
across their whole range. **Recorded as not separable at three reps**
rather than as a 6% regression — which is what it would have been called if
the medians alone were read.

---

## 5. Claim 2, judged

**Upheld.** Its falsifier asks for a loss where the target was never asleep,
and the three places that could show one are `cores = 1` (1.030 / 1.017),
K = 4 and K = 16 (0.999, 1.095 against 0.912, 0.957) and the R2 curve
(within the noise floor at three points, better at the fourth). None of them
shows a loss.

**The mechanism the claim rests on is separately visible.** D3's flag means
a send costs a syscall only when the destination is asleep, and the
`sched_wakes_sent` / `sched_wakes_received` counters RW5 added make that
countable from outside the process: on a `cores = 4` shipped run of 2,000
statements, the instance wrote **4,028 wakes** — one per statement per
direction, plus a handful — and the sum of the four cores'
`sched_wakes_received` is 4,028 exactly. A build that woke unconditionally
would show a number many times that.

---

## 6. Gates

- **Rows in = rows out** in every cell: **1,320,000 INSERTs attempted,
  1,320,000 executed, 0 refused**, `verify = rows as expected` in all 84
  runs.
- **The null cell**: not run, same argument as cell 2 §6 — every arm is a
  separate process with a fresh server and a fresh data file, the shape
  SS-B finding 10 showed does not carry the harness's ordering bias.
- Raw driver output for all 84 runs is archived beside this file in
  `bench/v2.3.0/archive/cell4-hot-path/`.

## 7. What this cell does not answer

- **K = 1**, which is cell 3's and where RW3's cost lives (0.900×
  throughput, +31.5 µs p50). This cell bounds that cost from above; it does
  not remove it.
- **The `relaxed` regime**, cell 2's, where the wire is half the statement.
- **A host whose cores are busy.** Everything here runs on 8 CPUs with at
  most 16 sessions, and cell 3 §5 already says the trade reads differently
  where the arrival cores are not free.
