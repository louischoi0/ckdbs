# XG4 — scenario 2 crosses owners at last, and the prepare leg is the bill

**Headline. The measurement XE §4.1 could not take now runs, and it prices
a real booking's cross-owner cost for the first time: 0.47× the local
throughput and 2.16× the latency at one booker, on exactly three device
syncs against a local booking's one.** Every earlier number for this shape
was a synthetic substitute, because the typed client could not read foreign
data at all.

**The three coordinator legs are the chain on a real booking too** — they
account for the whole commit to within **1.4%** at one booker and **0.2%**
at eight, matching XF4's +0.15% on its synthetic shape. So a cross-owner
booking is now leg-addressable, which is what XG4 was for.

**And the largest leg is `prepare`, at 35-39% of the commit.** That much
XF4 predicted. What it did *not* predict is where the leg's time goes:
XF4 measured 1,488 µs of it as transport and scheduling rather than the
device, and **at one booker that figure is 31 µs — 2.2%.** The rest is the
participant's own `fdatasync`. The "half a cross-owner commit is not the
device" finding is a property of **load**, not of the protocol: it appears
at eight concurrent coordinators (35%) and is essentially absent at one.

**One clause of H-XG4 is refused outright, and it is worth stating loudly:
SA-T0 removes nothing from a scenario-2 booking.** `shipped_readonly_prepares`
is **0** in every cross-owner cell. A booking writes to its participant —
XD6 said so — so the participant is never write-free and always keeps its
durable prepare. SA-T0's lever is real and this workload does not price it;
`bench/v2.5.0/results-rr-read-half-*` remains the only file that does.

Measured on the worktree `xf` at **`08c5592`** (`git describe --tags` →
**`v2.7.0-44-g08c5592`**), branch `xf-shipped-read`, executing
`instructions/v2.7.2/workorder-xg.md` row XG4.

## 1. Provenance

| | |
|---|---|
| Date/time (UTC) | 2026-09-01, 03:10-03:22 |
| Commit measured | `08c5592` — XG1's answer edge plus XG4's own two fixes (§2), which is the first commit on which this shape runs at all |
| Binary | `build-release/kds_server`, copied out before the first cell (ck-tester rule 5), `sha256` `f5c56a2e86105729…` |
| Driver | `bench/wal_sync_decomposition_probe.py`, `--durability group --bookings 1000`, scale `--organizations 500 --ships 50 --operations 500 --cargos 5000 --seed 1` |
| Cells | 5 configurations × b ∈ {1, 8} × 3 repeats = **30**, all green, **0 failures** |
| Interleaving | by repeat: every configuration runs once before any runs twice, so a drift in the machine lands on all of them |
| Device | `/dev/root`, **ext4**, `df -T` checked. Never tmpfs |
| Host | 8 logical CPUs = 4 physical × 2 threads, AMD EPYC 9V74, Linux 6.17.0-1022-azure; `bench/wait_quiet.sh` before every cell. One unrelated `kds_server` from another checkout resident at 0.8% CPU |
| Test suite | 3,106 of 3,106 passing in `build-release` at this commit |
| Raw output | `bench/v2.7.2/archive/xg4-scenario2-crossowner/` — 30 JSON summaries, the run log, the harness and the analysis script |

**The scale is smaller than XD's** (500/50/500/5,000 against
2,000/200/2,000/20,000) so thirty cells fit a sane wall clock. It is
**identical across every arm**, which is what keeps the comparison inside
this file sound — and it is why **no absolute here may be diffed against
`results-xd-commit-decomposition` or `results-scenario2-cores-*`**. The
ratios are this file's contribution; the absolutes are this scale's.

## 2. Two bugs this measurement found, before any number

Reported first because they are the reason the first attempt read 10 s per
booking, and because both were invisible to a green 3,106-cell suite.

**Core 0's executor never got the answer edge.** `SetRemoteSteps` was wired
in `CoreRuntime`, which builds a *peer*; core 0 is the `Expeditor` and
builds its own executor and step server. Every unit cell passed because
**the rig's owner is a peer** — so a real instance answered `Unsupported`
for every typed shipped read of a core-0-owned relation, which under
`placement = creating` is every relation.

**The EOF wait burned the whole deadline on a refusal.** XG1's park waits
for the answer edge's EOF because the terminator can outrun the rows; a
refusal opens no edge, so a 39 µs `Unsupported` became a 10 s one. Now
gated on the reply having arrived *and being OK*.

After both, on a two-core instance: a foreign read inside a transaction
returns its rows in **1.3 ms**, byte-identical to what the autocommit
remote-step edge answers for the same statement, against **10,002 ms** of
refusal before.

## 3. A harness trap, caught by reading a cell rather than by a failure

The first matrix run's `peer_listeners = on` cells reported
**`landed_local: true`, zero shipped statements, `syncs_per_booking`
1.00** — the booker had landed on the relation-owning core, so a *local*
booking wore a cross-owner label and nothing in the output failed.

Under `SO_REUSEPORT` a client cannot choose its core, so a cross-owner cell
is only cross-owner **by luck**. The probe already signals it
(`--require-shipped`, exit 42); the harness ignored it. Fixed with the
retry loop XE's own harness used, and **it fired on 5 of the 30 cells** —
so without it, one cell in six in this file would have been a local
booking reported as a cross-owner one. Any future run of this shape must
keep the flag.

Riding with it: `xowner_` joined the probe's tracked `SHOW META` prefixes.
Without that the file could not report per leg at all, which is half of
what XG4 asks for.

## 4. What a booking costs when it crosses an owner

Medians of three repeats; `book pNN` is the whole eight-statement booking
transaction, `s/book` its device syncs.

| config | b | TPS | book p50 (µs) | book p95 | book p99 | s/book | TPS floor | p50 floor |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `cores=1` | 1 | 402.2 | 2,292.3 | 2,943.0 | 5,957.4 | 1.001 | 37.1% | 9.7% |
| `cores=2` nopl | 1 | 418.2 | 2,224.4 | 2,787.3 | 4,594.2 | 1.001 | 2.4% | 1.1% |
| **`cores=2` pl** | 1 | **190.4** | **4,949.4** | 6,355.8 | 10,842.6 | **3.009** | 3.4% | 2.2% |
| `cores=4` nopl | 1 | 408.4 | 2,216.1 | 2,873.5 | 5,992.8 | 1.001 | 7.8% | 5.3% |
| **`cores=4` pl** | 1 | **189.7** | **5,049.3** | 6,310.2 | 11,205.5 | **3.011** | 2.7% | 2.4% |
| `cores=1` | 8 | 643.2 | 11,891.3 | 17,071.9 | 21,371.7 | 0.860 | 11.0% | 8.2% |
| `cores=2` nopl | 8 | 655.1 | 11,944.9 | 15,275.1 | 20,086.6 | 0.859 | 5.1% | 3.2% |
| **`cores=2` pl** | 8 | **410.4** | 11,841.9 | 31,275.8 | 38,071.5 | 1.670 | 33.1% | **127.4%** |
| `cores=4` nopl | 8 | 603.1 | 12,635.2 | 17,705.2 | 22,409.0 | 0.848 | 8.0% | 7.1% |
| **`cores=4` pl** | 8 | **346.2** | 18,356.1 | 33,284.7 | 43,659.5 | 2.628 | 2.3% | 3.5% |

**At one booker the cross-owner cost is 0.47× the throughput and 2.16× the
p50**, and it is the same at two cores and at four — which it should be:
under `placement = creating` every relation is one core's, so there is
exactly **one participant** either way. That the two agree to within 0.4%
is the check that the cell measures the protocol and not the topology.

**`s/book` is 3.009 against a local booking's 1.001.** XD's three-syncs
model, reproduced for the first time on a *real booking* rather than on a
synthetic two-insert transaction. At eight bookers it falls to 1.670
(cores=2) and 2.628 (cores=4) — XD2's batching discount, and it is weaker
at four cores because the same eight bookers are spread over more
participants' queues.

**`cores=2, pl, b=8` carries a 127% p50 floor and this file does not lean
on it.** Two of its three repeats agree; one is far out. Its TPS floor is
33%. Reported because a floor computed and then hidden is worse than a
wide one, and because it is the only cell here that is not tight.

## 5. The legs — a real booking, decomposed

Mean microseconds per cross-owner commit, medians of three repeats.
`prep`/`decis`/`decid`/`whole` are the coordinator's;
`p.prep`/`p.ack`/`p.dur` the participant's (XF4's instrument).

| config | b | prep | decis | decid | whole | p.prep | p.ack | p.dur |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `cores=2` pl | 1 | **1,379** | 1,199 | 1,262 | 3,788 | 1,348 | 3 | 1,236 |
| `cores=2` pl | 8 | **2,015** | 1,278 | 1,883 | 5,178 | 1,307 | 239 | 1,511 |
| `cores=4` pl | 1 | **1,320** | 1,216 | 1,243 | 3,765 | 1,294 | 3 | 1,200 |
| `cores=4` pl | 8 | **2,344** | 1,453 | 2,140 | 5,950 | 1,540 | 194 | 1,523 |

**The three legs are the chain.** Their sum against `whole`: −1.37%, +0.04%,
−0.37%, +0.22%. XF4 measured +0.15% on a synthetic shape and this is the
same statement about a real booking — nothing material lives outside the
three, so every future claim about this protocol can be made about a leg.

**`prepare` is the largest leg: 36.4%, 38.9%, 35.1%, 39.4% of the commit.**
XF4 said so first and this confirms it on the shape that matters.

**Where the prepare leg's time goes is *not* what XF4 measured**, and this
is the file's second finding. XF4 found 1,487.7 µs of that leg — 26% of the
whole commit — was transport and scheduling rather than either core's
device. Here:

| config | b | prepare leg | participant's own device | transport + scheduling |
|---|---:|---:|---:|---:|
| `cores=2` pl | 1 | 1,379 | 1,348 | **31 (2.2%)** |
| `cores=4` pl | 1 | 1,320 | 1,294 | **26 (2.0%)** |
| `cores=2` pl | 8 | 2,015 | 1,307 | 708 (35.1%) |
| `cores=4` pl | 8 | 2,344 | 1,540 | 804 (34.3%) |

**At one coordinator the prepare leg is the device, essentially entirely.**
The transport is 26-31 µs — the ring round trip, which
`bench/v2.3.0/` measured at 20.0 µs on an idle peer and which this
reproduces. At eight it is a third of the leg. So *"half a cross-owner
commit is not the device"* is true **under concurrency and false at rest**,
and XF4's figure should be read as a load measurement rather than a
protocol property. Nothing in XF4 is retracted — its cells were all at
b=8 — but its sentence needs the qualifier and gets one here.

**XE1's ack-at-append is visible and behaving.** `p.ack` is **3 µs** at one
booker: the participant acknowledges at the append, as XE1 built it. Its
own record becomes durable 1,233 µs later (`p.dur − p.ack`), which is the
wait that left the chain. At eight the ack itself costs 194-239 µs — the
reactor is busy — and the deferred remainder is 1,272-1,329 µs.

## 6. `shipped_readonly_prepares` is zero, and that answers H-XG4's last clause

**No cross-owner cell recorded a single read-only prepare.** SA-T0 skips
the `TXN_PREPARE` record for a participant that wrote nothing; a scenario-2
booking **writes to its participant**, which XD6 established by counting
four such statements, so the participant is never write-free.

H-XG4's clause — *"and SA-T0 removes the read-only participants' prepare
syncs from it"* — is therefore **refused for this shape**. SA-T0 is not
wrong and is not unexercised (the §2 diagnostic shows it firing on a
read-only enrolment, `shipped_readonly_prepares=1`,
`xowner_part_prepare_n=0`); it simply has nothing to remove here. XD6 said
this in advance — *"on a read-heavy workload, which is the only kind that
prices it"* — and `bench/v2.5.0/results-rr-read-half-*` remains the only
file that prices the lever.

## 7. The hypotheses

**H-XG1 — held.** A projected read inside a cross-owner transaction is
served typed over the answer edge; scenario 2's booking opens with exactly
that and now runs. The §2 diagnostic shows the rows arriving byte-identical
to the autocommit route's.

**H-XG2 — split**, in its own file
(`results-xg2-portal-close-tax-*`): the mechanism reproduced, the
magnitude did not transfer from XE's host, and the bridge XG asked that
cell to be does not exist.

**H-XG3 — half held.** Scenario 2 runs whole under `peer_listeners = on`
at cores ∈ {1, 2, 4} — 30 of 30 cells green, no new `UnknownOutcome`
class, and the refusal at `:4257` narrowed rather than widened. **The kill
matrix is not green because it has not been run**: XG3's process-kill half
is owed (`workplan-shipped-read-typed.md` §8c), so the hypothesis's second
clause is unverified rather than confirmed.

**H-XG4 — split, and the split is the file's point.** The increment *does*
decompose on XF4's legs (three legs, ±1.4%) and the prepare leg *is*
dominant (35-39%), both as predicted. But the prediction that SA-T0 would
remove the read-only participants' prepare syncs is **refused**: this shape
has no read-only participant. And a prediction nobody made is contradicted
along the way — the prepare leg's transport share is a function of load,
not of the protocol.

## 8. What this file does not answer

**No absolute here is comparable to XD's or to `results-scenario2-cores-*`**
(§1's scale). Anyone wanting that comparison must re-run those files at
this scale or this file at theirs.

**The kill matrix on the answer edge is owed** and is XG3's, not this
cell's; four crash points are placed and none has been fired by a harness.

**`cores=1` measures no peer listener at all** — the flag is meaningless
with one core — so it is the local baseline and not a third arm.

**Nothing here varies the durability class.** Every cell is `group`; the
`strict` and `relaxed` behaviour of a real booking's cross-owner commit is
unmeasured on this shape.
