# AO-S7 C3 — what the lock family costs, at `v2.7.0-304-g5e94dc8`

AR2 §9 step 5, run 2026-09-09 on `ao-s7-prices`. The question C3 was written
to answer is *"whether tuple-granularity borrowing wins or loses on the
shape scenario 2 has"*, and the two arms it needs are many sessions on **one
row** and the same sessions on **their own rows**, the second pricing R3's
relation-level `IX` alone.

**The answer is that neither is measurable on this engine, and the reason is
structural rather than a limit of the host.** Three runs agree, and the
finding is worth more than the numbers: the tuple lock has no contender,
because every write to a relation runs on that relation's **owner core** and
the reactor serialises what runs on a core. Eight sessions and then
thirty-two, all updating one row, never collide on the lock — they queue at
the core before they reach it.

---

## Rules (`bench/README.md`)

1. **Release, rebuilt at the measured commit.** `build-release` and
   `build-release-nolock`, both configured and built at `5e94dc8`. The
   engine's source at build time was `origin/main` unmodified except for the
   A/B arm's patch, described below; the doc edits and the new driver that
   land with this stage do not enter the binary.
2. **Block device.** `/home/cdkbs/ao-s7-c3` on `/dev/root`, ext4 (`df -T`).
   Not tmpfs.
3. **A copy of the binary, hashed.** Both copied into the run directory and
   every server started from the copy:
   - `kds_server-lock` `48854aa0d9fcf03e13264a015730b24d5f5c93ae36673deffaaf1c8bbc9b9781`
   - `kds_server-nolock` `3bf3e0ef9348e1bc4416695939bb11b7aa6a8a12b5ba9070b227c223ea9faa96`
4. **Host load, per cell.** `/proc/loadavg` and the competing-build `pgrep`
   are written by the driver into every run's header and into each JSON
   under `archive/`. **No competing build in any cell**; load 0.32–0.78.
5. **Ports chosen.** 15490 and 15491. Not 15432, which on this box belongs
   to an unrelated `kds_server`.

Host: 8 CPUs. **Not the two-CPU box some notes describe** — the rule that
binds is 4, and it bound here.

## The A/B arm, and why it needed a patched binary

`locks_ == nullptr` is not a configuration: `max_locks_per_txn` must be at
least 1 and `expeditor.cpp` refuses 0 by name, *"there is no unbounded
setting"*. So the no-family arm is a **second binary**, built from
`origin/main` with two lines changed — the `LockTable::Create` at
`expeditor.cpp:1040` skipped, and the `locks_->SetWakeRegistry` at `:1657`
guarded. The patch was reverted immediately after the build and is in no
commit. **The first build of it exited at startup**, which is itself worth
recording: nothing in the tree runs an `Expeditor` without a lock table, so
the null path had never been exercised outside a fixture.

## The driver, and the proof it ran first

`tools/lock_contention_benchmark.py`, new with this stage. No existing
driver serves C3's shape: `multicore_benchmark.py` measures N
*non-interfering relations* and says parity is the honest expectation.

AO-0 item 23 (ratified 2026-09-09) required the driver to be proved before
it priced anything. The proof run is `archive/prove.json`: the same five
arms against both binaries, checking that the **controls agree across
them** — a read takes no borrow and `SHOW META` resolves no relation, so a
family difference showing up there would be the driver or the host.

| control | lock | no-lock-family | gap |
|---|---|---|---|
| `select-hot` p50 | 531.0 µs | 525.6 µs | 5.4 µs |
| `ping` p50 | 383.5 µs | 392.7 µs | −9.2 µs |

The controls agree to within the arms' own noise. The driver is measuring
the servers.

## Cell 1 — the marked configuration: `group` durability

raft-marks-2026-09-08 §4 names `group` durability for the switch-condition
cell. Under it, **nothing about the lock family is visible at all**, and the
reason is in the same table:

| arm | lock p50 | no-lock p50 |
|---|---|---|
| `update-hot` | 2880.2 µs | 2885.8 µs |
| `update-disjoint` | 2885.5 µs | 2855.5 µs |
| `update-disjoint-again` (floor) | 2830.7 µs | 2865.0 µs |
| `select-hot` | 531.0 µs | 525.6 µs |
| `ping` | 383.5 µs | 383.5 µs |

hot − disjoint: **−5.3 µs** (lock, floor 54.8 µs), **+30.3 µs** (no-lock,
floor 9.5 µs). Both inside their floors.

**An update costs 2880 µs and a point read 531 µs on the same server.** The
difference is the commit's durability wait: 2349 µs, **82% of the
statement**. Anything the lock family costs is two orders of magnitude
below that, which is why the marked cell cannot answer its own question —
and that is a finding about the cell, not about the family.

## Cells 2 and 3 — `relaxed`, so the commit stops hiding the answer

Same servers, `durability = relaxed`, two independent runs plus one at four
times the sessions.

| run | sessions | arm | lock p50 | no-lock p50 | floor (lock / no-lock) |
|---|---|---|---|---|---|
| A | 8 | `update-hot` | 440.0 | 426.7 | |
| A | 8 | `update-disjoint` | 436.7 | 430.8 | 1.0 / 6.0 µs |
| B | 8 | `update-hot` | 428.9 | 395.5 | |
| B | 8 | `update-disjoint` | 427.9 | 395.5 | 0.6 / 4.9 µs |
| C | 32 | `update-hot` | 1473.1 | 1480.5 | |
| C | 32 | `update-disjoint` | 1474.5 | 1494.4 | 1.0 / 6.6 µs |

An update falls from 2880 µs to 437 µs, so **group commit was 85% of it**.

**Contention costs nothing measurable, in every run.** hot − disjoint:

| run | lock | no-lock-family |
|---|---|---|
| A (8 sessions) | +3.3 µs (floor 1.0) | −4.1 µs (floor 6.0) |
| B (8 sessions) | +1.0 µs (floor 0.6) | 0.0 µs (floor 4.9) |
| C (32 sessions) | −1.4 µs (floor 1.0) | −13.9 µs (floor 6.6) |

And **zero refusals in every arm of every run** — 32 sessions hammering one
row produced not one `TxnConflict`. Throughput *rose* from 8 to 32 sessions
(15.5k → 18.3k qps), so the box was not saturated either.

**R3's relation `IX`, alone**, is `update-disjoint` across the two binaries —
and this is the one number the host cannot resolve:

| run | disjoint delta | control offset (`ping` / `select-hot`) | corrected |
|---|---|---|---|
| A | +5.9 µs | +2.5 / +3.5 µs | ≈ +3 µs |
| B | +32.4 µs | +19.1 / +21.3 µs | ≈ +12 µs |
| C | −19.9 µs | +16.4 / −34.6 µs | inconclusive |

The two servers drift against each other by as much as the effect, and the
controls say so. **The honest reading is an upper bound: R3's key costs
under 12 µs on a 430 µs statement — below 3%, and probably nearer 1%.** A
number that needed the two arms inside one binary would need an off-switch
the engine does not have.

---

## What this decides

**The spin primitive is not tried** (AO-0 item 11, raft-marks §4). The
threshold that mark wrote is that the partition latch's share must exceed
the cross-core wake cost, C1's ~46% of a statement. The *whole family's*
share is under 3% and its contended arm is unmeasurable at 32 sessions.
The latch is not the bottleneck, and a spin buys nothing measurable. The
mutex stands, and AR0 D2(a)'s "spinlocks (atomics)" stays amended.

**AO-R2's partition count is not re-measured, and does not need to be.** The
sweep cell was there to find the count at which partition collisions cost
something. Collisions require two cores in the table at once on one
relation, which is the thing these runs establish does not happen: 64 ×
cores is not a number this workload can distinguish from 16 or 256.

**AO-0 item 1's cap stays 65,536 and stops being `[provisional]`.** Nothing
here approached it — the cap bounds a transaction's *ledger*, and the shapes
that reach it are bulk statements, not contention.

**E12's price, by item 24's ratified decomposition.** One table operation is
bounded by the disjoint delta, which covers a relation `IX` acquire, a tuple
`X` acquire and both releases: under 12 µs for four, so **≲ 3 µs per table
operation**. A positioned read taking an `IS` per page would pay ≲ 3 µs ×
pages. On a point read that is under 1% of a 530 µs statement; on a scan it
is linear in pages and is the number AO-S6e-b's item 19 was actually
worried about — **and it is the one thing here that is an extrapolation
rather than a measurement**, because nothing in the engine takes an `IS` to
measure.

**E7's default is not decided here.** C3 was to decide it, and C3's answer
is that the shape it was written for does not exist on this engine: there is
no tuple-granularity win or loss to read off, because the owner core
serialises before the tuple lock is reached. E7 needs a different cell, and
naming it is M3's.

## The insight, stated plainly

**AO-S6e closed by asking which of AR2's units has a contender on this
engine at all. This run answers: on this shape, none of them.** Ownership
routing puts one writer core on a relation and the reactor serialises what
runs on a core, so two writers never hold the table at once — the lock
family is taken, released, and never fought over. Its cost is the
bookkeeping, not the contention, and the bookkeeping is under 3% of a
statement whose commit is 85%.

That is not an argument against the family: it is what makes AR2-A §1's
axis — refusals converted into waits — cheap. It *is* an argument against
pricing the units by contention benchmarks on this engine, and against the
spin, the partition sweep, and any further unit added because a lock manager
usually needs one.

**What would change it** is the mover (R12/M3), which is the first thing
that would put a second core into a relation's key space, and spreading
(`kRangeSizeOff` today), which is the first thing that would put two owners
on one relation. Both are M3's. Until one of them lands, a contention cell
on this engine measures the reactor.
