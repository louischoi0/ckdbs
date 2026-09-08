# AM-S6 — M0 against the present engine, interleaved, and what it could not measure

**The stage's own question is unanswered at `cores = 8`, and answered
"no difference within about 1%" at `cores = 1`.** That is the result, and
this document's main content is why: on this host the eight-core cells
have a run-to-run spread of **46% to 80%** on *both* engines, which is
larger than any difference between them. No amount of reading these
numbers produces the delta AM-S6 was written to produce; a different
instrument does.

AM-S6 (`instructions/v3.0.0/workorder-am-m1-shared-pool.md`) asks for
"the baseline against AL-S8's, same cells, same host", and AW's §0 item 2
lifted the interleaved-A/B suspension for this stage alone because "D15
permits a delta only within one engine on one host, and AM-S6's whole
deliverable is that delta". This is that A/B.

**Two scope corrections, stated before the numbers.**

1. **This is not M1's cost.** AM-S6 was scoped when `HEAD` was M1. It is
   not any more: the B arm carries M1 (the shared pool, page latches, the
   free-map latch, the scan-ring port), **and** AN's instance read view,
   AO's lock family with its cross-core waits and deadlock detector, AU's
   waker table, AV's rig, SUS-1, and the device-growth change. A delta
   here is M0-to-now and attributes to nothing narrower.
2. **The comparator is the BTREE re-baseline, not AL-S8's files.** SUS-1
   refuses `CREATE TABLE … HEAP`, so the B arm cannot run AL-S8's schema
   at all; both arms here run the BTREE drivers of AS-Q6's mark
   (`raft-marks-2026-09-08-as-q6.md`), and
   `results-scenario0-stockmarket-btree-v2.7.0-157-gf6ed10c.md` and its
   scenario2 sibling are the standalone form of the A arm.

## 1. Stamp

| Field | Value |
|---|---|
| Date/time | 2026-09-08, 04:39:56–04:50:37 UTC (passes 1–2) and 04:52–05:0x UTC (passes 3–4) |
| Worktree | the run used the main checkout's drivers on `main` at `cd5c60e`; the B binary was built in `am-s6-baseline` at `1e7148f`; the write-up is on `am-s6-results` |
| **A arm** | `f6ed10c` = `v2.7.0-157-gf6ed10c`, AR0 M0's engine. `/home/cdkbs/bench-runs/am-s6-ab/kds_server_A`, `sha256 2ab1960bc056e7cc5c59be4946a2cf1250b4e65b941934c96ebf736e80435af3` — **the hash AL-S8's own stamp records** |
| **B arm** | `1e7148f` = `v2.7.0-286-g1e7148f`. `/home/cdkbs/bench-runs/am-s6-ab/kds_server_B`, `sha256 465b925f85c780468d753b3fa013d1675325441e79a7fee1b2952da57fde1a00`, Release, built at that commit (source mtime 04:37:20 UTC) |
| Drivers | `cd5c60e`, BTREE, identical for both arms; AL-S8's arguments verbatim |
| Device | `/home/cdkbs`, `ext4`, `/dev/root`, 52% used. Not tmpfs |
| Host | 8 logical CPUs, AMD EPYC 9V74, 1 socket × 4 cores × 2 threads/core — AL-S8's host, which is the condition AW-R1 stopped the stage on twice |
| Server config | `log_level = warn`, `placement = namespace`, defaults otherwise; `cores`, `durability` and `peer_listeners` per cell |

**The host was settled before the run.** A first launch was aborted and
discarded because it began 16 seconds after the B arm's build finished,
with a one-minute load of 2.69; the run below started at load 0.34 with
no build or test process. Nothing from the aborted launch is in this
document or its archive.

## 2. What was run

**64 runs**: the eight AL-S8 cells × two arms × **four passes**. Passes
alternate which arm goes first — 1 and 3 run A then B, 2 and 4 run B then
A — so neither arm systematically warms the page cache for the other.
Every run got its own server process from its own hashed binary, its own
data file, and its own precheck (`/proc/loadavg` plus a build check;
none found a build, in any of the 64).

Two passes were run first and **two more were added because two passes
could not resolve the cells** — pass 1 and pass 2 disagreed in sign on
two of eight, and disagreed in magnitude by up to 12× on others. The
extra passes did not resolve them either; §4 is why.

## 3. Throughput — four runs per arm per cell

TPS, in pass order 1–4. `med` is the median of four; `spr` is
`max/min − 1` for that arm's own four runs.

| Cell | A runs | A med | A spr | B runs | B med | B spr | B/A med |
|---|---|---|---|---|---|---|---|
| `s0-c1-g` | 658.4 752.6 773.9 734.9 | 743.8 | 17.5% | 742.2 756.8 728.2 757.6 | 749.5 | 4.0% | **+0.8%** |
| `s0-c8-g` | 469.0 781.2 551.0 554.1 | 552.5 | **66.6%** | 538.6 535.5 783.0 545.3 | 542.0 | **46.2%** | −1.9% |
| `s0-c1-s` | 202.1 205.7 209.9 205.5 | 205.6 | 3.9% | 206.6 205.6 207.6 203.0 | 206.1 | 2.3% | **+0.2%** |
| `s0-c8-s` | 216.3 210.9 210.1 207.7 | 210.5 | 4.1% | 210.5 210.7 214.9 214.3 | 212.5 | 2.1% | +1.0% |
| `s2-c1-g` | 627.3 602.6 616.4 612.1 | 614.2 | 4.1% | 616.9 580.5 608.1 619.3 | 612.5 | 6.7% | **−0.3%** |
| `s2-c8-g` | 325.2 194.1 349.1 317.0 | 321.1 | **79.9%** | 389.7 326.0 317.2 341.7 | 333.9 | 22.9% | +4.0% |
| `s2-c1-s` | 549.5 541.7 560.8 560.5 | 555.0 | 3.5% | 572.2 542.0 559.3 566.2 | 562.8 | 5.6% | **+1.4%** |
| `s2-c8-s` | 304.7 293.1 322.4 307.6 | 306.1 | 10.0% | 324.1 326.8 321.4 337.9 | 325.5 | 5.1% | +6.3% |

**Every delta is smaller than at least one arm's own spread.** Not one
cell resolves.

**And the per-pass pairwise sign is mixed in every cell but one.** Each
pass gives a paired A-and-B measurement minutes apart; here is B/A − 1
per pass:

| Cell | p1 | p2 | p3 | p4 | |
|---|---|---|---|---|---|
| `s0-c1-g` | +12.7% | +0.6% | −5.9% | +3.1% | mixed |
| `s0-c8-g` | +14.8% | −31.5% | +42.1% | −1.6% | mixed |
| `s0-c1-s` | +2.2% | −0.0% | −1.1% | −1.2% | mixed |
| `s0-c8-s` | −2.7% | −0.1% | +2.3% | +3.2% | mixed |
| `s2-c1-g` | −1.7% | −3.7% | −1.3% | +1.2% | mixed |
| `s2-c8-g` | +19.8% | +68.0% | −9.1% | +7.8% | mixed |
| `s2-c1-s` | +4.1% | +0.1% | −0.3% | +1.0% | mixed |
| `s2-c8-s` | +6.4% | +11.5% | −0.3% | +9.9% | mixed |

Had this stage run two passes and stopped — which is what it was going to
do — `s0-c1-g` would have read **+12.7%** and `s2-c8-g` **+19.8%**, and
both are artefacts. The third and fourth passes are what make that
visible.

## 4. The finding: `cores = 8` is not a stable measurement on this host

The spreads in §3 sort cleanly by core count, on **both** engines:

| | TPS spread, A | TPS spread, B |
|---|---|---|
| the four `cores = 1` cells | 3.5%, 3.9%, 4.1%, 17.5% | 2.3%, 4.0%, 5.6%, 6.7% |
| the four `cores = 8` cells | 4.1%, 10.0%, **66.6%**, **79.9%** | 2.1%, 5.1%, 22.9%, **46.2%** |

`s0-c8-g` is **bimodal on both arms**: A ran 469, 781, 551, 554 and B ran
539, 536, 783, 545. One run in four, on each engine, lands near 780 and
the rest near 540. `s2-c8-g` is the same shape, 194–349 on A and 317–390
on B. The high mode is not an engine property — both engines reach it,
and neither reaches it reliably.

**What it most likely is, stated as a hypothesis and not measured**: at
`cores = 8` the engine pins eight reactors onto 4 physical cores × 2
SMT threads, and AL-S8's own config table already warned that pinned
reactors "serialize whole workloads behind each other". Which reactors
share a physical core with which, and where the eight trader or booker
client processes land against them, is decided per run by the OS. That
would produce exactly a bimodal per-run distribution that no amount of
in-run averaging removes. **Testing it needs a run that records
placement, which this one does not.**

**The consequence for this stage**: at `cores = 8`, four samples per arm
cannot see a difference below roughly 50%, and no plausible M0-to-now
difference is that large. The `cores = 8` half of AM-S6 is **not
measurable with this instrument**, and saying so is the honest
deliverable.

## 5. Median latency — the same verdict, and the one near-miss

Median-of-four p50, `txn` for scenario0 and `booking` for scenario2, with
each arm's own spread over its four runs:

| Cell | A p50 med | A spr | B p50 med | B spr | B/A |
|---|---|---|---|---|---|
| `s0-c1-g` | 10,094 | 6.2% | 10,043 | 3.2% | −0.5% |
| `s0-c8-g` | 9,333 | 5.3% | 9,391 | 0.4% | +0.6% |
| `s0-c1-s` | 32,229 | 3.3% | 32,501 | 2.7% | +0.8% |
| `s0-c8-s` | 33,124 | 36.7% | 31,195 | 14.1% | −5.8% |
| `s2-c1-g` | 10,122 | 2.9% | 10,235 | 2.9% | +1.1% |
| `s2-c8-g` | 16,895 | 79.1% | 14,184 | 71.1% | −16.0% |
| `s2-c1-s` | 10,684 | 2.6% | 10,607 | 5.0% | −0.7% |
| `s2-c8-s` | 16,879 | 28.7% | 14,338 | 24.0% | **−15.1%** |

The four `cores = 1` cells agree with §3 to within 1.1%: **the two
engines are the same engine at one core**, in throughput and in median
latency alike.

**`s2-c8-s` is the one near-miss and it is recorded as that, not as a
result.** Its pairwise ratio is below 1 in **all four passes** — 0.955,
0.836, 0.959, 0.665 — which is the only cell in either table whose sign
never flips, and the median is 15% lower on the current engine. But its
arms' own spreads are 29% and 24%, larger than the effect, so four
samples cannot call it. A sign test over four paired trials is p = 0.0625
one-tailed at best, which is not a finding either. **What it is: the one
place worth pointing a better instrument at**, and it is the contended
eight-core freight cell whose `COMMIT` the BTREE baseline identified as
carrying the whole core-count penalty.

## 6. Correctness

Every one of the 64 runs completed `rc=0` and committed its full target —
5,000 transactions per scenario0 run, 3,000 bookings per scenario2 run —
with zero torn statements. Scenario2's `--verify` failures persist on
both arms at the rate the BTREE baseline recorded and AL-S8 recorded
before it; the engines do not differ there either, which is a further
statement that the invariant finding is neither M0's nor the present
engine's alone.

## 7. What this run says

- **At `cores = 1`, M1 and everything since it costs nothing measurable.**
  Four cells, two instruments, all within 1.4%. For a change set that
  added a page latch on every frame, a structure latch on the frame
  table, a latch on the free map, an instance visibility window read by
  every reader, and a lock table on every dispatcher, "no measurable cost
  at one core" is the claim AR0's `cores = 1`-byte-identical rule wanted
  and the first time it has been measured end to end.
- **At `cores = 8`, this host cannot answer the question**, and the
  reason is a bimodal per-run distribution both engines share.
- **AM-S6 as written is therefore complete and its delta is not
  produced.** The stage asked for a number; the honest answer is that the
  number is below this instrument's floor at one core and unmeasurable at
  eight.

## 8. What would resolve `cores = 8`

In the order CLA would try them, none of them ordered:

1. **Record placement per run** — which reactor is on which CPU, and
   where the client processes land — and check whether the two modes
   correspond to two placements. That is a driver and harness change, so
   it is a stage of its own and not a measurement.
2. **Pin the reactors deterministically** (`taskset` on the server, or a
   config that names the CPU set) so placement stops being a per-run
   draw. This changes the thing being measured and must be stated
   wherever its numbers appear.
3. **More samples**, if 1 and 2 are refused: at a 66% spread and a
   plausible effect under 10%, resolving it needs on the order of a
   hundred paired runs per cell rather than four. At ~20 s a run that is
   about an hour per cell, which is affordable and is the brute-force
   answer.
4. **A longer per-cell run** (a larger `--txn-per-user`), which averages
   within a run rather than across runs — but only helps if the mode is
   drawn per run rather than held for the run's life, which 1 would tell.

## 9. Noise floor

**Established, and it is this document's most reusable output.** Per arm,
over four runs on a settled host:

| | one core | eight cores |
|---|---|---|
| TPS spread | **2.3%–6.7%**, with one 17.5% outlier (`s0-c1-g` A, whose 658.4 was the session's first run of all) | **2.1%–79.9%** |
| p50 spread | 2.6%–6.2% | 0.4%–79.1% |

**A `cores = 1` delta below ~7% is noise on this host; a `cores = 8`
delta below ~50% is noise.** That applies to every past single-run
eight-core number in `bench/v3.0.0/`, the BTREE baseline's included —
its `s0-c8-g` of 794.3 is the high mode of a bimodal draw, and both
baseline files' §8 now say so.

Archive: `bench/v3.0.0/archive/am-s6-v2.7.0-286-g1e7148f/` — 64 per-run
JSON summaries, 64 prechecks, the timeline and both run scripts. Driver
stdout is not archived; the JSON carries the same phase data.
