# Physical optimizer, shadow v1 — the zero-idle-cost check and the report's own price

Workplan `PX07` (`docs/workplan-physical-optimizer.md`); spec
`docs/feat-physical-optimizer.md` (R1-R12). Measured 2026-08-09.

**Headline: `physical_optimizer = shadow` costs nothing at rest — measured at
+0.02% TPS on the freight scenario and +0.06% (median) on the stockmarket
scenario against `off`, both far inside the run-to-run noise floor — and the
report itself prices at ~60 µs of server CPU for the bare form and
~60 µs + 24 ns per slot examined for the per-relation survey.** The claim was
structural before it was measured — the two modes differ only in one branch
inside `SHOW RELAYOUT`'s handler, which no scenario statement ever reaches,
and the planner is pull-only (R10) with no background task — so the A/B here
*verifies* rather than discovers, and the honest prediction was "exact
noise". That is what came out. The more consequential finding is in the
archived reports themselves (§4): **in both real workloads, every hot
chain-walk shape sits on a btree relation, which R5 scopes the v1 mover out
of, and the heap relations the mover could touch are write-only or lightly
read** — the first shadow data point for §6's gate decisions, and it points
at a scoping question before it points at a gate.

---

## Run stamp

| | |
|---|---|
| executed | 2026-08-09, 05:57-06:24 UTC |
| branch | `worktree-imperative-cooking-bee` (the PX01-PX06 worktree; base branch `feat-assertion`) |
| commit | measured at `3643b5e` **plus the then-uncommitted PX01-PX06 changes** — the feature under test existed only in the working tree when the binary was built. That tree state was committed mid-session, verbatim, as **`e0e8df4`** (06:13 UTC): every physical-optimizer source the binary was built from is byte-identical to `e0e8df4` by `git diff` (mtimes 05:47, pre-build). |
| tree | dirty at build (the PX changes, uncommitted). Unrelated assertion-enforcement work landed in the same worktree at 06:17 UTC, **after** the 06:02 build — it is in no binary and no number here. |
| binary | `build-release/kds_server`, built 2026-08-09 06:02:07 UTC from the tree above. `./build` (Debug) was not used for any number. |
| build type | Release (`cmake -B build-release -DCMAKE_BUILD_TYPE=Release`) |
| device | `/dev/nvme0n1p1` (xfs) under `$HOME` (`~/bench-px07/`); not tmpfs |
| machine | 2 vCPU AMD EPYC 7571; ambient load from resident agent processes ~0.2-0.3 of one core throughout — see the noise floor below |
| server config | defaults (`cores = 1`, `durability = group`, `isolation = read-committed`, `decay_half_life = 600`) plus exactly one key per side: `physical_optimizer = shadow` vs `physical_optimizer = off` |
| verification | every run's `--verify` passed: scenario0 balance verify 200/200 accounts × all 9 runs; scenario2 invariant verify 100 checks, 0 failures × all 9 runs |

No engine code was changed for this measurement, so the test suite was not
re-run for it; the drivers' own verify phases are the correctness check each
number rests on.

## Method

Drivers and flags are documented in `bench/docs/README.md`; this file states
findings only. Both scenarios ran **fixed work, not fixed time**, so a slow
configuration is not also a smaller sample:

- `scenario0_stockmarket.py --users 10000 --assets 10000 --traders 2 --txn
  --txn-per-user 1.0 --seconds 240 --seed 1 --verify 200` — exactly 10,000
  four-statement transactions, TPS over the time they took. 10,000 users
  matches `bench/results-cabin.md`'s disk-backed configuration; the work
  target is smaller (10,000 vs 100,000 transactions) to fit eight interleaved
  runs, and the sample is 10,000 measured transactions per run either way.
- `scenario2_freight.py --organizations 200 --ships 40 --operations 400
  --cargos 5000 --bookings 1500 --seed 1 --verify 25` — exactly 1,500
  committed bookings, the same scale `bench/docs/README.md` documents.

**Interleaved pairs, fresh everything**: four A/B pairs per scenario, order
alternated (shadow-off, off-shadow, shadow-off, off-shadow), a fresh data
file and fresh server per run. Server CPU was read from
`/proc/<pid>/stat` (utime+stime) around the whole driver invocation — valid
as a comparison because the two sides execute an identical statement stream
by construction (same seed, same work target).

The `SHOW RELAYOUT` pricing harness is a session scratch script, not a
committed driver: one connection, the forms interleaved round-robin for the
latency percentiles (400 rounds after 10 warmups), then one block per form
for server CPU (block sizes in the tables). `PING` is the control — the
client+socket round trip with no engine work behind it. **A committed
`tools/relayout_price.py` with a `bench/docs` entry is the follow-up task
this leaves open**; every statement it issued is reproduced verbatim in the
tables and archives below.

**Noise floor, from inside the run.** Within-mode TPS spread was 2.1-2.8%
on scenario2 and 6.5% on scenario0's shadow side; scenario0's `off` side
caught one contaminated run (`b-off`, 466.8 TPS against siblings at
608-649 — ambient interference on a 2-core box, visible as a 21.4 s work
phase against ~16 s siblings). Scenario2's load phases also showed a bimodal
fsync latency (~450 µs vs ~1,050 µs p50 per autocommitted insert) that
struck one run of *each* mode (`a-shadow`, `d-off`) — uncorrelated with the
knob, and the measured booking/commit phases were flat across all eight runs
regardless. Any delta below these spreads is not a finding.

---

## 1. The A/B: zero idle cost, verified

Be precise about the mechanism first: `off` and `shadow` differ in one
branch at the top of `HandleShowRelayout` (`off` answers a one-line notice),
plus the config parse. Neither scenario ever issues `SHOW RELAYOUT`, the
planner runs only when asked (R10), and there is no background task, so the
two servers execute identical code for every statement measured. The A/B
cannot show a real difference unless the feature leaks somewhere unintended;
its job is to catch that leak. It did not.

### scenario0 (10,000 txns of 2 INSERT + 2 UPDATE, group durability)

| run | TPS shadow | TPS off | server CPU shadow (s) | server CPU off (s) |
|---|---|---|---|---|
| a | 600.3 | 608.3 | 10.08 | 10.04 |
| b | 639.4 | 466.8 † | 10.23 | 11.30 † |
| c | 605.1 | 635.5 | 10.01 | 9.73 |
| d | 640.6 | 649.1 | 9.70 | 9.55 |
| **mean** | **621.4** | **589.9** | **10.01** | **10.16** |
| **median** | **622.3** | **621.9** | | |

† contaminated by ambient interference (see noise floor); its wall clock and
CPU are both elevated with zero errors and zero conflicts in the driver.

Mean delta +5.3% (shadow "faster") is an artifact of the one contaminated
`off` run; the median delta is **+0.06%**, and excluding pair b entirely
flips the sign to −2.5%. All three readings sit inside the ±6.5% within-mode
spread: **no difference resolvable, as the mechanism predicts.** Server CPU
per run: −1.5% (shadow "cheaper"), same verdict.

### scenario2 (1,500 eight-statement bookings, group durability)

| run | TPS shadow | TPS off | server CPU shadow (s) | server CPU off (s) |
|---|---|---|---|---|
| a | 333.5 | 336.5 | 2.59 | 2.55 |
| b | 336.5 | 338.1 | 2.53 | 2.56 |
| c | 330.7 | 328.7 | 2.57 | 2.59 |
| d | 337.6 | 334.7 | 2.58 | 2.59 |
| **mean** | **334.6** | **334.5** | **2.567** | **2.572** |

Delta **+0.02% TPS, −0.19% server CPU** against a 2.1-2.8% spread: exact
noise. This is the cleaner of the two scenarios (no contaminated run) and
the stronger verification.

### Per-phase latency, pair d, both modes (client-measured, µs)

Scenario0's measured phases — every row a latency distribution with its op
count, per the documentation rules:

| phase | mode | ops | err | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|
| txn (4 stmts + BEGIN/COMMIT) | shadow | 10,000 | 0 | 3,086 | 1,459 | 2,477 | 2,534 | 7,914 | 13,099 |
| | off | 10,000 | 0 | 3,046 | 1,473 | 2,475 | 2,533 | 7,569 | 12,771 |
| trade-insert | shadow | 20,000 | 0 | 401 | 71 | 91 | 119 | 1,096 | 2,725 |
| | off | 20,000 | 0 | 397 | 68 | 91 | 117 | 1,090 | 2,707 |
| account-update | shadow | 20,000 | 0 | 407 | 73 | 106 | 128 | 1,108 | 2,702 |
| | off | 20,000 | 0 | 407 | 73 | 109 | 128 | 1,105 | 2,686 |
| profit-scan | shadow | 600 | 0 | 3,279 | 2,607 | 2,698 | 2,825 | 4,781 | 5,565 |
| | off | 600 | 0 | 3,147 | 2,604 | 2,690 | 2,765 | 4,149 | 4,846 |

Scenario2's, same pair:

| phase | mode | ops | err | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|
| booking (whole txn) | shadow | 1,503 | 0 | 2,939 | 827 | 2,763 | 2,875 | 3,184 | 4,458 |
| | off | 1,503 | 0 | 2,963 | 817 | 2,786 | 2,893 | 3,183 | 5,081 |
| commit | shadow | 1,500 | 0 | 1,102 | 907 | 1,019 | 1,051 | 1,191 | 1,369 |
| | off | 1,500 | 0 | 1,071 | 910 | 1,027 | 1,060 | 1,173 | 1,348 |
| cargo-lookup | shadow | 1,503 | 0 | 139 | 87 | 133 | 134 | 167 | 237 |
| | off | 1,503 | 0 | 141 | 89 | 132 | 134 | 165 | 292 |
| charge-insert | shadow | 8,423 | 0 | 116 | 60 | 112 | 115 | 136 | 245 |
| | off | 8,423 | 0 | 123 | 60 | 112 | 115 | 136 | 231 |
| operation-update | shadow | 1,500 | 0 | 125 | 72 | 121 | 124 | 144 | 240 |
| | off | 1,500 | 0 | 125 | 73 | 122 | 124 | 143 | 227 |

Percentile by percentile the two modes are the same distribution; the p50s
differ by less than the client's own jitter.

### Wait accounting for the measured transaction

The workload's unit is a sum of named waits; here is scenario2's booking
p50 (~2,875 µs, 10 round trips) decomposed with `PING` (p50 93 µs) as the
client+socket unit:

| wait | estimate | share | how known |
|---|---|---|---|
| client + socket round trips | ~10 × 93 ≈ 930 µs | ~32% | PING control, same connection |
| durability (group fsync at COMMIT) | ~960 µs (commit p50 1,051 − round trip) | ~33% | commit phase, isolated by the driver |
| read statements (4 pk lookups) | ~4 × 41 ≈ 165 µs engine | ~6% | lookup p50 134 − PING |
| write statements (~7.6 inserts/updates) | ~7.6 × 25 ≈ 190 µs engine | ~7% | write p50 118-124 − PING |
| lock / conflict wait | 0 | 0% | driver: 0 conflicts, 0 retries, all 8 runs |
| BEGIN + residue | remainder ~630 µs | ~22% | not separately instrumented — BEGIN's round trip, scheduling, and the estimate error live here |

The residue is stated rather than attributed: per-statement server-side
timing exists only under `--server-log` at debug level, which was not paid
here. The point of the table for PX07 is the top line of what it excludes:
**no wait in the transaction path belongs to the physical optimizer**, and
the A/B above is the experimental confirmation.

---

## 2. The report's own price

Measured on live servers straight after their workloads (no restart —
`last_seen` is a monotonic-clock reading and does not survive one), 400
interleaved latency rounds per form, then a server-CPU block per form.
Client latency includes the Python client's ~93 µs round trip; server CPU
is the engine's own cost (and includes ~40-46 µs of per-statement dispatch +
socket handling, visible in PING's row).

### scenario0's database (trades = 20,000 rows / 181 pages; upp = 600 rows / 5 pages; accounts = 20,012 rows, btree)

| form | ops | mean | p0 | p25 | p50 | p95 | p99 | max | server CPU/stmt (block) |
|---|---|---|---|---|---|---|---|---|---|
| PING (control) | 400 | 92.9 | 53.4 | 92.2 | 92.9 | 100.4 | 130.6 | 182.1 | 46.0 µs (n=5000) |
| SHOW RELAYOUT (bare) | 400 | 110.6 | 73.5 | 110.0 | 110.8 | 120.6 | 129.7 | 145.3 | 60.0 µs (n=3000) |
| SHOW RELAYOUT trades | 400 | 588.0 | 531.7 | 570.3 | 572.2 | 685.6 | 839.9 | 1,163.2 | 550.0 µs (n=400) |
| SHOW RELAYOUT user_periodic_profit | 400 | 123.7 | 90.2 | 121.3 | 122.3 | 139.0 | 155.1 | 283.5 | 75.0 µs (n=2000) |
| SHOW RELAYOUT accounts (btree: no walk) | 400 | 105.5 | 66.0 | 103.8 | 104.6 | 121.3 | 147.4 | 266.2 | 53.3 µs (n=3000) |

### scenario2's database (charges = 8,423 rows / 64 pages; freights = 1,500 rows / 15 pages; cargos = 5,000 rows, btree)

| form | ops | mean | p0 | p25 | p50 | p95 | p99 | max | server CPU/stmt (block) |
|---|---|---|---|---|---|---|---|---|---|
| PING (control) | 400 | 91.2 | 51.6 | 89.0 | 93.9 | 106.7 | 125.7 | 182.2 | 40.0 µs (n=5000) |
| SHOW RELAYOUT (bare, 8 relations) | 400 | 119.5 | 77.9 | 114.9 | 118.0 | 140.4 | 208.8 | 579.1 | 63.3 µs (n=3000) |
| SHOW RELAYOUT freights | 400 | 147.7 | 101.8 | 145.0 | 147.3 | 167.7 | 203.1 | 277.8 | 100.0 µs (n=2000) |
| SHOW RELAYOUT charges | 400 | 323.5 | 284.8 | 305.1 | 311.8 | 374.9 | 443.9 | 615.1 | 265.0 µs (n=2000) |
| SHOW RELAYOUT cargos (btree: no walk) | 400 | 104.1 | 63.2 | 103.4 | 106.4 | 122.3 | 137.2 | 245.8 | 50.0 µs (n=3000) |

### The row-set sweep (rule 9): 200 / 1,000 / 10,000

A dedicated fresh server; three identical four-column heap relations
(`id int64, grp int64, val int64, note varchar`) at 200 / 1,000 / 10,000
rows, every third row delete-marked (33%), each seeded with the same read
mix (20 FilterScans, 5 pk Ranges, 10 pk Lookups) so the planner has
non-zero weights. The size is the row of the table; the loader's row counts
map one-to-one to the relation names.

| form | slots examined | ops | mean | p0 | p25 | p50 | p95 | p99 | max | server CPU/stmt |
|---|---|---|---|---|---|---|---|---|---|---|
| PING (control) | — | 400 | 94.3 | 55.2 | 92.2 | 93.8 | 105.0 | 119.7 | 1,248.7 | 44.0 µs (n=5000) |
| SHOW RELAYOUT (bare, 3 relations) | 0 | 400 | 115.4 | 71.3 | 114.7 | 116.1 | 134.3 | 190.8 | 274.3 | 60.0 µs (n=3000) |
| SHOW RELAYOUT px200 | 200 | 400 | 111.9 | 65.0 | 111.7 | 114.8 | 128.1 | 138.8 | 176.4 | 63.3 µs (n=3000) |
| SHOW RELAYOUT px1k | 1,000 | 400 | 133.2 | 84.8 | 132.0 | 133.6 | 151.0 | 188.6 | 350.8 | 85.0 µs (n=2000) |
| SHOW RELAYOUT px10k | 10,000 | 400 | 362.6 | 321.4 | 345.7 | 347.5 | 429.0 | 509.1 | 652.9 | 300.0 µs (n=800) |

**The named form's cost model falls straight out of the sweep and the
scenario relations agree with it: ~60 µs fixed + ~24 ns per slot examined.**
Slope fits: sweep (300.0 − 63.3)/9,800 = 24.1 ns; trades
(550.0 − 60)/20,000 = 24.5 ns; charges (265.0 − 63.3)/8,423 = 23.9 ns. At
111 slots per page that is ~2.7 µs per page — the census reads the slot
directory and the Keystone word and decodes nothing, and the number shows
it. The delete-marked share does not change the price (marked slots are
examined like live ones), and the fixed ~60 µs is the same statistics +
catalog read the bare form does.

Three facts the tables pin:

- **The bare form never walks**: its price is flat at ~60-63 µs CPU whether
  the databases behind it hold 3, 5 or 8 relations of 600 or 20,000 rows —
  ~14-23 µs over PING, reading `sys.access_stats` and the catalog only.
  `PlanAllRelations` takes no PageStore, so this is by construction; the
  measurement confirms the construction.
- **A btree relation's named form is bare-priced** (~50-53 µs CPU): shapes
  and jurisdiction only, no survey, per R5.
- **Waits, for the record**: a `SHOW RELAYOUT` is read-only and
  single-statement — no durability wait, no lock/conflict wait (latching is
  core-local and the census yields per page like any walk), and no I/O wait
  in these runs because every surveyed page was resident (nothing evicts in
  this engine yet, `docs/spec-eviction.md`). Its latency is client+socket
  (~93 µs) + server CPU (tables above); the two sum to the observed p50s
  within a few µs, which is the whole decomposition.

---

## 3. What the shadow reports actually said (verbatim archives)

The first shadow data for §6's gate decisions. Weights are Q24.8
(`weight_q8 = 256` ≡ one undecayed use); `predicted_benefit` is R9's
pages-saved × decayed walk weight, in the same Q24.8.

### scenario0, bare form (suffix elided from names below for width; archived as printed)

```
relayout_relations=5
rel=users clustered=btree shapes=0 walk_weight_q8=0
plans=none reason=btree-outside-v1-mover-scope
rel=assets clustered=btree shapes=0 walk_weight_q8=0
plans=none reason=btree-outside-v1-mover-scope
rel=accounts clustered=btree shapes=2 walk_weight_q8=153600
shape kind=FilterScan columns_mask=0x2 uses=600 weight_q8=153600
shape kind=Lookup columns_mask=0x1 uses=200 weight_q8=51200
plans=none reason=btree-outside-v1-mover-scope
rel=trades clustered=heap shapes=0 walk_weight_q8=0
plan=compact blocked_on=reader-horizon surveyed=0 predicted_pages_saved=0 predicted_benefit=0 measured_pages_saved=0
plan=cluster blocked_on=ordered-between surveyed=0 predicted_pages_saved=0 predicted_benefit=0 measured_pages_saved=0
plan=defrag blocked_on=page-reuse surveyed=0 predicted_pages_saved=0 predicted_benefit=0 measured_pages_saved=0
rel=user_periodic_profit clustered=heap shapes=0 walk_weight_q8=0
plan=compact blocked_on=reader-horizon surveyed=0 predicted_pages_saved=0 predicted_benefit=0 measured_pages_saved=0
plan=cluster blocked_on=ordered-between surveyed=0 predicted_pages_saved=0 predicted_benefit=0 measured_pages_saved=0
plan=defrag blocked_on=page-reuse surveyed=0 predicted_pages_saved=0 predicted_benefit=0 measured_pages_saved=0
```

Named forms added the surveys:

```
rel=trades ... survey pages=181 live=20000 delete_marked=0 tuples_per_page=111
plan=compact blocked_on=reader-horizon surveyed=1 predicted_pages_saved=0 predicted_benefit=0 ...
rel=user_periodic_profit ... survey pages=5 live=600 delete_marked=0 tuples_per_page=133
plan=compact blocked_on=reader-horizon surveyed=1 predicted_pages_saved=0 predicted_benefit=0 ...
```

### scenario2, bare form (suffix elided as above)

```
relayout_relations=8
rel=organizations clustered=btree shapes=1 walk_weight_q8=0
shape kind=Lookup columns_mask=0x1 uses=1528 weight_q8=391168
plans=none reason=btree-outside-v1-mover-scope
rel=ships clustered=btree shapes=0 walk_weight_q8=0
plans=none reason=btree-outside-v1-mover-scope
rel=operations clustered=btree shapes=1 walk_weight_q8=0
shape kind=Lookup columns_mask=0x1 uses=1529 weight_q8=391424
plans=none reason=btree-outside-v1-mover-scope
rel=cargos clustered=btree shapes=2 walk_weight_q8=0
shape kind=Probe columns_mask=0x1 uses=76 weight_q8=19456
shape kind=Lookup columns_mask=0x1 uses=1503 weight_q8=384768
plans=none reason=btree-outside-v1-mover-scope
rel=fees clustered=btree shapes=0 walk_weight_q8=0
plans=none reason=btree-outside-v1-mover-scope
rel=recipes clustered=btree shapes=1 walk_weight_q8=385024
shape kind=FilterScan columns_mask=0x2 uses=1504 weight_q8=385024
plans=none reason=btree-outside-v1-mover-scope
rel=freights clustered=heap shapes=2 walk_weight_q8=77824
shape kind=FilterScan columns_mask=0x2 uses=228 weight_q8=58368
shape kind=Scan columns_mask=0x0 uses=76 weight_q8=19456
plan=compact blocked_on=reader-horizon surveyed=0 predicted_pages_saved=0 predicted_benefit=0 measured_pages_saved=0
plan=cluster blocked_on=ordered-between surveyed=0 predicted_pages_saved=0 predicted_benefit=0 measured_pages_saved=0
plan=defrag blocked_on=page-reuse surveyed=0 predicted_pages_saved=0 predicted_benefit=0 measured_pages_saved=0
rel=charges clustered=heap shapes=1 walk_weight_q8=54528
shape kind=FilterScan columns_mask=0x2 uses=213 weight_q8=54528
plan=compact blocked_on=reader-horizon surveyed=0 predicted_pages_saved=0 predicted_benefit=0 measured_pages_saved=0
plan=cluster blocked_on=ordered-between surveyed=0 predicted_pages_saved=0 predicted_benefit=0 measured_pages_saved=0
plan=defrag blocked_on=page-reuse surveyed=0 predicted_pages_saved=0 predicted_benefit=0 measured_pages_saved=0
```

Named forms: `freights` survey `pages=15 live=1500 delete_marked=0
tuples_per_page=105`; `charges` survey `pages=64 live=8423 delete_marked=0
tuples_per_page=133`; every compact prediction 0.

### The sweep, named forms — the prediction arithmetic exercised

```
rel=px200  survey pages=3   live=133  delete_marked=67   tuples_per_page=72
plan=compact blocked_on=reader-horizon surveyed=1 predicted_pages_saved=1  predicted_benefit=25
rel=px1k   survey pages=14  live=666  delete_marked=334  tuples_per_page=72
plan=compact blocked_on=reader-horizon surveyed=1 predicted_pages_saved=4  predicted_benefit=100
rel=px10k  survey pages=139 live=6666 delete_marked=3334 tuples_per_page=72
plan=compact blocked_on=reader-horizon surveyed=1 predicted_pages_saved=46 predicted_benefit=1150
```

At 33% delete-marks compact predicts reclaiming exactly the arithmetic:
139 → ceil(6666/72) = 93 pages, 46 saved, benefit 46 × walk weight
(6400 Q8 = 25 undecayed walk uses) = 1150 Q8. `cluster` and `defrag` carry
no predicted number by design — their inputs (a hot set; sequential-I/O
gain) are respectively uncollected and outside R9's pages metric, and the
report says 0 rather than faking one.

### The decayed weights are real (and quantized)

The same `px200` report re-captured after ~150 s of idleness:

```
rel=px200 clustered=heap shapes=3 walk_weight_q8=5375        (was 6400)
shape kind=FilterScan columns_mask=0x2 uses=20 weight_q8=4300  (was 5120)
shape kind=Range      columns_mask=0x1 uses=5  weight_q8=1075  (was 1280)
shape kind=Lookup     columns_mask=0x1 uses=10 weight_q8=2150  (was 2560)
plan=compact ... predicted_pages_saved=1 predicted_benefit=20  (was 25)
```

4300/5120 = 0.84 ≈ 2^(-4/16): four LUT buckets of R1's 16-per-half-life
table at `decay_half_life = 600`. The consequence worth writing down:
**weights quantize at half_life/16 = 37.5 s**, so a report taken within
37.5 s of the last access shows raw counts (`uses × 256` exactly — every
fresh capture above does), and decay becomes visible only across bucket
boundaries. The prediction decays with the weight, as R9 intends: benefit
25 → 20 with no new data, only time.

---

## 4. What the first shadow data teaches

1. **The demand is on btree relations; the v1 mover is scoped to heap ones.**
   In both workloads every heavy chain-walk shape sits on a btree relation —
   scenario0's `accounts` FilterScan (600 uses, weight 153,600) and
   scenario2's `recipes` FilterScan (1,504 uses, weight 385,024) — and both
   answer `plans=none reason=btree-outside-v1-mover-scope`. Meanwhile the
   heap relations a mover could legally touch are append-only ledgers:
   `trades` has **zero recorded shapes** (nothing ever reads it), and
   `freights`/`charges` carry modest verify-phase weights. So the first
   shadow data says the v1 heap-only mover would find nothing worth moving
   in either scenario — not because the engine is tidy, but because these
   workloads put their walks where R5 declined jurisdiction. Before any §6
   gate opens on this evidence, the R5 `[PROPOSED]` btree exclusion is the
   decision the data actually presses on.
2. **Real workloads here produce no delete-marks, so compact predicts zero
   everywhere it is in scope.** Neither scenario issues DELETE; UPDATE in
   this engine overwrites in place with the old version in undo, leaving no
   dead heap tuple. Compact debt comes only from DELETE — structurally
   unlike PostgreSQL, where the same scenario0 run left 988 dead tuples on
   `accounts` from its 20,000 updates (§5). The sweep proves the survey and
   the arithmetic work when marks exist; the scenarios prove the report is
   honest when they do not.
3. **An insert-only relation is invisible to the benefit model.**
   `sys.access_stats` records read shapes; `trades` (20,000 rows written,
   never queried) reports `shapes=0`, so any plan on it prices at 0. That is
   R9 behaving correctly — pages nobody reads are worth nothing to save —
   but it means the planner cannot see write-side or recovery-side benefits
   (a compacted chain is also cheaper to fault and to checkpoint), and R9's
   metric does not claim to.
4. **The report is cheap enough to poll.** ~60 µs CPU for the bare form and
   ~24 ns per slot for a survey put even the 20,000-row named form at half a
   millisecond; an operator sampling every relation once a minute would be
   invisible. The budget charge is the honest cap: the named form's walk
   spends `max_rows_touched` like any statement, so a survey of a relation
   larger than the budget refuses rather than serving a half-count (not
   exercised here — 10,000 slots against the default 100M).

---

## 5. PostgreSQL comparison

The honest analog is not the optimizer (PostgreSQL has autovacuum — a mover
with no gates) but its **visibility into the same physical facts**. The
scratch cluster (`tools/pg_setup.sh`, PostgreSQL 17.10, defaults, port
15433) was populated by the same twin workload at the same scale:
`pg_scenario0_stockmarket.py --users 10000 --traders 2 --txn
--txn-per-user 1.0 --seed 1` — 10,000 transactions at 828.2 TPS (KDS:
~621; recorded for context, not as this document's comparison).

| form | ops | mean | p0 | p25 | p50 | p95 | p99 | max | backend CPU/stmt |
|---|---|---|---|---|---|---|---|---|---|
| SELECT 1 (control) | 400 | 151.0 | 118.8 | 139.5 | 141.8 | 179.5 | 282.4 | 432.7 | 58 µs (n=5000) |
| pg_stat_user_tables (5 rels: n_live_tup, n_dead_tup, seq_scan, idx_scan) | 400 | 1,642.0 | 1,345.2 | 1,386.7 | 1,437.8 | 2,295.0 | 2,956.4 | 5,809.1 | 1,515 µs (n=2000) |
| SELECT count(*) FROM trades (20,000 rows — the walk analog) | 400 | 1,825.3 | 1,547.0 | 1,597.7 | 1,632.3 | 2,449.5 | 3,055.6 | 5,566.3 | 1,575 µs (n=400) |

- **pgstattuple — the true analog of the named form's delete-mark census —
  is not installed on this machine** (no contrib package), so the walk
  analog above is a `count(*)` Seq Scan: it prices a full read-only walk
  but does not measure dead-tuple density. Stated rather than substituted.
- KDS's bare report (60 µs CPU) is ~25× cheaper than the
  `pg_stat_user_tables` view read (1,515 µs), and its 20,000-slot survey
  (550 µs) is ~3× cheaper than PostgreSQL's 20,000-row `count(*)`
  (1,575 µs) — with the caveat that the census decodes nothing while
  `count(*)` runs the executor, and that PostgreSQL's view is priced per
  read while its underlying counters are maintained continuously by
  writers and the stats collector. The architectures put the cost in
  different places; the table prices the operator's question, "what is the
  physical state now", as each engine answers it.
- The structural observation worth more than the ratios: right after the
  run, `accounts_pgprice` showed `n_dead_tup = 988`; minutes later it read
  0 — **autovacuum had already acted, because PostgreSQL's mover exists**.
  KDS's shadow report shows debts nothing can yet collect (`blocked_on=`
  on every plan), and they persist until a gate opens. Same facts, opposite
  lifecycle — which is precisely what R11's shadow-first stance is for:
  the report exists so the decision to build the collector is made from
  numbers.

---

## Files

Raw outputs under `~/bench-px07/`: per-run driver JSON
(`scenario{0,2}-{a..d}-{shadow,off}.json` with `px07.server_cpu_s` /
`px07.wall_s` appended), pricing JSONs (`price-*.json`), verbatim archives
(`archive-scenario0.txt`, `archive-scenario2.txt`, `archive-sweep.txt`,
`archive-sweep-decayed.txt`, `archive-pg.txt`). Data files were deleted
after measurement (fresh files per run; nothing to re-run against).
