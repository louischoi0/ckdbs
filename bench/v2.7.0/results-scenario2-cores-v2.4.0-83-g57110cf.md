# Scenario 2 at 2, 4 and 8 cores — where the second core takes the booking

**Adding cores to this workload moves throughput in *both* directions, and
which one you get is decided by a config key nobody thinks of as a
performance knob.** On the shipped default — one listener, every session on
core 0 — scenario 2 gains monotonically with `cores`: **812 → 819 → 901 →
1,026 bookings/s** at 1/2/4/8, a **1.26×** at eight. Turn on
`peer_listeners`, so sessions actually land on the other cores, and the same
eight cores run the same workload at **586 bookings/s — 0.72× of one core**,
having bottomed at **0.29×** at two cores and one booker.

**The loss is not the split.** With `range_size_ids = 0` — nothing ever
splits, no range, no fan-in, no multi-owner relation — a peer-listener
instance still runs at **0.53×/0.64×/0.70×** of single-core. Insert
spreading accounts for **at most 13% of the gap** — 11.9% of it at two
cores, 13.0% at four, and none at eight. What costs is that a peer-accepted session under `placement =
creating` owns nothing: `SHOW META` on core 0 reads **`shipped_executed =
39,497`** against a workload of ~40,000 statements, so **essentially every
statement of every booking is carried to core 0 and run there**, and the
eight-statement booking becomes a cross-owner transaction that commits over
2PC.

**Removing the fsync separates the two costs cleanly.** Under `durability =
relaxed`, one listener, `cores = 1` and `cores = 8` are the same engine to
within 0.3% (**5,789 vs 5,807 bookings/s**) — so arm B's 1.26× was the
commit path, never scaling. And with the fsync gone the cross-core cost
stands naked: the same eight cores with peer listeners run at **935
bookings/s, 6.21× slower**, and a commit that costs **76.6 µs** on core 0
costs **3,053 µs** — about **3.0 ms of two-phase commit with no device in
it**.

**Spreading itself works, and the commit eats it.** From `cores = 1` to
`cores = 8` with peer listeners, `freight-insert` p50 falls **359.8 → 41.3
µs** and `charge-insert` p50 **146.8 → 27.0 µs** — each core appending to
its own range's chain tail, exactly what R4 buys. Over the same step the
commit rises **1,716.9 → 4,913.5 µs**. The ledger write got 8.7× cheaper and
the booking still lost.

**And the stock workload does not complete at `cores ≥ 2`.** 25 statements
refuse per cell, always the same shape — `freights JOIN cargos`, which
`CLAUDE.md` already names as unbuilt over a spread relation. The
consequence nothing had stated: **`--verify` silently drops from 100 checks
to 75**, because the driver treats an `ERR` reply as "nothing to check" and
`continue`s. Invariant I3 — *`organizations.outstanding` equals the
recomputed charges* — stops running on exactly the configuration whose
commit protocol is newest (§7).

## 1. Provenance

| | |
|---|---|
| Date/time (UTC) | 2026-08-31, 04:08–04:48 |
| Worktree | `v2.7.0` (`/home/ubuntu/ckdbs/.claude/worktrees/v2.7.0`) |
| Branch | `worktree-v2.7.0` |
| Commit measured | **`57110cf`** (`git describe --tags` → **`v2.4.0-83-g57110cf`**) |
| Branch tip after the run | The work branch was synced with `origin/main` after the last cell and fast-forwarded to **`38364d0`** (`v2.4.0-85-g38364d0`). The two commits it took (`wirkorder-AX.md`, `ratification-AX.md`) touch **`instructions/` only** — no `src/`, no `include/` — so the engine measured here is still `57110cf`'s, which is what this file's name and every number in it refer to |
| Tree state | **Clean.** `git status --short` empty before the first cell and after the last; no `src/` or `include/` edit in this session. The only additions are this file and its archive |
| Binary provenance | `build-release/kds_server`, configured and built this session from `57110cf` (`-DCMAKE_BUILD_TYPE=Release`), linked 04:04 UTC, then **copied** to `/home/ubuntu/bench-s2-cores/run/kds_server` at 04:08:45 and never touched again. `sha256` = `275317929d6dde4d384e90c6aa94725a9b39d3b017bc9b01b7e2471fa36bf9a4`, recorded in every cell's `.txt` and identical in all 29 |
| Build | `CMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`), gcc, `KDS_WITH_TLS=ON` |
| Test suite | **3,021 of 3,021 passing** — `build-release/tests/kds_tests` at this commit, 35.8 s, run after the build and before the first cell |
| Device | `/home/ubuntu` on `/dev/root`, **ext4** — checked with `df -T`, not assumed. 247 GB, 110 GB free. Not tmpfs; every data file under `$HOME/bench-s2-cores/cells/` |
| Host | **Intel Xeon Platinum 8488C, 8 logical CPUs = 4 physical cores, 2 threads/core**, 1 socket, KVM guest, 15 GiB RAM, Ubuntu 24.04, Linux 7.0.0-1011-aws |
| Host quiet | every cell gates on `bench/wait_quiet.sh` (no `cc1plus`/`ld`/`kds_tests`/`dpkg`, 1-minute load < 0.70) and samples `/proc/loadavg` for its own life. Max 1-minute load across all 29 cells: **1.44** (`d-c8-b8-r3`); 26 of 29 stayed under 1.00. No cell was discarded |
| Isolation | fresh server **and** fresh data file per cell; each cell refuses to start if its port is already bound |
| Server config | `placement = creating`, `durability = group` unless a cell says `relaxed`, `range_size_ids` at its **DA1 default of 65,536** unless a cell says 0, everything else default (`buffer_pool_frames = 0`) |
| Client | one process per booker plus the analytic reporter process, Python driver, one connection each |
| Scale | 2,000 organizations, 200 ships, 2,000 voyages, **20,000 cargos** |
| Work | `--bookings 5000 --seed 1` in arms A/B/D/E/F, `--bookings 3000 --verify 25` in arm C. Equal work, not equal time |
| Raw output | `bench/v2.7.0/archive/scenario2-cores-v2.4.0-83-g57110cf/` — every cell's `.json`, `.txt` and its `SHOW META` line, the three run logs, and the harness under `harness/` |

**Two deviations from `bench/results-scenario2-freight.md`, stated because
they make numbers here non-comparable with numbers there.** The scale is
**20,000 cargos, not 100,000** — 29 cells each load their own file, and the
load is the cell's cost, not the measurement's. And the **host is different
and much larger**: 8 logical CPUs against that document's 2, which is the
whole reason `cores = 4` and `cores = 8` are measurable at all. The August
2026 document's absolute TPS and this one's are two hosts' numbers; only the
ratios inside each file are safe to quote.

## 2. Why `peer_listeners` is an axis and not a footnote

Without it there is **one listener, on core 0**, and every client session is
served by core 0 whatever `cores` says. The other cores run their reactors,
own no session, and — under `placement = creating`, where every relation is
created by and belongs to core 0 — never take a range, because nothing on
them ever asks for a lease. That is visible in the counters rather than
inferred: in every arm-B and arm-F single-listener cell `SHOW META` reports
**no `split_relations` field at all** (the field is absent rather than
zeroed when nothing is split) and `shipped_statements = 0`.

So a `cores` matrix at the shipped default measures *background* cores. A
`cores` matrix with `peer_listeners = on` measures the engine's cross-core
path. **Both are worth having and they disagree in sign**, which is the
finding this file is mostly about. Arms A/C/E carry the listeners, arms B/F
do not.

`peer_listeners = on` is refused at `cores = 1` (`expeditor.cpp`'s
`CheckPeerListenerConfig`: one core has no peer to listen), so every
`cores = 1` row below is a single-listener row by necessity, and it is the
same row in both arms.

## 3. Arm A — the requested matrix, per-core listeners

Bookings/s, `--bookings 5000`, join-free (§7 says why the reporter's join is
excluded here). `cores = 1` is the reference row:

| cores | b=1 | b=4 | b=8 |
|---|---:|---:|---:|
| **1** | **695.0** | **734.7** | **812.0** |
| **2** | 204.4 | 310.3 | 376.1 |
| **4** | 248.5 | 420.6 | 476.4 |
| **8** | 289.5 | 498.4 | **586.3** |

As a ratio against the same booker count at one core:

| cores | b=1 | b=4 | b=8 |
|---|---:|---:|---:|
| **2** | 0.294× | 0.422× | 0.463× |
| **4** | 0.358× | 0.573× | 0.587× |
| **8** | 0.417× | 0.678× | **0.722×** |

**Every cell loses, and the curve is monotone in both directions.** More
cores recover the loss and never close it; more bookers recover the loss and
never close it. The worst cell in the matrix is the *smallest* multi-core
configuration — two cores, one booker, **0.294×** — which is the shape of a
fixed per-transaction cost rather than a contention effect: one booker
cannot contend with anything, so what it is paying is the protocol.

The cells, with where the time went:

| cell | cores | listeners | bookers | TPS | booking mean µs | commit mean µs | 8 stmts µs | ranges |
|---|---|---|---|---:|---:|---:|---:|---|
| `a-c1-b1` | 1 | off | 1 | 695.0 | 1,419.2 | 975.7 | 384.5 | — |
| `a-c1-b4` | 1 | off | 4 | 734.7 | 5,234.0 | 1,349.2 | 3,424.9 | — |
| `a-c1-b8` | 1 | off | 8 | 812.0 | 9,576.1 | 1,716.9 | 7,082.3 | — |
| `a-c2-b1` | 2 | on | 1 | 204.4 | 4,856.0 | 3,273.0 | 1,463.4 | `4061:2@2,4070:2@2` |
| `a-c2-b4` | 2 | on | 4 | 310.3 | 12,672.9 | 4,664.4 | 7,678.6 | `4061:2@2,4070:2@2` |
| `a-c2-b8` | 2 | on | 8 | 376.1 | 20,405.1 | 5,994.8 | 14,003.8 | `4061:2@2,4070:2@2` |
| `a-c4-b1` | 4 | on | 1 | 248.5 | 3,990.9 | 3,140.1 | 757.6 | `4061:2@2,4070:2@2` |
| `a-c4-b4` | 4 | on | 4 | 420.6 | 8,057.1 | 4,015.2 | 3,815.4 | `4061:4@4,4070:4@4` |
| `a-c4-b8` | 4 | on | 8 | 476.4 | 13,816.9 | 5,018.2 | 8,527.3 | `4061:4@4,4070:4@4` |
| `a-c8-b1` | 8 | on | 1 | 289.5 | 3,429.5 | 2,910.3 | 462.2 | `4061:2@2,4070:2@2` |
| `a-c8-b4` | 8 | on | 4 | 498.4 | 7,910.4 | 3,879.0 | 3,924.2 | `4061:3@3,4070:3@3` |
| `a-c8-b8` | 8 | on | 8 | 586.3 | 11,856.6 | 4,913.5 | 6,735.2 | `4061:5@5,4070:5@5` |

`booking mean` is a **queueing** number at b>1 — it includes waiting behind
the other bookers — so read `commit` and the eight statements for service
time. The `ranges` column is `SHOW META`'s own `split_relation_detail`,
`oid:ranges@stages`: oids **4061 and 4070 are `freights` and `charges`** — checked with
`DESCRIBE` against a freshly built schema, not inferred from creation order
— the two HEAP ledgers, and the only two relations in the schema that
`RangeEligible` admits — the other six are BTREE and D1 declines every one.

**The split tracks booker count, not core count.** At b=1 every core count
reaches exactly two ranges; at b=8, `cores = 8` reaches five or six. Ranges
open when a *peer* core asks for a lease block, and only a core with a
session on it ever asks.

## 4. Arm B — the shipped default, where cores do help

One listener, eight bookers, everything else as arm A:

| cell | cores | listeners | TPS | vs `cores = 1` | commit mean µs | 8 stmts µs | ranges |
|---|---|---|---:|---:|---:|---:|---|
| `a-c1-b8` | 1 | off | 812.0 | 1.000× | 1,716.9 | 7,082.3 | — |
| `b-c2-b8-nopl` | 2 | off | 818.6 | 1.008× | 1,658.8 | 7,105.8 | — |
| `b-c4-b8-nopl` | 4 | off | 900.9 | 1.109× | 1,517.7 | 6,509.2 | — |
| `b-c8-b8-nopl` | 8 | off | **1,026.3** | **1.264×** | 1,316.1 | 5,806.0 | — |

Monotone across four points, and **the gain is entirely in the commit**:
1,716.9 → 1,316.1 µs, a 23.3% fall, against a booking whose throughput rose
26.4%. The eight statements move 18% the same way, but they are not where
the time is.

`cores = 1` → `cores = 2` is +0.8%, inside §6's noise floor and reported as
no change. Only `cores = 4` and `cores = 8` clear it.

Nothing splits in any of these cells and nothing ships — `shipped_statements
= 0`, no `split_relations` field — so the extra cores do **no session work
at all**. §5's arm F says what they do instead.

## 5. Arms E and F — separating three costs

### E: is it the split, or is it the session's core?

Arm A conflates two changes. `peer_listeners = on` moves the session onto a
core that owns nothing; `range_size_ids = 65,536` (DA1's armed default) then
lets the ledgers split. Arm E keeps the first and removes the second —
`range_size_ids = 0`, the off-switch, so no range ever opens:

| cell | cores | listeners | `range_size_ids` | TPS | vs `cores = 1` | commit mean µs | ranges |
|---|---|---|---:|---:|---:|---:|---|
| `a-c1-b8` | 1 | off | 65,536 | 812.0 | 1.000× | 1,716.9 | — |
| `e-c2-b8-nosplit` | 2 | on | **0** | 427.8 | 0.527× | 3,625.6 | — |
| `e-c4-b8-nosplit` | 4 | on | **0** | 520.1 | 0.640× | 3,343.3 | — |
| `e-c8-b8-nosplit` | 8 | on | **0** | 570.7 | 0.703× | 3,619.7 | — |
| `a-c2-b8` | 2 | on | 65,536 | 376.1 | 0.463× | 5,994.8 | `4061:2@2,4070:2@2` |
| `a-c4-b8` | 4 | on | 65,536 | 476.4 | 0.587× | 5,018.2 | `4061:4@4,4070:4@4` |
| `a-c8-b8` | 8 | on | 65,536 | 586.3 | 0.722× | 4,913.5 | `4061:5@5,4070:5@5` |

Turning spreading off buys **+13.7% at two cores, +9.2% at four, and −2.7%
at eight** (the last inside the noise floor). Measured against the gap each
cell has to close to reach single-core, the split is worth **11.9% at two
cores, 13.0% at four and −6.9% at eight** — so it accounts for **at most an
eighth** of arm A's loss, and by `cores = 8` for none of it, where it is
arguably paying for itself: that is the cell where the ledger writes are
cheapest.

**The other seven eighths are the session's core**, and `SHOW META` names the
mechanism directly. `e-c2-b8-nosplit`'s core-0 connection reports
**`shipped_executed = 39,497`** and **`shipped_enrolments = 3,147`**, against
a workload of 5,000 bookings × ~8 statements ≈ 40,000. Nothing splits, so
nothing *can* run on a peer: the peer-accepted session carries every
statement to the owner as text, and the transaction it opens is a
cross-owner one that has to commit over the two-phase protocol.

### F: how much of any of this is the fsync?

`durability = relaxed`, one listener, eight bookers:

| cell | cores | listeners | durability | TPS | commit mean µs | commit p50 µs | server CPU per booking |
|---|---|---|---|---:|---:|---:|---:|
| `f-c1-b8-relaxed` | 1 | off | relaxed | 5,789.1 | 70.6 | 58.2 | 216 µs |
| `f-c8-b8-relaxed` | 8 | off | relaxed | **5,806.6** | 76.6 | 61.6 | 344 µs |
| `f-c8-b8-pl-relaxed` | 8 | **on** | relaxed | **935.0** | 3,053.3 | 2,879.3 | 624 µs |

Two results, both clean:

**Arm B's 1.26× was the fsync, not scaling.** With the device out of the
loop, `cores = 8` and `cores = 1` are **5,806.6 against 5,789.1 — 1.003×**,
an eighth of §6's noise floor. The extra cores cost 1.6× the server
CPU per booking (344 vs 216 µs) and return exactly nothing. So what they buy
under `durability = group` is a commit path that completes sooner; this run
does not resolve *which* part of it (the group committer's own thread
finding a free CPU is the obvious candidate, and it is a candidate, not a
finding).

**The cross-core cost is 6.21×, and it is not the device.** Same eight
cores, same relaxed durability, sessions moved onto the peers: **935.0
against 5,806.6**. The commit goes from **76.6 µs to 3,053.3 µs**, so
roughly **3.0 ms of two-phase commit with no fsync in it at all**. Per-booking
server CPU nearly triples, 216 → 624 µs, which is the shipping and the
protocol and nothing else.

## 6. Noise floor

Three runs of each of the two cells the scaling claims rest on, fresh file
every time:

| repeat | `a-c1-b1` (1 core) | `a-c8-b8` (8 cores, listeners) |
|---|---:|---:|
| 1 | 695.0 | 586.3 |
| 2 | 714.3 | 614.6 |
| 3 | 706.4 | 605.9 |
| **mean** | **705.2** | **602.3** |
| peak-to-peak | 2.74% | 4.70% |
| **floor** | **±1.37%** | **±2.35%** |

**Nothing below ±2.4% is reported as a result**, and the single-core figure
is the tighter of the two. This is a much quieter floor than
`bench/results-scenario2-freight.md`'s **±8.2%** — expected, and not a
change in the engine: that document measured a 2-vCPU host where the driver
and the server fought for the same two CPUs, and this one has eight.

Every claim in §3–§5 clears it comfortably. The three that do not, named
rather than buried: `cores = 1 → 2` in arm B (+0.8%), arm E's `cores = 8`
row against arm A's (−2.7%), and arm F's `cores = 1 → 8` (+0.3%). All three
are reported above as "no change".

The 8-core cell's own repeats also show the split count drifting — `5@5`,
`5@5`, `6@6` — which is honest: how many ranges open depends on which core
the kernel gave each of eight connections, and that is not seeded.

## 7. Arm C — the stock workload, and the check that stops running

Arms A/B/D/E/F run `--manifest-customers 0 --verify 0` so that every cell
does **identical engine work** and the grid measures one thing. Arm C runs
the workload as `bench/results-scenario2-freight.md` drives it — the full
analytic reporter and `--verify 25` — and it does not complete at
`cores ≥ 2`:

| cell | cores | listeners | TPS | statements refused | reporter `customer-statement` | `--verify` checks | verify failures |
|---|---|---|---:|---:|---|---:|---:|
| `c-stock-c1` | 1 | off | 708.7 | 0 | 50 of 50 ok | **100** | 0 |
| `c-stock-c2` | 2 | on | 307.5 | 25 | — | **75** | 0 |
| `c-stock-c4` | 4 | on | 443.4 | 25 | — | **75** | 0 |
| `c-stock-c8` | 8 | on | 534.2 | 25 | **50 of 60 refused** | **75** | 0 |

Every refusal is the same statement and the same reply:

```
SELECT f.id, f.cbm, f.price_per_cbm FROM freights_043300 AS f
  JOIN cargos_043300 AS c ON f.cargo_id = c.id WHERE c.org_id = 1275
->  ERR NOT_IMPLEMENTED retryable=0 relation 'freights_043300 AS f' has
    ranges on another core and this shape cannot fan in over them;
    reading it here would answer short
```

**That the join refuses is not news** — `CLAUDE.md`'s range-ownership row
already says a join over a spread relation "is not a shape gate at all and
stays unbuilt, so scenario 2 whole still does not run", and
`known-gaps.md`'s AG3 entry says the same. The engine refuses correctly,
naming the relation, and it refuses rather than answering short, which is
the behaviour the message promises.

**What is new is the second-order effect on the harness.** The driver's
`--verify` pass reads its I3 sample through that same join and handles a
failed read with

```python
if rows is None or stored is None or not stored:
    continue
```

so an `ERR` is indistinguishable from "no rows to check". The visible
consequence is the `--verify` column: **100 checks at one core, 75 at two,
four and eight** — exactly the 25-row I3 sample, silently gone. **I3 is
`operations.booked_cbm`'s sibling and the only invariant that recomputes
`organizations.outstanding` from the charge ledger**, so the check that
would catch a lost update in the credit column is the one that stops running
on the configuration whose commit protocol is newest. Zero failures at
`cores ≥ 2` in that column means *75 checks passed*, not *the workload was
verified*.

This is a **driver** gap, not an engine one, and it is `tools/scenario2_freight.py`'s
to fix: an `ERR` reply inside `verify()` should be counted and reported, not
`continue`d past. Not fixed here — this session measured, and changing the
driver mid-matrix would have measured two drivers.

## 8. Where a booking's time goes

Per-phase, at eight bookers, `mean` and the full percentile spread. The
three cells that matter are the reference (`a-c1-b8`), the best
single-listener cell (`b-c8-b8-nopl`) and the eight-core peer-listener cell
(`a-c8-b8`):

**commit** (µs)

| cell | p0 | p25 | p50 | p95 | p99 | max | mean |
|---|---:|---:|---:|---:|---:|---:|---:|
| `a-c1-b8` | 653.5 | 1,184.7 | 1,419.2 | 2,729.9 | 2,914.9 | 86,756.4 | 1,716.9 |
| `b-c8-b8-nopl` | 665.7 | 1,017.8 | 1,088.2 | 2,073.4 | 2,288.4 | 39,384.3 | 1,316.1 |
| `e-c8-b8-nosplit` | 706.7 | 2,359.3 | 3,846.8 | 5,990.2 | 6,952.1 | 179,801.0 | 3,619.7 |
| `a-c2-b8` | 2,197.7 | 4,726.9 | 5,719.5 | 8,978.2 | 10,331.2 | 75,671.5 | 5,994.8 |
| `a-c4-b8` | 2,170.3 | 3,880.3 | 4,727.0 | 7,504.4 | 8,784.9 | 129,301.7 | 5,018.2 |
| `a-c8-b8` | 907.4 | 3,884.6 | 4,754.8 | 7,106.6 | 8,055.9 | 159,289.7 | 4,913.5 |
| `f-c1-b8-relaxed` | 11.5 | 46.8 | 58.2 | 133.1 | 315.0 | 475.1 | 70.6 |
| `f-c8-b8-pl-relaxed` | 18.1 | 2,181.6 | 2,879.3 | 4,696.5 | 5,734.0 | 142,379.9 | 3,053.3 |

**freight-insert** (µs) — the write spreading is supposed to help

| cell | p0 | p25 | p50 | p95 | p99 | max | mean |
|---|---:|---:|---:|---:|---:|---:|---:|
| `a-c1-b8` | 16.9 | 56.6 | **359.8** | 1,444.4 | 1,580.9 | 7,981.6 | 627.0 |
| `b-c8-b8-nopl` | 21.0 | 77.2 | 718.2 | 1,133.8 | 1,268.3 | 22,666.9 | 563.0 |
| `e-c8-b8-nosplit` | 17.7 | 65.7 | 116.1 | 2,062.0 | 2,961.8 | 32,193.7 | 625.8 |
| `a-c2-b8` | 16.8 | 65.9 | 1,064.4 | 3,615.3 | 4,763.6 | 68,383.8 | 1,303.4 |
| `a-c4-b8` | 16.4 | 39.7 | 71.5 | 2,903.0 | 3,925.2 | 18,380.2 | 734.1 |
| `a-c8-b8` | 16.1 | 31.6 | **41.3** | 2,041.6 | 3,085.7 | 156,511.8 | 399.0 |
| `f-c1-b8-relaxed` | 17.5 | 46.0 | 67.0 | 137.4 | 316.1 | 25,069.1 | 85.0 |
| `f-c8-b8-pl-relaxed` | 15.8 | 29.6 | 37.6 | 1,657.0 | 2,424.7 | 139,300.8 | 357.3 |

**charge-insert** (µs) — 5.59 rows per booking, the same ledger

| cell | p0 | p25 | p50 | p95 | p99 | max | mean |
|---|---:|---:|---:|---:|---:|---:|---:|
| `a-c1-b8` | 12.7 | 46.1 | **146.8** | 1,432.0 | 1,569.6 | 86,647.3 | 544.3 |
| `b-c8-b8-nopl` | 17.3 | 61.3 | 88.7 | 1,104.9 | 1,239.2 | 41,685.3 | 433.2 |
| `e-c8-b8-nosplit` | 15.2 | 57.8 | 98.2 | 2,040.1 | 2,973.7 | 32,316.4 | 549.7 |
| `a-c2-b8` | 11.1 | 40.8 | 230.0 | 3,797.2 | 5,007.0 | 69,516.4 | 1,051.0 |
| `a-c4-b8` | 10.3 | 24.9 | 41.2 | 2,816.2 | 3,912.8 | 127,248.9 | 551.9 |
| `a-c8-b8` | 10.7 | 23.6 | **27.0** | 1,927.5 | 3,059.5 | 155,900.2 | 264.6 |
| `f-c1-b8-relaxed` | 14.6 | 51.0 | 64.2 | 141.1 | 321.2 | 25,190.5 | 82.0 |
| `f-c8-b8-pl-relaxed` | 11.7 | 22.9 | 26.0 | 1,013.3 | 2,037.1 | 19,038.1 | 158.7 |

**Read this pair against the commit table and the whole result is one
sentence.** Going from one core to eight with per-core listeners, the two
ledger writes get **8.7×** and **5.4×** cheaper at p50 — that is R4's insert
spreading doing exactly what it was built to do, each core appending to its
own range's chain tail instead of contending for one — while the commit gets
**2.86×** more expensive. A booking has one commit and 6.59 ledger writes, and
the commit is the larger number by an order of magnitude, so the booking
loses.

Two reads move the other way and are worth naming. `cargo-lookup` (a pk
lookup on a BTREE relation core 0 owns whole) goes from **254.8 µs p50 to
952.5 µs** — that is the shipped round trip, since the peer session cannot
serve it locally. And `manifest-scan` (`SELECT * FROM freights WHERE
operation_id = ?`, a fan-in over the spread ledger) goes **281.3 → 1,308.0
µs p50**, which is the fan-in gathering every row across the ring to filter
at the session.

## 9. What this run does not answer

- **Whether `placement = rotate` changes arm A's sign.** Every cell here is
  `placement = creating`, so core 0 owns all eight relations and a peer
  session owns nothing to serve locally — which is exactly the arrangement
  DA2 closed the placement question on (rotation negative at seven writer
  cores, `crosscore.md` §9). It is also the arrangement that makes arm E's
  `shipped_executed = 39,497` inevitable. A `rotate` matrix would ask a
  different question and this run does not.
- **Which part of the group-commit path arm B's 1.26× comes from.** §5's F
  arm proves it is the commit and not scaling; it does not name the
  mechanism. The counters that move with it are the idle policy's
  (`sched_idle_blocks` 0 at one core → 32,293 at eight, `sched_wakes_sent` 0
  → 330) and the per-core WAL anchor count (1 → 8). Naming a cause would
  need an A/B inside the committer, not a workload.
- **The read-only-participant optimisation's value here.** `CLAUDE.md`
  already carries it as the largest measured cost the cross-owner line
  leaves, and the booking's first four statements are all reads that enrol.
  This run prices the whole booking, not that leg.
- **Anything above eight cores, or with more bookers than CPUs.** At
  `cores = 8` and `--bookers 8` the server's eight reactors, eight booker
  processes and the reporter share 8 logical CPUs on 4 physical ones. The
  b=8 column is therefore an oversubscribed measurement by construction, and
  the b=1 column — where nothing contends and the multi-core loss is
  *worst* — is the cleaner reading of the protocol's fixed cost.
- **Scenario 2 whole at `cores ≥ 2`.** §7's join refuses, so the customer
  axis of this workload has never been measured on a spread relation. It
  will not be until the two-step pipeline can plan a spread relation as a
  stage.
- **Overhead of any change**, in the Session Workflow's sense: this session
  changed no engine code, so there is no A/B to run. Per the operator's
  2026-08-24 amendment the interleaved overhead measurement is suspended in
  any case.
