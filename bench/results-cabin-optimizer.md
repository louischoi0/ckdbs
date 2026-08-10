# The cabin optimizer, measured: free when idle, and the full Cabin win when it acts

PHY08's benchmark note (`docs/feat-physical-optimizer.md` Part II, workplan
`docs/workplan-physical-optimizer.md`). Two questions, two answers:

1. **What does `cabin_optimizer = on` cost a workload it can do nothing
   for?** Nothing measurable. Across three interleaved A/B/A runs the
   on-arm's p50 deltas sit inside the same-configuration noise gap, and the
   tick itself — priced directly from server CPU over 6,000 idle ticks —
   costs **~2–3 µs of CPU per tick**, which at the default 10 s cadence is
   below one part per million of one core.
2. **What is a self-created Cabin worth?** Exactly what a declared one is
   worth, because after the CREATE it *is* one — and on a hot non-pk
   equality re-probing one value, that is **10.9× on client p50 and 19× on
   server CPU at 10,000 rows** (1,439 µs → 132 µs p50), with the controller
   creating the Cabin **1.4 s after being switched on** (three 500 ms
   snapshots — `confirm_snapshots = 3`, working as configured) and the
   served reply verified byte-identical to the walked one. The contrast
   with `bench/results-cabin.md`'s declared-Cabin wash (+3.2 % TPS at a
   23.9 % hit rate) is not the structure and not who created it — it is the
   **hit rate**, 99.8 % here, and that is a property of the workload.

## Run stamp

| | |
|---|---|
| executed | 2026-08-10, 05:41–05:53 UTC |
| branch / commit | `feat-assertion` @ `f1d59b5` |
| tree | dirty — docs, `README.md`, `tests/cabin_optimizer_exec_test.cpp`, `bench/txn_layers_bench.cpp` (another task's PHY08 docs half in flight); none of the modified files links into `kds_server`, so the binary corresponds to the committed engine sources |
| binary | `build-release/kds_server`, Release; `cmake --build` re-run this session found nothing to compile — the binary's mtime (05:16 UTC) predates HEAD's commit timestamp (05:28) only because the merge commit was cut after the build |
| host / device | EC2, 2 × AMD EPYC 7571; data files under `~/bench-cabinopt/` on the EBS root volume (`nvme0n1p1`, xfs) — not tmpfs |
| machine state | quiet at measurement time (`pgrep cc1plus` = 0; a concurrent Release build was detected before the first run and waited out) |
| server config | defaults (`cores = 1`, `durability = group`, `cabins = on`, checkpoint 5000 ms) plus, per case: **null** — `cabin_optimizer = off` at boot, `cabin_optimizer_snapshot_interval_ms = 10` (deliberately lowered from the 10,000 ms default to make the null result strong: 100 ticks/s instead of 0.1); **improve** — `cabin_optimizer = off` at boot, interval **500 ms**, thetas/confirm/budget at defaults (θ_create 300 pct, θ_drop 50, confirm 3, budget 1024 pages). Both cases drive PO8's runtime `SET CABIN_OPTIMIZER` switch |
| fresh state | every numbered run below is a fresh server process on a freshly created data file |
| drivers | `tools/cabin_optimizer_benchmark.py` (both cases) and `tools/pg_cabin_optimizer_benchmark.py` (the twin) — flags and invocations in `bench/docs/README.md` |

Row-set sweep (both cases): relations of **200 / 1,000 / 10,000 rows**, the
driver's built-in size axis. In the improve case the `val` domain is
`rows / 10`, so the hot value (`val = 7`) matches **exactly 10 rows at every
size** — the axis moves relation size, never answer size.

---

## 1. Zero eligible candidates: the on/off delta is inside the noise floor

The workload is pk lookups (`WHERE id = ?` — a `Lookup` step names no
candidate column) and logged INSERTs (not fingerprint-tracked), so the
controller ticks 100 times a second, snapshots, and finds nothing to decide.
Three arms — `off1`, `on`, `off2` — interleave per round on one server and
one data file via `SET CABIN_OPTIMIZER`, with the same drawn arguments per
round; `off1` vs `off2` is the same configuration by construction and is the
noise floor. The end-of-run view confirmed the premise both runs:
`managed=0 creates=0` after ~6,200 ticks.

**Run A** (60 rounds; per arm: 10,800 lookups + 600 inserts):

| phase | ops | mean µs | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| pk-200[off1] | 3,600 | 124.1 | 72.8 | 124.4 | 125.8 | 145.6 | 165.4 | 409 |
| pk-200[on] | 3,600 | 124.5 | 71.4 | 124.1 | 125.7 | 145.0 | 173.4 | 523 |
| pk-200[off2] | 3,600 | 125.5 | 71.2 | 124.5 | 125.8 | 144.9 | 185.1 | 1,142 |
| pk-1k[off1] | 3,600 | 123.9 | 71.6 | 122.3 | 125.7 | 141.3 | 177.5 | 5,016 |
| pk-1k[on] | 3,600 | 125.3 | 73.2 | 124.8 | 126.6 | 144.8 | 170.3 | 373 |
| pk-1k[off2] | 3,600 | 124.5 | 73.2 | 124.1 | 125.8 | 142.4 | 253.4 | 849 |
| pk-10k[off1] | 3,600 | 125.4 | 72.1 | 124.1 | 125.5 | 141.5 | 238.6 | 341 |
| pk-10k[on] | 3,600 | 131.3 | 73.5 | 126.7 | 130.1 | 148.4 | 192.3 | 1,042 |
| pk-10k[off2] | 3,600 | 159.7 | 72.6 | 124.7 | 126.0 | 136.2 | 153.5 | 131,944 |
| insert[off1] | 600 | 1,055.5 | 761.5 | 1,013.0 | 1,041.4 | 1,154.3 | 1,282.8 | 4,117 |
| insert[on] | 600 | 1,068.6 | 520.4 | 1,016.1 | 1,042.7 | 1,157.2 | 2,077.9 | 4,249 |
| insert[off2] | 600 | 1,063.8 | 485.6 | 1,016.1 | 1,046.6 | 1,157.5 | 1,425.5 | 4,204 |

**Run B** (fresh file, new seed — note the whole client floor moved from
~125 to ~96 µs between runs, which is exactly why the arms interleave inside
one run instead of comparing across two):

| phase | ops | mean µs | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| pk-200[off1] | 3,600 | 105.9 | 73.1 | 94.0 | 96.0 | 124.5 | 150.7 | 6,108 |
| pk-200[on] | 3,600 | 111.5 | 81.0 | 94.0 | 96.1 | 145.6 | 432.4 | 4,370 |
| pk-200[off2] | 3,600 | 114.8 | 72.6 | 94.1 | 96.1 | 129.6 | 351.1 | 4,594 |
| pk-1k[off1] | 3,600 | 102.6 | 78.9 | 93.1 | 96.1 | 120.6 | 154.1 | 3,196 |
| pk-1k[on] | 3,600 | 118.8 | 79.4 | 94.6 | 97.0 | 132.9 | 302.6 | 9,580 |
| pk-1k[off2] | 3,600 | 111.8 | 72.7 | 93.5 | 95.9 | 126.7 | 196.4 | 6,271 |
| pk-10k[off1] | 3,600 | 152.1 | 72.7 | 93.6 | 95.7 | 117.2 | 151.4 | 170,611 |
| pk-10k[on] | 3,600 | 108.0 | 75.2 | 96.6 | 100.0 | 131.0 | 178.2 | 5,226 |
| pk-10k[off2] | 3,600 | 109.6 | 74.1 | 94.1 | 96.2 | 122.4 | 177.2 | 6,511 |
| insert[off1] | 600 | 1,051.4 | 442.5 | 991.4 | 1,038.8 | 1,243.5 | 2,080.7 | 6,127 |
| insert[on] | 600 | 1,083.3 | 583.6 | 1,002.1 | 1,037.1 | 1,219.6 | 2,351.4 | 7,370 |
| insert[off2] | 600 | 1,038.3 | 457.7 | 996.0 | 1,036.9 | 1,169.7 | 1,389.1 | 5,295 |

**Run C** — the same matrix with the size order **reversed** inside each arm
block (10k, 1k, 200), run because A and B both showed the on-arm's largest
p50 delta (+4 µs) on the phase measured *last*, and last-position and
largest-relation were confounded:

| phase | ops | mean µs | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| pk-10k[off1] | 3,600 | 119.9 | 71.4 | 93.5 | 123.2 | 144.9 | 181.1 | 6,402 |
| pk-10k[on] | 3,600 | 120.7 | 72.0 | 94.9 | 125.7 | 145.9 | 221.9 | 5,067 |
| pk-10k[off2] | 3,600 | 118.3 | 72.5 | 92.3 | 122.4 | 142.9 | 172.2 | 6,430 |
| pk-1k[off1] | 3,600 | 117.0 | 70.1 | 89.7 | 121.3 | 141.1 | 173.3 | 5,620 |
| pk-1k[on] | 3,600 | 119.6 | 74.1 | 93.1 | 125.7 | 146.8 | 191.0 | 2,771 |
| pk-1k[off2] | 3,600 | 117.6 | 72.3 | 93.2 | 123.6 | 139.5 | 168.5 | 5,862 |
| pk-200[off1] | 3,600 | 135.7 | 65.6 | 92.6 | 119.2 | 135.7 | 233.4 | 73,238 |
| pk-200[on] | 3,600 | 117.2 | 66.3 | 92.5 | 118.8 | 138.3 | 168.5 | 3,983 |
| pk-200[off2] | 3,600 | 107.7 | 65.5 | 88.9 | 111.2 | 132.5 | 152.1 | 1,149 |
| insert[off1] | 600 | 953.9 | 362.0 | 538.6 | 1,022.3 | 1,280.1 | 3,749.2 | 6,408 |
| insert[on] | 600 | 874.2 | 379.0 | 504.7 | 1,020.7 | 1,140.8 | 1,355.6 | 2,379 |
| insert[off2] | 600 | 903.0 | 374.7 | 543.7 | 1,021.8 | 1,170.9 | 1,495.7 | 7,837 |

Derived summary (p50 deltas, not distributions):

| run | on − mean(off), p50 | off1 ↔ off2 gap, p50 | verdict |
|---|---|---|---|
| A | −0.1 / +0.9 / **+4.3** µs (200/1k/10k) | 0.0 / 0.1 / 0.5 µs | 10k delta above the p50 gap — investigated in C |
| B | +0.1 / +1.0 / **+4.0** µs | 0.1 / 0.2 / 0.5 µs | same pattern |
| C (reversed) | +2.9 / +3.3 / +3.7 µs (10k/1k/200) | 0.8 / 2.3 / **8.0** µs | the shift appears at every position and size; the same-config gap itself reaches 8 µs |

The +4 µs did **not** follow relation size once the order moved — run C
shows a comparable shift at every size while its own off↔off gap widens to
8 µs. Server CPU per arm agrees: the on arm cost +2.6 to +3.5 µs/op over
`off2` in A and B, against an `off1`↔`off2` same-configuration gap of
+8.8 to +9.6 µs/op. A delta below the same-configuration gap on both
meters is not a finding, and per this harness's own floor rule (a Python
client carries ~100 µs of every row here), it is reported as: **any
per-statement cost of `cabin_optimizer = on` with zero candidates is below
~4 µs on a ~100 µs statement, indistinguishable from noise**. The insert
rows are flat outright (p50 within 1 µs across arms in B).

### The tick, priced directly

Client latency cannot resolve a background task, so the tick is measured as
server CPU (`/proc/<pid>/stat`, utime+stime) over two 60 s **idle** windows
per run — switch on, then switch off — divided by the tick count
`SHOW CABIN_OPTIMIZER` reports. The off window carries the reactor's idle
cost and the disabled tick's predicate-and-return, so the difference is the
enabled tick body: snapshot over this run's fingerprint population plus a
`Decide` over zero candidates.

| run | ticks in on-window | on-window CPU | off-window CPU | per enabled tick |
|---|---|---|---|---|
| A | 5,997 | 0.88 s | 0.87 s | 1.7 µs |
| B | 5,998 | 0.89 s | 0.87 s | 3.3 µs |

The meter's resolution is one jiffy (10 ms) per window, which over ~6,000
ticks is ±1.7 µs/tick — so the honest statement is **2–3 µs of CPU per
enabled tick, bounded above by ~5 µs**. At this run's deliberately
aggressive 10 ms cadence that is ~0.025 % of one core; at the default
10,000 ms cadence it is **~0.3 µs of CPU per second — below one part per
million of one core**. The workplan's target ("unmeasurable — snapshot cost
only") is met, and the snapshot cost now has a number.

---

## 2. The improvement case: 1.4 s from switch-on to a serving Cabin, then 10.9×

Per size, three phases on the hot statement `SELECT * FROM t WHERE val = 7`
(10 matching rows at every size): **walk** — 1,200 probes with the
controller off, the pre-Cabin baseline; **transition** — `SET
CABIN_OPTIMIZER ON`, probes continue while the controller snapshots,
confirms and CREATEs (time-bound, not equal work — excluded from the
before/after comparison); **served** — 1,200 probes with the Cabin serving.
Primary run (fresh file; a first run on a different file and seed showed
the same lifecycle at every size with equal-or-larger wins — 14.5× at 10k,
where machine noise inflated its walk phase — but carried ±36 µs drift on
its pk controls, so the cleaner repeat is the one tabled and the smaller,
better-floored ratios are the ones claimed):

| phase | ops | mean µs | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| pk-200[pre] | 300 | 130.6 | 118.4 | 124.8 | 127.0 | 141.9 | 151.9 | 194 |
| probe-200[walk] | 1,200 | 154.2 | 117.1 | 150.4 | 153.9 | 172.7 | 218.7 | 1,377 |
| probe-200[transition] | 9,000 | 155.5 | 106.2 | 152.8 | 153.9 | 169.0 | 193.2 | 1,294 |
| probe-200[served] | 1,200 | 127.0 | 79.5 | 125.5 | 131.2 | 145.8 | 195.3 | 596 |
| pk-200[post] | 300 | 130.3 | 104.9 | 125.4 | 127.0 | 144.6 | 168.7 | 197 |
| pk-1k[pre] | 300 | 128.9 | 114.0 | 123.8 | 125.0 | 142.5 | 205.4 | 250 |
| probe-1k[walk] | 1,200 | 267.2 | 227.3 | 257.9 | 259.5 | 301.1 | 375.7 | 633 |
| probe-1k[transition] | 5,050 | 276.3 | 134.7 | 258.0 | 259.7 | 307.0 | 386.3 | 30,191 |
| probe-1k[served] | 1,200 | 129.4 | 79.3 | 126.8 | 131.9 | 146.4 | 172.3 | 1,310 |
| pk-1k[post] | 300 | 129.4 | 101.3 | 124.6 | 126.7 | 142.1 | 156.0 | 200 |
| pk-10k[pre] | 300 | 128.2 | 119.6 | 125.3 | 126.4 | 138.5 | 162.3 | 251 |
| probe-10k[walk] | 1,200 | 1,528.0 | 1,414.8 | 1,424.6 | 1,439.2 | 2,108.3 | 2,355.8 | 4,213 |
| probe-10k[transition] | 950 | 1,470.2 | 127.6 | 1,423.5 | 1,437.9 | 2,012.0 | 2,284.3 | 2,802 |
| probe-10k[served] | 1,200 | 136.2 | 81.2 | 131.0 | 131.9 | 152.6 | 316.4 | 431 |
| pk-10k[post] | 300 | 131.3 | 73.2 | 102.7 | 124.2 | 273.7 | 324.7 | 341 |

The before/after, side by side (derived from the table above; server CPU
per op sampled per 100-probe block):

| rows | walk p50 | served p50 | client win | walk CPU/op | served CPU/op | server win |
|---|---|---|---|---|---|---|
| 200 | 153.9 µs | 131.2 µs | **1.17×** (−14.8 %) | 108 µs | 75 µs | 1.4× |
| 1,000 | 259.5 µs | 131.9 µs | **1.97×** (−49.2 %) | 208 µs | 67 µs | 3.1× |
| 10,000 | 1,439.2 µs | 131.9 µs | **10.9×** (−90.8 %) | 1,458 µs | 75 µs | **19.4×** |

The served p50 is flat at ~132 µs across two orders of magnitude of
relation size — the probe resolves 10 entries whatever the relation holds —
while the walk is linear in rows. The client ratio understates the engine
ratio because ~57 µs of every served probe is the Python client and socket
(§3). The pk controls (in-run noise floor) moved 0.0 / +1.7 / −2.2 µs p50;
every reported delta is 10–100× above that. The walk's p95 shoulder at 10k
(2,108 µs against a 1,439 µs p50) recurred in both runs and is unattributed
— nothing in today's observability separates a checkpoint-tick stall from a
scheduling one (`docs/observability.md` is still a proposal).

### The controller's own evidence

Time from `SET CABIN_OPTIMIZER ON` to the catalog row, and to the first
served probe:

| rows | create after | serving after | enabled ticks used | B (q16→dec) | C (q16→dec) | B/C | θ_create |
|---|---|---|---|---|---|---|---|
| 200 | 1.45 s | 1.45 s | 3 | 10,196 | 3.0 | ~3,400 | 3.0 |
| 1,000 | 1.43 s | 1.43 s | 3 | 68,487 | 13.0 | ~5,270 | 3.0 |
| 10,000 | 1.40 s | 1.40 s | 3 | 272,257 | 130.0 | ~2,094 | 3.0 |

Three consecutive 500 ms snapshots — `confirm_snapshots = 3` behaving as
specified, and the create lands on the third. Two observed identities worth
recording: **C equals the walk's measured page cost exactly** (3 / 13 / 130,
the same numbers `ANALYZE` prints as `pages=`) — the observed scan is the
observed build cost, as PHY02's header says — and serving begins in the
same 50-probe block as the create, because the n=2 observation (`misses=2`
below, exactly, at every size) is absorbed in milliseconds at this probe
rate.

The managed entry (10k, end of run):

```
rel=coh_10k_r2 column=val state=ACTIVE cabin_id=3 pages=1 streak=0
benefit_q16=17842569216 cost_q16=8519680 hint_fail_pct=0 coverage_miss_pct=0
last_action=CREATE reason=sustained-benefit
```

`SHOW CABINS` for the same Cabin: `origin=auto status=active observed=1
entries=10 hits=1225 misses=2` — the ownership tag that separates an
engine-created Cabin from a declared one, and the **post-create hit rate:
99.8 %** (2 misses ever, the two probes the n=2 rule requires before a
value's set is observed). `ANALYZE` before and after, same statement, same
`pattern_id`:

```
walk:   step 0 FilterScan  coh_10k_r2                    examined=10000 pages=130
served: step 0 CabinProbe  coh_10k_r2 cabin=3 on=col1 value=7 cabin_optimizer=true
        opens=1 examined=10 matched=10 sel=100% pages=20 cabin_hits=1 cabin_entries=10 hint_hits=10
```

`cabin_optimizer=true` is PHY06's managed-probe mark; `pages=20` is 10
entries × one root + one leaf per pk resolve through the clustered tree.
`--verify` compared a walked reply against a served reply row-for-row,
order included, at every size: **identical** — required, since a Cabin
chooses where to look, never what is visible.

---

## 3. Where the latency goes

The measured unit is one statement round trip from a Python client. Its
decomposition, per component, for the improve case's 10k rows:

| wait type | walk (1,528 µs mean) | served (136 µs mean) | how measured |
|---|---|---|---|
| server CPU (execute + format) | 1,458 µs (95 %) | 75 µs (55 %) | `/proc/<pid>/stat` per 100-probe block |
| client + socket round trip | ~70 µs (5 %) | ~60 µs (44 %) | remainder; consistent with this harness's documented ~100 µs floor and the ~126 µs pk-lookup rows |
| read (page fetch) wait | 0 | 0 | all pages resident after load; nothing evicts engine-wide, so a warm walk does no I/O — the walk cost is CPU over resident pages |
| durability / commit wait | n/a | n/a | read-only statement |
| lock / conflict wait | n/a | n/a | single connection, no concurrent writer; core-local latches have no contention to wait on at `cores = 1` |

The null case's insert rows are the durability wait's row: ~1,040 µs p50 of
which the group-commit fsync on EBS is the overwhelming share (a batch of
one is a batch) — unchanged across all three arms, which is the write-path
half of the null result. What cannot be decomposed further with today's
surfaces: the in-server split between queueing and execution, and the
walk's p95 shoulder (§2) — both need the tracing `docs/observability.md`
proposes.

---

## 4. Against PostgreSQL

PostgreSQL 17.10, the scratch cluster of `tools/pg_setup.sh` on port 15433
at defaults, same rows from the same generator, same probes, via
`tools/pg_cabin_optimizer_benchmark.py`. The comparison's point is not the
seq-scan race: it is that **at defaults PostgreSQL has no counterpart to
this feature** — nothing observes the hot predicate and builds a serving
structure for it, so its plan on the five-thousandth probe is the plan on
the first (`EXPLAIN (ANALYZE, BUFFERS)` per size confirmed `Seq Scan`,
buffers 2 / 8 / 74).

| phase | ops | mean µs | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| pk-200[control] | 300 | 192.2 | 146.1 | 185.6 | 186.8 | 214.1 | 224.0 | 435 |
| probe-200[seqscan] | 1,200 | 253.0 | 183.5 | 229.1 | 232.2 | 297.6 | 816.0 | 1,050 |
| pk-1k[control] | 300 | 191.7 | 144.5 | 181.5 | 186.8 | 224.1 | 298.8 | 479 |
| probe-1k[seqscan] | 1,200 | 328.5 | 268.9 | 311.8 | 318.0 | 373.6 | 457.1 | 789 |
| pk-10k[control] | 300 | 193.0 | 146.3 | 187.0 | 190.1 | 218.7 | 241.0 | 423 |
| probe-10k[seqscan] | 1,200 | 1,291.1 | 1,192.2 | 1,221.3 | 1,238.0 | 1,694.6 | 2,007.0 | 5,117 |

| rows | KDS walk p50 | PG seq scan p50 | KDS served p50 | served vs PG |
|---|---|---|---|---|
| 200 | 153.9 µs | 232.2 µs | 131.2 µs | **1.8×** |
| 1,000 | 259.5 µs | 318.0 µs | 131.9 µs | **2.4×** |
| 10,000 | 1,439.2 µs | 1,238.0 µs | 131.9 µs | **9.4×** |

Both columns carry each client's socket cost (~100 µs KDS, ~150 µs PG per
the pk controls). Two honest notes. Before the Cabin exists, **PostgreSQL's
walk beats KDS's at 10k by 14 %** — 74 buffers against 130 pages for the
same 10,000 rows, which is the 64-byte `inline_cell_width` padding tax on
row density, visible here as a scan-speed gap. And an operator who declared
an index on either engine would beat both walks — the comparison at
defaults is the honest one for a feature whose whole point is acting
*without* the operator.

---

## 5. What this run says about the engine

**A self-created Cabin is a declared Cabin plus a decision, and the
decision was priced at ~2–3 µs per tick.** After `CREATE`, the optimizer's
Cabin is the same store, the same probe step, the same n=2 observation and
the same verification path as `bench/results-cabin.md`'s declared one — the
only new artifacts are the `origin=auto` tag, the ANALYZE mark, and the
managed entry. So the two documents differ in one variable only, and it is
not the machinery.

**That variable is the hit rate, and it belongs to the workload.**
`results-cabin.md` measured a wash on disk (+3.2 % TPS, p50 flat) at a
23.9 % hit rate — 5,900 probes drawn uniformly over 10,000 users, where the
arithmetic of random sampling caps the repeat probability. This workload
re-probes one value and the hit rate is 99.8 %, worth −90.8 % on the hot
statement's p50. The optimizer does not change that arithmetic; what it
adds is that the CREATE now *requires* measured repetition (B/C over
θ_create for three consecutive snapshots) instead of an operator's guess —
the B/C ratios here were 2,000–5,300 against a threshold of 3, so this
workload was the easy case, and this run says nothing about where the
threshold bites on a marginal shape. It also says nothing about the
lifecycle past CREATE — no DECAYING or DROP was exercised here; the
deterministic replay harness (`tests/cabin_optimizer_replay_test.cpp`,
PHY07) owns those paths.

**The Cabin's crossover is not the index's crossover.** `results-index.md`
measured a secondary index *losing* 11 % on a range at 200 rows; the
optimizer's Cabin at 200 rows still won 15 % on p50 and 31 % on server CPU,
because a 3-page walk still decodes 200 rows while the probe resolves 10
through 20 resident pages. At this engine's row widths the observed-value
equality shape has no size floor worth defending — which retroactively
supports treating θ_create as a frequency-times-cost gate rather than a
minimum-relation-size one.

**The null result held under a 1,000× cadence handicap.** Every idle-cost
number in §1 was taken at a 10 ms snapshot interval; the shipped default is
10 s. A controller whose enabled tick costs 2–3 µs of CPU and whose
disabled tick is one predicate is background machinery the statement path
cannot see — consistent with PX07's zero-idle-cost verdict for the shadow
planner, and the second data point for the pattern that this engine's
optimizer components observe for free and pay only when they act.
