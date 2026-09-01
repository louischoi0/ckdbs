# XG2 — the portal-close tax, refunded: 29.8 µs a statement on this host

**Headline. The workaround cost one round trip per statement, and getting
it back is worth 29.8 µs of p50 — 41.3% of what a statement cost with it.**
Batched `C_CLOSE` **42.4 µs p50 / 45.5 µs mean**; trailing `C_CLOSE`
**72.2 / 73.7**. The batched arm's three-repeat spread is **0.47%**, which
is the tightest floor anything in this line has measured and leaves no room
to read the delta as noise.

**H-XG2 splits, and the half that fails is the number.** The hypothesis
said reverting the workaround "recovers the measured 11-12 µs per
statement". The *mechanism* reproduces exactly — the workaround is one
extra round trip and removing it removes exactly one round trip — but the
**magnitude does not transfer across hosts**: 29.8 µs here against XE's
11.1 µs there. A loopback round trip is not a portable constant, and this
is the second time in three files that a magnitude measured on XE's host
did not survive a move (XF3's addendum was the first). What should be
quoted is the shape: **one round trip per statement, whatever a round trip
costs on the machine in question.**

**What this does *not* do, stated before anything is read against it.** It
does not reconnect XE's absolutes to XD's. XG's own §Measurement calls this
re-baseline "the bridge that makes XE-era and XG-era absolutes comparable",
and it is not one: XE's 28.1/39.2 µs pair was taken on a different machine
with a different client, so the bridge it would need is a *host* constant
and this file measures a host-specific one. XE's quarantine note therefore
stays up, and XG5 should not lift it on this file's evidence.

Measured on the worktree `xf` (`/home/cdkbs/ckdbs/.claude/worktrees/xf`) at
**`9a4dd76`** (`git describe --tags` → **`v2.7.0-30-g9a4dd76`**) plus XG2's
uncommitted server fix and client revert — the tree state is stated in §1
because the binary is not a committed commit's.

## 1. Provenance

| | |
|---|---|
| Date/time (UTC) | 2026-09-01 |
| Commit | `9a4dd76` (XG0, docs only) **plus XG2's working-tree changes**: the one-line portal erase in `src/server/kwp_session.cpp`, its two tests, and `tools/kwp.py`'s revert. Committed immediately after this file, so the results commit is the first that carries both |
| Binary | `build-release/kds_server`, copied out of the build tree before the first cell (ck-tester rule 5), `sha256` `a9dce88b0eaac5aa…` |
| Driver | `bench/portal_close_tax_probe.py` (new), `--cores 1 --durability relaxed --statements 2000 --repeats 3` |
| Arms | **both in the driver, not in `tools/kwp.py`** — see §2 |
| Device | `/dev/root`, ext4, `df -T` checked. Never tmpfs |
| Host | 8 logical CPUs = 4 physical × 2 threads, AMD EPYC 9V74, Linux 6.17.0-1022-azure; `bench/wait_quiet.sh` before the run (loadavg 0.67). One unrelated `kds_server` from another checkout resident at 0.8% CPU, named rather than omitted |
| Test suite | 3,095 of 3,095 passing in `build-release` (3,093 before; the two new cells are `KwpSessionTest.AFailedStatementTakesItsPortalWithIt` and `.ErroringPastThePortalCapNoLongerWedgesTheSession`) |
| Raw output | `bench/v2.7.2/archive/xg2-portal-close-tax/xg2-tax.json` |

## 2. Why both arms live in the driver

`tools/kwp.py` is the thing under test, and by the time this ran it had
already been reverted — so a driver that called it would have measured
whichever way that file happened to be, and only that way. Both framings
are therefore written out in the probe, side by side, where a reader can
check that they differ in exactly one thing:

```
batched   PARSE BIND EXECUTE CLOSE SYNC      -> read to S_READY          (1 round trip)
trailing  PARSE BIND EXECUTE SYNC            -> read to S_READY
          CLOSE SYNC                         -> read to S_READY          (2 round trips)
```

`durability = relaxed`, `cores = 1`, following XE §3's isolation cell: the
quantity is a per-statement round trip and a device sync in the middle of
it would drown it.

## 3. The cells

Three repeats per arm, **interleaved** — one repeat of each in turn, so a
drift in the machine lands on both — 2,000 single-row `INSERT`s each.

| | batched | trailing | Δ |
|---|---:|---:|---:|
| p0 (µs) | 38.1 | 64.7 | +26.6 |
| p25 | 42.1 | 71.8 | +29.7 |
| **p50** | **42.4** | **72.2** | **+29.8 (+70.3%)** |
| p95 | 52.2 | 84.6 | +32.4 |
| p99 | 59.4 | 92.9 | +33.5 |
| **mean** | **45.5** | **73.7** | **+28.2** |
| three-repeat p50 spread | **0.47%** | 35.60% | |

Medians of each arm's three repeats; the per-repeat tables are in the
archived JSON.

**The trailing arm's spread is 35.6% and it is one repeat, not a
distribution.** Its three p50s are 72.1, 72.2 and **97.8**; the first two
agree to 0.1 µs and the third is an excursion. It moves the *floor* and not
the median, and the finding does not rest on it — the delta is 29.8 µs
against a batched floor of 0.47%. Reported rather than dropped, because a
floor computed and then discarded is a floor nobody can check.

**The delta is flat across the distribution** (+26.6 at p0, +33.5 at p99),
which is what one fixed round trip looks like and is the strongest evidence
here that the mechanism is what the arms say it is. A cost that varied with
the statement would not do that.

## 4. What the server change actually fixes, and how it is held

The tax was never the defect — it was the price of working around one.
`kwp_session.cpp`'s skip-to-sync discards every frame up to the next
`C_SYNC`, so a `C_CLOSE` pipelined behind a statement that *errors* is
dropped: the portal leaked on exactly the statements that most needed it
closed, and at `kMaxSessionPortals` (64) every further `C_BIND` was refused
until the 60 s idle sweep freed one — a retrying client running at one
statement per portal lifetime.

XG-R8 erases the portal on the error arm, where it is already in hand. Two
unit cells hold it: one asserts a failed statement leaves `portal_count()`
at 0 and that closing the vanished name is not itself an error; the other
runs `kMaxSessionPortals + 8` failing statements on one session and then a
successful one, which is the defect's own reproduction and which answered
`RESOURCE_EXHAUSTED` before.

**Checked end to end as well**, because the unit tests drive the session
object and not the client: 100 failing statements through the reverted
`tools/kwp.py` against a real server, then an `INSERT` and a `SELECT` that
must still work. 100 refusals, one row back. Before the fix that session
was unusable after the 64th.

**The newline arm did not move.** The full suite is green including the
byte-identity cells, which is what XG's conclusion 1 asks of every commit
in this order.

## 5. What this file leaves open

**The bridge XG asked this cell to be does not exist**, and §Headline says
why: XE's numbers are another machine's. If reconnecting XE-era absolutes
to XD-era ones matters, it needs both arms re-run on one host, which is a
different cell from this one and is not owed by XG2's text as written.

**Nothing here measures the leak's *cost*, only its workaround's.** How
much a wedged session cost a real workload is unmeasured and now
unmeasurable on this build — the wedge is gone. XE §3's account of a
benchmark matrix stalling on it is the only record of that, and it is a
narrative rather than a number.

**One repeat of the trailing arm excursed** (§3) and no cause was chased.
It is one of six cells and it does not carry the finding, but a reader
comparing this file's trailing absolutes to another's should use 72.2 and
know the third repeat exists.
