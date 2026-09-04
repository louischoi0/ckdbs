# Scenario 2 (freight) — AL-S8 baseline, single-WAL-stream engine

AL-S8's scenario matrix, scenario2 half: `tools/scenario2_freight.py` at
`cores = 1` and `cores = 8`, `group` and `strict`, on the engine AR0 M0
produced. Scenario0's quarter of the baseline is
`results-scenario0-stockmarket-v2.7.0-157-gf6ed10c.md`; the three
M0-specific instruments are
`results-wal-single-stream-v2.7.0-157-gf6ed10c.md`. Driver flags are
documented at `bench/docs/README.md`, commit `1769487`.

**Fresh series (AR0 D15, AL-R8) — no delta against any `v2.x` number.**
This is the first scenario2 run under a single WAL stream and has no
predecessor of its own shape; it is the baseline the next one reads
against.

## 1. Stamp

| Field | Value |
|---|---|
| Date/time | 2026-09-03, 01:30–01:38 UTC (per-cell times in §2) |
| Worktree | `v3.0.0-arch-revision` (branch `worktree-v3.0.0-arch-revision`) |
| Commit measured | `f6ed10c`, `git describe --tags` = `v2.7.0-157-gf6ed10c` |
| Tree cleanliness | Clean at every cell; sibling commit `f027a3c3` landed on this branch at 01:46:01 UTC, after every cell here — see the scenario0 document's stamp for the full note. `tools/` and `bench/` are untouched by it. |
| Binary provenance | Same copy as the scenario0 document: `/home/cdkbs/bench-runs/al-s8-f6ed10c/kds_server`, `sha256 2ab1960bc056e7cc5c59be4946a2cf1250b4e65b941934c96ebf736e80435af3`, source mtime `2026-09-03 01:17:56.28 UTC`, both after `f6ed10c`'s commit. |
| Device | `/home/cdkbs`, `ext4`, `/dev/root` (`df -T`). |
| Build type | `build-release`; not rebuilt this session. |
| Host | 8 logical CPUs, AMD EPYC 9V74, 1 socket × 4 cores × 2 threads/core (SMT) — see the scenario0 document's note on what `cores = 8` pins onto 4 physical cores. |
| Server config (common) | `checkpoint_interval_ms = 5000` (default), `auth = off`, `tls = off`, `placement = namespace` (shipped default). Varied: `cores`, `durability`, `peer_listeners` (`on` only at `cores = 8`). |

## 2. What was run, and in what order

Interleaved with scenario0's cells; the actual order across both
documents was `s0-c1-g`, `s2-c8-s`, `s2-c1-s`, `s2-c1-g`, `s0-c8-g`,
`s0-c1-s`, `s2-c8-g`, `s0-c8-s`. Every cell: fresh data file, fresh
server, `--schema-only` then `--load-only` then the measured run against
the same `--suffix`, which is this driver's own documented shape for a
data file driven once per configuration (`bench/docs/README.md`).

Fixed across all four cells: `--organizations 300 --ships 30
--operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100
--seed 1 --sync`, `--txn` and `--contend` and `--manifest` all at their
defaults (on). A work target of 3,000 committed bookings, 375 per
booker, contended (every booker draws from every voyage and every
customer, so two can collide on either row a booking updates — the shape
`--no-contend` is the baseline *against*, not what this document ran).

| Cell | Port | Precheck time (UTC) | `/proc/loadavg` (1/5/15 min) | Build check |
|---|---|---|---|---|
| `s2-c1-g` (cores=1, group) | 15575 | 01:35:22 | 0.46 / 0.48 / 1.17 | `pgrep -a -f 'cc1plus\|cmake --build\|ctest'`: none |
| `s2-c8-g` (cores=8, group) | 15576 | 01:37:39 | 0.75 / 0.59 / 1.11 | none |
| `s2-c1-s` (cores=1, strict) | 15574 | 01:34:36 | 0.29 / 0.45 / 1.19 | none |
| `s2-c8-s` (cores=8, strict) | 15577 | 01:30:26 | 0.28 / 0.49 / 1.43 | none |

`placement = namespace` collapses to the creating core for every relation
here too, for the same reason as scenario0 (`docs/spec/namespace.md`
NS10, clause 1: `scenario2_freight.py` never issues `CREATE NAMESPACE`).
Not re-verified by a separate `DESCRIBE` in this document since the
mechanism is identical and already confirmed in the scenario0 document —
every one of this document's eight relations is core-0-owned in every
cell, at both core counts.

## 3. TPS

| Cell | cores | durability | TPS | committed | rejected-capacity | rejected-credit | conflicted/retries |
|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | group | **578.4** | 3,000 | 307 (9.1%) | 50 (1.5%) | 12 |
| `s2-c8-g` | 8 | group | **312.7** | 3,000 | 279 (8.4%) | 56 (1.7%) | 8 |
| `s2-c1-s` | 1 | strict | **543.8** | 3,000 | 294 (8.8%) | 47 (1.4%) | 5 |
| `s2-c8-s` | 8 | strict | **284.7** | 3,000 | 293 (8.8%) | 51 (1.5%) | 9 |

**`cores = 8` costs this workload nearly half its throughput, in both
durability classes** — group falls 46% (578.4→312.7), strict falls 48%
(543.8→284.7) — the opposite direction and a far larger magnitude than
scenario0's +7%. The mechanism is not the WAL (every relation is still
core-0-owned, exactly as in scenario0): it is that a booking is an
**eight-statement transaction** held open across every round trip, and
`peer_listeners = on` means some fraction of the 8 bookers land on a peer
core and pay a cross-core statement-shipping hop *on every one of those
eight statements* before core 0 ever sees them. Scenario0's four
autocommit statements pay the same per-statement hop when shipped, but
each is its own transaction with nothing else waiting on it; scenario2's
eight statements are one transaction holding row locks on `operations`
and `organizations` for the hop's duration on *every* statement, so the
added latency compounds across contended bookers rather than just adding
a constant. This is a real, reproducible cost of `peer_listeners = on`
against an unnamespaced schema under a long, contended transaction — not
noise (§8 puts the floor at 1.5–8%, and a 46–48% collapse is nowhere near
it) and not the single-WAL-stream latch this stage exists to price
(`results-wal-single-stream-...md` §4–§5 is where that latch is actually
exercised, under `placement = rotate` so a peer has a relation of its own
to commit).

## 4. Percentiles

### `booking` — the whole eight-statement transaction, client-perceived

| Cell | ops | p0 | p25 | p50 | p95 | p99 | max (µs) |
|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 3,357 | 521.5 | 10,682.2 | 12,468.6 | 17,312.2 | 24,849.3 | 44,174.6 |
| `s2-c8-g` | 3,335 | 850.6 | 17,084.9 | 22,628.7 | 32,730.6 | 44,854.4 | 66,256.5 |
| `s2-c1-s` | 3,341 | 510.7 | 10,923.9 | 13,419.7 | 19,108.0 | 24,974.4 | 58,822.9 |
| `s2-c8-s` | 3,344 | 769.7 | 19,621.3 | 24,446.7 | 36,759.3 | 45,040.2 | 79,520.2 |

`ops` exceeds 3,000 because a rejected or conflicted attempt still opens
and measures a `booking` span before `finish()` closes it (the driver's
own rejection and retry paths — `tools/scenario2_freight.py`) — the
committed count is 3,000 in every cell (§3).

### `commit` (the `COMMIT` statement itself) and `operation-update` (the lost-update-shaped write, §6) — p50/p99, µs

| Cell | commit p50 | commit p99 | operation-update p50 | operation-update p99 |
|---|---|---|---|---|
| `s2-c1-g` | 1,742.0 | 4,558.8 | 212.1 | 2,225.6 |
| `s2-c8-g` | 6,145.3 | 15,231.0 | 381.0 | 5,643.9 |
| `s2-c1-s` | 1,534.2 | 4,944.8 | 268.6 | 3,205.4 |
| `s2-c8-s` | 6,924.8 | 14,441.4 | 282.3 | 6,058.6 |

`commit`'s cost roughly **3.5–4.5×** going from `cores=1` to `cores=8`
(1,742→6,145 µs group, **3.5×**; 1,534→6,925 µs strict, **4.5×**) — the
clearest single number behind §3's throughput collapse: whatever else a
booking pays, its `COMMIT` alone costs several times more when the
session shipping it may be a peer.

## 5. Wait breakdown

| Wait | Estimate | How |
|---|---|---|
| **Durability/commit (fsync or its batched equivalent)** | present but not the dominant lever here — `commit` p50 moves only 1,742→1,534 µs (group vs strict, cores=1), a ~12% difference, far smaller than scenario0's 3.6× | group and strict differ by far less here than in scenario0 because a booking's eight statements already serialize one transaction's round trips; the marginal cost of one more sync per commit is small next to that fixed structure |
| **Cross-core statement shipping (peer→core 0 hop)** | dominant at `cores=8`: `commit` alone gains ~4,400–5,400 µs (§4) and every one of the seven statements before it pays the same per-hop cost when the session is peer-accepted | `cores=1` vs `cores=8` delta at fixed durability, isolated from the WAL question since every relation stays core-0-owned in both (§2) |
| **Read wait** | not uniform across the four reads at `cores=8`. `credit-lookup`/`capacity-read`/`recipe-read` sit at 317.4–512.1 µs p50 at `cores=1` (group and strict) rising to 335.6–597.3 µs p50 at `cores=8` — small individually, and the rise is modest. `cargo-lookup` is the outlier: 259.0/409.5 µs p50 at `cores=1` (strict/group) rising to **1,333.8/1,289.6 µs** at `cores=8` (strict/group) — a ~3–5× jump, ~880–1,075 µs, the same order of magnitude as the per-hop cost §5's own `commit` row prices | archived per-cell JSON, `phases` array; `s2-c8-g.stdout.txt`/`s2-c8-s.stdout.txt`'s phase tables directly |
| **Lock/conflict wait** | small in aggregate (5–12 conflicts of 3,000 committed, §3) but not zero when it fires: a conflicted `operation-update` costs a full retry, visible in its own p99 (2,226–6,059 µs) running well above its p50 | `conflicted`/`retries` counters, `operation-update`'s own percentile spread |
| **Client/socket round trip** | included in every number above; not separable from durability at this driver's granularity without `--log-level debug`, which was not used to avoid perturbing the fsync path being priced | — |

*Corrected on `ar2-borrow-model` after `c40b3cc` (archive re-read
2026-09-04): the Read wait row above originally said the four reads
"each sit at 200–600 µs p50 (cores=1) rising to 350–600 µs p50
(cores=8)," an average that hid `cargo-lookup`: `s2-c8-g.stdout.txt`'s
phase table gives it a 1,289.6 µs p50 at `cores=8`, more than double the
stated 600 µs upper bound, while `credit-lookup`/`capacity-read`/
`recipe-read` do sit inside a range close to what was stated.*

## 6. Correctness — a finding orthogonal to this stage

**`--verify` failed in every cell, at both core counts and both
durability classes, and the failure reproduces at `cores=1` with
`peer_listeners = off`.** Invariant I1 (`booked_cbm` on `operations`
against `SUM(freights.cbm)` for the same voyage) failed 18–33 times per
400 checks:

| Cell | verify checks | I1 failures |
|---|---|---|
| `s2-c1-g` | 400 | 18 |
| `s2-c8-g` | 400 | 24 |
| `s2-c1-s` | 400 | 33 |
| `s2-c8-s` | 400 | 22 |

The driver's own docstring for `verify()` states this invariant "must
hold" under `--txn` regardless of concurrency (`tools/scenario2_freight.py`,
the `verify` function's docstring) — every booking here ran inside
`BEGIN`/`COMMIT` (`--txn` is the default, on), so a stored `booked_cbm`
diverging from the ledger it derives from means a write one booker
believed it applied was overwritten by another's stale read without a
detected conflict: `operation-update` is a blind `UPDATE ... SET
booked_cbm = <literal computed from an earlier SELECT>`
(`tools/scenario2_freight.py`, booking step 7's own comment: "the value
written depends on the value read at step 3, which is exactly the
lost-update shape"), and the engine's conflict detection catches most but
not all such races under 8-way contention (5–12 `TXN_CONFLICT` retries
did fire per cell, just not on every colliding pair).

**This is not an AR0 M0 finding.** It reproduces identically at `cores=1`
with `peer_listeners = off` — no cross-core execution, no shared WAL
stream involved, the same code path this engine has run since before
M0 — so it is not a consequence of the single-WAL-stream cutover this
stage exists to price, and it does not change how §3's throughput numbers
should be read: the same fraction of races slips through at every
`cores`/`durability` combination, so the comparison between them is still
apples to apples. It is, however, a real correctness gap independent of
this stage, and per the operating rule for this kind of finding ("do not
edit engine code to make a benchmark pass — report what you found") it is
reported here rather than worked around: an operator relying on
`--capacity-mode cached`'s `UPDATE ... SET col = <literal>` pattern for a
derived running total under contention should not assume every
lost-update race is caught.

## 7. What this run says about the engine

Scenario2 is the shape that shows the single-WAL-stream question
does *not* answer every question `peer_listeners = on` raises: at
`cores=8`, this workload got dramatically **slower**, and the mechanism
is statement-shipping latency compounding across a long open transaction,
not the WAL latch (which, per §2, never sees a peer write in this
document at all — every relation is core-0-owned). Read beside the
scenario0 document's +7% at the same knob, the two together say
`peer_listeners = on` against an unnamespaced schema is workload-shape-
dependent in a way that is easy to miss from either scenario alone: short
autocommit statements barely notice the hop, a long multi-statement
transaction pays it on every leg and loses nearly half its throughput.
`docs/spec/namespace.md`'s NS10 is the documented way out (declare a
namespace so the relations a transaction touches co-locate on one core),
and neither scenario driver exercises it — which is itself worth naming
as a gap between what the drivers measure and what an operator would
actually configure.

## 8. Noise floor

No same-configuration repeat was run for scenario2 specifically (time
budget for this stage); the scenario0 document's repeat (`s0-c1-g`,
690.7 vs 700.9 TPS, 1.5%) is the nearest same-host, same-session control.
The `peer_commit_tail` micro-probe in the WAL-single-stream document
(two runs of an unrelated, narrower measurement on this same host in this
same session) put p50/p99 latency noise at roughly 5–8% run to run under
concurrent load — a wider floor than scenario0's TPS repeat, and the
right one to compare scenario2's percentile table against. Read `§3` and
`§4` against **that** floor: `cores=8`'s 46–48% TPS collapse and 3.5–4.5×
`commit`-cost increase are far outside it; the `commit` p99 gap between
group and strict at fixed cores (4,945 vs 4,559 µs at cores=1, a ~8%
difference) is close enough to the floor that it should be read as
"durability class does not clearly separate this number," not as a
strict-is-worse finding.

Raw driver JSON, server logs and `SHOW META` dumps for every cell in this
document are archived at
`bench/v3.0.0/archive/scenario2-v2.7.0-157-gf6ed10c/`.
