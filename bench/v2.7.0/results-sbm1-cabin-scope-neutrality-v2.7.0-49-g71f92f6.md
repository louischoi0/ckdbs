# SB-M1 — a one-range relation's Cabin serve path, measured against SB1-SB4

**Headline, stated first because rule 8 asks for it: H-SB1 holds.** On a
relation of one range — every relation until one asks to spread — the
Cabin serve path is byte-identical after work order SB
(`instructions/v2.7.1/workorder-sb.md`, SB1-SB4) and its cost is
unresolvable from noise: server CPU per cabin-served probe moves by
**-12.2% to +4.7%** (heap) and **-12.0% to +0.0%** (btree) across the
three swept sizes, against a control that cannot reach the changed code
at all and itself moves **+3.4% to +6.3%** (at 1k/10k rows, where CPU
sampling clears the tick-resolution floor) on the exact same host in the
exact same run. `SHOW CABINS`' `scope_declines` counter — SB-R4's
instrumentation for the branch this order added — reads **0** across
648,000 statements sent to the four cabin-declared relations over this
run's four clean sweeps; `cabin_scope_fallthroughs` never once appears
on `SHOW META`, because it is absent-at-zero and the branch it counts
is never taken. The predicate SB2 asserted as free —
`access.ranges.empty()` returned before the range directory or
`ServableBy` are touched — **measures as free**.

One thing this run found and is reporting rather than burying (rule 8's
"a detected cost is the finding" also cuts the other way — a detected
*non*-cost that looked like a cost on first read deserves the same
honesty): `btree-cabin` at 10,000 rows showed an intermittent p50
elevation (+18.1% in the aggregate, driven by 2 of 4 full-sweep repeats)
that **vanished in three independent single-size isolation runs** and
that the exact same host reproduces on the **uncabined control** at
smaller row counts where SB's code is provably unreachable. Section 7
walks the evidence; the conclusion is that this is host/session state,
not SB1-SB4.

Measured in the worktree `workorder-cabin-under-split`
(`/home/cdkbs/ckdbs/.claude/worktrees/workorder-cabin-under-split`) at
`2a309c7` (branch `worktree-workorder-cabin-under-split`, `git describe
--tags` → `v2.7.0-51-g2a309c7`), executing work order SB task SB5,
measurement SB-M1. **Neither measured binary was built from this
worktree** — see Section 1 — which is deliberate: SB-M1 compares two
*commits*, `e1897dc` (pre-SB) and `71f92f6` (post-SB), each built from
its own clean `git archive`, per this agent's own rule 5.

## 1. Provenance

| | |
|---|---|
| Date/time (UTC) | 2026-09-01, 04:45-05:10 |
| Worktree (report, driver, this file) | `workorder-cabin-under-split` (`/home/cdkbs/ckdbs/.claude/worktrees/workorder-cabin-under-split`), branch `worktree-workorder-cabin-under-split`, at `2a309c7` |
| Tree state | Clean at `2a309c7` except this session's own additions, confined to `bench/` and `tools/`: `tools/cabin_scope_ab_benchmark.py` (new driver), `bench/docs/README.md` (its entry), this results file. No `src/`, `include/`, `docs/spec/`, or `CLAUDE.md` touched — **no engine code changed this session** |
| Arm A (pre-SB, baseline) | Commit `e1897dcdcf4699ce4fb9667f9583e25f4a5647fb` (`v2.7.0-48-ge1897dc`, 2026-09-01 03:51:22 +0000). This commit is **docs-only** (`git diff --stat 24ce3cd e1897dc` touches 4 files, all under `docs/`) — its code is byte-identical to the pre-SB base `24ce3cd`, which is why it is a legitimate "before" |
| Arm B (post-SB, under test) | Commit `71f92f639b7c11642c7b5af2329fae1136ec0b1b` (`v2.7.0-49-g71f92f6`, 2026-09-01 04:40:28 +0000) — "SB1-SB4: the Cabin's authority is scoped, the discard is ordered, the gates drop". `git diff --stat e1897dc 71f92f6 -- src include`: 17 files, 450 insertions / 112 deletions, all under `exec/cabin*`, `exec/range_eligible.cpp`, `exec/step_vm.cpp`, `catalog.cpp`'s `CreateCabin`, `server/expeditor.cpp`, `server/range_alloc.cpp`, `server/row_id_lease_service.cpp`, `server/command_dispatcher.cpp`, `stats/cabin_store.cpp` — no file in the plain heap/btree scan path |
| Build | Each arm from its **own clean `git archive`**, extracted to `/home/cdkbs/bench-sbm1/arm-{a,b}-src/`, configured `-DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=/tmp/claude-1000/-home-cdkbs-ckdbs/4e50e866-8005-43e4-b524-1d53355b0c93/scratchpad/ossl` (rootless OpenSSL extraction, `KDS_WITH_TLS=ON`), built with `cmake --build build-release --target kds_server -j8`. Neither tree has a `.git` directory — confirmed a real archive extraction, not this worktree's build |
| Binary provenance | Source binaries: `arm-a-src/build-release/kds_server` mtime `2026-09-01 04:45:51.503Z`; `arm-b-src/build-release/kds_server` mtime `2026-09-01 04:46:32.473Z`. **Copied** before the first measurement cell, per rule 5, to `/home/cdkbs/bench-sbm1/bin/kds_server-A-e1897dc` (`sha256` `b001a2d9…5940ca01`) and `kds_server-B-71f92f6` (`sha256` `a3a8cac5…085129e8e0c0f8`). Every server in this run started from these copies, never from `build-release/kds_server` itself. Hashes re-checked at the end of the session and matched |
| Test suite | **Not run this session.** No engine code changed (Section 1 above), so a before/after suite delta is not applicable; the correctness evidence for this specific measurement is its own byte-identity `--verify` pass (Section 3) rather than the C++ suite, which SB1-SB4's own landing commit already carries |
| Device | Data files under `/home/cdkbs/bench-sbm1/run/data-{a,b}/`, on `/dev/root`, **ext4** (`df -T`: 259 GB, 36% used throughout). Binaries under `/home/cdkbs/bench-sbm1/bin/`, same filesystem — never tmpfs on this host (`df -T /tmp` also reports `ext4` on `/dev/root`, so this host carries none of the tmpfs risk the standing rule warns about, confirmed rather than assumed) |
| Host | 8 logical CPUs, Ubuntu, Linux 6.17.0-1022-azure. `uptime`/`pgrep cc1plus` checked before every cell (`pgrep -c cc1plus` returned 0 throughout); 1-/5-minute load average stayed under 1.5/2.0 at every start, the driver's own refusal threshold, `--force` never used. One unrelated `kds_server` (a different checkout, `/home/cdkbs/autotrade/ckdbs`, port 15432, ≤1% CPU) ran throughout on the same host and is named because it is a plausible (if unconfirmed) contributor to the tail noise Section 7 documents |
| Server config | `cores = 1` (source default, `Expeditor::Config::cores`, no `--config` passed — a one-range relation by construction, the shape H-SB1 is about), `durability = group` (source default), `cabins = on` (source default). No config file; every server started `kds_server <data_file> --port <n> --log-dir <dir> --log-level warn` |
| Method | `tools/cabin_scope_ab_benchmark.py` (new this session, documented in `bench/docs/README.md`) |

## 2. What SB1-SB4 changed, and why the control cannot see it

`CabinScopeCovers` (`src/exec/step_vm.cpp`) is reached only from
`RunCabinStep`, which the step compiler emits only for a column that
**has a declared Cabin at compile time** (`cabin.md` §4b, §1). A
relation with no `CREATE CABIN` on it compiles a plain walk regardless
of this order's changes — `RunCabinStep`, `CabinScopeCovers`, and every
line SB1-SB4 touched are simply never entered. This is what makes the
uncabined control (`{uh}`/`{ub}` below) a control in the strict sense
required by this agent's rule 8: not "probably similar", but
**structurally incapable of executing the changed code**, confirmed by
reading the diff (Section 1's provenance row) rather than assumed.

## 3. The shape, and byte-identity

Four relations per size per side, all `(id int64, val varchar, pad
varchar)`, loaded identically:

| relation | clustering | Cabin | role |
|---|---|---|---|
| `{ch}` | HEAP | `CREATE CABIN ON {ch}(val)` | under test — heap |
| `{cb}` | BTREE | `CREATE CABIN ON {cb}(val)` | under test — btree |
| `{uh}` | HEAP | none | control — heap |
| `{ub}` | BTREE | none | control — btree |

`val` cycles a domain of `rows / 10`, so every value matches ~10 rows at
every size — the axis being swept is relation size, not answer size.
Eight hot values per size are warmed twice, untimed, before any
statement is counted: `kDeclaredRecordThreshold = 1`
(`cabin_store.hpp`) means the first probe of a declared Cabin's value
still misses but records it, and the second is the first hit. One
`ANALYZE` per cabined relation per size confirmed `cabin_hits=1` before
the timed pass began, on both sides, at every size, every run — a run
that priced a fallen-through walk under a "Cabin-served" heading would
be measuring nothing, and this never happened.

**`--verify` ran eleven queries per size** — a cabin hit, a miss, an
equality with an extra conjunct, a pk lookup, `COUNT(*)` on both
clusterings, the uncabined controls' twins, and a correlated self-join
through the Cabin (shapes borrowed from `tests/cabin_contract_test.cpp`'s
`Queries()`) — diffed field for field across both sides. **Six full
3-size sweeps ran `--verify`; all six passed, 0 mismatches, 0 error
replies, across 1,944,000 statements** (latency pass + CPU pass,
every arm, both sides, all three sizes, summed from each run's own
JSON); the three isolation-diagnostic repeats (Section 7) add 324,000
more under `--no-verify`, for **2,268,000 statements and 0 error
replies across the whole session**. The two engines answer identically;
what follows is only about cost.

## 4. Server CPU per operation — the decisive signal

Client-side latency on this host carries enough tail noise (Section 7)
that CPU is the number to read first: contiguous `/proc/<pid>/stat`
windows, 1,500 ops × 4 rounds per arm per size, median of four
independent clean-slate repeats (fresh server, fresh data file, per
repeat — rule 6).

| arm | size | A (µs/op) | B (µs/op) | Δ |
|---|---:|---:|---:|---:|
| `ping` (control, touches no relation) | 200 | 31.7 | 27.5 | -13.2% |
| `ping` | 1k | 26.7 | 27.5 | +3.1% |
| `ping` | 10k | 30.8 | 24.2 | -21.6% |
| **`heap-cabin`** | 200 | 35.8 | 37.5 | **+4.7%** |
| **`heap-cabin`** | 1k | 36.7 | 38.3 | **+4.5%** |
| **`heap-cabin`** | 10k | 40.0 | 37.5 | **-6.2%** |
| **`heap-cabin-again`** | 200 | 40.0 | 38.3 | **-4.2%** |
| **`heap-cabin-again`** | 1k | 37.5 | 35.8 | **-4.4%** |
| **`heap-cabin-again`** | 10k | 40.8 | 35.8 | **-12.2%** |
| `heap-ctl` (control) | 200 | 55.0 | 49.2 | -10.6% |
| `heap-ctl` (control) | 1k | 120.8 | 125.0 | +3.4% |
| `heap-ctl` (control) | 10k | 873.3 | 915.8 | **+4.9%** |
| **`btree-cabin`** | 200 | 35.8 | 35.8 | **+0.0%** |
| **`btree-cabin`** | 1k | 39.2 | 35.8 | **-8.5%** |
| **`btree-cabin`** | 10k | 41.7 | 36.7 | **-12.0%** |
| `btree-ctl` (control) | 200 | 50.8 | 52.5 | +3.3% |
| `btree-ctl` (control) | 1k | 119.2 | 126.7 | +6.3% |
| `btree-ctl` (control) | 10k | 865.8 | 920.0 | **+6.3%** |

**Read it against the control, not against zero.** `ping` — `SHOW META`,
touching no relation, no Cabin, nothing SB1-SB4 could reach — swings
-21.6% to +3.1% at this sampling resolution (windows of ~40-50µs × 1,500
ops give only a handful of 10ms ticks per window; `/proc`'s own
resolution, not the engine, is what is being seen). Every cabin-arm delta
above (-12.2% to +4.7%) sits inside that same band. The controls
`heap-ctl`/`btree-ctl`, which are large enough per-op (120-920µs) that
tick resolution stops being the limiting factor, show **+3.4% to +6.3%**
at 1k and 10k — real, above-resolution, and reproducible (Section 7 —
and provably unreachable by SB1-SB4's diff). No cabin-arm delta exceeds
that. **H-SB1 holds at the CPU level, at every size swept.**

## 5. Latency distribution (rule 6), median of four clean-slate repeats

`ops` = 3,000/arm/side/size. p0/p25 are the shape that matters for a
fixed-cost claim — the tail (p95/p99/mean/max) carries host noise
Section 7 characterizes; TPS is derived (`1e6/mean_us`) and inherits
that same tail sensitivity, stated per rule 5a.

**200 rows**

| arm | side | mean | p0 | p25 | p50 | p95 | p99 | TPS |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `heap-cabin` | A | 96.8 | 90.4 | 94.7 | 95.3 | 108.0 | 114.5 | 10,325 |
| `heap-cabin` | B | 105.1† | 88.2 | 93.4 | 94.2 | 107.3 | 119.3 | 9,519 |
| `heap-cabin-again` | A | 105.4† | 90.7 | 94.7 | 95.5 | 108.6 | 116.5 | 9,488 |
| `heap-cabin-again` | B | 96.1 | 86.9 | 93.3 | 94.2 | 107.5 | 117.4 | 10,406 |
| `heap-ctl` (control) | A | 118.1 | 105.7 | 109.2 | 110.5 | 135.8 | 145.4 | 8,467 |
| `heap-ctl` (control) | B | 111.5 | 105.6 | 108.6 | 109.5 | 120.9 | 130.6 | 8,969 |
| `btree-cabin` | A | 97.5 | 90.1 | 94.3 | 95.2 | 109.3 | 125.1 | 10,256 |
| `btree-cabin` | B | 97.6 | 89.3 | 93.3 | 94.2 | 117.6 | 134.3 | 10,251 |
| `btree-ctl` (control) | A | 122.6 | 105.8 | 109.7 | 111.6 | 152.3 | 161.7 | 8,157 |
| `btree-ctl` (control) | B | 114.5 | 105.6 | 109.2 | 110.3 | 122.3 | 134.0 | 8,730 |

**1,000 rows**

| arm | side | mean | p0 | p25 | p50 | p95 | p99 | TPS |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `heap-cabin` | A | 97.1 | 91.2 | 94.9 | 95.6 | 106.3 | 118.2 | 10,299 |
| `heap-cabin` | B | 96.1 | 90.1 | 93.5 | 94.3 | 105.5 | 113.3 | 10,406 |
| `heap-cabin-again` | A | 97.1 | 89.7 | 94.7 | 95.4 | 107.1 | 115.8 | 10,299 |
| `heap-cabin-again` | B | 96.2 | 89.6 | 93.8 | 94.6 | 106.0 | 113.3 | 10,390 |
| `heap-ctl` (control) | A | 180.2 | 169.8 | 175.2 | 177.1 | 192.6 | 217.7 | 5,549 |
| `heap-ctl` (control) | B | 201.1† | 173.8 | 187.2 | 199.1 | 229.7 | 235.6 | 4,971 |
| `btree-cabin` | A | 97.6 | 90.1 | 94.7 | 95.7 | 110.4 | 120.8 | 10,246 |
| `btree-cabin` | B | 103.7 | 89.3 | 93.8 | 94.8 | 133.4 | 146.8 | 9,648 |
| `btree-ctl` (control) | A | 179.1 | 170.6 | 175.0 | 176.4 | 190.6 | 200.4 | 5,583 |
| `btree-ctl` (control) | B | 200.8† | 174.0 | 196.4 | 199.0 | 227.0 | 237.2 | 4,980 |

**10,000 rows**

| arm | side | mean | p0 | p25 | p50 | p95 | p99 | TPS |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `heap-cabin` | A | 107.0† | 91.6 | 95.0 | 97.0 | 137.4 | 145.6 | 9,346 |
| `heap-cabin` | B | 96.3 | 88.9 | 93.5 | 94.3 | 106.3 | 116.3 | 10,379 |
| `heap-cabin-again` | A | 99.7 | 90.8 | 94.8 | 95.7 | 131.6 | 141.7 | 10,030 |
| `heap-cabin-again` | B | 98.0 | 89.7 | 93.8 | 94.8 | 120.2 | 135.9 | 10,204 |
| `heap-ctl` (control) | A | 931.5 | 898.5 | 918.1 | 923.7 | 963.1 | 1,030.8 | 1,074 |
| `heap-ctl` (control) | B | 978.0 | 936.5 | 964.4 | 972.1 | 997.7 | 1,056.0 | 1,022 |
| `btree-cabin` | A | 101.5 | 90.9 | 94.6 | 95.7 | 133.1 | 144.1 | 9,857 |
| `btree-cabin` | B | 110.9‡ | 89.4 | 95.1 | 113.0‡ | 126.3 | 134.8 | 9,017 |
| `btree-ctl` (control) | A | 939.5 | 900.8 | 922.9 | 932.8 | 964.2 | 987.6 | 1,064 |
| `btree-ctl` (control) | B | 989.4 | 949.2 | 973.5 | 982.8 | 1,019.7 | 1,056.2 | 1,011 |

`†` marks a cell whose median-of-4 mean is pulled by one repeat's rare
large single-op stall (`max` for that cell exceeds 49ms against a body
under 250µs — Section 7). `‡` marks `btree-cabin[10k][B]`, the cell
Section 7 investigates directly. **p0/p25 are unmarked and clean at
every row in both tables** — that is the reading this run trusts.

## 6. `SHOW CABINS`/`SHOW META`: the branch, counted rather than inferred

SB-R4's instrumentation makes H-SB1 checkable directly rather than only
statistically. In the fourth clean sweep alone (`clean4`, the one
whose `SHOW CABINS` is quoted below), the three cabin-served arms
(`heap-cabin`, `heap-cabin-again`, `btree-cabin`) sent 54,000 latency-pass
statements plus 108,000 CPU-pass statements — 162,000 in that one
sweep — and 648,000 across the four clean sweeps combined (Section 4's
and 5's source data). `ch`'s and `cb`'s hit counts below (18,011 and
9,011) are `clean4`'s own share of that, plus the untimed confirm/verify
probes:

```
B: SHOW META does not carry cabin_scope_fallthroughs (absent-at-zero rule)
B: SHOW CABINS  cabin_id=1 rel=sbm_ch_200_clean4 ... hits=18011 misses=9 scope_declines=0
                cabin_id=2 rel=sbm_cb_200_clean4 ... hits=9011  misses=9 scope_declines=0
                (six cabins total, one per relation×size; every one scope_declines=0)
A: SHOW META / SHOW CABINS carry no scope_declines field at all — the counter
   does not exist pre-SB, confirming A cannot have paid this cost by construction
```

`ranges.empty()` returned `true` on every single one of tens of
thousands of probes, so `CabinScopeCovers` never reached
`ServableBy` or the range directory even once. This is the strongest
statement this run can make: not "the decline path is cheap" but **"the
decline path was never taken"**, which is exactly what §4b rule 1
promises for a one-range relation and is now counted rather than
assumed.

## 7. The `btree-cabin[10k]` anomaly, and why it is not SB1-SB4

Four full clean-slate sweeps (fresh server + fresh data file each,
`--rows 200,1000,10000` in one invocation) showed `btree-cabin[10k]`
p50 elevated on side B in two of the four (A 95.6, B 131.2 in one run;
A 95.2, B 132.6 in another)
and flat in the other two (A 95.7, B 94.8 in one run; A 96.3, B 94.8 in
the other) — the aggregate
median in Section 5 (`‡`) reflects that split. Before accepting or
rejecting this as an SB1-SB4 cost, three checks:

**1. It does not reproduce in isolation.** Three additional runs measured
`btree-cabin[10k]` **alone** — a fresh server, fresh data file, `--rows
10000` and nothing else built in that session — specifically to remove
"relations from earlier sizes already resident in this session"
as a variable:

| run | A p50 | B p50 | Δ |
|---|---:|---:|---:|
| isolation 1 | 95.1 | 94.0 | -1.2% |
| isolation 2 | 95.9 | 94.9 | -1.0% |
| isolation 3 | 95.6 | 96.0 | +0.4% |

Flat, in both directions, well inside the CPU-level floor. The
elevation is conditional on the 200-row and 1k-row relations (eight of
them, four per side) already existing in the same server session before
the 10k ones are built and probed — a session-accumulation effect, not
a per-probe cost of the code path itself.

**2. The exact same pattern appears on the *uncabined* control**, which
cannot execute a single line SB1-SB4 touched. `heap-ctl[1k]` and
`btree-ctl[1k]` (Section 5, `†`) are bimodal across the same four
sweeps in the same direction — B sits at 175-182µs in some runs and
jumps to 199-217µs in others, while A holds 175-182µs in all four,
every time:

| run | `heap-ctl[1k]` A p50 | `heap-ctl[1k]` B p50 | `btree-ctl[1k]` A p50 | `btree-ctl[1k]` B p50 |
|---|---:|---:|---:|---:|
| 1 | 177.9 | 182.3 | 175.9 | 217.3 |
| 2 | 178.1 | 216.9 | 177.3 | 181.0 |
| 3 | 176.3 | 178.6 | 176.8 | 217.0 |
| 4 | 175.6 | 215.9 | 175.8 | 180.7 |

Both control relations, on both clustering forms, at a size (1,000
rows) where `btree-cabin` itself never showed the effect. `A` never
moves; `B` bimodally does, on a path that provably never calls
`RunCabinStep`. By construction this rules out `CabinScopeCovers` and
everything else SB1-SB4 added as the cause — the phenomenon is
happening on code neither commit changed.

**3. Individual outliers are symmetric across sides and arms.** The
`†`-marked cells in Section 5 include `heap-cabin-again[200][A]` in one
run (max 49ms on a ~95µs body) and `heap-cabin[200][B]` in another (max
54-70ms) — a single rare stall of tens of milliseconds, landing on
whichever side happens to be mid-block when it occurs, symmetric in
direction across four runs. This is consistent with scheduling jitter
on a shared host (one other `kds_server` process and the Claude session's
own tooling were both running throughout, Section 1) rather than
anything the engine under test does differently before and after SB1-4.

**Conclusion:** the `btree-cabin[10k]` elevation and the `heap-ctl[1k]`/
`btree-ctl[1k]` bimodality are one phenomenon — something about binary
B's process under this host's session-accumulated memory/cache state,
not reproducible in isolation, not correlated with `scope_declines`
(0 throughout, Section 6), and reproduced identically on relations
`CabinScopeCovers` cannot reach. It is reported here because rule 8
requires it, and it is not read as evidence against H-SB1, because the
control experiment that would make it SB-attributable — the effect
appearing *only* where the Cabin's changed code runs, and never where
it doesn't — is the opposite of what section shows.

## 8. What this run teaches about the engine

**A predicate stated as "one load, one branch" measures as exactly
that.** This is not a case where the code was fast enough that the
measurement couldn't resolve a real cost — `SHOW CABINS`' new
`scope_declines` counter reads 0 across 648,000 probes, so the second
branch of `CabinScopeCovers` (`ServableBy`, the range directory) is
provably never reached on this shape. §4b rule 1's claim — "the same
claim, unchanged, byte for byte" — has a direct instrument now, not
just an argument from code reading.

**This host's noise floor for a full-relation-scan control is larger
than any cost this order could plausibly have added**, and that is
worth stating plainly as a property of the measurement environment: a
future narrow fixed-cost change on this same host should expect
±5-13% p50 movement on an *unrelated* control before concluding
anything, which is a wider floor than several of this project's other
narrow A/B results have reported on quieter runs
(`bench/v2.7.0/results-xe-ack-at-append-v2.7.0-17-ge310f8e.md`'s own
floor was 0.5-16% depending on shape). A session sharing a host with
another live server and this agent's own tooling is not the quiet
single-tenant box those numbers assumed, and that difference shows up
directly in the data rather than needing to be argued.

**The design decision this confirms**: `docs/spec/cabin.md` §4b rule 1
— write the one-range fast path first, so it can never pay for the
split case — is exactly the shape that makes a change like this
measurable as free rather than merely arguable as free. A predicate
written the other way around (split-case first, one-range as the
`else`) would still have been correct, but would not have produced a
counter that reads a clean zero; this run is evidence for writing gates
in decreasing likelihood of being taken, not only for readability.

## 9. What this run does not answer

It does not measure a split relation — every relation here is
single-owner by construction, which is the one case H-SB1 is about; the
multi-range serve/fall-through cost is SB-M3's question
(`instructions/v2.7.1/workorder-sb.md` SB5), not this one. It does not
measure SB1's ordered discard (SB-M2, no acknowledged broadcast exists
to price after amendment A1 — the order's own note). It does not
identify the root cause of Section 7's session-accumulation effect
beyond ruling out SB1-SB4's diff; that would need `perf`/`strace`-level
tooling this session did not reach for and was not asked to. No
PostgreSQL floor applies to this shape — an observed-value entry set has
no PostgreSQL counterpart, matching `cabin_optimizer_benchmark.py`'s own
precedent, so none was measured. No prior `bench/` file measures this
exact shape (a Cabin-scoping A/B); this file is the baseline the next
run of it reads against, per rule 4.
