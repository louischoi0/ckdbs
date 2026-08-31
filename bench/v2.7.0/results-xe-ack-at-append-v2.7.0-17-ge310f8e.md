# XE — the ack moved to the append: correctness holds, the b=1 saving does not

**Headline, stated first because rule 8 asks for it: H-XE1's own falsifier fires.**
At one booker-equivalent (a serial loop of cross-owner commits, one
connection), the ack-at-append change (XE1, `8e76417`) saves **88-193 µs**
of commit p50/mean on this device — under the **500 µs** floor the work
order set as the line between a finding and noise, and inside this shape's
own **16.4%** three-repeat spread. The mechanism the order itself named as
the risk is what the numbers show: a closed serial loop has nothing to hide
the deferred sync behind, so the next transaction's own prepare queues
behind the previous commit's now-deferred drain, and the wait that moved
off the ack comes back on the next iteration's critical path. **At eight
concurrent coordinators the picture reverses and grows**: p50 falls
**25.9%** (5,761 → 4,272 µs) and mean falls **24.0%**, both several times
wider than either arm's own three-repeat spread (12.0% / 8.9%) — a real,
repeatable saving, in the opposite direction from H-XE2's prediction that
the saving should *shrink* with load. Correctness holds throughout: the
kill matrix's 45 cells (15 points × 3 passes, including the two new
`decide_acked_predurable` cells and one targeted checkpoint cell) all
resolve to `COMMIT` on both relations with equal counts, and
`shipped_enrolment_expiries`/`txn_in_doubt_unresolved` read 0 in every one.

Measured in the worktree `measure-v2.7.1`
(`/home/ubuntu/ckdbs/.claude/worktrees/measure-v2.7.1`) at `e310f8e`
(`git describe --tags` → **`v2.7.0-17-ge310f8e`**), branch
`xe-ack-at-append`, executing `instructions/v2.7.1/workorder-xd.md` rows
XE2 and XE4. XE0/XE1/XE3 are already landed at this commit (`8e76417`,
`f979cd1`, `e310f8e` itself — a review pass); XE5's docs closure is the
operator's, not this file's.

## 1. Provenance

| | |
|---|---|
| Date/time (UTC) | 2026-08-31, 08:12-09:22 |
| Worktree | `measure-v2.7.1` (`/home/ubuntu/ckdbs/.claude/worktrees/measure-v2.7.1`) |
| Branch | `xe-ack-at-append` |
| Commit measured | **`e310f8e`** (`git describe --tags` → **`v2.7.0-17-ge310f8e`**). Work order lives under `instructions/v2.7.1/`; the operator has not named a `v2.7.1` version of record, so this file follows `v2.7.0`, per the 2026-08-25 filing rule |
| Tree state | Clean at the start. This session's changes are confined to `bench/` and `tools/` — `bench/txn_2pc_kill_matrix_probe.py` (XE2's cells), `bench/xe4_crossowner_commit_probe.py` (new), `bench/wal_sync_decomposition_probe.py` (one-line compatibility fix, §3), `tools/kwp.py` (one compatibility fix, §3), `bench/docs/README.md`, and this results file plus its archive. No `src/`, no `include/`, no `docs/`, no `CLAUDE.md`, no `instructions/`. HEAD did not move |
| Binary provenance — `build-release/kds_server` (used for XE2, and for one isolated round-trip measurement in §3) | mtime `2026-08-31 08:07:05.80Z`, `sha256` `2904f635…d350950`, built from `e310f8e` before this session started (no rebuild during it — confirmed by re-checking the hash at the end) |
| Binary provenance — the XE4 A/B pair | `/home/ubuntu/xe-probe/kds_server-base` (pre-XE1, `sha256` `9f8b17af1115…`) and `/home/ubuntu/xe-probe/kds_server-xe1` (`8e76417`, `sha256` `7b603df28b90…`), handed to this session already built. **Verified rather than trusted**: `git diff 8e76417..e310f8e -- src include` touches only comments, a test file, `docs/inflight/known-gaps.md`, and one RAII wrapper (`CommandDispatcher::CommitAckScope`) that restores the *previous* `commit_ack_` value where the code it replaced always restored the literal default — behaviourally identical on every path reachable today, since `commit_ack_` is never non-default when `DispatchAsync` is entered (no nested dispatch exists). Confirmed independently by `strings`: `decide_acked_predurable` (XE1's new crash point) is absent from `kds_server-base`, present once in `kds_server-xe1` and once in `build-release/kds_server`. **Both binaries are copied into the run's own directory** (`/home/ubuntu/bench-xe/kds_server-{base,xe1}`), hashes re-checked after copying and matching the sums above; every server in XE4 started from these copies, never from `build-release`'s own file. On this evidence, `kds_server-xe1` measures the same engine `e310f8e` does for every question this order asks, and is cited as such throughout §4 |
| Build | `CMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`) for all three binaries (confirmed by their timing: sub-millisecond in-process work throughout) |
| Test suite | **3,086 of 3,086 passing**, `build-release/tests/kds_tests`, run independently twice this session (35.9s, 35.6s) — once before XE2/XE4, once after all engine-adjacent tooling changes. No `src/`/`include/` changed this session, so a before/after delta is not applicable; cited to confirm the launching session's own count |
| Device | Data files under `/home/ubuntu/bench-xe/`, on `/dev/root`, **ext4** (`df -T`: 259 GB, 68-64% used across the session). Never tmpfs |
| Host | 8 logical CPUs = 4 physical cores, 2 threads/core, Ubuntu, Linux 7.0.0-1011-aws — the same host `results-xd-commit-decomposition` ran on |
| Host quiet | `uptime`/`pgrep cc1plus` checked before every phase (never running); XE4's harness additionally gates every cell on `bench/wait_quiet.sh` (1-minute load < 0.70). Load stayed in 0.13-0.67 throughout — this session ran alone on the box |
| Server config | XE2: `cores = 3`, `placement = rotate` (`fastpath.*`: `cores = 1` or `placement = creating`), `peer_listeners = on`, `durability` unvaried (D2 is where every crash point lives). XE4 nopl: `cores = 8`, `peer_listeners = off`, `durability` per row. XE4 pl (the substitute probe, §4.1): `cores = 2`, `placement = rotate`, `peer_listeners = on`, `durability` per row |
| Raw output | `bench/v2.7.0/archive/xe-ack-at-append-v2.7.0-17-ge310f8e/` — `xe2-kill-matrix/` (the full 45-cell JSON and log), `xe4-matrix/` (28 cells' JSON plus the harness's own run log), `harness/` (the shell script that drove XE4) |

## 2. XE2 — the crash matrix, extended: 45/45 hold, including the new window

**H-XE3 holds without qualification.** The matrix gained the two ordinal
cells at `participant.decide_acked_predurable` (XE1's new crash point,
between the ack and the drain) and one targeted cell beyond the matrix's
shape, `checkpoint.decide_acked_predurable_gap`. Three passes each, same
equal-counts oracle, plus a check this order added (`indoubt_health`,
not in the work order's own text but implied by H-XE3's falsifier): `shipped_enrolment_expiries` and `txn_in_doubt_unresolved`, read from
each participant core after every restart. Both are 0 in all 45 cells.

| point | expected (each relation) | pass 1 | pass 2 | pass 3 | verdict |
|---|---:|---|---|---|---|
| `coordinator.before_prepare` | 0 | 0,0 | 0,0 | 0,0 | PASS |
| `participant.prepare_logged_predurable` | 0 | 0,0 | 0,0 | 0,0 | PASS |
| `participant.prepare_logged_predurable:2` | 0 | 0,0 | 0,0 | 0,0 | PASS |
| `participant.prepare_durable_prereply` | 0 | 0,0 | 0,0 | 0,0 | PASS |
| `participant.prepare_durable_prereply:2` | 0 | 0,0 | 0,0 | 0,0 | PASS |
| `coordinator.prepared_predecide` | 0 | 0,0 | 0,0 | 0,0 | PASS |
| `coordinator.decided_presend` | 1 | 1,1 | 1,1 | 1,1 | PASS |
| `participant.decide_applied_preack` | 1 | 1,1 | 1,1 | 1,1 | PASS |
| `participant.decide_applied_preack:2` | 1 | 1,1 | 1,1 | 1,1 | PASS |
| **`participant.decide_acked_predurable`** (new) | 1 | 1,1 | 1,1 | 1,1 | PASS |
| **`participant.decide_acked_predurable:2`** (new) | 1 | 1,1 | 1,1 | 1,1 | PASS |
| `resolution.coordinator_stream_gone` | refuses, names the layer | analysis | analysis | analysis | PASS |
| `fastpath.local_only` | never enters the protocol | COMMIT,alive | COMMIT,alive | COMMIT,alive | PASS |
| `fastpath.cores1` | never enters the protocol | COMMIT,alive | COMMIT,alive | COMMIT,alive | PASS |
| **`checkpoint.decide_acked_predurable_gap`** (new, targeted) | 1 | 1,1 | 1,1 | 1,1 | PASS |

**45/45 cells PASS.** `resolution.coordinator_stream_gone` refuses by
name every pass (`refused_by = "analysis (anchor check, one layer above
the resolver)"`, matching XD's own reading — analysis's absent-anchor
check fires before the resolver's own absent-stream arm is ever reached).
Every `fastpath.*` cell stays alive through the whole run, confirming D1's
one-owner path never enters the protocol regardless of XE1.

**The new window resolves exactly like its sibling — via redo, not via a
weaker guarantee.** At `decide_acked_predurable`, the killed process has
appended its own `COMMIT` record but not yet drained it; the file's bytes
survive the `SIGKILL` regardless (the kernel's page cache, not the
process, holds them until an explicit `fsync` or a real device-level
crash — this instrument cannot and does not claim to test the latter), and
recovery redoes from them exactly as it would from a fully-synced record.
`mixed = True` fired at ordinal 2 in every pass, confirming the asymmetric
case — one participant resolved by redo, the other by resolution against
the coordinator's stream — was genuinely reached, not merely possible.

### 2a. The targeted checkpoint cell: source-proven rather than raced

The order asked for one cell beyond the matrix's shape: a kill *after the
ack, before the drain, with a checkpoint forced in between* — the window
`cross-owner-txn.md` §2c's checkpoint argument covers. **Built, run, and
its own docstring says plainly what it does and does not show, because the
literal race cannot be constructed on this engine and that is itself a
finding, not a gap in the attempt.**

Source-verified: under `kAtAppend`, the append
(`WalManager::Commit`'s `++pending_group_commits_`, a plain increment, no
I/O — `wal/manager.cpp:203-205`) and the crash
(`shipped_statement_executor.cpp:889-900`, the ack sent then
`CrashPointHit` called immediately) run inside **one**
`CoroTask::Poll()` call with no `co_await` between them —
`include/kds/sched/coro.hpp`'s `on_done_` callback fires synchronously in
the same `Poll()` that ran the coroutine to completion. Nothing — the
ordinary drain or a checkpoint alike — can execute between two calls in
the same synchronous stack frame, and a fuzzy checkpoint is furthermore
multi-tick by design (`checkpointer.hpp`: "spreads across reactor
iterations"), so it could not complete inside a zero-width gap even if one
existed. The window the order names is **empty by construction**, not
merely hard to hit.

What the cell measures instead, honestly: with the checkpointer forced to
cycle continuously throughout the whole scenario (`checkpoint_interval_ms
= 20`, `log_level = debug`, a 500 ms pre-transaction delay giving ~25
cycles on both peer cores before the targeted transaction even begins,
confirmed via 108-111 `anchor published` log lines per pass), the outcome
at this crash point is unchanged — still `COMMIT` on both relations,
identical to the plain `decide_acked_predurable` cell with no active
checkpointer. That agreement is corroborating evidence for the source
argument, not a substitute for it: the argument itself is that
`Checkpointer::Complete()`'s durability wait
(`wal/checkpointer.cpp:230-236`, `wal_.EnsureDurable(end_lsn.value())`) is
the **same** `Sync()` function an ordinary drain calls
(`wal/manager.cpp:153` vs `:225-231`) — not a second, weaker mechanism —
so any checkpoint that runs at all after an append and before a real
crash necessarily carries it, exactly as the spec argues, by sharing code
rather than by a special case.

## 3. Two excursions this session needed, both outside XE4's own row

**Flagged plainly, per the operator's instruction, because both blocked
XE4's cells before it could measure anything — they are not part of what
the order asked for, and they are not engine changes.**

**(1) `tools/kwp.py`'s `execute()` leaked a portal on any statement that
errored with `max_rows == 0` (the common case) — fixed, and the fix
costs a round trip.** Verified against the server:
`kwp_session.cpp:389-391` discards every non-`C_SYNC` frame while
`skipping_to_sync_` holds ("discarded, silently"), so a `C_CLOSE` bundled
in the same batch as a failing `PARSE`/`BIND`/`EXECUTE` — the design the
milestone (`eecda94`) shipped, its own comment claiming "closed on both
arms" — is dropped exactly on the statements that most need it closed,
and the portal leaks to `kMaxSessionPortals` (64). A session that errors
64 times, which is an ordinary rate under any workload with real
conflicts, then refuses every further statement, permanently — this is
what stalled the first attempt at every cell in this file (millions of
tight-loop retries against a session that could no longer bind a portal).
Fixed by never bundling `C_CLOSE`: it is now always its own frame, sent
after the statement's own `S_READY` has already been read, which is the
one guarantee `C_SYNC` gives regardless of how the statement went.

**This is a real defect and the right fix, and it changes the measuring
instrument mid-line — stated exactly, not glossed:**

- **It costs a round trip on the common path.** `max_rows == 0` used to
  close for free inside the one batch; every non-transaction-control
  statement (`execute()` — ordinary `SELECT`/`INSERT`/`UPDATE`, not
  `BEGIN`/`COMMIT`/`ROLLBACK`, which ride a separate frame pair, §3a)
  now pays an extra `C_CLOSE` + `C_SYNC` + read-to-`S_READY` after its
  own `S_READY`. **Measured directly**, isolated from any device sync
  (`durability = relaxed`, `cores = 1`, 2,000 single-row `INSERT`
  statements, `build-release/kds_server` at `e310f8e`, machine quiet):
  the old (pre-fix) client's round trip is **28.1 µs p50 / 30.0 µs
  mean**; the fixed client's is **39.2 µs p50 / 42.3 µs mean** — the fix
  costs **~11-12 µs per statement** on the success path. Small in
  absolute terms, but real, and it is why §4's absolute latencies do not
  read against XD's.
- **So absolute per-statement latencies in this file are not comparable
  to `results-xd-commit-decomposition`'s** (at `951a91a`), which was
  measured with the pre-fix client. **The A/B delta inside this file is
  unaffected** — both `base` and `xe1` run through the identical (fixed)
  client — so every base-vs-xe1 comparison in §4 stands; no cell in this
  file is read beside an XD absolute number.
- **§3a below checks which side of the timer the extra round trip falls
  on**, per the operator's third instruction, rather than assumed.

**(2) A KWP `STOP` closes the socket instead of replying**
(`known-gaps.md`: "`STOP` is reachable only on the debug port"), which was
propagating out of `wal_sync_decomposition_probe.py`'s `main()` uncaught
and skipping the `--json` write below it — every cell's measurement was
already collected by that line. Fixed by catching
`ConnectionError`/`OSError` around that one call (the driver's own
`finally` block already force-terminates the process regardless), and the
same guard was needed in the kill matrix's `fastpath.*` cells and in the
new `xe4_crossowner_commit_probe.py`. **No latency number is affected** —
`STOP` runs after every measurement in every driver that needed this fix.

Both fixes are scoped to `tools/kwp.py` and `bench/`; neither touches
`src/`, `include/`, or engine behaviour. The kill matrix's counting step
needed a third, related change — routing the post-restart row count
through the newline debug surface rather than KWP (§4.1's refusal applies
to a shipped read regardless of driver) — documented in
`bench/docs/README.md`'s entry for `txn_2pc_kill_matrix_probe.py` rather
than repeated here, since it changes no number in this file.

### 3a. Where the extra round trip falls, relative to what XE4 times

**Outside the timed window, on both arms, confirmed by source rather than
assumed.** `BEGIN`/`COMMIT`/`ROLLBACK` never call `execute()` at all —
`ckdbs_cli.ServerConnection.send_command` special-cases all three to
`Connection.begin()`/`commit()`/`rollback()` (`tools/kwp.py:486-496`),
which send `C_TXN_BEGIN`/`C_TXN_COMMIT`/`C_TXN_ABORT` and read
`S_TXN_OK` — a wholly separate frame pair from `execute()`'s
`PARSE`/`BIND`/`EXECUTE`/portal path the fix touched. This is true in
both drivers this file uses: `bench/xe4_crossowner_commit_probe.py`
times exactly `conn.send_command("COMMIT")`
(`t0 = time.perf_counter(); r = conn.send_command("COMMIT"); dt = …`),
and `tools/scenario2_freight.py`'s `commit` phase times exactly the same
call (`send(client, phases["commit"], "COMMIT")`,
`tools/scenario2_freight.py:914`). The fix's extra round trip lands on
the `INSERT`/`SELECT`/`UPDATE` statements that precede the timed window
(both arms, identically), inflating overall wall-clock/TPS by a few
statements' worth of ~11-12 µs each, but never the `commit` percentile
tables in §4 — those measure only the `C_TXN_COMMIT`/`S_TXN_OK` round
trip, on both sides of the timer, on both arms.

## 4. XE4 — the A/B: nopl unchanged, pl's b=1 saving unresolvable, b=8's real

### 4.1 Why the requested `pl` cells could not run as asked, and what ran instead

**`bench/wal_sync_decomposition_probe.py`'s `peer_listeners = on` cells —
what the order asked XE4 to run — cannot execute at all today, on any of
the three binaries, for a reason orthogonal to and predating XE1.**
`eecda94` ("Milestone KW: KWP/1 is the protocol the server speaks") landed
on `main` the same day as XE1 and merged into this branch ahead of it.
Under KWP/1 every session carries a result sink, and
`CommandDispatcher::ShipStatement` refuses a shipped **read** to a session
that has one (`command_dispatcher.cpp` — "a shipped read cannot answer a
typed client", `known-gaps.md`'s own documented, acknowledged gap).
Scenario 2's every booking opens with exactly such a read (`book_once`'s
cargo lookup, before any write), so a booker whose connection lands on a
peer core — the entire premise of a cross-owner cell — refuses at its
first statement. **Confirmed empirically before concluding this**:
`tools/scenario2_freight.py --bookers 1` against a `peer_listeners = on`
instance refused every booking attempt, identically on `kds_server-base`,
`kds_server-xe1` and `build-release/kds_server`.

**Reported as `wal_sync_decomposition_probe.py`'s `pl` cells: not
executed.** In its place, `bench/xe4_crossowner_commit_probe.py` (new
this session, documented at `bench/docs/README.md`) measures the one
thing XE4 actually asks about — the participant's decide-commit latency
under D2 — with a shape nothing in this refusal touches: `BEGIN`; one
`INSERT` (id omitted, so each owner core issues its own — a
caller-supplied pk is refused on a peer core by design,
`workplan-peer-writer.md` §7a) into each of two relations under
`placement = rotate`; `COMMIT`, timed alone; no `SELECT` anywhere in the
measured path. `--cores 2` puts both relations on the single peer core
`rotate` has room for (`rotate` never places on core 0), which is a
**one-participant** cross-owner shape — matching scenario 2's own booking,
since every relation it declares is core 0's — confirmed by
`syncs_per_commit` landing at 3.00-3.01 under `group`/`strict` and 2.02
under `relaxed` at `--concurrency 1`, exactly XD's own model. **What this
substitute is not**: it does not exercise the per-statement shipping cost
a real booking pays for its other 5-7 statements, only the last one —
`COMMIT`, the leg XE1 changed — so its absolute latencies are a lower
bound on a real booking's cross-owner cost, not a replacement for
`results-xd-commit-decomposition`'s own numbers (also, per §3, measured
with a different client).

**`group`/`nopl` ran exactly as the order asked**, via
`wal_sync_decomposition_probe.py` at XD's own scale (2,000 organizations,
200 ships, 2,000 operations, 20,000 cargos, 5,000 bookings, `cores = 8`) —
`peer_listeners = off` means every booker lands on core 0 regardless, so
nothing here ever reaches the shipped-read refusal.

### 4.2 `group`/`nopl` — unchanged, as conclusion 3 requires

Three repeats each, fresh server and data file per repeat, `--bookers 1`
and `--bookers 8`, 5,000 bookings each (ops = 15,000 across the three
repeats):

| cell | base TPS | xe1 TPS | Δ TPS |
|---|---:|---:|---:|
| `group, nopl, b=1` | 513.2 | 515.1 | +0.37% |
| `group, nopl, b=8` | 799.8 | 808.9 | +1.14% |

**Commit-latency distribution** (µs, median of 3 repeats' own summaries):

| cell | p0 | p25 | p50 | p95 | p99 | mean |
|---|---:|---:|---:|---:|---:|---:|
| `group, nopl, b=1` base | 666.0 | 908.9 | 955.1 | 1,111.7 | 1,219.9 | 958.0 |
| `group, nopl, b=1` xe1 | 661.5 | 911.5 | 960.9 | 1,138.1 | 1,233.2 | 969.8 |
| `group, nopl, b=8` base | 762.2 | 1,056.2 | 1,111.9 | 2,043.3 | 2,199.8 | 1,250.8 |
| `group, nopl, b=8` xe1 | 744.7 | 1,047.9 | 1,107.4 | 2,042.6 | 2,200.0 | 1,221.2 |

**Noise floor, this shape** (peak-to-peak / median across the 3 repeats):
p50 spread is 0.48-0.83% and TPS spread is 0.84-1.26% across all four
rows. Every delta above (p50: +0.61%, −0.40%; TPS: +0.37%, +1.14%) sits
inside that floor. **Conclusion 3 holds: a one-owner transaction — the
changed site is reached only by an enrolled cross-owner participant — is
untouched, verified rather than assumed, at both booker counts.**

### 4.3 `group`/`pl` — H-XE1's falsifier fires at b=1; a real, larger, opposite-direction saving at b=8

Three repeats each, fresh server and data file per repeat, `--concurrency
1 --txns 2000` and `--concurrency 8 --txns 4000` (equal committed work,
rule 7 — a transient `TXN_CONFLICT retryable=1`, the row-id lease grant's
own cadence and unrelated to the protocol under test, is retried in place
rather than counted; every cell reached its full target, 0 uncounted
errors):

| cell | base TPS | xe1 TPS | Δ TPS |
|---|---:|---:|---:|
| `group, pl, b=1` | 303.9 | 328.3 | +8.02% |
| `group, pl, b=8` | 602.7 | 803.5 | **+33.32%** |

**Commit-latency distribution** (µs, median of 3 repeats' own summaries;
ops = 2,000/repeat at b=1, 4,000/repeat at b=8):

| cell | p0 | p25 | p50 | p95 | p99 | mean |
|---|---:|---:|---:|---:|---:|---:|
| `group, pl, b=1` base | 1,977.1 | 2,545.1 | 2,718.3 | 3,644.1 | 3,818.6 | 2,838.6 |
| `group, pl, b=1` xe1 | 1,955.1 | 2,477.2 | 2,629.9 | 3,254.5 | 3,437.3 | 2,645.6 |
| `group, pl, b=8` base | 2,915.1 | 4,871.5 | 5,761.2 | 8,723.5 | 10,249.4 | 6,012.7 |
| `group, pl, b=8` xe1 | 2,247.5 | 3,760.2 | 4,272.2 | 6,902.6 | 7,958.5 | 4,569.4 |

**Noise floor, this shape** (peak-to-peak / median, p50, across the 3
repeats): `b=1` base **16.38%**, xe1 **6.26%**; `b=8` base **12.03%**, xe1
**8.89%** — several times wider than `nopl`'s, expected of a 3-leg
cross-owner chain compounding jitter from each leg's own device sync
against one leg's alone (`results-xd-commit-decomposition` §6 found the
same asymmetry).

**H-XE1 does not hold. Its falsifier — a saving under 500 µs — fires.**
The `b=1` delta is **−88.4 µs p50 (−3.25%)**, **−193.0 µs mean (−6.80%)**
— an order of magnitude under the 500 µs line, and smaller than base's own
16.4% three-repeat spread. This is not a small positive finding; it is a
finding that there is nothing here to resolve above the noise this shape
carries. **It matches, properly repeated, what an ad-hoc two-core probe
outside this session's own scope already warned of**: three interleaved
repeats there reversed the sign entirely (base 2,223/2,649/2,312 µs
against xe1 2,896/2,902/2,923 µs). This file's own three repeats do not
reverse sign, but they do sit inside the floor the ad-hoc probe's own
warning predicted, and the mechanism it named is the one the order's own
background section flagged as the risk: **a closed serial loop pays the
same chain regardless of where the wait sits**. `syncs_per_commit` stays
at 3.00-3.01 on both arms (XE1's own prediction — "total may stay near
3.00… the counter to watch is latency" — confirmed exactly), so the
record still syncs every time; what moved is only *when* something waits
for it, and at `b=1` the very next `BEGIN`'s prepare has nothing else to
run while the previous commit's deferred drain is still in flight on the
same single connection, so the wait that left the ack comes back on the
next iteration's own critical path.

**At `b=8` the picture is not "smaller" — it is larger, and in the
opposite direction from H-XE2's prediction.** p50 falls **1,489.0 µs
(−25.85%)**; mean falls **1,443.3 µs (−24.00%)**. Both deltas are several
times wider than either arm's own noise floor (12.0% base, 8.9% xe1),
which is the standard this file reads as a resolvable finding rather than
a noise-band one. H-XE2 predicted the opposite shape — a saving that
*shrinks* with load because XD2's drain-sharing discount already absorbs
part of the third leg before XE1 ever runs. **The direction is reversed
here**: under concurrency, `syncs_per_commit` still falls with load on
both arms (base 3.00 → 1.76, xe1 3.00 → 1.45 — batching discount, XD2's
own finding, reproduced), but the *latency* saving grows rather than
shrinks. §5 reads what this says about the mechanism; the finding itself
is that H-XE2, as stated, does not hold on this shape, and this file says
so rather than rounding a reversed pattern into a confirmed one.

### 4.4 The two controls: `strict, pl, b=1` and `relaxed, pl, b=8` — unchanged, one repeat each

Conclusions 1 and 3's controls, run once each per the order — D1 (`strict`)
takes no branch by construction, and this checks it:

| cell | base p50 | xe1 p50 | Δ | base syncs/commit | xe1 syncs/commit |
|---|---:|---:|---:|---:|---:|
| `strict, pl, b=1` | 2,783.4 µs | 2,815.4 µs | +1.15% | 3.0020 | 3.0020 |
| `relaxed, pl, b=8` | 3,811.9 µs | 3,902.6 µs | +2.38% | 1.2003 | 1.1820 |

**Both unchanged, as conclusion 1 requires.** A single-repeat delta of
1-2% on a shape whose own three-repeat floor (from §4.3, the nearest
comparable shape) runs 6-16% is not read as a finding either direction —
it is inside the noise this shape carries, which is exactly what "no
branch by construction" predicts and what a control is for. `relaxed`'s
syncs/commit (1.18-1.20) is XD's own "already-2.00" reading at a
*different* concurrency and core count (XD measured `b=8` at `cores = 8`
on the real scenario-2 shape; this file's `relaxed` row is `b=8` at
`cores = 2`, where the same-peer-core batching §4.3 found for `group`
applies here too) — not a disagreement with XD, a different point on the
same batching curve, named rather than left to look like one.

## 5. What this order teaches about the engine

**A durability wait moved off the ack does not automatically move off the
critical path — it depends on whether anything else can run while it is
in flight.** At `b=1`, a closed serial loop has nothing: the next
transaction's own first sync-requiring step queues behind the previous
one's now-deferred drain, so removing an inline park just relocates it one
statement later on the same connection. XE1's own background section
named this risk in its "syncs may stay near 3.00" caveat and this file's
own numbers confirm the count did stay near 3.00 while the latency did
not move — the two readings the order asked to be kept separate turned
out to genuinely separate, in exactly the way that makes the sync count
the wrong instrument for this specific claim.

**At `b=8`, the same change is a real, large win, plausibly for a
different reason than "less waiting per commit": fewer *parked
coroutines*.** Before XE1, every concurrent commit that shares the same
group-commit sync must itself park until that sync completes — under
load, several coroutines from several coordinator connections park on the
identical wake-up. After XE1, a D2 commit under `kAtAppend` never parks at
all; the reactor can advance straight to the next ready statement instead
of holding a population of parked tasks that all wake together when the
shared sync resolves. That is a queueing-discipline change, not a
per-commit latency change, and queueing effects are exactly the kind of
thing that grows rather than shrinks with concurrency — which is the
opposite of what H-XE2 assumed (that the *sync-sharing* discount, not the
*parking* discount, was the dominant mechanism at load). This file does
not separate the two mechanisms with a server-side per-leg timer — the
same instrument gap `results-xd-commit-decomposition` §8 named — so this
is offered as the more plausible reading of the b=8 result, not a proven
one.

**The engine-level gap this session found and worked around —
`ShipStatement`'s refusal of a shipped read to a typed client — is larger
than one benchmark's inconvenience.** It is already named in
`known-gaps.md` as the milestone's own known limitation, and this session
adds one data point to its cost: it makes `results-xd-commit-decomposition`'s
entire measured shape (a real booking, shipped reads and all) unrunnable
under `peer_listeners = on` until it is closed, which is a standing gap
for every future cross-owner measurement that needs a realistic
read-then-write workload rather than a pure-write substitute like this
file's.

## 6. What this order does not answer

**The b=1 finding is a non-finding, stated as one rather than omitted.**
No number in §4.3 supports a latency saving at one booker-equivalent on
this device, and none should be read as such. **The b=8 finding wants a
fourth repeat this file did not budget** — three repeats is what the order
specified, and the delta clears both arms' floors comfortably, but a
mechanism this surprising (opposite the stated prediction) is exactly the
kind of result worth a confirming run before it is treated as settled
engine behaviour. **No per-leg server-side timer exists** to separate
"fewer parks" from "less waiting," named again because it is the same gap
`results-xd-commit-decomposition` §8 and R6-B §8 both already named — a
third citation is not a new finding, only a growing bill. **The `pl`
shape XE4 actually asked for — a real scenario-2 booking under
`peer_listeners = on` — was not measured at all**, blocked by §4.1's
engine-level gap; this file's substitute answers the narrower question
(commit latency alone) but not the wider one (a real booking's full
cross-owner cost under D2's new ack timing), which stays open until the
shipped-read refusal is closed. Rule 9's row-set sweep does not apply: the
quantity every table here measures is a fixed transaction's own
commit-protocol cost, not a function of relation size, matching
`results-xd-commit-decomposition` §9's own statement of the same fact for
the same reason.

---

## Addendum, 2026-08-31 — XF3's confirming repeat: the saving reproduces, its size does not

**Added by work order XF row XF3** (`instructions/v2.7.1/workorder-xf.md`),
which §6 of this file asked for in its own words: *"a mechanism this
surprising (opposite the stated prediction) is exactly the kind of result
worth a confirming run before it is treated as settled engine
behaviour."*

**Verdict in one line: the b=8 saving is real, resolvable and in the same
direction — and it is 16.6% here against 25.9% there, so what reproduces
is the mechanism, not the magnitude.**

### A3.1 Provenance, and the one thing that could not be reproduced

| | |
|---|---|
| Date/time (UTC) | 2026-08-31, 13:49-13:51 |
| Worktree | `xf` (`/home/cdkbs/ckdbs/.claude/worktrees/xf`), branch `xf-shipped-read` at **`04403a1`** (`git describe --tags` → **`v2.7.0-22-g04403a1`**) — the tree the harness and the driver came from; no engine code of this tree was measured |
| Arms | rebuilt from their own commits, **not** §1's binaries: `kds_server-base` from **`85d2bda`** (`8e76417~1`, pre-XE1), `sha256` `6945f64e0f2019e3…`; `kds_server-xe1` from **`8e76417`**, `sha256` `ebe0ee2cce7923bd…`. Each built from a clean `git archive` of its own commit into its own tree, `CMAKE_BUILD_TYPE=Release`, and **copied out of its build tree before the first cell** (ck-tester rule 5) |
| Arm verification | `strings kds_server-base \| grep -c decide_acked_predurable` → **0**; the same on `kds_server-xe1` → **1**. The identical check §1 used, and it separates the arms by the crash point XE1 introduced rather than by trust |
| Driver | `bench/xe4_crossowner_commit_probe.py`, unmodified, `--cores 2 --durability group --concurrency 8 --txns 4000` — byte-identical arguments to §4.3's `group, pl, b=8` cells |
| Harness | `archive/…/xf3-confirm/run_xf3.sh`. **Arms interleaved** (base r1, xe1 r1, base r2, …) rather than run as two per-arm blocks, which is what `run_xe4_matrix2.sh` did and what CLAUDE.md's interleaved-A/B rule asks for |
| Repeats | **three per arm, not one.** The order asked for "one additional full repeat"; a single repeat on a machine with no floor of its own is a number with no error bar, so this took three and established the floor here (A3.2) |
| Device | `/dev/root`, **ext4**, 259 GB, 32% used — `df -T` checked on the actual workdir, not assumed. Never tmpfs |
| Host | 8 logical CPUs = 4 physical × 2 threads, **AMD EPYC 9V74**, Ubuntu, **Linux 6.17.0-1022-azure** |
| Host quiet | `uptime` gated by `bench/wait_quiet.sh` before every cell (1-min load < 0.70); the gate held the first cell for ~2 minutes while this session's own builds decayed. **One unrelated process was resident throughout and is named rather than omitted**: an unrelated `kds_server` on port 15432 belonging to another checkout, measured at **0.8% CPU** over its lifetime. It is small; it is not zero |
| Raw output | `bench/v2.7.0/archive/xe-ack-at-append-v2.7.0-17-ge310f8e/xf3-confirm/` — six JSON summaries, the run log, and the harness |

**The one thing that could not be reproduced, stated before any number is
read.** The order asked for "same binaries by hash, same harness, fresh
files". **The binaries by hash were not available and the host is not the
same machine** — §1 ran on `/home/ubuntu`, Linux 7.0.0-1011-aws; this ran
on an Azure AMD EPYC box. The CPU topology (8 logical / 4 physical), the
filesystem (ext4) and the device class match; the machine does not. So
this is **an independent replication, not the fourth repeat of the same
three**, and every absolute latency below is a different device's.

That has a direct consequence for how H-XF2 can be read, and it is not a
detail: the hypothesis's criterion — *"lands within the established floors
(base 12.0%, xe1 8.9%) of the recorded medians"* — is an **absolute**
comparison, and an absolute comparison across two machines tests the
machines, not the change. It is reported below anyway, because refusing to
report it would hide that it fails; but the reading that carries weight is
the **within-host delta**, which is what an A/B is for.

### A3.2 The cells: three repeats each, and this host's own floor

**Commit-latency distribution** (µs, median of 3 repeats' own summaries;
ops = 4,000/repeat; 0 uncounted errors on all six cells, 5-14 in-place
`TXN_CONFLICT` retries per cell — the row-id lease grant's own cadence,
the same transient §4.3 counted the same way):

| cell | p0 | p25 | p50 | p95 | p99 | mean | max | TPS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `group, pl, b=8` base | 3,435.3 | 5,697.4 | 6,516.3 | 11,411.0 | 16,804.1 | 7,187.3 | 33,879.2 | 521.8 |
| `group, pl, b=8` xe1 | 3,314.0 | 4,618.8 | 5,434.4 | 9,398.0 | 12,973.8 | 5,813.8 | 23,553.5 | 666.1 |
| **Δ** | **−3.53%** | **−18.93%** | **−16.60%** | **−17.64%** | **−22.79%** | **−19.11%** | **−30.48%** | **+27.66%** |

**Noise floor, this host, this shape** (peak-to-peak / median across the 3
repeats): p50 **base 3.11%, xe1 4.69%**; TPS **base 4.70%, xe1 5.67%**.

**Both floors are three to four times tighter than §4.3's** (12.03% /
8.89%) — this host is slower in absolute terms and markedly steadier. That
is worth stating on its own: §4.3's 12% spread was wide enough that the
finding rested on the delta being several times it, and here the same
finding rests on a delta *five times* the floor.

**The wait breakdown.** `syncs_per_commit` **1.560 → 1.240** (−20.5%),
against §4.3's 1.76 → 1.45 (−17.6%). Same direction, same magnitude of
discount, at a lower absolute level on both arms — XD2's batching discount
reproduced on a second machine, and the counter still says what §4.3 said
it says: *the record syncs every time; what moved is when something waits
for it*.

### A3.3 H-XF2, read against its own criterion and against the useful one

**As literally stated, H-XF2 fails.** This host's base p50 median sits
**+13.11%** from §4.3's recorded base median (6,516.3 against 5,761.2),
just outside the 12.0% floor the hypothesis named, and the xe1 median sits
**+27.20%** out (5,434.4 against 4,272.2), well outside 8.9%. Reported
first and plainly, because the hypothesis was written that way.

**It fails for a reason that is not about the engine.** Both arms moved
outward together, and the arm that moved further is the faster one — which
is what a different device looks like, not what a non-reproducing effect
looks like. A hypothesis whose criterion compares absolutes across
machines cannot separate the two, and this addendum says so rather than
scoring it.

**On the criterion that can be evaluated — the within-host A/B — the
finding reproduces, and the mechanism claim stands.** The delta is
**−16.60% of commit p50** and **+27.66% of TPS**, in the same direction as
§4.3's −25.85% / +33.32%, and it clears both arms' own floors by roughly
5×. §4.3's headline claim — *at eight concurrent coordinators the
ack-at-append saving is real and large, reversing H-XE2's prediction that
it would shrink under load* — is confirmed on a second machine, on
independently rebuilt binaries, with the arms interleaved.

**What does not reproduce is the size, and the gap is not small.** 16.6%
against 25.9% is two thirds of the reported effect. Anyone quoting a
number for this change should quote a **range** — *16-26% of commit p50 at
eight concurrent coordinators, device-dependent* — and not either endpoint
as the value. That is this addendum's actual contribution: §4.3 measured
the effect once and could not know which part of it was the device;
two hosts now say most of it is the change and a meaningful part of it is
not.

**One shape in the distribution is worth naming**, because it is where the
two hosts agree most. The saving is **smallest at p0** (−3.5%) and grows
monotonically through the tail (p25 −18.9%, p95 −17.6%, p99 −22.8%, max
−30.5%). A commit that had nothing to wait for saves nothing — there was
no queued drain to leave — and a commit deep in the tail is exactly one
that was queued behind others. That is the same reading §5 offered from
§4.3's numbers, now visible directly in the percentile shape rather than
inferred from the median alone. **It is still not proof**: it remains
consistent with both mechanisms §5 named, and separating them is XF4's
per-leg timers, not this addendum's.

### A3.4 What this addendum does not answer

The b=1 non-finding was not re-run — §6 asked for a repeat of the b=8 pair
and this is that. The `pl` shape XE4 actually asked for, a real scenario-2
booking, is still blocked by §4.1's shipped-read refusal; XF0's survey
(`docs/inflight/blocked/workplan-shipped-read-typed.md`) and its ratification
ask are what stand in front of closing it, and XF2 is the measurement that
reopens when it does. The per-leg timers are still unbuilt, so §A3.3's
percentile-shape reading is still an inference from totals — the fourth
file to say so.
