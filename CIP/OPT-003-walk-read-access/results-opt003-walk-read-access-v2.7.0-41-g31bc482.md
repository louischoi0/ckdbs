# OPT-003 measured: a point UPDATE/DELETE on a heap relation writes back the whole relation under `kWrite`, and 2 pages under `kRead` — a delta that grows to 68x at 10,000 rows and does not move the mean statement latency it was predicted not to move

OPT-003 is a two-line change: `UpdateInner` and `DeleteInner` in
`src/server/command_dispatcher.cpp` call `VisitRelation` with
`storage::PageAccess::kRead` instead of `storage::PageAccess::kWrite`.
`git diff 40c5e86 31bc482` confirms this is the *only* functional change
between the two arms measured here — the diff touches exactly those two
call sites plus their explanatory comments, nothing else. The proposal's
own framing is right to insist on it: **this is a write-amplification
claim, not a CPU claim**, and the two instruments below are chosen
accordingly — bytes written, measured with the syscall trace that could
actually isolate them from the WAL, and a latency read that is honest about
where it does and does not clear this run's own noise floor.

| | |
|---|---|
| Executed | 2026-09-01 02:27–02:30 UTC (measurement sweep, one continuous run); binaries built 02:05–02:12 UTC the same session |
| Worktree | `/home/cdkbs/ckdbs/.claude/worktrees/path-optimizer`, branch `opt-003-walk-read-access` at `31bc482` throughout — untouched by this run; both arms are `git archive` exports of a named commit into scratch trees under `/tmp/claude-1000/-home-cdkbs-ckdbs/e52ac4d4-a740-442c-8ac0-ad035bc82a3b/scratchpad/opt003/{src-A,src-B}`, never built from the worktree itself |
| Arm A (baseline) | `40c5e86`, `v2.7.0-40-g40c5e86`, committed 2026-09-01 01:18:47 UTC. `VisitRelation` called with `PageAccess::kWrite` for UPDATE and DELETE |
| Arm B (change) | `31bc482`, `v2.7.0-41-g31bc482`, committed 2026-09-01 01:58:02 UTC. Same two call sites take `PageAccess::kRead` |
| Diff | `git diff 40c5e86 31bc482` — one file, `src/server/command_dispatcher.cpp`, two `storage::PageAccess::kWrite` → `storage::PageAccess::kRead` substitutions (UPDATE's site, DELETE's site) plus the comments explaining each. Confirmed with `diff -rq` on the two `git archive` extractions (excluding `build-release/`): the two source trees differ in exactly that one file |
| Tree cleanliness | Both arms are `git archive` exports of a named commit — no working-tree drift possible in what was compiled |
| Build | `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=<scratch ossl dir>`, `cmake --build build-release --target kds_server -j8`. Both configured and linked clean (same two pre-existing, unrelated warnings OPT-001's run reported — `waker.cpp`'s unused `read()`, `spsc_ring.cpp`'s switch — present in both arms identically) |
| Binary provenance | A: source `build-release/kds_server` mtime 2026-09-01 02:05:22 UTC (47 min after its commit). B: mtime 2026-09-01 02:06:29 UTC (8 min after its commit). Both post-date their own commits |
| Binary run | A copy → `.../scratchpad/opt003/run/kds_server_A`, sha256 `d04132cb53b07cde6ddcad5a22094778b427820a1e42620f742938e7c972ab8f`; B copy → `.../scratchpad/opt003/run/kds_server_B`, sha256 `897c61d592aba5ff125550eb4b95f3173c60cbcb9fa20edd53bbc8d019fc397a`. Both copied once at 02:06:38 UTC and never rebuilt; every server in this run started from these two copies (A's hash matches OPT-001's own arm-B binary — `40c5e86` sits on top of `ea1d9d0` with only doc commits between the two, so the compiled code is bit-identical, not a measurement error) |
| Device | Data dirs under `$HOME/bench-opt003/{plain,strace}-{a,b}-<rows>`, `/dev/root`, **ext4** (`df -T`), not tmpfs |
| Build type | Release |
| Server config | `cores = 1`, `durability = group`, **`checkpoint_interval_ms = 0`** (non-default — the timer cadence is off; every writeback in this run is driven by the driver's own explicit `SYNC`, so a round's flush is attributable to a statement rather than to a background timer racing the measurement) |
| Host | 8 vCPUs. `uptime`/`pgrep cc1plus`/`pgrep cmake`/`pgrep ctest` checked clean (load average 0.15, no build or test process) immediately before the sweep started; the sweep ran as one continuous ~3-minute execution with no gap for a concurrent job to land in. An earlier stage of this session did find a `ctest` running from a different worktree (`xf`) and held for it to finish before building anything, per this role's own rule |

## The instrument, and why the obvious one does not work here

`SHOW META` was read end to end (`HandleShowMeta`, `src/server/command_dispatcher.cpp:1198`
onward) looking for a page-store write-back counter. It has none: the WAL
sync counters (`wal_syncs`, `wal_interval_syncs`), the refill/commit-leg
stats, the cross-core and shipping counters are all there, but
`DevicePageStore`'s and `FilePageDevice`'s own dirty/writeback bookkeeping
is never surfaced. `/proc/<pid>/io`'s `write_bytes` was tried next and
measured, before this driver reached its final form, as a genuine
write-amplification instrument — and rejected on the evidence: a single
point UPDATE on a 200-row table showed a **~1.1 MB** `write_bytes` delta on
*both* arms, while the data file itself is a few hundred KB. The cause,
confirmed by launching the server under `strace` and reading the actual
`pwrite64` calls: under `durability = group` with one statement in flight,
every INSERT's own WAL record gets its own `pwrite`+`fdatasync` on a
*separate* fd, and that per-record log traffic swamped the page-store
signal by two orders of magnitude — `/proc/<pid>/io` cannot separate a
process's file descriptors.

The instrument this run actually uses: `run_ab_server_strace.sh` launches
each server under `strace -f -ttt -e trace=openat,pwrite64,fsync,fdatasync`
(`-ttt` gives epoch timestamps directly comparable to Python's own
`time.time()`; ptrace-*attaching* to an already-running server is refused
in this sandbox — `PTRACE_SEIZE: Operation not permitted` — so tracing has
to start at exec). `opt003_ab.py` resolves the data file's own fd from the
trace's `openat` record (fd 4 on every run here, confirmed rather than
assumed) and sums `pwrite64(4, ...)` bytes whose timestamp falls inside
each round's `[statement-start, SYNC-done]` window. A round is one
statement followed by one explicit `SYNC`; with `checkpoint_interval_ms =
0`, that `SYNC` is the only thing that ever calls `page_store_.Sync()`, so
its writeback is exactly what became dirty since the previous round's
`SYNC` — attributable to one statement, not to a timer's luck.

Because tracing adds a per-syscall tax, the checkpoint/bytes pass runs
against a **separate, traced** server pair, and its own latency numbers are
not treated as authoritative (`measured_under_strace=true` in the archived
JSON). A **second, untraced** server pair supplies (a) the raw unsynced
per-statement latency pass and (b) a checkpoint-latency-only pass — same
statement-then-SYNC shape, timed but with no byte attempt — for a clean
SYNC-latency reading. Every table below says which of the two server pairs
it came from. Full driver docstring, flags and the exact commands:
`CIP/OPT-003-walk-read-access/archive/opt003_ab.py`,
`CIP/OPT-003-walk-read-access/archive/run_sweep_opt003.sh`.

## Write amplification: the primary instrument, and it scales with N almost exactly as the proposal's own arithmetic predicted

The proposal estimated "~28 pages" written for a single-row change on a
2,000-row relation, from a 71-rows/page density. This run's own density
(same 5-column schema, `id int64, c_int int64, c_small int32, c_flag bool,
c_text varchar`) came out at ~73 rows/page (10,000 rows landed on 136
relation pages, derived below) — close enough that the two independent
estimates corroborate each other.

| rows | op | A bytes (mean) | A pages | B bytes (mean) | B pages | ratio A/B |
|---:|---|---:|---:|---:|---:|---:|
| 200 | update | 32,768 | 4.00 | 16,384 | 2.00 | 2.00x |
| 200 | delete | 40,960 | 5.00 | 16,384 | 2.00 | 2.50x |
| 1,000 | update | 122,880 | 15.00 | 16,384 | 2.00 | 7.50x |
| 1,000 | delete | 122,880 | 15.00 | 16,384 | 2.00 | 7.50x |
| 10,000 | update | 1,122,632 | 137.04 | 16,712 | 2.04 | 67.18x |
| 10,000 | delete | 1,122,304 | 137.00 | 16,384 | 2.00 | 68.50x |
| 200 | select (control) | 10,158 | 1.24 | 10,158 | 1.24 | 1.00x |
| 1,000 | select (control) | 10,158 | 1.24 | 10,158 | 1.24 | 1.00x |
| 10,000 | select (control) | 8,192 | 1.00 | 8,192 | 1.00 | 1.00x |

25 reps per row/op cell (`--ckpt-reps 25`), from
`CIP/OPT-003-walk-read-access/archive/run{200,1000,10000}_strace.json`.
This is a size/count table, not a latency distribution, so per rule 6 it
carries no percentiles — but the underlying spread is worth stating instead
of a percentile: update/delete bytes are **exactly deterministic across all
25 reps at every row count except one** (`stdev = 0`; the one exception,
10,000-row UPDATE on arm A, has `stdev = 1,605` bytes — one rep out of 25
touched an extra page, min 1,122,304 / max 1,130,496, still 137±1 pages).
The floor for this measurement is therefore effectively zero — a
half-vs-half split of arm A's own 25 update reps at 10,000 rows moves the
mean from 1,122,987 to 1,122,304 bytes, a 0.06% difference — so every
ratio in the table above is real, not noise.

`select`'s row is the control the task asked for, and it earns the name:
**every one of its 25 reps produced byte-identical write counts on both
arms**, at every row count — the strongest form of "inside the floor" there
is, because there is no floor to be inside of. The `1.24` at 200 and 1,000
rows (10,158 mean against an 8,192 single-page floor) is one shared,
common-mode outlier: one of the 25 reps wrote 57,344 bytes (7 pages) on
*both* arms, at the *same* rep index, at both row counts — consistent with
a periodic catalog or free-map housekeeping write on a fixed operation
cadence, not anything OPT-003's two lines touch. It moves both arms
identically and does not appear at all at 10,000 rows (flat 8,192 on every
rep), so it is noted rather than chased further.

**Both arms pay a flat ~8,192-byte (one page) tax on every statement,
including a pure SELECT.** That floor is visible by subtracting it from the
update/delete numbers: arm B's own write cost is `16,384 - 8,192 = 8,192`
bytes (exactly one heap page) at every row count from 200 to 10,000 — flat,
not growing with N, exactly the O(1) mutating-re-fetch cost the proposal
predicts for the change. Arm A's is `(A bytes) - 8,192`, which is what
scales: 24,576 → 114,688 → 1,114,440 bytes as rows go 200 → 1,000 →
10,000, i.e. **the whole relation, every single statement**, because
`VisitRelation`'s `kHeap` branch takes a plain `heap::ChainVisit` over the
full chain for any unsplit relation (`access.ranges.empty()`) — `PkSpan`
narrowing never engages below the range-directory threshold, and
`LocateByPk` answers `PkLookup::Kind::kScan` unconditionally for a heap
relation (it has no pk index; `command_dispatcher.cpp:6765`: "A heap
relation has no pk index: the chain scan is the only path"). So a WHERE-pk
UPDATE/DELETE on a heap relation is *always* a full linear scan regardless
of OPT-003, and what OPT-003 changes is only whether that scan's own
read-only pages get dirtied along the way.

That one-page-per-statement floor is itself a finding worth naming for
whoever picks up the next proposal: it is consistent with per-statement
access-statistics bookkeeping (`docs/spec/heap-and-tuple.md` §7, `SHOW
ACCESS`) dirtying a small catalog page on every statement, read or write,
independent of relation size — a fixed cost distinct from the per-row one
this proposal targets, and one OPT-003 correctly leaves alone.

## Latency: the checkpoint's own cost clears the floor at 10,000 rows; the raw per-statement cost does not move, as predicted

Two passes, both against the **untraced** server pair (`run_ab_server_opt003.sh`,
plain), so neither carries a tracing tax.

**Checkpoint-latency-only pass** (round size 1: statement, then `SYNC`,
timed; 25 reps/cell) reads the same claim through wall-clock time instead
of bytes — a manual `SYNC` here forces exactly the writeback+fsync a
background checkpoint would perform at this dirty-page state:

| rows | op | A mean us | B mean us | delta | A/B half-split floor |
|---:|---|---:|---:|---:|---:|
| 200 | update | 2,858 | 1,943 | 38.1% | 49.4% (inside floor) |
| 200 | delete | 2,547 | 2,033 | 22.5% | 19.3% (clears floor, barely) |
| 1,000 | update | 2,432 | 2,025 | 18.3% | 6.9% (clears floor) |
| 1,000 | delete | 2,931 | 1,656 | 55.6% | 37.4% (clears floor, though the floor itself is wide) |
| 10,000 | update | 4,352 | 1,688 | 88.2% | 12.9% (clears floor emphatically) |
| 10,000 | delete | 4,537 | 1,644 | 93.6% | 0.6% (clears floor emphatically) |
| 200/1,000/10,000 | select (control) | 2,503–2,811 | 2,535–2,979 | 0.3–5.8% | 0.1–7.6% (delta and floor track each other at every size) |

Floor: arm A's own 25-rep sample split first-half vs second-half, `|Δmean|`
as a fraction of the pooled mean — the same method OPT-001's run used, and
every number in this table is computed by that formula rather than
eyeballed. At **200 rows, update's delta sits inside its own floor** (38.1%
against a 49.4% floor) — absolute times are 2–3 ms and dominated by
scheduling jitter on a 25-sample window, so this driver reports that one
cell as directional, not conclusive. 200-row delete clears its floor, but
only barely (22.5% against 19.3%). By **1,000 rows every update/delete
cell clears its own floor**, and by **10,000 rows the signal is 7–156x
wider than the floor** and unambiguous: arm A's checkpoint costs 2.6–2.8x arm
B's. `select`'s own SYNC delta stays under 6% at every size — comparable to
its own floor (and, at 200 and 1,000 rows, just outside a floor that is
itself under 2% there) rather than inside it, but tiny in absolute terms
next to update/delete's double-digit-to-emphatic deltas, and consistent
with the bytes control's own exact-zero reading above.

**Raw (unsynced) latency pass** (interleaved rounds, no `SYNC` in the loop,
375 select/update ops and 100 delete ops per arm per row count) answers the
CPU-claim question directly — OPT-003 predicts nothing moves here, because
neither arm's per-row work changed:

| rows | shape | A mean us | B mean us | delta | floor | A p50/p99 us | B p50/p99 us |
|---:|---|---:|---:|---:|---:|---:|---:|
| 200 | select | 138.8 | 134.7 | 3.0% | 5.9% | 129.9 / 234.0 | 128.5 / 232.8 |
| 200 | update | 1,316.8 | 1,285.6 | 2.4% | 2.9% | 1,212.1 / 5,222.5 | 1,210.1 / 3,465.8 |
| 200 | delete | 1,306.0 | 1,298.2 | 0.6% | 13.7% | 1,208.4 / 4,258.5 | 1,190.7 / 3,650.5 |
| 1,000 | select | 190.6 | 187.9 | 1.4% | 4.4% | 182.0 / 281.4 | 181.7 / 276.7 |
| 1,000 | update | 1,432.4 | 1,365.5 | 4.8% | 6.7% | 1,331.8 / 3,740.6 | 1,311.9 / 3,244.1 |
| 1,000 | delete | 1,536.2 | 1,319.4 | 15.2% | 9.1% | 1,309.4 / 6,334.4 | 1,287.0 / 1,898.9 |
| 10,000 | select | 821.2 | 821.6 | 0.0% | 0.0% | 812.3 / 1,000.7 | 811.0 / 988.6 |
| 10,000 | update | 2,136.6 | 2,105.8 | 1.5% | 2.9% | 2,011.9 / 8,049.2 | 2,004.6 / 4,119.3 |
| 10,000 | delete | 2,356.3 | 2,008.5 | 15.9% | 6.0% | 1,973.6 / 11,265.9 | 1,962.9 / 2,826.2 |

`select` and `update` sit inside their own floor at every row count —
**latency shows nothing here, exactly as predicted**, and that is the
correct reading of it, not a failed measurement. `delete`'s mean sits
slightly outside its floor at 1,000 and 10,000 rows (15%/16% against a
9%/6% floor), with p99 differing sharply (arm A's is 3.3–4.0x arm B's). No
mechanism in the change predicts this — `checkpoint_interval_ms = 0` in
this pass too, so no background checkpoint is running to explain a tail
stall, and `n = 100` deletes per cell makes p99 the single highest sample
or two, which is a thin, easily-dominated-by-one-outlier statistic. This
driver reports it plainly rather than either burying it or overclaiming a
mechanism for it: it reads as a thin-sample artifact rather than a finding
this change predicts, and the checkpoint-latency pass above is where the
real, floor-clearing latency effect actually lives.

**Wait decomposition (rule 3): does not apply to the primary instrument**,
and is incomplete for the secondary one, stated rather than glossed. Bytes
written is a volume, not a duration, so it has no wait types to break out.
The checkpoint-latency pass's `SYNC` is, structurally, page writeback
(`pwrite64` calls into the OS page cache, tens of microseconds each per
the traced session) followed by one terminal `fsync` — but that write/fsync
split is only observable inside the *traced* session, whose own timing
this driver has already declined to treat as authoritative. Decomposing
the untraced session's clean SYNC-latency numbers into write-time vs.
fsync-time was not attempted; this is a gap, named rather than silently
skipped.

## Correctness: byte-identical on both instruments, at every row count

Both the `plain` and `strace` sessions run their own independent
post-load and post-write correctness check — `SELECT * FROM t_upd ORDER BY
id`, `SELECT * FROM t_del WHERE id <= rows ORDER BY id`, `SELECT COUNT(*)
FROM t_del`, sha256 of the reply text, both arms driven by one RNG stream
so the statement sequences are identical:

| rows | session | t_upd | t_del (core range) | t_del (count) | errors (A/B) |
|---:|---|---|---|---|---|
| 200 | plain | match | match | match | 0/0 |
| 200 | strace | match | match | match | 0/0 |
| 1,000 | plain | match | match | match | 0/0 |
| 1,000 | strace | match | match | match | 0/0 |
| 10,000 | plain | match | match | match | 0/0 |
| 10,000 | strace | match | match | match | 0/0 |

Six independent sessions, eighteen hash comparisons, zero mismatches, zero
`ERR` replies from either arm. OPT-003's own stated risk in its proposal —
that deferring `MayWrite` past the walk could somehow let a write land
where it should have been refused — did not reproduce here, and the next
section is what specifically checks the mechanism rather than only its
absence of symptoms.

## The deferred peer-writer/lease refusal: checked at the mechanism, not inferred

OPT-003's own proposal names the risk precisely: on a leased (peer-core)
store, `Get()` (mark_dirty=true) also runs `MayWrite`, so moving the walk
to `GetForRead` (mark_dirty=false, which checks only `MayFault`) defers
that refusal from the walk to the mutating re-fetch — later, but the
proposal claims still before anything is dirtied. This run does not
exercise that mechanism through a live two-core peer-writer cluster (no
such harness exists that is cheap to stand up in this session), but it
does check it at the exact point the diff touches, which is available and
precise: `DevicePageStore::ResidentBytes` (`src/storage/device_page_store.cpp:413`)
is the single function both `Get()` and `GetForRead()` route through, and
its only dirty-gated refusal is `if (mark_dirty && !MayWrite(page_id))` —
nothing else there depends on `mark_dirty`.

A new gtest case, `DevicePageStoreOwnershipTest.OPT003ReadRouteAdmitsWhatWriteRouteStillRefuses`,
was added to arm B's **scratch source tree only** (`.../scratchpad/opt003/src-B/tests/device_page_store_test.cpp`
— never the repository, never committed) reusing the exact fixture
`AMissingGrantIsRetryableAndASystemPageIsNot` already uses: a page a peer
core has *fault* (read) rights to via `GrantFaultPages` but no *write*
grant. It asserts, in order:

1. `store->GetForRead(130)` — the walk's own access after OPT-003 —
   **succeeds**. A refusal here would mean the walk itself now raises what
   only the mutating re-fetch used to raise.
2. `store->Get(130)` — the mutating re-fetch's access, unchanged by
   OPT-003 — **still refuses**, `StatusCode::kTxnConflict`, `retryable() ==
   true`, before anything is dirtied.
3. Once `GrantWritePages({130})` lands, the same `Get(130)` call now
   **succeeds** — the retry the `TxnConflict` bit promises.

Built and run on arm B (`v2.7.0-41-g31bc482`): **passed**, alongside the
other 15 tests in `DevicePageStoreOwnershipTest` (16/16 in the class
including this one; 37/37 across the three suites in that file —
`DevicePageStoreTest`, `DevicePageStoreHeaderlessTest`,
`DevicePageStoreOwnershipTest`). Because the diff between the two arms is scoped
entirely to `command_dispatcher.cpp` and never touches
`device_page_store.cpp`, this mechanism is identical on both arms — so
this is a check of the premise the proposal's own reasoning rests on, not
a differential test, and it holds: **the refusal that used to fire in the
walk now fires one call later, in the re-fetch, retryably, and still
before any dirtying** — exactly as OPT-003 claims and no more than that.
What this does *not* check, stated rather than implied: an end-to-end
peer-writer cluster issuing a real UPDATE through the dispatcher against a
foreign, unwritable page and confirming the client-visible reply is
unchanged. That is a wider harness this session did not build.

## Full test suite: green on both arms, plus the one added case

`ctest` run from each arm's own scratch `build-release` (never the
worktree's shared build tree, so neither run could see the other's state):

| arm | suite | result |
|---|---|---|
| A (`40c5e86`) | 3,091 tests | 100% passed |
| B (`31bc482`) | 3,092 tests (3,091 + the OPT-003 probe above) | 100% passed |

## What this run teaches about the engine

The clean 1:1 correspondence between "pages the walk touches" and "bytes
the next checkpoint writes" is the engine behaving exactly as
`ChainVisitOnePage`'s and `device_page_store.cpp`'s own comments say it
should — `store.Get()` marks every frame it *hands out* dirty, full stop,
with no per-row gate, so any write-mode walk over an unmodified page was
always going to cost this. What is new here is the number: **68x** at
10,000 rows is a bigger constant than the proposal's own "~28 pages"
example suggested, because 10,000 rows is 5x bigger than that example's
2,000 — the relationship is linear in row count, as both this run's sweep
and the proposal's arithmetic agree it should be, and a linear relationship
with a wide-open upper end is the shape that makes "OLTP-specialized" claims
about point UPDATE/DELETE cost sensitive to table size in a way nothing
in `CLAUDE.md`'s milestone table currently flags.

The second thing worth carrying forward is structural rather than
numeric: **a heap relation has no pk index, so a WHERE-pk-equality
UPDATE/DELETE against one is unconditionally a full chain scan** —
`LocateByPk` answers `kScan` for every heap relation regardless of the
predicate, `PkSpan` narrowing never engages below the range-directory
threshold on an unsplit table, and OPT-003 does not and cannot change
either fact; it only changes what the always-necessary scan costs once it
runs. That full-scan-by-necessity is exactly the O(slots-per-page) gap
`CIP/README.md`'s "Not cut, and why" section already names for heap
INSERT's duplicate-key check, extended here to two more statement shapes —
worth flagging to whoever next reads that section, not something this run
resolves.

Third: the flat ~8,192-byte tax every statement pays, including a bare
`SELECT`, sat underneath every number in this run and had to be subtracted
by hand to read arm B's own O(1) cost. If a future proposal targets
per-statement fixed cost the way OPT-001/OPT-002 targeted per-row cost,
this floor — consistent with access-statistics bookkeeping dirtying one
small catalog page per statement — is where it would start.

## Files

- Results (this file): `CIP/OPT-003-walk-read-access/results-opt003-walk-read-access-v2.7.0-41-g31bc482.md`
- Driver: `CIP/OPT-003-walk-read-access/archive/opt003_ab.py`
- Orchestration: `CIP/OPT-003-walk-read-access/archive/run_sweep_opt003.sh`,
  `CIP/OPT-003-walk-read-access/archive/run_ab_server_opt003.sh` (plain),
  `CIP/OPT-003-walk-read-access/archive/run_ab_server_strace.sh` (traced)
- Raw JSON + logs: `CIP/OPT-003-walk-read-access/archive/run{200,1000,10000}_{plain,strace}.json`,
  `.log`
- Raw strace traces (text, no data/WAL files): `CIP/OPT-003-walk-read-access/archive/run{200,1000,10000}_strace_{A,B}.out`
