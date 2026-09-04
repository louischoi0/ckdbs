# AR2 §9 step 1, cell C2 — local parallel inserts (scenario 0, stockmarket)

`tools/scenario0_stockmarket.py` at `cores = 8`, `peer_listeners = on`,
`durability = group`, with insert spreading armed
(`range_size_ids = 65536`, the ratified size, and `= 4096`, K-f's group
optimum) against the same shape with spreading off (AL-S8's `s0-c8-g`
re-run) and at `cores = 1`. This is AR2 §9's C2
(`instructions/v3.0.0/ar2-architecture-revision-borrow-model.md` §9 step
1): "does it beat 754.7 TPS" — whether local per-core `INSERT` execution
beats the shipped baseline. C1 (co-location ceiling, scenario 2) is
`results-ar2-c1-colocation-v2.7.0-178-g92cb654.md`; the two share one run
and one archive (§10 below).

**It does not.** Both armed cells lose to the shipped baseline, and the
mechanism — verified against this run's own `SHOW META` and server logs,
not merely inferred — is not simply "a shipped `INSERT` became a cheaper
local one." §5 has the detail.

**Fresh series against this engine's own history (rule 4).** The
baseline is AL-S8's `s0-c8-g`/`s0-c1-g`
(`bench/v3.0.0/results-scenario0-stockmarket-v2.7.0-157-gf6ed10c.md` §3),
measured on this same host about 6.5 hours earlier at a different commit.
PostgreSQL was not measured in this run; AL-S8's own file did not run the
PostgreSQL twin for scenario 0 either.

## 1. Stamp

| Field | Value |
|---|---|
| Date/time | 2026-09-03, 07:59:11–08:05:24 UTC (per-cell times in §2) |
| Worktree | `ar2-borrow-model` (branch `worktree-ar2-borrow-model`) |
| Commit measured | `92cb654`, `git describe --tags` = `v2.7.0-178-g92cb654` |
| Tree cleanliness | Clean at measurement time and at the time of writing this document (`git status --short` empty in this worktree). |
| Binary provenance | Copy at `/home/cdkbs/bench-runs/ar2-c1c2-92cb654/kds_server`, `sha256 d6b2c4202a929e545bace8d570cd6fec42640a58461536d4c4ab3709e3872f52` (verified in this session). Source `build-release/kds_server` mtime `2026-09-03 07:55:36.53 UTC`, copy mtime `07:57:09.40 UTC`; commit `92cb654`'s own timestamp is `07:48:26 UTC` — the binary postdates the commit it measures. |
| Engine delta against AL-S8's commit | `git diff --stat f6ed10c 92cb654 -- src include`: 13 files (`bootstrap`, `server/core_runtime`, `server/expeditor`, `server/superblock` — AM-S0's per-core-volume/`Expeditor` split; `txn/instance_visibility` new, `txn/manager`, `txn/trx_id` — AN-S0/AN-S1/AN-S1b's read view and trx-id leasing). **None of it touches `src/exec`, `src/storage/heap`, `src/server/command_dispatcher.cpp` (statement shipping / range routing / `SHOW META`), or `include/kds/server/range_alloc.hpp`** — the id-block spreading mechanism this document exercises is byte-identical to AL-S8's. `tools/` unmodified between the two commits (verified). |
| Device | `/home/cdkbs`, `ext4`, `/dev/root` (`df -T`, recorded per cell; unchanged across all cells, 48% used). |
| Build type | `build-release` (Release); not rebuilt this session. |
| Host | 8 logical CPUs, AMD EPYC 9V74, 1 socket × 4 cores × 2 threads/core (SMT); confirmed by `lscpu` this session. |
| Ports | 15589 (unused smoke cell) through 15602. `pgrep -a -f 'cc1plus|cmake --build|ctest|kds_server'` shows only the unrelated `kds_server` (pid 899, `/home/cdkbs/autotrade/...`, port 15432) throughout, no concurrent build, in every one of this document's cells. |
| Server config (common) | `checkpoint_interval_ms = 5000`, `auth = off`, `tls = off`, `placement = namespace`, `durability = group` in every cell (strict not run). Varied: `cores` (1 or 8), `peer_listeners` (on/off, moot at `cores=1`), `range_size_ids` (absent = spreading off / `kRangeSizeOff`; `65536`; `4096`). |

## 2. What was run, and in what order

Interleaved with C1's scenario-2 cells (that document's §2 has the full
merged order). This document's seven cells:

| Cell | Port | `cores` | `range_size_ids` | Precheck (UTC) | loadavg (1 min) | Build check | Driver exit |
|---|---|---|---|---|---|---|---|
| `s0-c8-sp0` | 15591 | 8 | off | 07:59:11 | 0.58 | clean | `[0]` |
| `s0-c8-sp65536` | 15593 | 8 | 65536 | 07:59:55 | 1.19 | clean | `[1]` |
| `s0-c1` | 15596 | 1 | off | 08:01:06 | 0.88 | clean | `[0]` |
| `s0-c8-sp4096` | 15595 | 8 | 4096 | 08:03:08 | 0.42 | clean | `[1]` |
| `s0-c8-sp0-r2` | 15599 | 8 | off | 08:04:18 | 1.00 | clean | `[0]` |
| `s0-c8-sp65536-r2` | 15600 | 8 | 65536 | 08:04:31 | 0.98 | clean | `[1]` |
| `s0-c1-r2` | 15602 | 1 | off | 08:05:09 | 1.52 | clean | `[0]` |

Each cell: fresh data file, fresh server from the hashed binary copy.
Fixed across all seven, AL-S8's own flags verbatim (`run_cells.py`'s
`S0_ARGS`): `--users 100 --accounts-per-user 3 --assets 30 --traders 8
--txn-per-user 50 --verify 200 --seed 1 --sync` — a work target of 5,000
committed business transactions (2 `INSERT trades` + 2 `UPDATE accounts`
each), split 625 per trader; `--profit` (periodic reporting) at its
default, on. A driver exit of `1` means at least one statement replied
`ERR` (`tools/scenario0_stockmarket.py:1521`,
`sys.exit(1 if errors else 0)`) — every armed cell exits 1, every unarmed
one exits 0 (§7).

## 3. TPS

Rule 5a: throughput, not delay.

| Cell | cores | `range_size_ids` | TPS | committed | torn |
|---|---|---|---|---|---|
| `s0-c8-sp0` | 8 | off | 382.2 †contaminated, §9 | 5,000 | 0 |
| `s0-c8-sp0-r2` | 8 | off | **703.9** | 5,000 | 0 |
| `s0-c8-sp65536` (C2) | 8 | 65536 | **590.1** | 5,000 | 7 |
| `s0-c8-sp65536-r2` (C2) | 8 | 65536 | **533.8** | 5,000 | 7 |
| `s0-c8-sp4096` (C2) | 8 | 4096 | **523.1** | 5,000 | 6 |
| `s0-c1` | 1 | off | 506.0 †contaminated, §9 | 5,000 | 0 |
| `s0-c1-r2` | 1 | off | **720.1** | 5,000 | 0 |
| `s0-c8-g` [measured, AL-S8] | 8 | off | 754.7 | 5,000 | 0 |
| `s0-c1-g` [measured, AL-S8] | 1 | off | 700.9 | 5,000 | 0 |

**Spreading loses against the shipped baseline on this host.** Reading
the clean `sp0-r2` (703.9) as the reference (§9 excludes the contaminated
first `sp0` run): `sp65536-r2` is **−24.1%** (533.8), `sp4096` is
**−25.7%** (523.1). Even the least-bad spreading number, the first
`sp65536` run (590.1), is **−16.2%** against the clean baseline and still
below AL-S8's own `s0-c8-g` (754.7) by a wider margin still. **Neither
value R5 offers a choice between — the ratified `65536` and K-f's
group-optimal `4096` — beats the baseline; both lose by roughly the same
amount** (24.1% and 25.7%, a 1.6-point gap between them that is itself
inside this document's own noise floor, §9). Armed spreading also costs
correctness-adjacent overhead the unarmed baseline does not pay: 6–7
`torn` transactions per 10,000 attempted `INSERT trades`, discussed in
§6–§7.

## 4. Percentiles

Every row is a `Phase.summary()` distribution (`tools/bench_common.py`),
read from `<cell>.run.stdout.txt` / the driver's own JSON — nothing below
is recomputed.

### `txn` — 2×`INSERT trades` + 2×`UPDATE accounts`, one client round trip each, µs

| Cell | ops | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| `s0-c8-sp0` | 5,000 | 8,911.0 | 14,171.4 | 19,986.8 | 23,919.8 | 32,479.3 | 42,372.5 |
| `s0-c8-sp0-r2` | 5,000 | 8,601.8 | 9,943.4 | 10,304.4 | 16,103.2 | 23,549.3 | 39,234.2 |
| `s0-c8-sp65536` | 5,007 | 5,574.2 | 11,502.2 | 12,492.0 | 17,880.8 | 36,128.8 | 117,823.7 |
| `s0-c8-sp65536-r2` | 5,007 | 4,722.2 | 12,847.8 | 13,834.2 | 20,499.5 | 26,871.2 | 55,547.0 |
| `s0-c8-sp4096` | 5,006 | 4,588.7 | 12,996.8 | 14,075.8 | 20,326.2 | 28,271.6 | 60,155.5 |
| `s0-c1` | 5,000 | 8,380.5 | 10,495.2 | 10,866.6 | 16,433.4 | 149,024.8 | 556,078.0 |
| `s0-c1-r2` | 5,000 | 9,395.0 | 10,470.7 | 10,804.2 | 12,967.6 | 15,993.2 | 37,221.6 |
| `s0-c8-g` [measured, AL-S8] | 5,000 | 8,597.2 | 9,720.5 | 10,041.2 | 13,781.3 | 17,277.6 | 32,932.5 |
| `s0-c1-g` [measured, AL-S8] | 5,000 | 8,646.2 | 10,577.5 | 10,973.9 | 14,641.4 | 18,901.8 | 24,976.2 |

`ops` exceeds 5,000 in the spreading cells because a torn transaction's
statements are still individually measured (§6).

### `trade-insert` (`INSERT`, spreads under C2) and `account-update` (`UPDATE`, never spreads), µs

| Cell / phase | ops | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| `sp0` trade-insert | 10,000 | 1,653.1 | 3,604.6 | 4,881.4 | 6,116.2 | 8,920.0 | 27,597.9 |
| `sp0` account-update | 10,000 | 2,210.1 | 3,766.6 | 4,899.2 | 6,328.9 | 9,236.1 | 24,360.2 |
| `sp0-r2` trade-insert | 10,000 | 1,325.9 | 2,430.1 | 2,533.3 | 4,071.1 | 7,257.4 | 19,955.4 |
| `sp0-r2` account-update | 10,000 | 1,344.5 | 2,436.4 | 2,537.2 | 3,996.7 | 6,429.7 | 20,014.5 |
| `sp65536` trade-insert | 10,014 | 1,134.1 | 2,250.4 | 2,427.8 | 3,779.1 | 7,979.9 | 35,834.7 |
| `sp65536` account-update | 10,014 | 1,319.0 | 3,178.5 | 3,600.6 | 6,077.1 | 9,128.1 | 44,307.4 |
| `sp65536-r2` trade-insert | 10,014 | 1,136.2 | 2,178.0 | 2,352.3 | 4,847.2 | 8,009.4 | 35,793.4 |
| `sp65536-r2` account-update | 10,014 | 1,054.9 | 3,410.2 | 4,286.2 | 7,401.5 | 10,917.3 | 19,677.7 |
| `sp4096` trade-insert | 10,012 | 1,123.8 | 2,186.3 | 2,354.9 | 4,826.0 | 8,254.1 | 44,425.6 |
| `sp4096` account-update | 10,012 | 1,065.1 | 3,485.5 | 4,360.6 | 7,671.6 | 11,677.2 | 33,654.2 |
| `s0-c1` trade-insert | 10,000 | 1,465.6 | 2,573.0 | 2,676.1 | 3,695.9 | 9,935.3 | 546,566.5 |
| `s0-c1` account-update | 10,000 | 2,269.6 | 2,579.3 | 2,681.0 | 3,602.9 | 9,053.6 | 546,457.7 |
| `s0-c1-r2` trade-insert | 10,000 | 1,502.5 | 2,566.8 | 2,654.8 | 3,284.1 | 5,038.4 | 17,269.2 |
| `s0-c1-r2` account-update | 10,000 | 2,271.3 | 2,568.5 | 2,657.3 | 3,244.1 | 5,270.4 | 17,270.8 |

AL-S8's own table for this pair carried p50/p99 only: `s0-c8-g`
trade-insert 2,461.3 / 6,286.1, account-update 2,454.4 / 6,256.8 —
quoted as published, not backfilled.

## 5. Wait breakdown, and the verified mechanism behind §3's loss

Rule 3: name each wait and give it a share. This section also answers
the two items this document was asked to verify against the raw data
rather than assume.

**The loss does not sit where the naive story puts it.** Comparing the
clean `sp0-r2` (spreading off) against `sp65536-r2` (spreading on, same
host state, same target): `txn` p50 rises 10,304.4 → 13,834.2 µs
(+34.3%; `sp4096`: +36.6%). Read per statement:

| Statement | `sp0-r2` p50 | `sp65536-r2` p50 | `sp4096` p50 | Δ (r2 spreading − sp0-r2) |
|---|---|---|---|---|
| `trade-insert` (`INSERT`, spreads) | 2,533.3 | 2,352.3 | 2,354.9 | **−181.0 µs (−7.1%), faster** |
| `account-update` (`UPDATE`, never spreads) | 2,537.2 | 4,286.2 | 4,360.6 | **+1,749.0 µs (+68.9%), slower** |

**The statement whose execution mode changed (`trade-insert`) got
slightly *faster*; the statement whose execution mode is identical in
every cell (`account-update`, always shipped to core 0) absorbed almost
the whole regression, consistently across both `range_size_ids` values.**
Doubling each per-statement delta for the transaction's two of each
(`−362 µs` saved on inserts, `+3,498 µs` added on updates) nets `+3,136
µs` — close to, not exactly, `txn`'s own `+3,529.8 µs` p50 delta (the two
do not have to reconcile exactly: `txn` is one client-measured span
across all four statements, not a sum of the sub-phases' own
percentiles). **Hypothesis, tagged as such and unverified**: a
peer-local `INSERT`'s commit still waits on core 0's own WAL
group-commit drain (single-stream WAL, `docs/spec/wal.md` §3), and once
peers are generating their *own* local commit demand in addition to
core 0 servicing every shipped `UPDATE`, core 0's drain cadence lengthens
for everyone waiting on it — including the shipped `UPDATE`s that never
changed handling. This would explain why the regression lands on the
*unchanged* statement rather than the changed one, but `SHOW META` has no
per-commit mean wait counter split by core to confirm it directly (only
`shipped_wait_us_max`, a maximum); what follows is the closest
corroboration this run's counters allow.

**`SHOW META`'s `sched_foreground_polled_us` is consistent with the
hypothesis's direction.** In every one of the three spreading cells, a
peer's own local-commit cost (its foreground-polled time divided by its
own `wal_group_commits`) sits far above core 0's own per-shipped-statement
service cost:

| Cell | Peers doing local `INSERT` commits (`wal_group_commits` > 0), µs/commit | Core 0, µs/shipped-statement |
|---|---|---|
| `sp65536` | cores 2/4/5/6/7: 369.1 / 328.4 / 324.1 / 286.8 / 332.0 | 41.8 (`436,849 / 10,445`) |
| `sp65536-r2` | cores 1/2/3/7: 320.8 / 317.0 / 241.2 / 297.5 | 51.6 (`473,997 / 9,194`) |
| `sp4096` | cores 1/2/4/5/6/7: 287.6 / 281.6 / 263.9 / 249.8 / 235.6 / 201.4 | 59.2 (`519,302 / 8,770`) |

A peer's own local commit costs roughly **5–7× core 0's own cost for
servicing a shipped statement**, consistently across all three cells.
This is corroborating, not conclusive: `sched_foreground_polled_us` on
the peer side bundles the commit's own wait with whatever else that core
polled for in the same interval, and it says nothing about the
*requesting* side's wait for a shipped reply either. What would verify
the hypothesis directly is a per-commit durability-wait counter split by
core (or by local-vs-shipped origin) — it does not exist today.

**Item 1 — the shipped-statement arithmetic.** In `s0-c8-sp65536`
(first run only), core 0's `shipped_executed` (10,445) equals the exact
sum of every peer's own `shipped_statements` (0 + 1,253 + 422 + 1,253 +
3,760 + 1,253 + 2,504 = 10,445) — every shipped statement landed exactly
once, on core 0, with no loss or double-count. **The same equality holds
in the other two spreading cells too, once core 0's own outbound
shipping is folded in.** In `sp65536-r2` and `sp4096`, `trades`'
several owner cores mean core 0 itself now ships some statements out to
a peer — 1,249 in `sp65536-r2` (to peers 2 and 3, per their
`shipped_executed` of 1 and 1,248), 1,250 in `sp4096` (to peer 7).
Summing `shipped_statements` across *every* core, including core 0's own
1,249/1,250, gives 10,443 and 10,020 — and summing `shipped_executed`
across every core gives the same two totals, so **instance-wide shipped
equals executed in all three cells**. What does not equal core 0's
`shipped_executed` (9,194 / 8,770) is the sum restricted to the *peers
alone* — but that is the wrong subtraction, not a broken invariant:
peer-only `shipped_statements` (9,194 / 8,770) already equals core 0's
`shipped_executed` exactly, because core 0's own outbound 1,249/1,250
is what accounts for the rest. Every spreading cell's `SHOW META`
carries `split_relations=2`; the `trades` split (relation id 4018) is
**not** identical across cells — `4018:6@6` in `sp65536`, `4018:5@5` in
`sp65536-r2`, `4018:7@7` in `sp4096`, since the split point moves with
each run's own insert timing — while `user_periodic_profit` (id 4026)
does stay `2@2` in all three. So once `trades` has several owner cores,
a statement can ship to **whichever peer currently owns the target
range**, not only to core 0 — and, symmetrically, a write core 0 itself
issues can now ship *out* to a peer too. **Decomposing the total precisely into
"`UPDATE accounts`" versus "reporter/load rows" is not possible from
these counters**: `account-update`'s own attempted count (10,014 =
5,000×2 committed + 7×2 torn) is close to but does not equal any clean
subset of the totals above, and `shipped_statements` carries no
per-statement-type breakdown. Treat "≈10,000 of it is `UPDATE
accounts`, the rest is load/report/verify traffic that happened to land
on a peer" as the right order of magnitude, not an exact accounting.

*Corrected on `ar2-borrow-model` after `c40b3cc` (archive re-read
2026-09-04): this passage originally said the exact equality "does not
hold" in `sp65536-r2`/`sp4096`, comparing each cell's peer-only
`shipped_statements` sum (10,443 / 10,020) against core 0's own
`shipped_executed` (9,194 / 8,770) and reading the gap as a broken
invariant. Those peer-only sums include core 0's own outbound
`shipped_statements` in the raw `SHOW META` totals I quoted (1,249 /
1,250 — a statement core 0 issued that shipped to a peer, once `trades`
has several owner cores); once that is set aside, the peer-only sum
(9,194 / 8,770) already equals core 0's `shipped_executed` exactly, and
instance-wide `shipped_statements` equals `shipped_executed` in all
three cells (10,443 = 10,443; 10,020 = 10,020). The equality holds; it
was the reading, not the counters, that was wrong. Also corrected: the
`split_relation_detail` value quoted was `4018:6@6` — true only for
`sp65536` — read as identical across all three cells; it is `4018:6@6`
(sp65536), `4018:5@5` (sp65536-r2), `4018:7@7` (sp4096), while
`4026:2@2` genuinely is identical in all three. The paragraph's
conclusion — that a multi-owner `trades` lets a statement ship to
whichever peer currently owns the target range — stands.*

**Item 1, continued — which cores the trader sessions landed on.** For
the two *unarmed* cells (no multi-owner complication), the arithmetic
is clean enough to reconstruct: one trader session, wherever it lands,
contributes exactly `625 × 4 = 2,500` shipped statements (spreading off,
so every statement of every peer-landed trader ships). In `sp0`, peer
`shipped_statements` were core1=2,700, core2=2,500, core3=5,000,
core4=2,500, core5=0, core6=2,500, core7=0 — decomposing as core3
hosting **two** traders (5,000 = 2×2,500), cores 1/2/4/6 one each, cores
5/7 none, and core1's `+200` matching `--verify 200`'s read count
exactly if the verify connection also landed there (`2,500 + 200 =
2,700`) — **six of the eight traders on peers, two on core 0 itself**
(the shipped total, 15,200, is fully accounted for by 6×2,500+200 with
no remainder for the loader or reporter, so those two connections most
likely landed on core 0 too). In `sp0-r2`, the peer totals (2,500 +
2,500 + 5,000 + 7,500 + 2,500 = 20,000, exactly `8 × 2,500`) decompose
as core4 hosting **three** traders, core3 two, cores 1/2/5 one each —
**all eight traders on peers**, with no remainder at all for verify,
load or report, meaning those three connections landed on core 0 in
this repeat. This is inferred from the arithmetic, not from a
session-to-core log (none exists — `SHOW META` carries no per-connection
identifier), and it is only this clean because spreading was off; the
same reconstruction for the spreading cells is muddied by the
multi-owner-target effect above.

**Item 2 — the `torn` transactions.** Every logged conflict, in every
spreading cell, is the *same* class: a session whose transaction is
"bound to core 0" (i.e., landed on core 0's own listener) attempting an
`INSERT` after `trades`' growing tail range had already migrated to a
peer. `logs/s0-c8-sp65536/kdb.log` (7 lines, matching `torn=7` and
`trade-insert`'s own `errors=7` exactly):

```
ERR TXN_CONFLICT retryable=1 this transaction's writes are bound to
core 0 and relation 'trades_s0c8sp65536' is owned by core 7 ...  (×1)
ERR TXN_CONFLICT retryable=1 ... is owned by core 6 ...           (×6)
```

*Corrected on `ar2-borrow-model` after `c40b3cc` (archive re-read
2026-09-04): the archive directory
(`bench/v3.0.0/archive/ar2-c1c2-v2.7.0-178-g92cb654/`) holds no `logs/`
directory — the run's own README describing one as archived was wrong
(fixed there in this same pass) — so this quotation is not
independently reproducible from the archive the way the rest of this
document's numbers are; it is carried over from the run directory's own
`kdb.log` at measurement time. It is consistent with what the archive
does hold: `s0-c8-sp65536.meta.json`'s core-0 `cross_core_write_refusals=7`
with detail `0>6:4018=6,0>7:4018=1` (the same 1/6 split quoted above),
peer `shipped_refusals` summing to 7, and `trade-insert`'s own `errors=7`
in `s0-c8-sp65536.run.stdout.txt` — and the same triple match holds for
`sp65536-r2` (7/7/7) and `sp4096` (6/6/6), confirming the three-way
match below without needing the log file itself.*

matching core 0's own `cross_core_write_refusals=7`,
`cross_core_write_refusal_detail=0>6:4018=6,0>7:4018=1` exactly. The
same three-way match (log count = `torn` = core 0's
`cross_core_write_refusals` = the **sum of every peer's own
`shipped_refusals`**) holds in all three spreading cells: `sp65536`
7=7=7=7, `sp65536-r2` 7=7=7=7, `sp4096` 6=6=6=6. **What this run's data
does not let me confirm is that the peer-side `shipped_refusals` are
literally the same seven events**: every logged line says "bound to
core 0," never to a peer, so the seven conflicts are core 0's own local
write attempts refused by its own ownership check, not a peer's shipped
statement being refused — `shipped_refusals` counting the identical
number in every cell is either the same events double-counted from two
ends of one internal re-route, or a coincidentally equal, unlogged
class of its own. The numeric match across three cells at two different
magnitudes (7, 7, 6) is strong circumstantial evidence for the former,
but no single log line proves it. **The driver does not retry an
autocommit statement**: `tools/scenario0_stockmarket.py`'s trader loop
(around line 712) counts a statement that errors with no open
transaction as `torn` and moves on — this run's `S0_ARGS` carries no
`--txn`, confirmed by every cell's own JSON (`"txn": false`) — and the
process exits 1 because `errors` (the sum of every trader's error count)
is nonzero (`tools/scenario0_stockmarket.py:1521`). **Under AR2, this
refusal class is exactly what "refusal → wait" (R1, R10) would replace**:
a session bound to core 0 finding the range it needs now belongs to a
peer would park on a lock instead of aborting the statement. This is a
pointer to the draft, not a conclusion this run can support further —
whether that wait would itself have been cheap or expensive is C3's
question (§9 step 5 there), not this one's.

Other waits, briefer:

| Wait | Estimate | How |
|---|---|---|
| **Durability/commit** | dominant across every cell regardless of spreading: `sp0-r2`'s own trade-insert/account-update p50 (~2,533–2,537 µs) is close to AL-S8's own group-commit baseline (2,454–2,461 µs) — spreading does not remove this floor, it adds to it (above) | §4's tables |
| **Read wait** | n/a to `txn` itself (no read inside the four statements); `profit-scan`'s FilterScan runs concurrently and does not gate `txn`, unchanged from AL-S8 | AL-S8 §5, unchanged here |
| **Lock/conflict wait** | the `torn` class above; otherwise ~0 — each trader owns a disjoint account partition by construction, unchanged from AL-S8 | `torn` counters, §3 |
| **Client/socket round trip** | included in every number above, not separable without `--log-level debug` (avoided, as AL-S8's document explains, to not perturb the durability path being priced) | — |

## 6. A units correction to this run's own briefing

The "sync" phase (`ops=1`, the driver's single end-of-run flush) was
described ahead of this document as reading "~2.5–2.7 s" in the three
degraded cells against "AL-S8's 0.29 s." **That is a units slip, not a
finding: every value involved is in microseconds, not seconds** (the
`<cell>.run.stdout.txt` phase table's header, "mean us," applies to
every percentile column in the row). The corrected picture is still
worth reporting, because it does not say what the "s" version implied:

| Cell | `sync` (µs) | Degraded? |
|---|---|---|
| `sp0` | 2,486.7 | yes |
| `sp0-r2` | 2,642.4 | no |
| `sp65536` | 5,399.8 | no (torn, not degraded) |
| `sp65536-r2` | 1,694.6 | no |
| `sp4096` | 2,582.8 | no |
| `s0-c1` | 2,724.5 | yes |
| `s0-c1-r2` | 2,572.7 | no |
| `s0-c8-g` [measured, AL-S8] | 289.3 | — |
| `s0-c1-g` [measured, AL-S8] | 2,475.7 | — |

**`sync` does not separate the degraded cells from the clean ones** —
`sp0`'s own degraded run (2,486.7 µs) is *lower* than its clean repeat
(2,642.4 µs), and the widest spread in the table (`sp65536` 5,399.8 µs
vs its own repeat 1,694.6 µs, 3.2×) belongs to a pair the task's
briefing did not flag as degraded at all. What the corrected numbers do
show: **every `cores=8` cell in this run's `sync` phase is 6–19× slower
than AL-S8's own `cores=8` measurement** (289.3 µs) and instead sits in
the same band as AL-S8's own `cores=1` number (2,475.7 µs) — consistent
with, though not proven by, this run's own device probes (4 KiB +
`fdatasync`: 2.39 ms and 2.69 ms; 64 MiB + `fdatasync`: 161 MB/s and
174 MB/s — `bench/v3.0.0/archive/ar2-c1c2-v2.7.0-178-g92cb654/device-probes.txt`,
landed alongside this document in the same commit, `9e5068c`; *corrected
on `ar2-borrow-model` after `c40b3cc`, archive re-read 2026-09-04 — this
sentence originally said these values were "not as a file this document
can cite," which was true only until the same session wrote the file
below*). Read
as: this whole session's `fdatasync` path was several times slower than
whatever AL-S8's session experienced, uniformly across cells, which is a
host/day fact (§8), not a per-cell contention signal — `sync` is not the
right instrument for finding the three degraded cells; §9 is.

## 7. Correctness

`--verify 200` read 200 accounts back against the driver's own
arithmetic after every cell; **all 200 matched in all seven cells**, and
the balances are correct in every armed cell despite the 6–7 torn
transactions (a torn `INSERT` leaves an orphaned trade row with no
matching balance movement claimed, which is exactly what `torn` — not a
verify failure — is for; `tools/scenario0_stockmarket.py`'s own
docstring). Nothing in this document is a report of lost or corrupted
data — the throughput and latency numbers above describe a workload that
also produced the right data, matching AL-S8's own scenario0 finding.

## 8. AL-S8 delta, and why it reads as noise where it should

This document's engine differs from AL-S8's by the commits named in §1,
none of which touches the code path measured here (verified by `git
diff --stat`). The unarmed cells — the only ones with a like-for-like
AL-S8 predecessor — read:

| Shape | This run (clean) | AL-S8 | Delta |
|---|---|---|---|
| `cores=8`, spreading off | 703.9 (`sp0-r2`) | 754.7 | −6.7% |
| `cores=1` | 720.1 (`s0-c1-r2`) | 700.9 | +2.7% |

The `cores=1` delta (+2.7%) sits inside AL-S8's own stated noise floor
(§8 there: 1.5% same-configuration repeat, 5–8% under concurrent load).
**The `cores=8` delta (−6.7%) is larger than that floor** and is the one
number in this document that does not cleanly read as host/day noise by
that standard alone; §6's finding — this whole session's `fdatasync`
path was uniformly slower than AL-S8's — is the more specific,
data-backed explanation, since `cores=8`'s workload is far more
sync-sensitive (every peer's write ultimately waits on core 0's drain)
than `cores=1`'s single-connection path is. Read the −6.7% as a
consequence of §6's slower device/session state, not of the AM-S0/AN-S1
commits, which the `git diff --stat` in §1 shows do not touch this code.

## 9. Noise floor, and the degraded cells

**Two of this document's seven cells are contaminated: `s0-c8-sp0`
(first run, 382.2 TPS) and `s0-c1` (first run, 506.0 TPS).** (A third
degraded cell, `s2-c1`, belongs to the interleaved scenario-2 half of
this run and is documented in
`results-ar2-c1-colocation-v2.7.0-178-g92cb654.md` §8.) The two
scenario-0 cells do **not** share one symptom — they are two different
mechanisms, and conflating them understates what the raw data shows:

- **`s0-c8-sp0` is a sustained shift, not a tail event.** Its `txn` p50
  (19,986.8 µs) is essentially double its own clean repeat's (10,304.4
  µs), and every phase (`trade-insert`, `account-update`) shows the same
  roughly-2× shift across p0 through p95, not just the tail — its own
  `max` (42,372.5 µs) is unremarkable — well below `sp65536-r2`'s own
  `max` (55,547.0 µs), a cell this document otherwise treats as clean
  (§9's own pairing). This looks like sustained host contention for this
  cell's whole ~14 s window, not a single stall.
- **`s0-c1` is a rare, severe tail event on an otherwise-normal body.**
  Its `txn` p50 (10,866.6 µs) sits within 0.6% of its own clean repeat's
  (10,804.2 µs) — **the body of the distribution is fine** — but its p99
  (149,024.8 µs) and max (556,078.0 µs, 0.56 s) are far outside anything
  else in this document, and the same shape appears in `trade-insert`
  (max 546,566.5 µs), `account-update` (max 546,457.7 µs) and
  `profit-insert` (p99 133,521.0 µs, max 546,486.8 µs — not shown in
  §4's tables, from the raw stdout table). A handful of statements
  landed on a severe, brief stall; the rest of the run was normal.

Both cells are excluded from every comparison in §3/§8 above except
their own table rows. Their precheck `loadavg` (0.58 and 0.88) is not
elevated relative to clean cells in the same run (e.g. `sp0-r2` at 1.00,
`s0-c1-r2` at 1.52) — whatever caused either symptom is not visible in
the coarse per-minute load average, consistent with a brief or
host-level event rather than a sustained competing process (the `pgrep`
precheck rules out a concurrent build in every cell, §1).

**The run-to-run floor from the clean repeat pairs:**

| Pair | Values | Spread ((max−min)/min, matching `results-am-s1-page-latch-v2.7.0-183-gc985d37.md`'s "own spread" definition) |
|---|---|---|
| `s0-c8-sp0-r2` alone (no clean partner — `sp0` is excluded) | 703.9 | n/a |
| `s0-c8-sp65536` / `-r2` | 590.1 / 533.8 | **10.5%** |
| `s0-c1-r2` alone (no clean partner — `s0-c1` is excluded) | 720.1 | n/a |

**The `sp65536` pair's own 10.5% spread is wide** — wider than every
floor this suite has previously recorded (AL-S8's 1.5%/5–8%, C1's own
0.3–1.1% in the same run, §8 there) — and it is the reason §3 states
C2's two `range_size_ids` values (590.1/533.8 vs 523.1) as "losing by
roughly the same amount" rather than ranking them: the 56.3-point gap
between `sp65536`'s two runs (590.1 − 533.8) is wider than the 10.7-point gap between
`sp65536-r2`'s and `sp4096`'s single runs, so this document cannot
distinguish "4096 vs 65536" from "this cell's own run-to-run noise" at
one repeat each. **Both spreading configurations losing to the shipped
baseline is not in question — the margin is; both `sp65536` values and
`sp4096`'s sit clearly and entirely below `sp0-r2` (703.9) even after
allowing for a spread this wide.**

*Corrected on `ar2-borrow-model` after `c40b3cc` (archive re-read
2026-09-04): this passage originally computed the gap as 66.7 points and
the spread as 9.5%, i.e. (max−min)/max. 590.1 − 533.8 is 56.3, not 66.7;
and the spread is now read with the same definition
`results-am-s1-page-latch-v2.7.0-183-gc985d37.md` uses for its own "own
spread" columns, (max−min)/min, so the two documents' spread figures are
comparable — for this pair that is 56.3/533.8 = 10.5%, not 9.5%
((max−min)/max). The values 590.1/533.8 themselves, and the 10.7-point
`sp65536-r2` vs `sp4096` gap, were already correct and are unchanged.*

## 9a. Row-set size — not swept, named as a gap

Every cell runs AL-S8's own fixed target: 5,000 committed transactions,
100 users, 287 accounts. Rule 9 asks for a sweep at 200/1K/10K rows at
minimum so a fixed cost can be told apart from a per-row one; this run
does not do that — it documents a fixed-shape configuration re-run
already taken (AR2 §9 step 1), not a cardinality sweep, and this
session's own instruction was to document, not re-measure. **This
document cannot say whether spreading's loss (§3, §5) is a fixed
per-run cost (e.g. more id-block leases to set up) or grows with
`--traders`/the row count** — that is a real gap a future C2-shaped run
at a second size would close, not a finding this run supports.

## 10. What this means for AR2

Restated as the answer to §9 step 1's own question: **no — spreading
does not beat the shipped baseline on this shape, at either
`range_size_ids` value, and it adds a new refusal class** (6–7 torn
`INSERT`s per 10,000, §5–§6). The mechanism §5 verifies is not "a
shipped write became a cheaper local one" — the statement that changed
handling (`trade-insert`) got slightly *faster*; the statement that did
not change handling (`account-update`) absorbed the loss, consistent
with (not proven by) a single shared WAL drain now serving more
independent local-commit demand.

Consequences for the draft:

- **§9 step 1's own text**: "if it does not beat 754.7 TPS, group commit
  bounds ingest and finer borrowing cannot help." This document's
  cleanest comparison (`sp0-r2` 703.9 vs `sp65536-r2` 533.8, both `r2`,
  both this session) says it does not beat the *shipped baseline
  measured in this same session* (703.9), a fortiori AL-S8's 754.7.
  Recommend reading this sentence as satisfied on the "does not beat"
  branch — but see the next point before taking its consequent as
  settled.
- **§1's "measured premise," second bullet**: "the cost on short
  autocommits: scenario 0 gains 7.6–7.7% at `cores = 8`" — that was
  spreading-off, comparing `cores=8` against `cores=1` with every write
  still shipped. This document adds the missing half: **arming
  spreading does not extend that gain — it reverses it**, by roughly
  24–26% against this session's own shipped `cores=8` baseline.
  Recommend §1 cite this document beside that bullet so the premise
  reads as "shipping already wins at short autocommits; local execution
  makes it worse," not left implicit.
- **§9 step 1's causal reading — "group commit bounds ingest" — is an
  incomplete diagnosis** per §5 above: the regression is not on the
  statement group-commit would bound (the `INSERT`, which got faster);
  it is on the unrelated shipped `UPDATE`, pointing at drain-cadence
  contention from *added* concurrent local-commit demand rather than a
  per-`INSERT` bound. Recommend R5/R12 (execution locality, E7) weigh
  this before proposing "local unless routed" as the default for
  `INSERT`: on this shape, making writes more local made an
  *unrelated* statement slower, which a design that reasons per-statement
  about hop-vs-local cost would not predict.
- **R1/R10 (borrow scope, deadlock)** — §5's Item 2 names the refusal
  class the lock-wait model would replace; this document does not (and,
  per §9 step 5's own gate — C3 runs "after M2's row lock exists" —
  cannot before M2) say whether the replacement wait is cheap. Flagged,
  not answered.
- **C3** (contention under the row lock) still cannot run before M2,
  unchanged by this document.

Raw driver JSON, `SHOW META` dumps, server logs, configs and orchestrator
scripts for every cell in this document (and C1's) are archived at
`bench/v3.0.0/archive/ar2-c1c2-v2.7.0-178-g92cb654/`
(`results-ar2-c1-colocation-v2.7.0-178-g92cb654.md` is the other half of
this run).
