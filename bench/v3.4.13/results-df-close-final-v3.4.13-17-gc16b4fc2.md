# DF's close: the cached index fold takes 0.18 µs off an index-served read and leaves a tuple key read at +0.07 µs; all of DF on a point read is not resolved against the 0.5 µs bar

On worktree `da-q8-memory-bound-key` (branch `worktree-df-statistics-coverage`)
at `c16b4fc2` (`v3.4.13-17-gc16b4fc2`, D), whose tree is B's (`ec5ddfe8`,
`v3.4.13-14-gec5ddfe8`) plus one change - the index usage fold: an index's counts
are single-writer relaxed words reached through a pointer the dispatcher keeps in
an eight-slot cache by oid, so a repeated read of an index takes no latch,
does no hash lookup and reads no clock - D costs a point statement, against
A (`8e7c820f`, `v3.4.13-10-g8e7c820f`, `main` before DF-S2; DF-S1 is in it), at
`cores = 1`, `relaxed`, 1,000 rows, ten lifetimes, server CPU a statement, both
driver paths pooled:

| Shape | A | B - A | **D - A** | **D - B** | Floor A' - A |
|---|---|---|---|---|---|
| select-pk | 20.85 µs | +0.14 µs [+0.10, +0.17] | **+0.07 µs [+0.02, +0.10]** | -0.04 µs [-0.08, -0.01] | -0.05 µs [-0.07, +0.00] |
| select-index | 24.05 µs | +0.35 µs [+0.29, +0.40] | **+0.18 µs [+0.12, +0.25]** | **-0.18 µs [-0.22, -0.13]** | -0.09 µs [-0.15, -0.04] |
| update-lit | 20.96 µs | -0.07 µs [-0.11, -0.03] | +0.02 µs [-0.01, +0.06] | +0.10 µs [+0.07, +0.13] | -0.02 µs [-0.06, +0.02] |
| insert | 19.57 µs | -0.08 µs [-0.12, -0.03] | +0.04 µs [+0.00, +0.08] | +0.11 µs [+0.08, +0.15] | -0.13 µs [-0.18, -0.10] |

**Verdict against DF-Q10 (a)'s bar** (all of DF at most 0.5 µs of server CPU a
point statement at `cores = 1`). **Tuple key read: not resolved, with the central
estimate at the bar**: DF-S1's +0.44 µs [+0.27, +0.54]
(`bench/v3.4.13/results-cx-close-bisect-v3.4.13-1-g11f72ce9.md`) plus D - A of
+0.07 µs [+0.02, +0.10] is about +0.51 µs, naive interval +0.29 to +0.64 µs,
which straddles 0.5 (and A sits 0.05 µs above A' in this run, so D - A may read
that much low). **Index-served read: not resolved, because DF-S1's share was never
measured on that shape**: D - A, the cost of DF-S2 to DF-S4 with the cached fold,
is +0.18 µs [+0.12, +0.25], a lower bound of what all of DF costs it; if DF-S1
cost an index read what it costs a tuple key read (an assumption, +0.44 µs)
the sum would be +0.62 µs, over; this file does not claim it. For writes D - A
is +0.02 and +0.04 µs, with DF-S1's write share unmeasured, so the bar is not
decided there either. Nothing is rounded into a pass, and the decision on the bar
is the operator's (DF-Q10 (a)).

**What the cached fold bought: -0.18 µs [-0.22, -0.13] on an index-served read
(-0.21 literal, -0.14 prepared), resolved from zero** by its pooled interval and
by the lifetime medians (their bootstrap [-0.30, -0.03]; 8 of 10 lifetimes
below zero; D below B in 303 of 480 blocks), against a single lifetime's floor
that reaches ±0.4 µs. It is most of what the knob run said the fold costs
(`B - K1` there +0.21 µs). **On a tuple key read it bought -0.04 µs [-0.08,
-0.01], not resolved** (lifetime medians [-0.10, +0.04]); on writes D is *above* B
by +0.10 and +0.11 µs, a sign nothing in D's change explains (below).

## The four files, one close

| File | What it measured | Reading |
|---|---|---|
| `results-df-close-v3.4.13-14-gec5ddfe8.md` | B against A, ten lifetimes, three row counts, one `cores = 8` lifetime | DF-S2 to DF-S4 cost a tuple key read +0.17 µs and an index-served read +0.35 µs, writes inside the floor; the same at 200 and 10,000 rows; `cores = 8` not resolved on this host |
| `results-df-close-remedy-v3.4.13-15-g9f588fa2.md` | C = B with a cheaper session cost sample (`9f588fa2`), ten lifetimes | C - B +0.00 to +0.04 µs on reads: the remedy bought nothing resolvable, and it was reverted (HEAD `0e180fc6`, B's tree); the file stays as the record of why |
| `results-df-close-knobs-v3.4.13-14-gec5ddfe8.md` | four patched exports of B (no index fold, no session words, no published stamp, all three), ten lifetimes | the three knobs together remove about three quarters of B - A on a read (+0.14 of +0.18 µs, +0.24 of +0.33 µs); no single knob is separated from the binaries' offsets; the fold's removal alone, `B - K1`, was +0.21 µs on `select-index` and +0.08 µs on `select-pk` |
| this file | D, the cached index fold | D - B -0.18 µs on `select-index`, the one move the code made, in the direction and about the size the knob run said it could |

The through-line: DF's statistics cost a point statement, after DF-S1's
+0.44 µs, a further +0.07 µs on a tuple key read and +0.18 µs on an index-served
one, nothing resolvable on a write, and the cached fold is what moved. The
remedy of the session sample and the rest of the knobs' attribution did not
point at anything else that can be removed with a resolved gain.

## Run stamp

| Item | Value |
|---|---|
| Executed | 2026-11-19, build 08:25:28 to 08:26:46 UTC; ten four-arm lifetimes 08:26:53 to 08:39:57 UTC |
| Branch / worktree | `worktree-df-statistics-coverage`, worktree `da-q8-memory-bound-key`, `c16b4fc2`; nothing committed by this run |
| A | `8e7c820f`, sha256 `ab5b9ee7cc7875bbe199ee402214b8e12176c8a00f909eb6e0ca1aae4cba9437` (the first file's binary) |
| A' | A's binary again, a second server of it in every lifetime: the floor |
| B | `ec5ddfe8`, sha256 `35bac562e5bfcc23614cd70aa35e9fda44359b52a5fa3f6eb9b8024fb4021604` (reused) |
| D | `c16b4fc2` (`v3.4.13-17-gc16b4fc2`), sha256 `a8e1abd2e34ef1cfe2a193fdc3e330c0ee6869ecbaa46c61390a722525052921`, built 08:26:46 UTC from a `git archive` export under `/home/louis/dfclose/srcc16b4fc2`, Release, tests and sim off, 78 s; newer than its commit; every server started from a copy in `/home/louis/dfclose/run/` |
| Device | data files and WAL under `/home/louis/dfclose/run/wd_f_*/` on `/dev/nvme0n1p1`, ext4; not tmpfs |
| Host | Intel Xeon Platinum 8581C @ 2.10 GHz, 8 CPUs, Linux 6.12.116+deb13-cloud-amd64, no PMU; server on CPU 3, driver on CPU 5 (SMT siblings 7 and 1 watched) |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, every other key default; ports 32700 to 32703 |
| Host load | 1-minute load 1.05 to 4.33 at the cells; 78 of 80 cells clean (`insert literal` of lifetimes 8 and 10 unclean, dropped from the paired tables); the process-level gate never held |

## Method

The first file's design with arms A, A', B, D: ten lifetimes, every server
fresh with a fresh data file, arms interleaved in blocks of 500 statements (24
blocks, 12,000 statements an arm, cell and lifetime), the block's order
rotating by one server per block and the `--arm` order by lifetime number.
Cells `select-pk`, `select-index`, `update-lit`, `insert`, each `literal` and
`prepared`; one connection; `--rows 1000` (the point statements do not scale with
rows, the first file's sweep at B is the evidence; it was not re-run at D). A
delta is the median over blocks of the block-paired difference, pooled over
the lifetimes with a 2,000-resample bootstrap over blocks; the floor is A' - A;
`D - B` does not involve A. 0 bad replies.

```
python3 tools/bn_census.py --arm A=<run>/kds_server_A --arm A2=<run>/kds_server_A --arm B=<run>/kds_server_B \
  --arm D=<run>/kds_server_D --workdir <run>/wd_f_i --port 32700 --pin-server 3 --pin-driver 5 --cores 1 \
  --rows 1000 --scan-rows 1000 --ops 12000 --blocks 24 --runs 1 --only select-pk,select-index,update-lit,insert \
  --cell-quiet-pct 15 --cell-quiet-wait 60 --cell-retries 2 --cell-clean-pct 25 --out <run>/out_f_i.json
```
(`lifef.sh f 1 10 12000 24 select-pk,select-index,update-lit,insert 1000 32700 5 3 A A2 B D`; `A2` is the driver's label for A'; `tools/bn_census.py` unmodified, git blob `a6aee7135c964c75a01a695627055ef95d54b5b7`.)

## Result: per cell, pooled

Each cell is the median of the block-paired delta [95 % bootstrap over blocks]
and the number of lifetime medians above zero:

| Cell | A server CPU | B - A | D - A | D - B | A' - A (floor) | lifetime medians of A' - A: range | blocks D < B | D - B as share of A |
|---|---|---|---|---|---|---|---|---|
| select-pk literal | 21.06 us | +0.16 us [+0.13, +0.21], 9 of 10 | +0.09 us [+0.02, +0.16], 7 of 10 | -0.08 us [-0.13, +0.01], 4 of 10 | -0.06 us [-0.12, -0.00], 4 of 10 | -0.44..+0.24 us | 134 of 240 | -0.38 % |
| select-pk prepared | 20.64 us | +0.11 us [+0.07, +0.17], 9 of 10 | +0.04 us [+0.00, +0.09], 7 of 10 | -0.02 us [-0.07, +0.01], 3 of 10 | -0.03 us [-0.07, +0.04], 6 of 10 | -0.38..+0.23 us | 130 of 240 | -0.10 % |
| select-index literal | 24.33 us | +0.40 us [+0.34, +0.43], 10 of 10 | +0.20 us [+0.10, +0.26], 8 of 10 | -0.21 us [-0.27, -0.15], 1 of 10 | -0.12 us [-0.18, -0.04], 4 of 10 | -0.34..+0.38 us | 160 of 240 | -0.86 % |
| select-index prepared | 23.77 us | +0.30 us [+0.18, +0.38], 10 of 10 | +0.18 us [+0.08, +0.26], 7 of 10 | -0.14 us [-0.21, -0.06], 2 of 10 | -0.08 us [-0.15, +0.01], 5 of 10 | -0.39..+0.54 us | 143 of 240 | -0.59 % |
| update-lit literal | 21.28 us | -0.05 us [-0.11, -0.01], 4 of 10 | +0.03 us [-0.01, +0.08], 6 of 10 | +0.08 us [+0.04, +0.13], 7 of 10 | +0.01 us [-0.06, +0.05], 6 of 10 | -0.19..+0.21 us | 86 of 240 | +0.38 % |
| update-lit prepared | 20.64 us | -0.08 us [-0.13, -0.03], 1 of 10 | +0.01 us [-0.04, +0.06], 5 of 10 | +0.11 us [+0.06, +0.14], 9 of 10 | -0.05 us [-0.10, +0.02], 3 of 10 | -0.22..+0.07 us | 81 of 240 | +0.53 % |
| insert literal | 19.86 us | -0.05 us [-0.15, +0.05], 4 of 8 | +0.05 us [-0.03, +0.08], 4 of 8 | +0.11 us [+0.06, +0.18], 8 of 8 | -0.18 us [-0.22, -0.11], 2 of 8 | -0.24..+0.09 us | 67 of 192 | +0.58 % |
| insert prepared | 19.27 us | -0.09 us [-0.12, -0.03], 3 of 10 | +0.04 us [-0.02, +0.11], 6 of 10 | +0.11 us [+0.06, +0.15], 10 of 10 | -0.12 us [-0.17, -0.07], 4 of 10 | -0.31..+0.07 us | 78 of 240 | +0.57 % |

By shape, the two paths pooled (the lifetime-median column is D - B):

| Shape | A server CPU | B - A | D - A | D - B | A' - A (floor) | lifetime-median D - B: median [95 % bootstrap over lifetimes], range, lifetimes above 0 | blocks D < B |
|---|---|---|---|---|---|---|---|
| select-pk | 20.85 us | +0.14 us [+0.10, +0.17] | +0.07 us [+0.02, +0.10] | -0.04 us [-0.08, -0.01] | -0.05 us [-0.07, +0.00] | -0.05 us [-0.10, +0.04], -0.23..+0.05, 4 of 10 | 264 of 480 |
| select-index | 24.05 us | +0.35 us [+0.29, +0.40] | +0.18 us [+0.12, +0.25] | -0.18 us [-0.22, -0.13] | -0.09 us [-0.15, -0.04] | -0.17 us [-0.30, -0.03], -0.41..+0.08, 2 of 10 | 303 of 480 |
| update-lit | 20.96 us | -0.07 us [-0.11, -0.03] | +0.02 us [-0.01, +0.06] | +0.10 us [+0.07, +0.13] | -0.02 us [-0.06, +0.02] | +0.10 us [+0.03, +0.19], -0.00..+0.23, 9 of 10 | 167 of 480 |
| insert | 19.57 us | -0.08 us [-0.12, -0.03] | +0.04 us [+0.00, +0.08] | +0.11 us [+0.08, +0.15] | -0.13 us [-0.18, -0.10] | +0.12 us [+0.10, +0.14], +0.06..+0.19, 8 of 8 | 145 of 432 |

**The index fold.** `select-index` is where D moved: D - B is -0.21 µs
literal and -0.14 prepared, D below B in 160 and 143 of 240 blocks, and D - A is
+0.20 and +0.18 µs against B - A of +0.40 and +0.30: about half of B's
cost on that shape is gone, what is left (+0.18 µs) is the cost of the walk of
the chain's steps, the wider `StepStats`, the per-seek counter, the stamp and
the session words, as the knobs file split it. On `select-pk` the same code is
not reached (a tuple key read has no index step) and D - B is -0.04 µs,
inside the floor; the +0.07 µs D - A there is the rest of DF-S2 to DF-S4.

**Writes: a sign D's change does not explain.** `update-lit` and `insert` are
+0.10 and +0.11 µs above B, in 9 of 10 and 8 of 8 lifetimes, and a write
does not reach the index fold. Against A the picture is different: B - A is
-0.07 and -0.08 µs (B *below* A) and D - A is +0.02 and +0.04 µs. So it is B
that sits low on writes in this lifetime set - B - A on writes was -0.03 and
+0.07 µs in the first file and -0.08 and +0.08 in the remedy's, a spread of
0.15 µs between sets of lifetimes - and D's write cost against A is inside the
floor (A' - A is -0.02 and -0.13 µs on the same cells). It is not a finding
about D, and it is not attributed; if a reader needs one number for a write,
it is D - A, +0.02 µs [-0.01, +0.06] and +0.04 µs [+0.00, +0.08], and DF-S1's
write share is still unmeasured.

**The floor.** A' - A is -0.02 to -0.13 µs pooled and ±0.4 µs for a single
lifetime (lifetime medians -0.44 to +0.54 µs): A is the slower process of two
of one binary here by about 0.05 to 0.13 µs, so every `X - A` reads that much
low, and a delta of that size is not attributed. The pooled `D - B` on
`select-index` (-0.18 µs) is twice that shape's pooled floor (-0.09 µs),
resolved by ten lifetimes and not by one.

### Every lifetime

#### select-pk / literal

| Lifetime | A | A' | B | C | B - A | D - A | D - B | A' - A (floor) | blocks D < B | clean (attempts) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 20.69 us | 20.81 us | 20.73 us | 20.64 us | +0.02 us | -0.08 us | -0.08 us | +0.01 us | 16 of 24 | yes (1) |
| 2 | 20.59 us | 20.53 us | 21.15 us | 20.99 us | +0.26 us | +0.16 us | -0.05 us | -0.02 us | 14 of 24 | yes (1) |
| 3 | 20.80 us | 20.42 us | 20.69 us | 20.49 us | +0.02 us | -0.24 us | -0.23 us | -0.41 us | 20 of 24 | yes (1) |
| 4 | 20.67 us | 20.82 us | 20.72 us | 20.84 us | +0.04 us | +0.12 us | +0.05 us | +0.02 us | 11 of 24 | yes (1) |
| 5 | 21.37 us | 21.37 us | 21.58 us | 21.46 us | +0.24 us | +0.04 us | -0.16 us | -0.03 us | 16 of 24 | yes (1) |
| 6 | 20.78 us | 20.94 us | 21.04 us | 21.40 us | +0.40 us | +0.33 us | +0.17 us | +0.13 us | 8 of 24 | yes (1) |
| 7 | 21.81 us | 21.68 us | 21.99 us | 22.06 us | +0.21 us | +0.29 us | +0.11 us | -0.10 us | 9 of 24 | yes (2) |
| 8 | 21.10 us | 20.91 us | 21.32 us | 21.67 us | +0.15 us | +0.23 us | +0.04 us | -0.17 us | 10 of 24 | yes (1) |
| 9 | 22.15 us | 21.73 us | 21.75 us | 21.79 us | -0.09 us | -0.28 us | -0.13 us | -0.44 us | 13 of 24 | yes (2) |
| 10 | 20.61 us | 21.28 us | 21.17 us | 20.80 us | +0.22 us | +0.25 us | -0.13 us | +0.24 us | 17 of 24 | yes (1) |
| **pooled** | 21.06 us | 21.05 us | 21.21 us | 21.21 us | **+0.16 us** [+0.13, +0.21] | **+0.09 us** [+0.02, +0.16] | **-0.08 us** [-0.13, +0.01] | **-0.06 us** [-0.12, -0.00] | 134 of 240 | 10 clean |

#### select-pk / prepared

| Lifetime | A | A' | B | C | B - A | D - A | D - B | A' - A (floor) | blocks D < B | clean (attempts) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 20.40 us | 20.41 us | 20.00 us | 20.04 us | -0.12 us | -0.04 us | +0.04 us | +0.02 us | 11 of 24 | yes (1) |
| 2 | 20.69 us | 20.70 us | 20.98 us | 21.03 us | +0.19 us | +0.08 us | -0.17 us | -0.01 us | 14 of 24 | yes (1) |
| 3 | 20.05 us | 19.85 us | 20.30 us | 20.03 us | +0.09 us | -0.18 us | -0.24 us | -0.24 us | 16 of 24 | yes (1) |
| 4 | 19.88 us | 19.89 us | 19.92 us | 19.94 us | +0.07 us | +0.11 us | +0.00 us | +0.04 us | 12 of 24 | yes (1) |
| 5 | 20.80 us | 20.72 us | 20.80 us | 20.79 us | +0.07 us | +0.00 us | -0.04 us | +0.02 us | 13 of 24 | yes (1) |
| 6 | 20.18 us | 20.13 us | 20.28 us | 20.16 us | +0.26 us | +0.14 us | -0.07 us | -0.10 us | 15 of 24 | yes (1) |
| 7 | 20.96 us | 21.00 us | 21.19 us | 21.18 us | +0.15 us | +0.12 us | -0.00 us | +0.03 us | 12 of 24 | yes (1) |
| 8 | 20.94 us | 21.11 us | 21.12 us | 21.18 us | +0.14 us | +0.21 us | +0.03 us | +0.12 us | 11 of 24 | yes (3) |
| 9 | 20.98 us | 20.73 us | 20.99 us | 20.93 us | +0.00 us | -0.15 us | -0.03 us | -0.38 us | 13 of 24 | yes (2) |
| 10 | 21.51 us | 21.56 us | 21.63 us | 21.73 us | +0.13 us | +0.30 us | -0.06 us | +0.23 us | 13 of 24 | yes (1) |
| **pooled** | 20.64 us | 20.61 us | 20.72 us | 20.70 us | **+0.11 us** [+0.07, +0.17] | **+0.04 us** [+0.00, +0.09] | **-0.02 us** [-0.07, +0.01] | **-0.03 us** [-0.07, +0.04] | 130 of 240 | 10 clean |

#### select-index / literal

| Lifetime | A | A' | B | C | B - A | D - A | D - B | A' - A (floor) | blocks D < B | clean (attempts) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 23.72 us | 23.70 us | 24.26 us | 23.62 us | +0.37 us | -0.13 us | -0.34 us | +0.14 us | 20 of 24 | yes (1) |
| 2 | 23.88 us | 23.94 us | 24.41 us | 24.28 us | +0.57 us | +0.25 us | -0.11 us | -0.05 us | 13 of 24 | yes (1) |
| 3 | 23.66 us | 23.30 us | 23.85 us | 23.52 us | +0.28 us | -0.17 us | -0.41 us | -0.34 us | 21 of 24 | yes (1) |
| 4 | 23.74 us | 23.96 us | 24.17 us | 24.21 us | +0.37 us | +0.46 us | +0.06 us | +0.04 us | 10 of 24 | yes (1) |
| 5 | 24.31 us | 24.15 us | 24.63 us | 24.63 us | +0.29 us | +0.36 us | -0.00 us | -0.30 us | 12 of 24 | yes (1) |
| 6 | 23.91 us | 23.69 us | 24.49 us | 24.19 us | +0.32 us | +0.21 us | -0.18 us | -0.32 us | 14 of 24 | yes (1) |
| 7 | 25.30 us | 25.45 us | 25.68 us | 25.44 us | +0.41 us | +0.05 us | -0.28 us | +0.14 us | 20 of 24 | yes (1) |
| 8 | 25.04 us | 24.97 us | 25.43 us | 25.24 us | +0.30 us | +0.26 us | -0.06 us | -0.14 us | 13 of 24 | yes (1) |
| 9 | 24.83 us | 24.56 us | 25.33 us | 24.86 us | +0.56 us | +0.03 us | -0.49 us | -0.34 us | 21 of 24 | yes (1) |
| 10 | 24.90 us | 25.36 us | 25.51 us | 25.43 us | +0.49 us | +0.37 us | -0.15 us | +0.38 us | 16 of 24 | yes (1) |
| **pooled** | 24.33 us | 24.31 us | 24.78 us | 24.54 us | **+0.40 us** [+0.34, +0.43] | **+0.20 us** [+0.10, +0.26] | **-0.21 us** [-0.27, -0.15] | **-0.12 us** [-0.18, -0.04] | 160 of 240 | 10 clean |

#### select-index / prepared

| Lifetime | A | A' | B | C | B - A | D - A | D - B | A' - A (floor) | blocks D < B | clean (attempts) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 23.08 us | 23.02 us | 23.13 us | 22.86 us | +0.09 us | -0.11 us | -0.25 us | +0.02 us | 19 of 24 | yes (1) |
| 2 | 24.42 us | 24.23 us | 24.80 us | 24.63 us | +0.50 us | +0.21 us | -0.28 us | -0.24 us | 17 of 24 | yes (2) |
| 3 | 23.10 us | 22.65 us | 23.26 us | 22.81 us | +0.02 us | -0.28 us | -0.40 us | -0.39 us | 20 of 24 | yes (1) |
| 4 | 23.09 us | 23.27 us | 23.46 us | 23.53 us | +0.48 us | +0.42 us | +0.11 us | +0.13 us | 10 of 24 | yes (1) |
| 5 | 23.70 us | 23.51 us | 23.95 us | 23.87 us | +0.25 us | +0.22 us | -0.05 us | -0.20 us | 13 of 24 | yes (1) |
| 6 | 23.95 us | 24.02 us | 24.01 us | 24.07 us | +0.00 us | +0.30 us | -0.12 us | +0.06 us | 13 of 24 | yes (1) |
| 7 | 24.19 us | 24.27 us | 24.45 us | 23.67 us | +0.38 us | +0.16 us | -0.11 us | +0.10 us | 14 of 24 | yes (2) |
| 8 | 24.01 us | 23.86 us | 24.21 us | 24.28 us | +0.21 us | +0.45 us | +0.20 us | -0.01 us | 8 of 24 | yes (2) |
| 9 | 23.92 us | 23.60 us | 24.50 us | 23.71 us | +0.21 us | -0.13 us | -0.32 us | -0.39 us | 15 of 24 | yes (1) |
| 10 | 24.23 us | 25.00 us | 24.65 us | 24.61 us | +0.46 us | +0.42 us | -0.13 us | +0.54 us | 14 of 24 | yes (1) |
| **pooled** | 23.77 us | 23.74 us | 24.04 us | 23.81 us | **+0.30 us** [+0.18, +0.38] | **+0.18 us** [+0.08, +0.26] | **-0.14 us** [-0.21, -0.06] | **-0.08 us** [-0.15, +0.01] | 143 of 240 | 10 clean |

#### update-lit / literal

| Lifetime | A | A' | B | C | B - A | D - A | D - B | A' - A (floor) | blocks D < B | clean (attempts) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 20.52 us | 20.55 us | 20.52 us | 20.84 us | +0.02 us | +0.11 us | +0.07 us | +0.02 us | 9 of 24 | yes (1) |
| 2 | 22.15 us | 22.40 us | 21.95 us | 22.17 us | -0.30 us | -0.03 us | +0.19 us | +0.09 us | 7 of 24 | yes (1) |
| 3 | 20.69 us | 20.55 us | 20.61 us | 20.54 us | -0.13 us | -0.08 us | -0.01 us | -0.19 us | 12 of 24 | yes (1) |
| 4 | 20.70 us | 20.61 us | 20.52 us | 20.89 us | -0.22 us | -0.06 us | +0.21 us | -0.04 us | 6 of 24 | yes (1) |
| 5 | 21.33 us | 21.43 us | 21.30 us | 21.29 us | -0.07 us | +0.03 us | -0.01 us | +0.05 us | 12 of 24 | yes (1) |
| 6 | 20.78 us | 20.85 us | 20.85 us | 20.86 us | -0.11 us | +0.05 us | +0.22 us | -0.01 us | 7 of 24 | yes (1) |
| 7 | 21.69 us | 21.75 us | 21.75 us | 21.95 us | +0.00 us | +0.19 us | +0.14 us | +0.02 us | 4 of 24 | yes (1) |
| 8 | 20.59 us | 20.65 us | 20.59 us | 20.69 us | +0.01 us | +0.05 us | +0.07 us | +0.11 us | 7 of 24 | yes (1) |
| 9 | 22.29 us | 21.93 us | 22.22 us | 22.38 us | -0.07 us | -0.07 us | +0.07 us | -0.15 us | 10 of 24 | yes (1) |
| 10 | 22.06 us | 22.22 us | 22.05 us | 22.05 us | +0.05 us | +0.18 us | -0.01 us | +0.21 us | 12 of 24 | yes (1) |
| **pooled** | 21.28 us | 21.30 us | 21.24 us | 21.37 us | **-0.05 us** [-0.11, -0.01] | **+0.03 us** [-0.01, +0.08] | **+0.08 us** [+0.04, +0.13] | **+0.01 us** [-0.06, +0.05] | 86 of 240 | 10 clean |

#### update-lit / prepared

| Lifetime | A | A' | B | C | B - A | D - A | D - B | A' - A (floor) | blocks D < B | clean (attempts) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 19.90 us | 19.82 us | 19.87 us | 19.82 us | +0.00 us | -0.07 us | +0.02 us | -0.06 us | 11 of 24 | yes (1) |
| 2 | 21.20 us | 21.26 us | 21.30 us | 21.09 us | -0.02 us | -0.04 us | +0.01 us | +0.07 us | 12 of 24 | yes (1) |
| 3 | 19.92 us | 19.78 us | 19.73 us | 20.04 us | -0.21 us | +0.14 us | +0.36 us | -0.15 us | 4 of 24 | yes (1) |
| 4 | 19.91 us | 19.98 us | 19.69 us | 20.01 us | -0.09 us | +0.08 us | +0.23 us | -0.05 us | 4 of 24 | yes (1) |
| 5 | 20.72 us | 20.53 us | 20.55 us | 20.56 us | -0.10 us | -0.04 us | +0.06 us | -0.02 us | 10 of 24 | yes (1) |
| 6 | 20.44 us | 20.91 us | 20.28 us | 20.74 us | -0.05 us | +0.08 us | +0.25 us | +0.03 us | 8 of 24 | yes (1) |
| 7 | 21.86 us | 21.29 us | 21.18 us | 21.34 us | -0.11 us | -0.07 us | +0.19 us | -0.09 us | 6 of 24 | yes (2) |
| 8 | 19.99 us | 19.96 us | 19.86 us | 19.96 us | -0.03 us | +0.05 us | +0.14 us | -0.04 us | 6 of 24 | yes (1) |
| 9 | 21.35 us | 21.18 us | 21.17 us | 21.05 us | -0.22 us | -0.27 us | -0.07 us | -0.22 us | 14 of 24 | yes (1) |
| 10 | 21.15 us | 21.25 us | 21.02 us | 21.11 us | -0.08 us | +0.07 us | +0.10 us | +0.06 us | 6 of 24 | yes (2) |
| **pooled** | 20.64 us | 20.60 us | 20.47 us | 20.57 us | **-0.08 us** [-0.13, -0.03] | **+0.01 us** [-0.04, +0.06] | **+0.11 us** [+0.06, +0.14] | **-0.05 us** [-0.10, +0.02] | 81 of 240 | 10 clean |

#### insert / literal

| Lifetime | A | A' | B | C | B - A | D - A | D - B | A' - A (floor) | blocks D < B | clean (attempts) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 19.77 us | 19.56 us | 19.57 us | 19.58 us | -0.22 us | -0.12 us | +0.08 us | -0.22 us | 8 of 24 | yes (1) |
| 2 | 20.75 us | 20.44 us | 20.99 us | 20.62 us | -0.04 us | -0.11 us | -0.04 us | -0.22 us | 13 of 24 | NO (1) |
| 3 | 19.69 us | 19.50 us | 19.82 us | 19.54 us | +0.02 us | +0.08 us | +0.24 us | -0.17 us | 8 of 24 | yes (1) |
| 4 | 19.65 us | 19.53 us | 19.45 us | 19.78 us | -0.29 us | -0.06 us | +0.11 us | -0.19 us | 8 of 24 | yes (1) |
| 5 | 20.69 us | 20.71 us | 20.55 us | 20.64 us | -0.10 us | -0.12 us | +0.09 us | -0.21 us | 8 of 24 | yes (1) |
| 6 | 19.71 us | 19.44 us | 19.64 us | 19.91 us | +0.06 us | +0.19 us | +0.19 us | -0.24 us | 4 of 24 | yes (1) |
| 7 | 19.83 us | 19.55 us | 19.71 us | 19.77 us | -0.01 us | -0.08 us | +0.12 us | -0.21 us | 10 of 24 | yes (1) |
| 8 | 19.60 us | 19.59 us | 19.71 us | 19.67 us | +0.23 us | +0.24 us | +0.05 us | +0.04 us | 10 of 24 | yes (1) |
| 9 | 20.81 us | 20.68 us | 20.82 us | 20.76 us | -0.03 us | +0.05 us | +0.04 us | -0.12 us | 10 of 24 | NO (1) |
| 10 | 19.94 us | 19.77 us | 20.21 us | 20.30 us | +0.18 us | +0.22 us | +0.07 us | +0.09 us | 11 of 24 | yes (1) |
| **pooled** | 19.86 us | 19.71 us | 19.83 us | 19.90 us | **-0.05 us** [-0.15, +0.05] | **+0.05 us** [-0.03, +0.08] | **+0.11 us** [+0.06, +0.18] | **-0.18 us** [-0.22, -0.11] | 67 of 192 | 8 clean |

#### insert / prepared

| Lifetime | A | A' | B | C | B - A | D - A | D - B | A' - A (floor) | blocks D < B | clean (attempts) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 18.79 us | 18.63 us | 18.60 us | 18.59 us | -0.18 us | -0.17 us | +0.04 us | -0.14 us | 11 of 24 | yes (1) |
| 2 | 19.94 us | 19.82 us | 19.83 us | 19.98 us | -0.19 us | -0.03 us | +0.01 us | -0.08 us | 10 of 24 | yes (1) |
| 3 | 19.09 us | 18.97 us | 18.84 us | 18.99 us | -0.13 us | -0.08 us | +0.15 us | -0.29 us | 10 of 24 | yes (1) |
| 4 | 18.68 us | 18.71 us | 18.65 us | 18.78 us | -0.12 us | +0.13 us | +0.13 us | +0.03 us | 6 of 24 | yes (1) |
| 5 | 20.05 us | 19.67 us | 19.90 us | 19.99 us | -0.22 us | +0.03 us | +0.09 us | -0.30 us | 7 of 24 | yes (1) |
| 6 | 19.54 us | 19.11 us | 19.07 us | 19.52 us | -0.07 us | +0.15 us | +0.25 us | -0.31 us | 5 of 24 | yes (1) |
| 7 | 19.59 us | 19.33 us | 19.49 us | 19.63 us | -0.13 us | -0.02 us | +0.16 us | -0.29 us | 4 of 24 | yes (1) |
| 8 | 19.14 us | 19.70 us | 19.18 us | 19.30 us | +0.15 us | +0.14 us | +0.14 us | +0.07 us | 9 of 24 | yes (1) |
| 9 | 19.54 us | 19.74 us | 19.51 us | 19.77 us | +0.14 us | +0.01 us | +0.05 us | +0.03 us | 10 of 24 | yes (1) |
| 10 | 18.35 us | 18.40 us | 18.55 us | 18.72 us | +0.11 us | +0.26 us | +0.11 us | +0.06 us | 6 of 24 | yes (1) |
| **pooled** | 19.27 us | 19.21 us | 19.16 us | 19.33 us | **-0.09 us** [-0.12, -0.03] | **+0.04 us** [-0.02, +0.11] | **+0.11 us** [+0.06, +0.15] | **-0.12 us** [-0.17, -0.07] | 78 of 240 | 10 clean |

### Latency and throughput (driver-side)

QPS is derived (`1,000,000 / mean µs` of a serial single-connection driver);
each figure is the median over lifetimes of the lifetime's per-arm statistic;
the driver emits p0, p25, p50, p90 and p99 (no p95) and no wait-event
breakdown. 120,000 statements an arm and cell (`insert literal` of lifetimes 8 and
10, unclean, is dropped from the paired tables only; this table keeps it). On `select-pk literal` the server is on-CPU about 21 µs of a client mean
of about 54 µs; the rest is the client's CPU and the socket; `relaxed` has no
fsync and one connection no lock wait, so no per-wait-type share is booked.

| Cell | Arm | statements | QPS (1e6 / mean) | p0 | p25 | p50 | p90 | p99 | server CPU | driver CPU |
|---|---|---|---|---|---|---|---|---|---|---|
| select-pk literal | A | 120,000 | 18,590 qps | 47.7 us | 49.6 us | 50.7 us | 58.1 us | 72.9 us | 20.79 us | 31.98 us |
| select-pk literal | A' | 120,000 | 18,456 qps | 47.5 us | 49.5 us | 50.8 us | 58.2 us | 74.9 us | 20.93 us | 31.98 us |
| select-pk literal | B | 120,000 | 18,333 qps | 47.8 us | 49.8 us | 50.8 us | 59.0 us | 76.2 us | 21.16 us | 32.07 us |
| select-pk literal | D | 120,000 | 18,485 qps | 47.8 us | 49.8 us | 50.8 us | 58.4 us | 74.4 us | 21.20 us | 31.90 us |
| select-pk prepared | A | 120,000 | 19,402 qps | 44.1 us | 46.2 us | 47.4 us | 57.6 us | 74.7 us | 20.75 us | 29.38 us |
| select-pk prepared | A' | 120,000 | 19,458 qps | 44.3 us | 46.0 us | 47.4 us | 57.2 us | 73.2 us | 20.71 us | 29.11 us |
| select-pk prepared | B | 120,000 | 19,308 qps | 44.4 us | 46.2 us | 47.5 us | 57.8 us | 74.4 us | 20.89 us | 29.31 us |
| select-pk prepared | D | 120,000 | 19,265 qps | 44.1 us | 46.1 us | 47.3 us | 57.6 us | 76.6 us | 20.86 us | 29.17 us |
| select-index literal | A | 120,000 | 17,308 qps | 49.5 us | 52.7 us | 54.0 us | 66.0 us | 90.3 us | 24.11 us | 32.55 us |
| select-index literal | A' | 120,000 | 17,304 qps | 49.4 us | 52.5 us | 54.0 us | 66.2 us | 91.1 us | 24.05 us | 32.77 us |
| select-index literal | B | 120,000 | 17,149 qps | 49.9 us | 53.0 us | 54.4 us | 66.8 us | 95.3 us | 24.56 us | 32.58 us |
| select-index literal | D | 120,000 | 17,195 qps | 49.6 us | 52.7 us | 54.2 us | 66.7 us | 98.9 us | 24.45 us | 32.41 us |
| select-index prepared | A | 120,000 | 18,308 qps | 46.1 us | 48.8 us | 50.4 us | 61.2 us | 81.8 us | 23.94 us | 29.10 us |
| select-index prepared | A' | 120,000 | 18,151 qps | 46.2 us | 48.8 us | 50.3 us | 61.0 us | 83.3 us | 23.73 us | 29.12 us |
| select-index prepared | B | 120,000 | 18,253 qps | 46.1 us | 49.0 us | 50.6 us | 62.0 us | 82.3 us | 24.11 us | 29.19 us |
| select-index prepared | D | 120,000 | 18,300 qps | 46.0 us | 48.9 us | 50.3 us | 61.4 us | 82.5 us | 23.79 us | 29.21 us |
| update-lit literal | A | 120,000 | 21,613 qps | 36.1 us | 41.3 us | 42.1 us | 50.5 us | 68.1 us | 21.05 us | 23.71 us |
| update-lit literal | A' | 120,000 | 21,506 qps | 36.2 us | 41.2 us | 42.1 us | 52.3 us | 69.9 us | 21.14 us | 23.73 us |
| update-lit literal | B | 120,000 | 21,674 qps | 36.7 us | 41.1 us | 42.0 us | 51.2 us | 70.5 us | 21.08 us | 23.66 us |
| update-lit literal | D | 120,000 | 21,706 qps | 36.0 us | 41.2 us | 42.0 us | 51.1 us | 69.7 us | 21.09 us | 23.74 us |
| update-lit prepared | A | 120,000 | 23,387 qps | 34.2 us | 38.1 us | 38.7 us | 48.0 us | 75.7 us | 20.58 us | 21.01 us |
| update-lit prepared | A' | 120,000 | 23,254 qps | 34.3 us | 38.1 us | 38.7 us | 48.6 us | 80.0 us | 20.72 us | 21.00 us |
| update-lit prepared | B | 120,000 | 23,193 qps | 34.6 us | 38.0 us | 38.6 us | 48.0 us | 80.1 us | 20.41 us | 21.17 us |
| update-lit prepared | D | 120,000 | 23,327 qps | 34.2 us | 38.1 us | 38.8 us | 48.2 us | 75.3 us | 20.65 us | 21.03 us |
| insert literal | A | 120,000 | 22,232 qps | 37.9 us | 40.2 us | 41.0 us | 48.8 us | 67.8 us | 19.80 us | 23.56 us |
| insert literal | A' | 120,000 | 22,409 qps | 38.5 us | 40.2 us | 40.8 us | 49.4 us | 67.5 us | 19.58 us | 23.63 us |
| insert literal | B | 120,000 | 22,409 qps | 38.1 us | 40.1 us | 40.8 us | 49.1 us | 69.1 us | 19.76 us | 23.49 us |
| insert literal | D | 120,000 | 22,175 qps | 38.7 us | 40.3 us | 40.9 us | 49.4 us | 67.3 us | 19.85 us | 23.52 us |
| insert prepared | A | 120,000 | 23,992 qps | 35.0 us | 37.3 us | 38.0 us | 45.9 us | 64.4 us | 19.34 us | 21.11 us |
| insert prepared | A' | 120,000 | 24,013 qps | 35.1 us | 37.2 us | 38.0 us | 46.9 us | 65.4 us | 19.22 us | 21.08 us |
| insert prepared | B | 120,000 | 24,076 qps | 35.2 us | 37.2 us | 37.9 us | 46.1 us | 64.3 us | 19.12 us | 21.07 us |
| insert prepared | D | 120,000 | 24,022 qps | 35.4 us | 37.3 us | 38.0 us | 45.8 us | 66.2 us | 19.41 us | 21.13 us |

The percentiles and throughput do not separate the arms; the 0.1 to 0.4 µs
under discussion is below 2 % of the round trip.

## Against the bar

| Cell | DF-S1 | D - A | All of DF | Against 0.5 µs |
|---|---|---|---|---|
| tuple key read | +0.44 µs [+0.27, +0.54] (the bisect: `group`, its own driver, M - X pooled over four lifetimes) | +0.07 µs [+0.02, +0.10] | about +0.51 µs, naive interval +0.29 to +0.64 | **not resolved**, central estimate 0.01 µs over |
| index-served read | not measured on this shape | +0.18 µs [+0.12, +0.25] | not stated; at least +0.18 µs | **not resolved**; DF-S2 to S4 alone are 36 % of the bar |
| `UPDATE`, `INSERT` | write share not measured | +0.02 µs [-0.01, +0.06]; +0.04 µs [+0.00, +0.08] | not stated | **not resolved** |

The two terms come from different drivers and durabilities, and a binary's layout
moves a result by 0.1 to 0.3 µs, so they are not strictly additive; and D - A is
taken against an A that is the slower of two of its binary here. This
file takes no decision on the bar (DF-Q10 (a) sends a part over it back to the
operator).

## Time spent

| What | Time |
|---|---|
| Building D | 78 s (08:25:28 to 08:26:46) |
| Running, ten four-arm lifetimes | 13 min 4 s (08:26:53 to 08:39:57) |
| Held by the process-level quiet gate | none |
| Cell gate | 416 s in total, of which 160 s are the driver's 2 s window over 80 cells and the rest were waits for a quiet window before some cells |
| Whole round | about 15 min, inside the 20 minute budget |

## What was cut

Nothing from the design: ten lifetimes, four cells, both paths. As asked, no `cores = 8`
and no row-count sweep (B's, in the first file, stands for the point statements
not scaling with rows; D was not swept). Not measured: `strict` durability, scans,
multi-statement transactions, and DF-S1's share on an index-served read or a
write.

## What the close says about the engine

DF's statistics cost a point statement a small, per-statement, row-count-independent
amount of server CPU, and it is concentrated on reads: DF-S1's sample pair and
stamps on every statement, and on an index-served read a fold that D's cache
halves. What this file adds to the other three is that the one structural cost
that remained in B and was not a layout artefact - a latch, a hash lookup
and a clock read per index run - was real (-0.18 µs when taken out) and
removable without moving a result, while the session sample's walk, which the
remedy tried to cheapen, was not where the cost was. After the cache a tuple
key read pays about +0.51 µs for all of DF, a half-microsecond that the
measurement can neither place under the bar nor over it, and an index-served
read pays at least +0.18 µs beyond DF-S1.
