# AR2 §9 step 1, cell C1 — co-location ceiling (scenario 2, freight)

`tools/scenario2_freight.py` at `cores = 8`, `durability = group`, with
every session forced onto the owner core by turning off the other seven
listeners (`peer_listeners = off`), against the same shape at
`peer_listeners = on` (AL-S8's `s2-c8-g` re-run) and at `cores = 1`
(no peer path exists at all). This is AR2 §9's C1
(`instructions/v3.0.0/ar2-architecture-revision-borrow-model.md` §9 step
1): "how much of the 46% owner routing alone recovers." C2 (local
parallel inserts, scenario 0) is
`results-ar2-c2-spreading-v2.7.0-178-g92cb654.md`; the two share one run
and one archive (§9 below).

**Fresh series against this engine's own history (rule 4).** The
baseline is AL-S8's `s2-c8-g`/`s2-c1-g`
(`bench/v3.0.0/results-scenario2-freight-v2.7.0-157-gf6ed10c.md` §3),
measured on this same host about 6.5 hours earlier at a different commit.
PostgreSQL was not measured in this run — no floor number is reported
for this shape here; AL-S8's own file did not run the PostgreSQL twin
for scenario 2 either, so there is nothing to omit that a prior run
established.

## 0. A methodology note on how C1 was realized

AR2 §9 step 1 describes C1 in words as "the relations a booking touches
declared in one namespace (NS10)." That is not what this run varies, and
the substitution is deliberate, not a shortcut: AL-S8 already established,
for this exact driver and schema, that `placement = namespace` collapses
every one of `scenario2_freight.py`'s eight relations onto the **creating**
core regardless — `scenario2_freight.py` never issues `CREATE NAMESPACE`,
so `AssignOwnerCore` answers with the creating core on every run, which is
core 0 because DDL always runs there
(`results-scenario2-freight-v2.7.0-157-gf6ed10c.md` §2, confirmed there by
`DESCRIBE` on the scenario0 sibling document). **A namespace declaration
would be a no-op here**: there has only ever been one relation-owning core
in this driver's schema, with or without one. AL-S8's own 46% loss
(§3 there) happened with every relation *already* core-0-owned, which is
exactly what its own §3 says: "the mechanism is not the WAL... it is that
a booking is an eight-statement transaction... and `peer_listeners = on`
means some fraction of the 8 bookers land on a peer core." The variable
that matters is which core the *session* lands on, not which core the
*relation* is declared to prefer — and `peer_listeners` is the knob that
controls exactly that (`docs/spec/protocol.md`'s accept path,
`SO_REUSEPORT` across the per-core listeners). `peer_listeners = off`
collapses every session onto core 0's own listener, which is the only
mechanism available today that actually forces "every write lands where
the data already is" for this schema. §9 below names the AR2 sentence
this note recommends changing.

## 1. Stamp

| Field | Value |
|---|---|
| Date/time | 2026-09-03, 07:58:42–08:05:24 UTC (per-cell times in §2) |
| Worktree | `ar2-borrow-model` (branch `worktree-ar2-borrow-model`) |
| Commit measured | `92cb654`, `git describe --tags` = `v2.7.0-178-g92cb654` |
| Tree cleanliness | Clean at measurement time and at the time of writing this document (`git status --short` empty in this worktree). |
| Binary provenance | Copy at `/home/cdkbs/bench-runs/ar2-c1c2-92cb654/kds_server`, `sha256 d6b2c4202a929e545bace8d570cd6fec42640a58461536d4c4ab3709e3872f52` (verified in this session). Source `build-release/kds_server` mtime `2026-09-03 07:55:36.53 UTC`, copy mtime `07:57:09.40 UTC` (93 s later); commit `92cb654`'s own timestamp is `07:48:26 UTC`, so the binary was built **after** the commit it claims to measure, and every server in this document started from the copy, never from `build-release/` directly. |
| Engine delta against AL-S8's commit | `git diff --stat f6ed10c 92cb654 -- src include`: 13 files, 1082 insertions / 97 deletions — `bootstrap.{hpp,cpp}`, `server/core_runtime.{hpp,cpp}`, `server/expeditor.{hpp,cpp}`, `server/superblock.hpp` (AM-S0: the per-core-volume/`Expeditor` split), and `txn/instance_visibility.{hpp,cpp}` (new file), `txn/manager.{hpp,cpp}`, `txn/trx_id.{hpp,cpp}` (AN-S0/AN-S1/AN-S1b: the instance read view, trx-id leasing, the idle-core-burn fix). **None of this diff touches `src/exec`, `src/storage/heap`, `src/server/command_dispatcher.cpp` (statement shipping / `SHOW META`), or the query layer** — the exact mechanism this document measures is byte-identical to what AL-S8 measured. `tools/` is untouched (`git diff --stat f6ed10c 92cb654 -- tools` is empty of any modification, only new files under `bench/`). Also verified: `git diff --stat f710b3d 92cb654 -- src include` is empty, so the two commits between `origin/main`'s tip and this measurement (`841046a`, `92cb654`) are genuinely docs-only, confirming the binary's own provenance claim. |
| Device | `/home/cdkbs`, `ext4`, `/dev/root` (`df -T`, recorded per cell in `<cell>.cell.json`; unchanged across all cells, 48% used). |
| Build type | `build-release` (Release); not rebuilt this session — measured, not re-verified against `CMakeLists.txt`'s default (which is Debug). |
| Host | 8 logical CPUs, AMD EPYC 9V74, 1 socket × 4 cores × 2 threads/core (SMT) — confirmed by `lscpu` in this session, same topology AL-S8 recorded. |
| Ports | 15589 (unused smoke cell) through 15602, chosen explicitly in every `<cell>.conf`. `pgrep -a -f 'cc1plus|cmake --build|ctest|kds_server'` in every precheck shows one unrelated `kds_server` (pid 899, `/home/cdkbs/autotrade/...`, port 15432) running throughout and no concurrent build; it is not this run's. |
| Server config (common) | `checkpoint_interval_ms = 5000`, `auth = off`, `tls = off`, `placement = namespace` (shipped default), `durability = group` in every cell (strict was not run — say so, not a gap this document closes). Varied: `cores` (1 or 8), `peer_listeners` (`on`/`off`, moot at `cores = 1`). |

## 2. What was run, and in what order

Six cells of this run's thirteen are scenario 2; C2's four (plus repeats)
are scenario 0 and are `results-ar2-c2-spreading-v2.7.0-178-g92cb654.md`'s.
The two scenarios were interleaved cell by cell (headline pair, then the
`cores=1` reference, then a full repeat block) so that neither scenario's
numbers are read against a colder or warmer host state than the other's:

| Cell | Port | `cores` | `peer_listeners` | Precheck (UTC) | loadavg (1 min) | Build check | Driver exit(s) |
|---|---|---|---|---|---|---|---|
| `s2-c8-plon` | 15590 | 8 | on | 07:58:42 | 0.33 | clean | `[0, 0, 0]` |
| `s2-c8-ploff` | 15592 | 8 | off | 07:59:30 | 1.08 | clean | `[0, 0, 0]` |
| `s2-c1` | 15594 | 1 | off | 08:00:26 | 0.92 | clean | `[0, 0, 0]` |
| `s2-c8-plon-r2` | 15597 | 8 | on | 08:03:24 | 1.24 | clean | `[0, 0, 0]` |
| `s2-c8-ploff-r2` | 15598 | 8 | off | 08:03:53 | 1.12 | clean | `[0, 0, 0]` |
| `s2-c1-r2` | 15601 | 1 | off | 08:04:46 | 1.34 | clean | `[0, 0, 0]` |

Each cell: fresh data file, fresh server from the hashed binary copy,
`--schema-only` then `--load-only` then the measured run against the same
`--suffix` (this driver's documented per-configuration shape,
`bench/docs/README.md` at `1769487`). Fixed across all six cells, AL-S8's
own flags verbatim (`run_cells.py`'s `S2_ARGS`): `--organizations 300
--ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000
--verify 100 --seed 1 --sync`, `--txn`/`--contend`/`--manifest` at their
defaults (on) — a work target of 3,000 committed, contended bookings, 375
per booker.

`s2-c8-ploff*`'s own `SHOW META` probe is the mechanical proof that
`peer_listeners = off` does what §0 claims: `<cell>.meta.json` records
`meta_attempts: 500` (the probe's retry ceiling) and
`meta_cores_reached: [0]` for both `ploff` cells — 500 connection
attempts, all of them landing on core 0, because no other core has a
listening socket to land on. `s2-c8-plon*` reaches all 8 cores in under
45 attempts.

## 3. TPS

Rule 5a: throughput, not delay.

| Cell | cores | `peer_listeners` | TPS | committed | rejected-capacity | rejected-credit | conflicted/retries |
|---|---|---|---|---|---|---|---|
| `s2-c8-plon` | 8 | on | **322.1** | 3,000 | 292 (8.7%) | 51 (1.5%) | 12 |
| `s2-c8-plon-r2` | 8 | on | **321.1** | 3,000 | 269 (8.1%) | 55 (1.7%) | 8 |
| `s2-c8-ploff` (C1) | 8 | off | **565.7** | 3,000 | 285 (8.5%) | 52 (1.6%) | 10 |
| `s2-c8-ploff-r2` (C1) | 8 | off | **559.5** | 3,000 | 284 (8.5%) | 50 (1.5%) | 7 |
| `s2-c1` | 1 | off | 214.4 †contaminated, §8 | 3,000 | 300 (8.9%) | 54 (1.6%) | 11 |
| `s2-c1-r2` | 1 | off | **575.4** | 3,000 | 302 (9.0%) | 50 (1.5%) | 12 |
| `s2-c8-g` [measured, AL-S8] | 8 | on | 312.7 | 3,000 | 279 (8.4%) | 56 (1.7%) | 8 |
| `s2-c1-g` [measured, AL-S8] | 1 | off | 578.4 | 3,000 | 307 (9.1%) | 50 (1.5%) | 12 |

**With every session on the owner core, `cores = 8` recovers to within
1.7–2.8% of `cores = 1`** — `565.7`/`559.5` against the clean `575.4`
(`562.6` averaged: `(575.4−562.6)/575.4 = 2.2%`; individually `1.7%` and
`2.8%`). AL-S8's own `cores = 8` number (`312.7`, `peer_listeners = on`)
sat at 46% of its `cores = 1` number (`578.4`); this run's `ploff` pair
sits at 97–98% of its own `cores = 1` reference. **The hop is the whole
46% loss, not part of it**: nothing else distinguishes `cores = 8` from
`cores = 1` for this workload (every relation was already core-0-owned at
both core counts, per AL-S8 §2 and unchanged here), so once no session
pays the hop, `cores = 8` and `cores = 1` are statistically the same
engine doing the same work through one core either way. This run's own
`plon`/`plon-r2` (322.1/321.1) and clean `c1-r2` (575.4) match AL-S8's
`s2-c8-g`/`s2-c1-g` (312.7/578.4) within 3.0% and 0.5% respectively — see
§7 for why that gap is host/day noise, not an engine change.

## 4. Percentiles

Every row is a `Phase.summary()` distribution (`tools/bench_common.py`,
read directly from `<cell>.run.stdout.txt` — no percentile below is
recomputed).

### `booking` — the whole eight-statement transaction, client-perceived (µs)

| Cell | ops | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| `s2-c8-plon` | 3,343 | 915.4 | 12,913.2 | 19,482.7 | 32,189.8 | 38,485.4 | 69,106.4 |
| `s2-c8-plon-r2` | 3,324 | 872.5 | 12,926.6 | 19,732.1 | 33,136.5 | 41,963.9 | 63,728.1 |
| `s2-c8-ploff` | 3,337 | 617.1 | 11,018.8 | 12,741.9 | 17,424.1 | 25,391.0 | 64,765.8 |
| `s2-c8-ploff-r2` | 3,334 | 667.6 | 10,986.5 | 12,775.7 | 18,122.8 | 24,532.8 | 85,019.5 |
| `s2-c1` | 3,354 | 643.1 | 10,985.4 | 12,872.2 | 128,570.5 | 467,742.9 | 1,694,990.1 |
| `s2-c1-r2` | 3,352 | 1,235.0 | 10,930.1 | 12,576.3 | 17,070.3 | 24,667.8 | 59,203.9 |

`ops` exceeds 3,000 for the same reason AL-S8 recorded: a rejected or
conflicted attempt still opens and measures a `booking` span before the
driver's own `finish()` closes it.

### `commit` (the `COMMIT` statement itself), µs

| Cell | ops | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| `s2-c8-plon` | 3,000 | 1,171.1 | 3,880.6 | 5,808.8 | 9,623.8 | 12,420.1 | 38,640.0 |
| `s2-c8-plon-r2` | 3,000 | 1,105.9 | 3,876.8 | 5,791.6 | 9,597.5 | 13,067.1 | 41,558.7 |
| `s2-c8-ploff` | 3,000 | 1,279.8 | 1,621.6 | 1,778.8 | 3,344.1 | 4,446.3 | 52,736.3 |
| `s2-c8-ploff-r2` | 3,000 | 1,273.1 | 1,587.1 | 1,745.9 | 3,431.2 | 5,215.1 | 11,073.7 |
| `s2-c1` | 3,000 | 1,220.9 | 1,580.6 | 1,760.2 | 4,168.1 | 149,426.8 | 1,359,390.7 |
| `s2-c1-r2` | 3,000 | 1,254.0 | 1,601.3 | 1,759.8 | 3,336.7 | 4,056.0 | 38,966.7 |

`commit` alone moves **3.3×** at p50 between `ploff` and `plon`
(1,778.8→5,808.8 µs; 1,745.9→5,791.6 µs in the repeat) — the same shape
AL-S8 found between its `cores=1` and `cores=8` cells (§5).

## 5. Wait breakdown

Rule 3: name each wait and give it a share.

| Wait | Estimate | How |
|---|---|---|
| **Cross-core statement shipping (peer→core 0 hop)** | dominant: accounts for the *entire* `cores=8` throughput loss (§3), not merely the largest share | `ploff` vs `plon` at fixed `cores=8`, isolating the hop cleanly since nothing else differs between the two configs (same core count, same relation ownership) |
| **Durability/commit (fsync or its batched equivalent)** | `ploff`'s own commit p50 (1,778.8/1,745.9 µs, no hop at all) is the durability-plus-RTT floor for this shape; `strict` was not run so the fsync-only component cannot be isolated further this run | `ploff` commit percentiles, §4 |
| **The hop is not evenly spread across the transaction's 8 statements — the `COMMIT` pays a disproportionate share, and one read pays roughly double the average of the rest** | `commit`'s own p50 delta (`plon`−`ploff`) is 4,030.0 µs (`plon-r2`: 4,045.7 µs) against a *whole-booking* p50 delta of 6,740.8 µs (`r2`: 6,956.4 µs) — **59.8%/58.2%** of the entire added latency sits in the one statement that both ships *and* then waits on core 0's own group-commit drain. Averaged across the other 7 statements the remainder is roughly 387–416 µs each, but that average hides real skew: `cargo-lookup` alone rises ~847 µs p50 (421.8→1,268.5 µs; `r2`: 438.1→1,257.8 µs) — close to double the per-statement average, and the size of §5's own 842.6 µs whole-booking-over-8 estimate — while the other three reads and the inserts move far less | per-phase p50 arithmetic, §4; consistent within 1.6 points across the repeat pair |
| **Read wait** | not uniform across the four reads. `credit-lookup`/`capacity-read`/`recipe-read` sit at 373.5–537.2 µs p50 in `ploff` (`r2`: 376.5–525.7 µs) and 336.8–539.1 µs p50 in `plon`/`plon-r2` — essentially flat, no hop cost visible. `cargo-lookup` is the outlier: 421.8/438.1 µs p50 in `ploff`/`ploff-r2` rising to 1,268.5/1,257.8 µs in `plon`/`plon-r2` — a ~3× rise, ~847 µs, matching the hop estimate above almost exactly. Individually still small next to the commit's ~4 ms, but not the uniform 220–540 µs this row originally reported | `<cell>.run.stdout.txt` phase table |
| **Lock/conflict wait** | small and roughly flat across configurations (7–12 conflicts of 3,000 committed in every cell, §3); not the mechanism behind the `cores=8` loss since it does not move with `peer_listeners` | `conflicted`/`retries` counters |
| **Client/socket round trip** | included in every number above; the driver is a single Python connection per booker, so this floor is shared with AL-S8's own caveat about not separating it further without perturbing the durability path being priced | — |

*Corrected on `ar2-borrow-model` after `c40b3cc` (archive re-read 2026-09-04): the two rows above originally read "the other ~7 statements split the remainder at roughly 387–416 µs each, closer to a plain routing hop with no durability wait stacked on it" and "sit at 220–540 µs p50 in `ploff`, rising to 340–540 µs p50 in `plon`." Both are averages that hid a single outlier: `<cell>.run.stdout.txt`'s phase table shows `credit-lookup`/`capacity-read`/`recipe-read` essentially flat (373.5–537.2 µs p50 in `ploff`, 336.8–539.1 µs in `plon`), but `cargo-lookup` rises from 421.8/438.1 µs to 1,268.5/1,257.8 µs — a ~3× jump the "220–540"/"387–416" ranges concealed. The 387–416 µs per-statement average itself is arithmetically correct (it is not restated); what was wrong is reading it as evenly spread.*

**Engine-side corroboration from `SHOW META`.** Core 0's own
`sched_foreground_polled_us` per booking is nearly flat between
configurations — 1,004.2 µs/booking in `ploff` (`3,012,739 / 3,000`,
where core 0 does 100% of the work itself) against 1,076.1 µs/booking in
`plon` (`3,228,153 / 3,000`, where core 0 also services every shipped
statement) — a **7.2% increase**, not 46%. Core 0's own service cost per
shipped statement is cheap: `3,228,153 / 41,060 shipped_executed = 78.6
µs`. The loss is therefore almost entirely on the **requesting peer's**
side of the hop — the wait for the reply — which `SHOW META` does not
expose as a summed quantity, only as a per-core maximum
(`shipped_wait_us_max`, 22,619–39,141 µs on the **nine** peers carrying
substantial ship traffic — over 1,000 `shipped_statements` — across both
`plon` cells (`plon`: cores 1, 3, 5, 7; `plon-r2`: cores 2, 3, 4, 5, 6),
landing right around the `booking` phase's own p99:
consistent with, though not proof of, the hop's wait being what fills
the tail. The **three** peers with only a handful of shipped statements each
(`plon` core 2 = 8, core 4 = 300; `plon-r2` core 7 = 8) show 10,246–40,619 µs instead — too small a sample per peer
to read as anything but noise, not included in the range above). A
per-commit or per-shipped-statement **mean** wait counter split by core
does not exist today; that is what would turn this corroboration into a
full accounting.

*Corrected on `ar2-borrow-model` after `c40b3cc` (archive re-read
2026-09-04): the paragraph above originally said "six peers" and "the
two peers with only a handful." Recounting `<cell>.meta.json`'s
per-core `shipped_statements` for both `plon` cells gives **nine** cores
above 1,000 (`plon` cores 1/3/5/7, `plon-r2` cores 2/3/4/5/6) and
**three** with a handful (`plon` core 2 = 8 and core 4 = 300, `plon-r2`
core 7 = 8) — the 22,619–39,141 µs and 10,246–40,619 µs ranges
themselves were already correct for these sets and are unchanged.*

**Per-statement hop cost.** Averaging the whole-booking delta over 8
statements, per AR2's own estimate: `(19,482.7 − 12,741.9) / 8 ≈ 842.6 µs`
in `plon` vs `ploff` (`r2`: `(19,732.1 − 12,775.7) / 8 ≈ 869.6 µs`) — the
`≈0.85 ms` figure §0 and AR2 §1 use. §5's own breakdown above shows this
average hides real skew: the commit's hop costs ~4.0 ms, the other seven
statements ~0.4 ms each.

## 6. Correctness

**`--verify` failed in every cell, at both core counts and both
`peer_listeners` states**, reproducing AL-S8's finding exactly: I1
(`booked_cbm` on `operations` against `SUM(freights.cbm)`) failed
16–32 times of 400 checks in every cell of this document (`s2-c8-plon`
30, `plon-r2` 20, `ploff` 18, `ploff-r2` 23, `c1` 32, `c1-r2` 16). This is
not a finding of this document — AL-S8 already traced it to
`tools/scenario2_freight.py`'s own lost-update-shaped `operation-update`
statement (`UPDATE ... SET booked_cbm = <literal>`, its own step-7
comment) racing under 8-way contention faster than `TXN_CONFLICT`
catches every colliding pair
(`results-scenario2-freight-v2.7.0-157-gf6ed10c.md` §6). It is named here
only so this document's own `--verify` output is not read as new
evidence of anything: the failure count does not track `peer_listeners`
or `cores` (18–32 failures regardless of configuration), so it is
orthogonal to what this document measures, exactly as AL-S8 found.

## 7. AL-S8 delta, and why it reads as noise

This document's engine (`92cb654`) differs from AL-S8's (`f6ed10c`) by
the AM-S0 and AN-S0/AN-S1/AN-S1b commits named in §1 — none of which
touches the statement-shipping or execution code this measurement
exercises (verified by `git diff --stat`, §1). The two runs' matching
cells agree within the range this suite has already established as noise
(AL-S8's own §8: 1.5% on a same-configuration repeat; the
`peer_commit_tail` probe put run-to-run latency noise at 5–8% under
concurrent load):

| Shape | This run | AL-S8 | Delta |
|---|---|---|---|
| `cores=8`, `peer_listeners=on` | 322.1 / 321.1 | 312.7 | +3.0% / +2.7% |
| `cores=1` (clean) | 575.4 | 578.4 | −0.5% |

Both deltas sit inside or at the edge of AL-S8's own stated floor. **Read
this as host/day variation, not as anything the AM-S0/AN-S1 commits
changed** — nothing in their diff touches this code path, and the
direction is inconsistent with a real effect anyway (`plon` reads
*faster* than AL-S8 while `c1` reads marginally slower). §8 below has
this run's own, tighter internal floor for the same comparison.

## 8. Noise floor, and the one contaminated cell

**`s2-c1` (first run, 214.4 TPS) is contaminated and excluded from every
comparison above except its own row in §3.** Its own repeat, `s2-c1-r2`
(575.4 TPS, run 08:04:46 UTC), is the clean reference and matches AL-S8's
`s2-c1-g` (578.4) within 0.5%. The contamination is visible directly in
the raw percentiles, not inferred from the TPS gap alone: `s2-c1`'s
`booking` p50 (12,872.2 µs) sits within 2.4% of its own repeat's p50
(12,576.3 µs) — **the body of the distribution is fine** — but its p99
(467,742.9 µs) and max (1,694,990.1 µs, 1.7 s) are catastrophic outliers
absent from the repeat (p99 24,667.8 µs, max 59,203.9 µs), and the same
shape appears in `commit` (p99 149,426.8 µs / max 1,359,390.7 µs against
the repeat's 4,056.0 µs / 38,966.7 µs) and in `load-cargos` (max
961,231.7 µs during the *load* phase, before any booker ran). This is a
handful of statements landing on a severe, brief external stall — most
plausibly a single host-level event (the precheck's `pgrep` shows no
concurrent build, so it is not that) — not a sustained shift in the
engine's own cost, which is why the table keeps the number but does not
use it.

**The run-to-run floor from the clean repeat pairs**, all within the same
run and thus not subject to §7's cross-run day-to-day question:

| Pair | Values | Spread |
|---|---|---|
| `s2-c8-plon` / `-r2` | 322.1 / 321.1 | 0.3% |
| `s2-c8-ploff` / `-r2` | 565.7 / 559.5 | 1.1% |
| `s2-c1-r2` alone (no clean partner — `s2-c1` is excluded) | 575.4 | n/a |

The `plon`/`ploff` pairs' 0.3–1.1% spread is the floor this document's
findings are read against. §3's 46-point (`plon` vs `ploff`) gap and
§4/§5's percentile and wait-breakdown deltas are far outside it; the
`plon` vs `plon-r2` and `ploff` vs `ploff-r2` differences themselves are
not findings — they are exactly what this floor predicts.

## 8a. Row-set size — not swept, named as a gap

Every cell in this document runs the one shape AL-S8 established and
this run reproduces: 3,000 bookings, 300 organizations, 4,000 cargos.
Rule 9 asks for a sweep at 200/1K/10K bookings at minimum so a fixed
per-transaction cost can be told apart from a per-row one; this run does
not do that — it was specified as a fixed-shape configuration re-run
(AR2 §9 step 1, and the task that produced this document), not a
cardinality sweep, and the instruction governing this session was to
document a run already taken, not to take a new one. **This document
therefore cannot say whether the ≈0.85 ms/statement hop or the
≈4.0 ms `COMMIT` hop (§5) is flat per transaction or grows with the
`--bookings`/`--cargos` shape.** That is a real gap, not a finding: a
future C1-shaped run at a second size would close it.

## 9. What this means for AR2

Restated as the answer to §9 step 1's own question: **the hop is the
whole 46% loss AL-S8 found, not merely its largest component** — a run
with no cross-core statement shipping at all (`ploff`) recovers to
within 1.7–2.8% of the single-core reference, and this run's own
`SHOW META` data (§5) shows core 0's own service cost for a shipped
statement is cheap (≈79 µs); the loss lives in the requesting peer's own
wait for the reply, which no counter sums today.

Consequences for the draft
(`instructions/v3.0.0/ar2-architecture-revision-borrow-model.md`):

- **§9 step 1's own text for C1** — "the relations a booking touches
  declared in one namespace (NS10)" — describes a mechanism that is a
  no-op for this driver and schema (§0). Recommend: name
  `peer_listeners = off` (or, more generally, "every session routed to
  the range's affinity core") as what C1 actually measures, since NS10
  governs relation placement and this workload's relations were already
  co-located before either arm ran.
- **§1's "measured premise"** bullet — "the cost of owner routing on a
  long transaction: scenario 2 at `cores = 8` loses 46%... attributed to
  the shipping hop compounding across an eight-statement transaction" —
  is now `[measured]` rather than `[attributed]`: this document isolates
  the hop directly (fixed `cores=8`, only `peer_listeners` varies) and
  the recovery is complete, not partial. Recommend citing this document
  beside that bullet.
- **§1's "read together" paragraph** — "a local write costs what an
  owner's write costs, and the hop costs up to half the throughput" —
  holds exactly for this shape at this concurrency, with the addition
  this document supplies: the hop's cost is not uniform across a
  transaction's statements (§5). A `COMMIT` that ships pays roughly 10×
  what a plain read or write statement's hop costs, because it stacks a
  routing wait on top of a durability wait rather than paying either
  alone. Recommend AR2 §5.5 ("Statement shipping and cross-owner
  transactions," the section that already owns the executor/affinity-route
  discussion) name this asymmetry rather than treat
  "the hop" as one flat cost — it matters for E7's default (§7 there):
  a design that routes ordinary statements locally but still ships a
  cross-owner commit would keep paying this disproportionate tax on
  exactly the statement that closes out the transaction.
- **C3** (contention under the row lock, §9 step 5 there) still cannot
  run before M2, unchanged by this document.

Raw driver JSON, `SHOW META` dumps, server logs, configs and orchestrator
scripts for every cell in this document (and C2's) are archived at
`bench/v3.0.0/archive/ar2-c1c2-v2.7.0-178-g92cb654/`
(`results-ar2-c2-spreading-v2.7.0-178-g92cb654.md` is the other half of
this run).
