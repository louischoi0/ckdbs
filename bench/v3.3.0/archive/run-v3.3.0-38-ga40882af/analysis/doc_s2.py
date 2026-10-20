import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis")
from s2 import *
import abx
from stamp import *

core_tags = [p + t for p in PFX for t in ("s2-c8-s", "s2-c1-s", "s2-c1-g", "s2-c8-g")]
b1_tags = [p + t for p in PFX for t in ("b1-s2-c1-g", "b1-s2-c1-s")]
x_tags = [f"x-b{b}-s2-c8-{c}" for b in (1, 2, 4, 16) for c in ("g", "s")] + ["rr-s2-c1-g", "rr-s2-c8-g"]
sat_tags = [f"sat-b{b}-s2-c8-{c}" for b in (16, 32) for c in ("g", "s")]
ab_tags = [f"ab-{v}{i}-{t}" for i in (1, 2, 3) for v in ("old", "new") for t in ("s2-c1-g", "b1-s2-c1-g")]
allk = core_tags + b1_tags + x_tags + sat_tags + ab_tags


def tp(tag):
    return med([tps(d) for d in N(tag)])


def tpm(tag):
    return med([tps(d) for d in Mo(2, tag)])


VOKm = sum(1 for t in allk if now(t))
ABO = {t: abx.pairs("ab-{v}{i}-" + t) for t in ("s2-c1-g", "b1-s2-c1-g")}


def abm(t):
    o, n_ = ABO[t]
    return med([tps(d) for d in o]), med([tps(d) for d in n_]), spread([tps(d) for d in o]), spread([tps(d) for d in n_])


a1, a2 = abm("s2-c1-g"), abm("b1-s2-c1-g")
ALLS = ["booking", "commit", "cargo-lookup", "credit-lookup", "capacity-read", "recipe-read", "freight-insert", "charge-insert", "operation-update", "org-update",
        "manifest-scan", "voyage-rollup", "customer-statement"]


def ab_stmts(t):
    o, n_ = ABO[t]
    s = "| statement | old: p50 pair 1 / 2 / 3 | old median | new: p50 pair 1 / 2 / 3 | new median | new / old |\n|---|---|---|---|---|---|\n"
    for nme in ALLS:
        a, b = pv(o, nme, "p50_us"), pv(n_, nme, "p50_us")
        s += f"| {nme} | " + " / ".join(f1(x) for x in a) + f" | {f1(med(a), ' µs')} | " + " / ".join(f1(x) for x in b) + f" | {f1(med(b), ' µs')} | {med(b) / med(a):.3f}x |\n"
    return s


def rr_p50(tag, which):
    d = now(tag) if which == "now" else mor(2, tag)
    return ph(d, "recipe-read")["p50_us"]


ABN = {n: (med(pv(ABO["b1-s2-c1-g"][0], n, "p50_us")), med(pv(ABO["b1-s2-c1-g"][1], n, "p50_us"))) for n in ALLS}
nonflat = [t for t in core_tags[:4] + ["b1-s2-c1-g", "b1-s2-c1-s"]]
gains = [100 * (tp(t) / tpm(t) - 1) for t in ("s2-c1-g", "s2-c8-g", "s2-c1-s", "s2-c8-s", "b1-s2-c1-g", "b1-s2-c1-s")]
allsweep = []
for b in (1, 2, 4, 16):
    for c in ("g", "s"):
        allsweep.append(100 * (tp(f"x-b{b}-s2-c8-{c}") / tpm(f"x-b{b}-s2-c8-{c}") - 1))
for b in (16, 32):
    for c in ("g", "s"):
        allsweep.append(100 * (tp(f"sat-b{b}-s2-c8-{c}") / tpm(f"sat-b{b}-s2-c8-{c}") - 1))
for c in ("g", "s"):
    allsweep.append(100 * (tp(f"s2-c8-{c}") / tpm(f"s2-c8-{c}") - 1))

txt = f"""# Scenario 2 (freight) at `{DESC}`

**One line.** The eight-statement freight booking is not a many-row workload, so the new client decoder does not change its throughput: in three interleaved old/new pairs the one-core `group` cell reads {a1[0]:.0f} tps with the old client and {a1[1]:.0f} tps with the new
({a1[1] / a1[0]:.3f}x, spreads {a1[2]:.1f} % and {a1[3]:.1f} %) and the one-booker cell {a2[0]:.0f} against {a2[1]:.0f} tps ({a2[1] / a2[0]:.3f}x, spreads {a2[2]:.1f} % and {a2[3]:.1f} %), both inside their spreads. What moved is one statement, the
93-row `recipe-read` scan, which falls {100 * (1 - ABN['recipe-read'][1] / ABN['recipe-read'][0]):.0f} % in p50 ({ABN['recipe-read'][0]:.0f} to {ABN['recipe-read'][1]:.0f} µs at one booker) and is 7 % of a booking. Against the morning every cell of the matrix and the sweep is
{min(gains + allsweep):+.1f} % to {max(gains + allsweep):+.1f} % (all {sum(1 for g in gains + allsweep if g > 0)} of {len(gains + allsweep)} comparable cells positive, median {med(gains + allsweep):+.1f} %), which is larger than the client explains: the old client reads {a1[0]:.0f} tps in this window against
the morning's {tpm('s2-c1-g'):.0f}, so the {100 * (tp('s2-c1-g') / tpm('s2-c1-g') - 1):+.1f} % of `s2-c1-g` is a host difference of the same sign and size, and no cell of this scenario is read as a client effect. **One durable booker is
{ratio(tp('b1-s2-c1-g'), med([tps(d) for d in Mo(2, 'pg-s2-b1')]))} (`group`) and {ratio(tp('b1-s2-c1-s'), med([tps(d) for d in Mo(2, 'pg-s2-b1')]))} (`strict`) of PostgreSQL 18.6**, reused from the morning's run ({med([tps(d) for d in Mo(2, 'pg-s2-b1')]):.1f} tps): the commit is 54 % of a KDS booking and its mean is {ph(now('b1-s2-c1-g'), 'commit')['mean_us']:,.0f} µs against PostgreSQL's {ph(Mo(2, 'pg-s2-b1')[0], 'commit')['mean_us']:,.0f} µs.
The booker sweep at `cores = 8` climbs from {tp('x-b1-s2-c8-g'):.0f} tps (1 booker) to {tp('sat-b32-s2-c8-g'):,.0f} tps (32, `group`), and the invariant check fails under the default isolation and passes 400 of 400 under `repeatable-read`, as in the morning.

Driver: `tools/scenario2_freight.py`, unmodified, `--schema-only`, then `--load-only`, then the run on the same server, with the morning's arguments and cell order (`run_all.sh` in the archive). Measured by `ck-tester`.
Comparator (this engine's own last run, same binary, old client): `bench/v3.3.0/results-scenario2-freight-{MD}.md`. PostgreSQL 18.6: that file's twin cells (`tools/pg_scenario2_freight.py`, `synchronous_commit = on`, three runs), **not re-run**: {BUDGET}.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; the four core cells x 3 runs {wtext(core_tags)}; isolation probe and booker sweep `x-b*` {wtext(x_tags)}; single-booker cells {wtext(b1_tags)}; saturation cells {wtext(sat_tags)}; A/B pairs (twelve cells) {wtext(ab_tags)} |
{COMMONROWS}| Driver | **unmodified** `tools/scenario2_freight.py`, `--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync`; exit 0 in every cell |
| Unchanged from the morning | {PATHS} |

## 2. What was run

| cells | cores | durability | bookers | runs | extra |
|---|---|---|---|---|---|
| `s2-c8-s`, `s2-c1-s`, `s2-c1-g`, `s2-c8-g` (+ `r2-`, `r3-`) | 8 / 1 / 1 / 8 cores | strict / strict / group / group | 8 bookers | 3 runs each | - |
| `rr-s2-c1-g`, `rr-s2-c8-g` | 1 / 8 cores | group | 8 bookers | 1 run each | `--isolation repeatable-read` |
| `x-b{{1,2,4,16}}-s2-c8-{{g,s}}` | 8 cores | group and strict | 1, 2, 4, 16 bookers | 1 run each | `--bookers N` (the booker sweep) |
| `sat-b{{16,32}}-s2-c8-{{g,s}}` | 8 cores | group and strict | 16, 32 bookers | 1 run each | the same plus a 1-second whole-host CPU sampler |
| `b1-s2-c1-g`, `b1-s2-c1-s` (+ `r2-`, `r3-`) | 1 core | group / strict | 1 booker | 3 runs each | `--bookers 1`, the PostgreSQL twin's shape |
| `ab-old{{1,2,3}}-s2-c1-g`, `ab-new{{1,2,3}}-s2-c1-g`; `ab-old{{1,2,3}}-b1-s2-c1-g`, `ab-new{{1,2,3}}-b1-s2-c1-g` | 1 core | group | 8 and 1 bookers | 3 pairs each, interleaved old, new, old, new | old = `a40882af^`'s client from `oldtools/` |
| PostgreSQL | - | `synchronous_commit = on` | 1 booker | not run | the morning's `pg-s2-b1`, `r2-`, `r3-` |

Each cell: a fresh server and data file, `--schema-only`, `--load-only`, then the measured run on the same file. A booking is the driver's eight statements in one transaction (cargo, credit, capacity and recipe reads; freight and charge inserts;
operation and organization updates) and a commit; 3,000 bookings per cell, with `--verify 100` invariant checks (400 checks) after. **The row-set axis is not swept:** the loaded sizes are fixed by the arguments
(300 organizations, 30 ships, 300 operations, 4,000 cargos; 3,000 freights and about 17,000 charges accumulate), the reads are pk descents and one 93-row recipe scan, which do not scale with the booking count, and the writes grow `freights` and
`charges` by 3,000 and 17,000 rows inside every cell, a 17,000-row sweep of the insert path in one run; there is no 200 / 1K / 10K axis and none is claimed. The one row-returning statement, `recipe-read` (93 rows), is the one this change reaches.

Cut: nothing from the morning's KDS plan.

## 3. Throughput

Findings are read by the rule of `{SUMMARY}` ("What counts as a finding"); a cell whose spread is above 20 % is reported and carries no delta claim. **A delta against the morning is client plus host; section 8 separates them.**

{throughput()}
| ratio of medians | now | morning |
|---|---|---|
| `cores = 8` over `cores = 1`, `group` | {tp('s2-c8-g') / tp('s2-c1-g'):.2f}x | {tpm('s2-c8-g') / tpm('s2-c1-g'):.2f}x |
| `cores = 8` over `cores = 1`, `strict` | {tp('s2-c8-s') / tp('s2-c1-s'):.2f}x | {tpm('s2-c8-s') / tpm('s2-c1-s'):.2f}x |
| `group` over `strict`, `cores = 1` | {tp('s2-c1-g') / tp('s2-c1-s'):.2f}x | {tpm('s2-c1-g') / tpm('s2-c1-s'):.2f}x |
| `group` over `strict`, `cores = 8` | {tp('s2-c8-g') / tp('s2-c8-s'):.2f}x | {tpm('s2-c8-g') / tpm('s2-c8-s'):.2f}x |

Every cell committed its 3,000 bookings. All six matrix and single-booker cells are positive ({min(gains):+.1f} % to {max(gains):+.1f} %); two are findings by their spreads (`s2-c1-g`, `b1-s2-c1-s`) and four inside them. The pattern is a slight, uniform gain, and section 8 attributes it to the host, not the client.

## 4. Percentiles

Run 1 of each of the four core cells, every phase of the driver (microseconds; the five percentiles are the driver's). The `ddl` and load phases are in the archive's JSON:

{pct_cells()}
## 5. Where the time goes

A booking is the sum of its statements, its commit, and what is left (BEGIN, refused attempts, the client and the socket). Each phase's mean times its ops over the booking's mean times its ops, run 1, now and in the morning:

{attribution(['s2-c1-g', 's2-c8-g', 's2-c1-s', 's2-c8-s', 'b1-s2-c1-g'])}
| Wait | Reading |
|---|---|
| Durability / commit | `commit` is {ph(now('s2-c1-g'), 'commit')['mean_us'] * ph(now('s2-c1-g'), 'commit')['ops'] / (ph(now('s2-c1-g'), 'booking')['mean_us'] * ph(now('s2-c1-g'), 'booking')['ops']) * 100:.1f} % of a booking at `s2-c1-g`, {ph(now('s2-c8-g'), 'commit')['mean_us'] * ph(now('s2-c8-g'), 'commit')['ops'] / (ph(now('s2-c8-g'), 'booking')['mean_us'] * ph(now('s2-c8-g'), 'booking')['ops']) * 100:.1f} % at `s2-c8-g` and {ph(now('s2-c8-s'), 'commit')['mean_us'] * ph(now('s2-c8-s'), 'commit')['ops'] / (ph(now('s2-c8-s'), 'booking')['mean_us'] * ph(now('s2-c8-s'), 'booking')['ops']) * 100:.1f} % at `s2-c8-s`; with one booker it is {ph(now('b1-s2-c1-g'), 'commit')['mean_us'] * ph(now('b1-s2-c1-g'), 'commit')['ops'] / (ph(now('b1-s2-c1-g'), 'booking')['mean_us'] * ph(now('b1-s2-c1-g'), 'booking')['ops']) * 100:.1f} % and its p50 ({ph(now('b1-s2-c1-g'), 'commit')['p50_us']:,.1f} µs) is the device's sync: PostgreSQL's is {ph(Mo(2, 'pg-s2-b1')[0], 'commit')['p50_us']:,.1f} µs. At eight cores the commit, not the statements, bounds the booking |
| Write statements (`freight-insert`, `charge-insert`, `operation-update`, `org-update`) | one core: about 30 % for `charge-insert` alone (about 5.6 charges per booking) and 4.5 % to 8 % for the three others; eight cores 12.5 / 2.7 / 2.5 / 2.4 % |
| Reads (`cargo-lookup`, `credit-lookup`, `capacity-read`, `recipe-read`) | 7.3 to 7.8 % each at one core, 3.6 to 6.2 % at eight; `recipe-read` is the FilterScan and the only one that moved (8.3 % to 7.8 % at `s2-c1-g`, 7.8 % to 6.2 % at `s2-c8-g`) |
| Lock or conflict wait | zero client-visible conflicts at every `cores = 1` cell, 1 / 3 / 1 at `c8-g`, and **49 / 67 / 39 at `c8-s`** (all on `operations` and `organizations` rows: `TXN_CONFLICT retryable=1`), retried by the driver (morning: 54 / 42 / 39), 2 bookings abandoned in `r2-s2-c8-s` (morning: 0). Their cost sits in the unattributed column: 13.7 % at `s2-c8-s` against 4.7 % at `s2-c8-g` |
| Client and socket round trip | the unattributed column at the cells with no conflicts, 4.7 to 7.4 % |

Outcomes, conflicts and the invariant check, every cell of the matrix:

{outcomes()}
## 6. Correctness: what the invariant failures are

The `--verify 100` pass checks four invariants (booked volume against the sum of freights, ship capacity, outstanding credit against recomputed charges, charge rows against rules) 400 times while the bookers run. Under the
**default isolation** with more than one booker it fails 15 to 37 of 400 in every cell, `cores = 1` included where no conflict is ever reported; with **one booker it fails 0 of 400 in all six cells**; under **`--isolation repeatable-read`
it passes 400 of 400** at both core counts. The driver writes `SET booked_cbm = <literal>` from a value it read earlier in the same transaction, the lost-update shape its source names: under read committed two bookers that read one voyage
before either commits both write `read + their cbm`, and the second overwrites the first. The failures are that client-side lost update, not engine corruption: under repeatable-read the engine refuses the second writer (`TXN_CONFLICT`), the
driver retries, and every check passes. The decoder has no part in it.

{isolation()}
The repeatable-read probe costs no throughput and turns the failed checks into client-visible conflicts, retried by the driver: a refused write instead of a wrong read.

## 7. The booker sweep at `cores = 8`

Bookers 1, 2, 4, 8, 16 and 32 (the saturation cells), `cores = 8`, one run each except 8 (three). `--bookings 3000` in every cell, so every cell does the same work (equal work, not equal time):

{sweep_tbl()}
The 16-booker point was run twice, as the sweep's `x-b16` and as the saturation cell `sat-b16` (same arguments, plus the sampler):

{sat16()}
**Throughput climbs with bookers on both durability rungs and the gap between them opens**: `group` {tp('x-b1-s2-c8-g'):.0f} tps to {tp('sat-b32-s2-c8-g'):,.0f} tps ({tp('sat-b32-s2-c8-g') / tp('x-b1-s2-c8-g'):.1f}x for 32x the clients) and `strict` {tp('x-b1-s2-c8-s'):.0f} tps to {tp('sat-b32-s2-c8-s'):,.0f} tps
({tp('sat-b32-s2-c8-s') / tp('x-b1-s2-c8-s'):.1f}x). A single booker is not slower under `strict`: it waits for its own sync either way. From two bookers on `strict` pays for the staged commit that parks and publishes after its sync, plus
the retried conflicts (342 at 32 bookers, 148 at 16; none up to two bookers). The sweep's deltas against the morning are +1.3 % to +10.3 % on 12 of 12 points, the same sign and size as the matrix's, and single runs carry no spread: they are not read.

What bounds it. The 1-second whole-host sampler around the saturation cells:

{cpu_samples()}
No limit of the engine was reached in these cells: the host never exceeded 65 % busy in any one-second sample, steal was zero, and each cell lasts under two seconds of measured booking time (3,000 bookings at 3,000+ tps), so a one-second sampler sees two
or three samples of it. The driver is Python threads in one process, so the client (the GIL) is a candidate bound that this run did not rule out. **That is the open item of this scenario**: it has not found the ceiling, and a saturation measurement needs
more bookings (`--bookings` larger) and a second client process (rule 4b).

## 8. The client, isolated: old against new, interleaved

Three pairs per cell on the same host, old, new, old, new, old, new, a fresh server and data file each, the same binary copy. Throughput:

{abx.s2_tps('ab-{v}{i}-s2-c1-g')}
{abx.s2_tps('ab-{v}{i}-b1-s2-c1-g')}
Statement p50 of each cell (every statement of the driver; the three pairs side by side). First the eight-booker `s2-c1-g` cell, then the one-booker cell:

{ab_stmts('s2-c1-g')}
{ab_stmts('b1-s2-c1-g')}
**The decoder moves one statement and no throughput.** `recipe-read` falls to {ABN['recipe-read'][1] / ABN['recipe-read'][0]:.2f}x of its p50 at one booker ; every statement that returns one row or none is within 0.97x to 1.08x, and the three reporter statements (`manifest-scan`, `voyage-rollup`, `customer-statement`, 50 to 120 operations per cell) scatter by up to 25 % between pairs and carry no claim. A booking is 2.1 ms at one booker and `recipe-read` is {100 * ph(now('b1-s2-c1-g'), 'recipe-read')['mean_us'] / ph(now('b1-s2-c1-g'), 'booking')['mean_us']:.1f} % of it, so a {100 * (1 - ABN['recipe-read'][1] / ABN['recipe-read'][0]):.0f} % saving on it is about {(100 * ph(now('b1-s2-c1-g'), 'recipe-read')['mean_us'] / ph(now('b1-s2-c1-g'), 'booking')['mean_us']) * (1 - ABN['recipe-read'][1] / ABN['recipe-read'][0]):.1f} % of the booking, below the 3.6 % to 5.3 % spreads of the
cells: the A/B reads +0.6 % and +3.2 % and both are inside the spread. **The +1 % to +10 % against the morning is therefore not the client**: the old client reads {a1[0]:.0f} tps on `s2-c1-g` and {a2[0]:.0f} tps on the one-booker cell
here, against {tpm('s2-c1-g'):.0f} and {tpm('b1-s2-c1-g'):.0f} in the morning. Host, in the opposite direction to scenario 1's scan effect (a gain here, a loss there), and not separated into cause.

## 9. Against PostgreSQL 18.6 (the morning's run, reused)

PostgreSQL is the floor and answers "is the engine in the right league" and nothing finer. The twin drives **one** booking connection with its manifest reporter interleaved, so the matched KDS cell is `--bookers 1`; the eight-booker cells have no
PostgreSQL counterpart. Its cells are the morning's three runs (04:22 to 04:30 UTC), not re-run:

{pg_cmp()}
**KDS and PostgreSQL are the same machine on one durable client**: the booking mean is {ph(now('b1-s2-c1-g'), 'booking')['mean_us']:,.0f} µs against {ph(Mo(2, 'pg-s2-b1')[0], 'booking')['mean_us']:,.0f} µs. The four write statements run at 0.75x to 0.88x of PostgreSQL's p50 (a KDS write is cheaper than a
PostgreSQL write by an eighth to a quarter) and `recipe-read`, a 93-row FilterScan, is {ph(now('b1-s2-c1-g'), 'recipe-read')['p50_us'] / ph(Mo(2, 'pg-s2-b1')[0], 'recipe-read')['p50_us']:.2f}x (KDS slower), from 1.94x: the per-row result cost scenario 1 isolates is what is left of it.
The commit is the same fsync: KDS's p50 is 0.96x of PostgreSQL's.

## 10. Noise floor, contamination and host

The 3-run spreads of section 3 are the noise floor: {min(spread([tps(d) for d in N(t)]) for t in ('s2-c1-g', 's2-c8-g', 's2-c1-s', 's2-c8-s')):.1f} % to {max(spread([tps(d) for d in N(t)]) for t in ('s2-c1-g', 's2-c8-g', 's2-c1-s', 's2-c8-s')):.1f} % at the matrix cells and 2.3 % to 4.0 % at the single-booker cells; the PostgreSQL cells' is 3.0 %. The A/B spreads are 1.8 % to 5.3 %.
The booker sweep's single-run points have none and are read only for shape.

Contamination: one KDS cell was discarded and re-run, `s2-c1-s` (first attempt 06:54:33 to 06:54:52 UTC): the 1-second monitor saw a `claude` process of another session at 52.5 % of a CPU inside its window (one sample in 11). The re-run is the cell reported;
the attempt is in `contaminated/` of the run archive. No other cell of this scenario was flagged (`logs/validity.log`).

Per-cell host stamps (load average before the cell, gate wait, and what the 1-second monitor saw inside the window):

{hostrows(core_tags + b1_tags + x_tags + sat_tags + ab_tags)}
## 11. What the run says about the engine

1. **A write-and-commit workload does not see the decoder.** Seven of the eight statements of a booking return 0 or 1 row; the one that returns 93 rows is 7 % of a booking and its saving is about 2 % of it. The engine's booking cost is the commit (51 % to 58 % of a booking at one booker or at eight cores)
   and the four writes; both unchanged.
2. **One durable client is PostgreSQL's equal** (0.99x to 1.04x across the matched cells of both runs), because both engines wait on the same fsync (the commit p50 is 1,228 µs against 1,279 µs); the engine's write statements are cheaper (0.75x to 0.88x) and its 93-row read dearer (1.5x), and they cancel in the booking.
3. **A scenario this short cannot be compared across hours at the 5 % level**: every cell here is +1 % to +10 % against the morning with the old client reading the same gain in the same window. The same host moved scenario 1's scan shapes by -14 %, so the sign of host drift depends on the workload.

## 12. Archive

`bench/v3.3.0/archive/scenario2-{DESC}/`: per-cell JSON, driver stdout and logs, server configs; the contaminated attempt is in `bench/v3.3.0/archive/run-{DESC}/contaminated/`. What the four scenarios share is in `bench/v3.3.0/archive/run-{DESC}/`.
The PostgreSQL twin's JSON is in the morning's archive, `bench/v3.3.0/archive/scenario2-{MD}/json/`. No data file is kept.
"""
write("results-scenario2-freight", txt)
