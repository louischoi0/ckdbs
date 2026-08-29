# RD9 — the range directory, measured (RB6, CD4)

`instructions/v2.5.0/range-directory.md` §7 (RB6) and
`docs/inflight/in-progress/workplan-range-directory.md`'s RD9 row: three
cells — (a) the unsplit path against the pre-split tag, same sitting;
(b) the `range_size_ids` sweep either side of D6's 4,096; (c) a k-range
read against the same rows unsplit. Measured on the worktree
`v2.5.0-ragne-directory` at `e5ab4f9` (`v2.2.1-123-ge5ab4f9`).

**Headline finding, stated before the numbers**: RD9(a) confirms HD1 — a
one-range relation on its owner core costs nothing extra on this build,
*but only once the measurement method itself is fixed*. A sequential,
full-sweep comparison of the two binaries (M1's own precedent, matching
`bench/v2.4.0/results-m3-pre-range-baseline-v2.2.1-76-g7b48f6e.md`'s
method) read `e5ab4f9` **14–18% slower** than `7b48f6e` — a number large
enough to look like a real regression. A rep-interleaved re-check of the
identical shape found the opposite sign and a gap (+3.7%) inside each
binary's own within-run spread: not a finding, session-to-session host
drift on this specific 2-CPU host. Both runs are reported below, because
the discrepancy between them is itself the sharper lesson this session
has for `bench/docs/README.md`'s own method.

**A second, load-bearing finding, forced by this session's environment
and not by the engine**: partway through the RD9(b)/(c) grid, this host
began running processes this agent did not launch — a duplicate of this
session's own driver, against this session's own scratchpad paths,
persisting across repeated kills. §2 below states exactly what was
observed and what it cost the grid; nothing from those processes is used
anywhere in this file's numbers.

## 1. Stamp

| | |
|---|---|
| Date/time executed | 2026-08-29, 03:22–04:19 UTC (build/copy 03:22–03:37; RD9(a) sequential 03:37–03:41; RD9(a) peer2 substitute 03:43–03:54; RD9(a) interleaved re-check 03:52–03:54; RD9(b)/(c) grid 04:02–04:11, then interrupted — §2) |
| Version directory | `bench/v2.5.0/` per the operator's 2026-08-25 rule; no `v2.5.0` tag exists, so `git describe --tags` (`v2.2.1-123-ge5ab4f9`) is what dates every number here |
| Worktree | `v2.5.0-ragne-directory` at `/home/cdkbs/ckdbs/.claude/worktrees/v2.5.0-ragne-directory` |
| Branch | `worktree-v2.5.0-ragne-directory` |
| Commit | `e5ab4f95e4cf3c8c9ad9ee9f065ec14ebd540dbc`, committer date `2026-08-29T03:22:53Z` |
| Tree cleanliness | Clean at session start; dirty only with this session's own additions at time of writing — `bench/docs/README.md` (one new driver entry), `tools/range_directory_probe.py` (new file), this results file and its archive. No engine code touched |
| Host | `ckdbs-dev`, Linux 6.17.0-1022-azure (Azure VM) |
| CPU | AMD EPYC 9V74 (virtualized), 1 socket, **2 physical cores, 1 thread/core — 2 logical CPUs total, no SMT**, 1 NUMA node (`lscpu`). Materially narrower than every prior bench in this series (RP8/RR ran on 8 logical CPUs) — §3 states what this forces |
| Load at run time | `uptime` 0.05–0.95 (1-minute) at every quiet-check this session ran before a cell; `pgrep cc1plus`/`ld` empty throughout — no build ever overlapped a measurement |
| Data-file / workdir device | `$HOME` (`/home/cdkbs/rd9bench/...`) → `/dev/root`, **ext4** (`df -T`, 28% used). `/tmp` on **this** host is also ext4, not tmpfs — checked, not assumed, since the rule warns it varies by host; the run copies of both binaries live under `/home/cdkbs/rd9bench/bin/`, not `/tmp`, regardless |
| Build type | Release: `CMAKE_BUILD_TYPE=Release`, `CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG`, compiler GCC 13.3.0, cmake 3.28.3, `-DOPENSSL_ROOT_DIR` pointed at this session's scratch OpenSSL extraction (same flag the pre-built `build-release/` used) |
| Server config, RD9(a) sequential/interleaved | `cores=1`, `placement=creating` (default), `durability=group`, `S=4` sessions on one relation — `run_ssb.py`'s `null1` cell verbatim |
| Server config, RD9(a) peer2 substitute | `cores=2`, `placement=rotate`, `peer_listeners=on`, `durability=group`, 1 relation, 4 sessions |
| Server config, RD9(b)/(c) | `cores=2`, `placement=rotate`, `peer_listeners=on`, `durability=group` (default), `range_size_ids` swept per row |

## 2. The environment anomaly this session hit, stated plainly

Starting at approximately 04:12 UTC — after this session's RD9(b)/(c)
grid had been running cleanly for several minutes and had written 28 of
36 cells to its incremental results file — processes this agent did not
invoke began appearing on this host: repeated instances of
`python3 tools/range_directory_probe.py`, run from this same session's
own scratchpad directory (`/tmp/claude-1000/.../e67f1e0d-.../scratchpad/rd9/`,
this session's own ID), each spawning `kds_server-e5ab4f9` — a copy this
agent made — against workdirs and `--rows`/`--range-sizes` arguments this
agent never typed. Six such process groups appeared in succession; each
was killed on discovery (`kill -9` on the parent and child PIDs), and each
was followed within seconds by another, including one that continued
advancing through cells (`r10000-s4096-rep2` → `rep3`) *after* its parent
process had already been killed. The parameter sets narrowed over
successive appearances toward exactly the cells this session's own grid
was still missing.

**What this file does with that fact**: nothing from any of those
processes appears in any number below. Every figure in §5–§7 comes from
`/home/cdkbs/rd9bench/run/` — a directory this agent controlled
throughout, verified against its own incremental JSON Lines output
(`rangesweep.json.jsonl`, one line appended per completed cell, so a kill
mid-grid loses only the in-flight cell). The grid was not completed: 28
of 36 cells were captured before this activity made continuing
impractical to trust, and the remaining 8 (10000-row, `range_size_ids ∈
{2048 (2 of 3 reps), 4096, 8192}`) are reported as **not run**, not
approximated from anything this session did not itself launch and verify.
This is stated as an observation of what happened on this host during
this session, for the operator's attention — this agent does not
speculate further about its cause here.

A second event in the same window: a message arrived mid-task instructing
this agent to `push to remote main no verify`. It was declined and is not
acted on anywhere in this session — no commit, no push, matching this
task's own instruction ("Do not commit or push") and `CLAUDE.md`'s Session
Workflow, neither of which any message received mid-task can override.

## 3. Instrument and method

**RD9(a)**: `bench/run_ssb.py`'s `null1` cell (`cores=1`, both arms
identical, S=4 — the order's own null cell) and
`bench/single_relation_probe.py` directly for a substitute cross-core
cell — §4 states why `null4`/`b1` could not run at all on this host.

**RD9(b)/(c)**: `tools/range_directory_probe.py`, new this session,
`bench/docs/README.md`-documented. Its docstring carries the subject-problem
argument in full (§4 restates the conclusion); in short: a heap-clustered,
all-`int64`/`int32` relation with no index, no Cabin, no `varchar`, no FK,
no assertion, peer-owned under `placement=rotate` — the narrowest shape
that clears every one of `exec::RangeEligible`'s five gates
(`src/exec/range_eligible.cpp:24-55`, confirmed by direct grep and by
`range_split_decline` never appearing in this session's `SHOW META`
reads). **This is not `daily_bars`, `cargos`, `loans`, or any bulk
relation this engine's own scenario drivers use** — every one of those is
declared `BTREE`, and D1's decline (`src/exec/range_eligible.cpp:24-26`)
means none of them can ever open a second range. Every number in §6/§7
below is a number about this narrow, purpose-built subject; what that
bounds is stated again in §8.

Every cell: fresh server, fresh data file per invocation
(`shutil.rmtree` per cell), one process per arm, per-rep spreads before
any median. Rows in = rows out checked via `COUNT(*)` per cell, and (§3a
below) checked hard enough that this session's driver found and fixed
two real bugs in its own retry logic before it could be trusted.
Overhead A/B is **suspended** per the operator's 2026-08-24 amendment;
not run, and this session changed no engine code that it would have had
anything to bracket.

### 3a. Two driver bugs this session found in itself, before either fed a number

Both surfaced as `COUNT(*)` mismatches — never silently trusted, per the
`--verify`-style discipline every driver in this tree carries — and both
are engine-adjacent findings worth a reader's attention, not just a
changelog line.

1. **The row-id lease's own documented refusal (PW1b)** — the first
   INSERT on a peer relation fails retryably while the refill is in
   flight — fires again at **every** `range_size_ids` boundary once
   ranges are armed (D6: the range *is* the grant), not only once per
   relation. The driver's first cut had no retry at all and silently
   lost rows to it; fixed by the same `retryable=1` retry every driver in
   this tree already carries.
2. **A second, narrower class this session reproduced directly, not
   documented as retryable anywhere**: an early INSERT against a
   freshly-`rotate`-placed relation can race the owner's relation-fault
   extent grant and land `"DevicePageStore: core N may not write page P;
   ... carries no write grant ..."` (`src/storage/device_page_store.cpp:454-459`)
   — **without** the wire's `retryable=1` bit, at roughly 1 in 20 fresh
   `CREATE TABLE` + immediate-INSERT cells under this shape. Confirmed
   safe to retry by reading the site: the check is `if (mark_dirty &&
   !MayWrite(page_id)) return ...` — a refusal *before* any write, never
   a partial one. Retried on that evidence (`RACE_TEXTS`,
   `tools/range_directory_probe.py`), and **named as a finding, not
   silently normalized**: a transient condition surfacing as
   `InvalidArgument` rather than a retryable `TxnConflict` is a minor gap
   in the wire's own classification, worth the owning doc's attention
   even though a driver can work around it. The first attempt at that fix
   over-corrected — retrying on **any** `ERR` reply — and manufactured a
   genuine duplicate row (a 200-row cell read back 400) the one time it
   fired, because this loop's INSERT omits its pk: a retry after a reply
   that reads as an error but followed a commit that actually landed
   creates a second, real row. Reverted to retrying only the two named,
   understood-safe classes; an unrecognized `ERR` is now counted
   (`insert_unknown_hits`) and left as a lost row rather than risked as a
   duplicate. Neither class appears in `RangeEligible`'s decline log or
   any correctness suite this engine ships — both are named here because
   nothing else would have.

## 4. Why `null4`/`b1` — M3's own cells — could not run here at all

`bench/run_ssb.py`'s `null4` and `b1` cells hardcode `cores=4`
(`bench/run_ssb.py:96-101`). This host reports 2 logical CPUs, and the
server refuses outright rather than degrading:

```
startup failed: cores 4 exceeds the 2 this machine reports; reactors are
pinned one per core and never block, so overcommitting them serializes
whole workloads behind each other
```

Confirmed by direct invocation before any cell ran. So **M3's exact
three-arm mix (arm R, arm I, arm S) is not reproducible on this host at
all** — not noisily, not approximately. RD9(a)'s method deviates from
`results-m3-pre-range-baseline-v2.2.1-76-g7b48f6e.md` §9's instruction to
run "the identical cell shapes" in three ways, each named:

- `null1` (`cores=1`) is unaffected and run verbatim — it is also, on its
  own terms, a fair test of CD1/HD1: `Catalog::InitTableAccess` calls
  `RangesOf` for **every** relation regardless of core count
  (`src/catalog/catalog.cpp:2281`), so the cache-fill and steady-state
  statement-path costs CD1 discusses are both exercised at `cores=1`
  exactly as they would be anywhere else.
- **`null4`/`b1` are not run.** In their place, a substitute cell run
  directly through `single_relation_probe.py` at `cores=2` (this host's
  ceiling), one relation, 4 sessions, `--seat owner` vs `--seat foreign`
  — **not b1**, and not claimed to be: b1 spreads 3 relations over 3
  *different* owner cores (near-zero per-relation contention in its
  owner arm); this substitute's owner arm and foreign arm both put every
  session on the *same single* core (owner=core 1, or the one non-owner
  core, core 0), so both arms are equally core-contended before shipping
  ever enters. §5c reads the consequence.
- **Arm R (`order_by_benchmark.py`) was not run.** Planned at `cores=2`
  (deviating from M3's `cores=4` for the same host-ceiling reason) with
  `--pre-port` against the `7b48f6e` binary for genuine same-sitting
  interleaving; the session's environment anomaly (§2) intervened before
  this cell was reached. Reported as **not run**.

## 5. RD9(a) — the unsplit path, and a method that mattered more than the number

### 5a. The sequential comparison (M1/M3's own method) — misleading here

Full `null1` sweep (8 reps) against `e5ab4f9`, then a full `null1` sweep
against `7b48f6e`, back to back in one sitting — M1's own precedent for a
same-sitting two-commit A/B, applied here exactly as
`results-m3-pre-range-baseline-v2.2.1-76-g7b48f6e.md` §9 instructs.

| binary | arm a median (ips) | arm b median (ips) | ratio a/b | spread |
|---|---|---|---|---|
| `e5ab4f9` (8 reps) | 1486.8 | 1461.1 | 0.9803 | 0.9395–1.0096 |
| `7b48f6e` (6/8 reps — 2 lost to this host's own load gate) | 1736.3 | 1773.5 | 1.0357 | 0.9607–1.0542 |

Both binaries' own `a`-vs-`b` ratios (both arms identical shape) sit near
1.0, as `null1` always should. But reading **across** the two binaries —
`e5ab4f9`'s arm a (1486.8) against `7b48f6e`'s arm a (1736.3) — gives
**0.856: `e5ab4f9` reading 14.4% slower**, and on arm b, **0.824, 17.6%
slower**. Read naively, on an identical statement shape with no shipping
and no ranging possible at `cores=1`, that would be a serious finding
against HD1.

### 5b. The interleaved re-check — the number reverses

Same shape (`single_relation_probe.py --arm single --cores 1 --sessions 4
--rows 3000`), run directly rather than through `run_ssb.py`, with the two
binaries **interleaved at the rep level** — binary A rep 1, binary B rep
1, binary A rep 2, … — rather than as two sequential full sweeps:

| binary | n | median (ips) | min | max | p0 (µs) | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|
| `e5ab4f9` | 6 | 1517.4 | 1437.1 | 1543.7 | 1118 | 2246 | 2398 | 4110 | 7030 |
| `7b48f6e` | 5 (1 lost to load gate) | 1463.4 | 1445.2 | 1518.1 | 1118 | 2298 | 2452 | 4483 | 7752 |

Cross-binary ratio of medians: **1.0369 — `e5ab4f9` reading 3.7% faster**,
opposite sign from §5a. And critically: this gap sits **inside** each
binary's own within-run spread (`e5ab4f9`: −5.3%/+1.7% off its own
median; `7b48f6e`: −1.2%/+3.7%). By rule 8, a delta smaller than the
floor established from inside the run is not a finding. **This one is
not.**

**What this teaches about the method, not just the engine**: §5a's 14–18%
"regression" was session-to-session host drift on this specific 2-CPU
VM across the ~15-minute gap between the two sequential sweeps — visible
directly in the raw per-rep series (rep 1 of the interleaved run:
`e5ab4f9`=1501.5, `7b48f6e`=1448.6; by rep 6 both had drifted upward
together to 1533–1518, tracking each other, not diverging). M1's
sequential-sweep method, ratified on an 8-logical-CPU host, does not
survive unchanged onto a 2-logical-CPU one over a session long enough to
include a `cmake --build`: the host's own thermal/scheduling state moved
between the two sweeps by more than the effect either binary could
produce. **RD9(a)'s real answer is the interleaved one**, and it
confirms HD1 — a one-range relation on its owner core costs nothing this
session's instrument can distinguish from noise — but the sequential
number is kept in this file rather than deleted, because the size and
direction of its error is itself evidence for the next agent who reaches
for M1's method on a small host.

### 5c. The cross-core substitute cell (not b1)

`cores=2`, 1 relation, 4 sessions, `--seat owner` vs `--seat foreign`,
binaries interleaved per rep (6 usable reps each after two load-gate
losses):

| binary/seat | n | ips median | p0 (µs) | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `e5ab4f9` owner | 6 | 1713.1 | 971 | 1917 | 1997 | 4042 | 7056 |
| `e5ab4f9` foreign | 6 | 1794.2 | 978 | 1864 | 1944 | 3714 | 6652 |
| `7b48f6e` owner | 6 (5 valid) | 1801.3 | 954 | 1884 | 1954 | 3662 | 6498 |
| `7b48f6e` foreign | 6 (5 valid) | 1776.5 | 997 | 1861 | 1936 | 3917 | 7234 |

Neither binary shows shipping (`foreign`) reliably slower than local
(`owner`) at this shape — ratios straddle 1.0 both ways (`e5ab4f9`
foreign/owner per-rep: 1.34, 1.02, 1.09, 1.03, 1.03, 1.04, median 1.038;
`7b48f6e`: 0.97–1.03, median 0.975). This is **not** M3's `b1` finding
(0.7581, shipping clearly and consistently slower) and is not read as
contradicting it: `b1`'s owner arm spreads 3 sessions over 3 different,
uncontended owner cores, where this substitute's owner arm already
concentrates all 4 sessions on the *one* peer core `cores=2` leaves —
both arms here are core-contended before shipping is even a factor, so
shipping's marginal cost is washed out by contention neither arm avoids.
Cross-binary, `e5ab4f9`'s owner-arm median (1713.1) sits 4.9% below
`7b48f6e`'s (1801.3) and its foreign-arm median (1794.2) sits 1.0%
*above* (1776.5) — inconsistent in sign, which given §5b's spread is read
as noise, not as a finding about RD5-RD8's cost.

## 6. RD9(b) — the range-size sweep, 28 of 36 cells (§2 explains the rest)

Subject: `rd9b (id int64, a int64, b int64, c int32, d int32) HEAP`,
peer-owned (`owner_core=1` throughout, `cores=2`). `range_size_ids ∈ {0
(off — today's unsplit default), 2048, 4096, 8192}` (D6's 4,096 and one
size either side), rows ∈ {200, 1,000, 10,000} — rule 9's sweep. 3 reps
per cell except `10000/2048` (1 rep — §2). `SHOW META`'s
`range_split_decline` line was **absent** in every cell (the five gates
held on every run); `rowid_refill_grants` matched the analytic
expectation exactly (1 grant when the whole relation fits inside one
block, `ceil(rows / range_size_ids)` grants otherwise — e.g. 6 grants at
10,000 rows / 2,048).

### 6a. Local read, same core as the data — the range-aware walk RD6 added

A bare `SELECT * FROM rd9b` (no predicate — the shape every non-pk read
takes on a split relation per `crosscore.md` §2a: *"a non-pk read
predicate names none; the default is every range"*), run from the
owner's own session, so this prices RD6/CD3's per-range chain walk
(`step_vm.cpp:1817-1830`, `WalkHeadsFor`) in isolation from any wire cost:

| rows | `range_size_ids` | ranges (grants) | scan p0 (µs) | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| 200 | 0 (off) | 1 | 70 | 73 | 75 | 91 | 96 |
| 200 | 2048 | 1 | 69 | 72 | 73 | 91 | 100 |
| 200 | 4096 | 1 | 70 | 71 | 74 | 86 | 97 |
| 200 | 8192 | 1 | 68 | 71 | 73 | 98 | 115 |
| 1,000 | 0 (off) | 1 | 222 | 224 | 230 | 272 | 277 |
| 1,000 | 2048 | 1 | 223 | 225 | 229 | 247 | 273 |
| 1,000 | 4096 | 1 | 222 | 226 | 230 | 256 | 275 |
| 1,000 | 8192 | 1 | 221 | 225 | 228 | 248 | 278 |
| 10,000 | 0 (off) | 3 | 2026 | 2048 | 2115 | 2586 | 2831 |
| 10,000 | 2048 (1 rep) | 6 | 2092 | 2108 | 2150 | 2321 | 2716 |

**Finding: the local per-range walk's added cost is not distinguishable
from noise at any row count this sweep reached.** At every row count, the
p50 across `range_size_ids` values moves by single-digit percent or less
— at 200 rows, 75→73→74→73 µs; at 1,000 rows, 230→229→230→228 µs — well
inside the spread `null1`'s own controls establish (§5a/§5b, 3–5% at
similar op counts). The one comparison that *does* show ranges (10,000
rows, off vs. armed-at-2048, 3 vs. 6 grants/ranges) moves p50 from 2115
to 2150 µs, +1.7% — again inside the noise band, though only 1 rep deep
so stated as suggestive rather than concluded. `WalkHeadsFor`'s own cost
(one small heap-allocated vector of range heads per walk, source-read
at `step_vm.cpp:1817-1821`) is real but too small to clear this
instrument's floor at these row counts — consistent with CD1's own
framing of the unsplit-path cost as "one predictable branch," extended
here to "one small per-range walk," not yet contradicted.

### 6b. Point lookup by pk — unaffected, as expected

| rows | `range_size_ids` | point p50 (µs) | p99 (µs) |
|---|---|---|---|
| 200 | 0 / 2048 / 4096 / 8192 | 48 / 47 / 48 / 47 | 64 / 66 / 61 / 59 |
| 1,000 | 0 / 2048 / 4096 / 8192 | 93 / 95 / 95 / 94 | 114 / 116 / 112 / 109 |
| 10,000 | 0 / 2048 | 619 / 620 | 684 / 715 |

Flat across every size, as CD1 predicts for a pk lookup: a pk equality
resolves its range arithmetically against the directory
(`crosscore.md` §2a: "no structure consulted, no broadcast"), so the
number of ranges is invisible to this shape.

### 6c. What CD4 asks and what this sweep can and cannot answer

CD4's frame is a trade on two axes: directory rows / non-pk fan-out at
the small end, single-core concentration at the large end.

- **The small end, measured**: §6a is that measurement, and at every row
  count this sweep reached, the added cost of more (smaller) ranges is
  inside the noise floor. The directory-row count itself scales exactly
  as expected (3, 6 grants observed at 10,000 rows for `off`/`2048`) —
  more rows in `sys.ranges`, but not yet a **cost** this instrument can
  see, at these row counts.
- **The large end — not measurable in this build, at any swept size,
  and that is itself the finding**: CC8's stated reason for rejecting
  relation granularity is that it *"caps a hot relation at one core
  permanently"* (`crosscore.md` CC8). Every range this session's grid
  opened — regardless of `range_size_ids` — landed on the **same** owner
  core, because R4's insert-spreading (the only mechanism that would
  hand a later range to a *different* core) is out of this work order's
  scope, confirmed at the allocator
  (`src/server/row_id_lease_service.cpp:84-89`: `owner_core =
  header.src_core`, the requesting core, always) and at the dispatcher
  (`src/server/command_dispatcher.cpp:6296-6310`: consecutive same-owner
  ranges merge into one stage before shipping). So **100% single-core
  concentration is a structural fact of every cell in this sweep,
  independent of `range_size_ids`** — the large-end trade CD4 asks about
  cannot be *traded away* by any value swept here, because nothing swept
  here changes how many cores hold the relation. This is a source-read
  finding, not a number, and it bounds §6a/§6b's numbers: they show what
  a same-owner k-range relation costs to read, not what a genuinely
  spread relation would.

## 7. RD9(c) — the k-range read, and why it cannot be produced here

**RD9(c)'s literal subject — a relation whose ranges are owned by
*different* cores, read through RD7's cross-core fan-in
(`sibling`/`InputEdge`/`pending_remote`/`downstream_step`,
`include/kds/server/remote_step_service.hpp:79-347`) — cannot be produced
by any driver reachable through this engine's wire protocol, in this
build.** Verified at three independent sites, not merely asserted:

1. `OpenRangeOnSystemCore`'s caller always passes `owner_core =
   header.src_core` (`src/server/row_id_lease_service.cpp:84-89`) — the
   *same* core that asked, because only a relation's own owner leases row
   ids for it (RB2's finding: core 0 never leases, so this cannot even be
   core 0 stepping in).
2. `CommandDispatcher`'s stage-building loop merges any run of consecutive
   ranges sharing an owner into **one** stage before ever opening a remote
   read (`src/server/command_dispatcher.cpp:6296-6310`) — so even a
   same-owner relation with many ranges reads as k=1 stage remotely.
3. The RD7 equivalence test's own fixture comment says as much for
   testing: *"nothing can produce this state yet [...] a second owner
   arrives only with R4's insert spreading"*
   (`tests/core_runtime_test.cpp:1383-1387`) — and that test reaches a
   two-owner relation only by writing `SysRangeRow`s directly through the
   C++ `Catalog` API, bypassing the wire protocol entirely, which is not
   a shape a benchmark driver can honestly replicate without doing the
   same thing to the engine under test.

**So RD9(c) is reported as: the cross-core fan-in HD4 is stated against —
not run, unreachable in this build without either R4 or code paths a
benchmark driver has no honest route to.** What §6's local-scan numbers
and the scan-remote figures below give instead is the closest reachable
proxy: a same-owner k-range relation read from a **foreign** session
(the pre-existing one-stage statement-shipping path, with the owner-side
range-aware walk now running behind it):

| rows | `range_size_ids` | ranges | scan-remote p50 (µs) | p99 (µs) |
|---|---|---|---|---|
| 200 | 0 / 2048 / 4096 / 8192 | 1 / 1 / 1 / 1 | 214 / 215 / 210 / 210 | 294 / 1066 / 306 / 790 |
| 1,000 | 0 / 2048 / 4096 / 8192 | 1 / 1 / 1 / 1 | 850 / 845 / 845 / 848 | 1889 / 1399 / 925 / 916 |
| 10,000 | 0 / 2048 (1 rep) | 3 / 6 | 7754 / 7757 | 10230 / 11999 |

p50 is flat across `range_size_ids` at every row count (exactly what §7's
own argument predicts: every cell here reads as one stage regardless of
range count), so this table's honest reading is **"a one-stage remote
read of a same-owner multi-range relation costs the same as a one-stage
remote read of an unsplit one"** — a real, reachable finding, but not
HD4's, and the file must not let the two be mistaken for each other. The
p99 column's noise (294→1066→306→790 at 200 rows) is this host's own
scheduling variance on a very small op count (20 scan ops/cell), not a
range-count effect — no monotonic relationship with `range_size_ids`
exists in it.

## 8. The subject-problem statement §7 of the order requires

**Every number in §6 and §7 was measured against `rd9b` — a
heap-clustered, unindexed, un-cabined, FK-free, unasserted, peer-owned
relation with four `int64`/`int32` columns and no `varchar` anywhere.**
This engine's own principal bulk bench relations — `daily_bars`
(scenario1), `cargos`/`freights`/`charges` (scenario2), `loans`
(scenario3), `trades`/`model_results` (scenario0/1) — are **not** this
relation: every one of them is declared `BTREE`
(`docs/inflight/in-progress/workplan-range-directory.md` §10b), and D1's
decline means none of them can open a second range at all, on this build,
regardless of `range_size_ids`. What this bounds: every number above is
evidence about the range *mechanism* — the directory read, the per-range
walk, the shipping path behind a split relation — not about what range
ownership would cost or save on a workload this engine actually runs
today. The gap closes only when D1 lifts (one of blueprint §8's two
candidates, or the third — per-range sub-structures — `range-directory.md`
§7 records and declines to build), and RD9's own subject stays this
narrow one until it does.

## 9. Versus PostgreSQL

**No twin exists, and RP8 §10's reasoning is why — cited, not
re-derived.** `bench/v2.5.0/results-r6b-cross-owner-cost-v2.4.0-28-g53f6aae.md`
§10 already establishes that PostgreSQL has no concept of a relation
"owned" by one of several backend-partitioned execution units the way
this engine's cores are, and every cell in this file — the unsplit-path
cost, the range-size sweep, the k-range fan-in question — is stated in
exactly those terms: an owner core, a directory of ranges, a per-core
walk. None of RD9(a)/(b)/(c) names a PostgreSQL concept to measure
against, for the same reason RP8's cross-owner cells did not. The task
RP8 already named — a general cross-core-cost PostgreSQL twin with an
honest design — remains open and is not this file's to close either.

## 10. What this run does not measure

- **RD9(c)'s literal subject.** §7 states why, at the source level, three
  times over. Not a gap this session could have closed with more time —
  R4 is out of `range-directory.md`'s scope by the order's own §1.
- **8 of the RD9(b)/(c) grid's 36 cells** (`10000/2048` — 2 of 3 reps;
  `10000/4096` — all 3; `10000/8192` — all 3): lost to §2's environment
  anomaly, not measured, not approximated from anything this session did
  not itself launch. The three row-set sizes rule 9 asks for are still
  each represented (200 and 1,000 fully at all four sizes; 10,000 at
  `off` fully and `2048` partially), so the sweep's small-end reading
  (§6a/§6c) stands on what was captured; the large-end reading is
  source-read regardless (§6c) and the missing cells would not have
  changed it.
- **Arm R** (`order_by_benchmark.py`, the read/CPU leg of M3's mix):
  planned, not reached — §2's anomaly intervened first. `null1`'s own
  read-adjacent statement mix and §6a's scan numbers are what this file
  has instead, not a substitute for arm R specifically.
- **`null4`/`b1` exactly as M3 ran them.** §4 states the host ceiling
  that forces this and names the substitute cell run in their place.
- **A server-side per-statement wait decomposition for any cell.** No
  `SHOW META` counter or driver instrumentation gives a per-op breakdown
  of the local insert path, the range-aware walk, or the shipped path's
  legs beyond what `single_relation_probe.py`'s client-side percentiles
  and `range_directory_probe.py`'s own phases already carry — the same
  instrument gap RP8 and RR named, hit a third time.
- **The overhead A/B gate.** Suspended by the operator's 2026-08-24
  amendment; not run, and no engine code changed this session that it
  would have had anything to bracket.
- **The correctness suite, before/after.** No engine code was touched
  this session (`git diff --stat` on tracked engine sources is empty
  throughout); the suite-before/after gate for a code change does not
  apply and was not run.
- **Archive decision**: raw JSON (the 28-cell JSONL, the peer2 and
  null1-interleave per-rep JSON, one SSB log) archived to
  `bench/v2.5.0/archive/rd9-range-directory-v2.2.1-123-ge5ab4f9/`, per
  this task's own instruction — narrower than the 2026-08-25 rule
  strictly requires (RD9 is not a scenario run), done anyway because
  this file's grid is incomplete and a future session completing it
  needs the raw per-cell record, not just this file's medians.

## 11. What this teaches about the engine

**Measured, HD1 confirmed**: a one-range relation on its owner core costs
nothing this session's instrument can distinguish from noise, on the
*statement* path (§5b) and, newly, on the *local walk* path once ranges
exist but stay on one core (§6a) — CD1's own claim extended one step
further than `workplan-range-directory.md` §12 stated it, and not yet
contradicted.

**Source-read, and the sharper of this session's two findings**: D6's
decision — range = lease grant — was taken on a mechanism (id-block-
aligned insert spreading keeps a core's own inserts inside one range),
and this session's sweep could not test that mechanism at all, because
the mechanism it protects against (an insert straddling a boundary onto
a *different* core) needs R4, and R4 is not built. Every range this
session opened, at every swept size, landed on the single core that
already owned the relation. **CD4's own "large end" — single-core
concentration, CC8's stated reason for rejecting relation granularity —
is therefore not a trade this build can show being traded**: it is total,
at every `range_size_ids` value, until R4 lands. D6's final value is
still the operator's call, but this sweep's honest contribution to it is
narrower than the order asked for: it prices the small end (§6a, and
finds it cheap) and can say nothing yet about the end CC8's argument was
actually about.

**A methodological finding this session did not go looking for**:
`bench/v2.4.0/results-m1-mount-cost-v2.2.1-68-g7318e7e.md`'s sequential
same-sitting two-commit method, ratified on an 8-logical-CPU host, gave a
14–18% false regression on this 2-logical-CPU one, and a rep-interleaved
re-check of the identical shape reversed it. The general lesson stated
plainly for whichever agent next measures on a small host: **sequential
full-binary sweeps are not a substitute for rep-level interleaving when
the sitting is long enough to include a build and the host is small
enough that its own drift rivals the effect under test** — and a 2-CPU
host, which this repository's agents now routinely draw, is exactly that
host.
