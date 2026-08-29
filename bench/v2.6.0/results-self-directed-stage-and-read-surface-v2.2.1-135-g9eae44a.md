# RS §7 — what a self-directed stage costs, and the read surface it opens

Work order `docs/inflight/in-progress/workplan-insert-spreading.md` §10-§11
(RS0-RS5): the self-directed stage and the fan-in client on every core are
built and green. This file answers CS1 — what a self-directed run costs
against a real remote hop and against no stage at all — and re-verifies
RS4's read-surface enumeration on this commit. Both cells ran; the four
cells the work order names as blocked by this host's 2-CPU ceiling did
not, and are reported as not run with the reason confirmed rather than
assumed.

**The headline: a self-directed stage is measurably cheaper than a remote
one for equivalent work, and the reason is a cross-reactor wake, not the
protocol.** `A - B` (one remote stage over none) costs **180.5 µs** at
p50; `C - A` (adding a self-directed stage on top of a remote one) is
**-87.6 µs** — negative, because the self-directed stage in arm `C`
carries the bulk of `spread`'s rows and still finishes faster than arm
`A`'s single remote fetch of a smaller relation. A scheduler-counter probe
run alongside the latency measurement (§3.3a) explains why directly: one
remote stage costs **~32 wake events per query** on each side of the hop;
a self-directed stage over the *same* server costs **2**, regardless of
how many rows it walks. §10b's decision to give a self-directed run no
message-free path is, if anything, **better justified** by this number —
the thing that path would save was already small relative to the protocol
overhead every stage pays, and the wake it *does* avoid is most of what
remote costs.

## 1. Provenance

| | |
|---|---|
| Date/time (UTC) | 2026-08-29, 11:09–11:18 |
| Worktree | `spread-relation` (`/home/cdkbs/ckdbs/.claude/worktrees/spread-relation`) |
| Branch | `worktree-spread-relation` |
| Commit measured | `9eae44a2b42092b42d31565aec9c856758f7a6ff` (`git describe --tags` → `v2.2.1-135-g9eae44a`) |
| Tree state at measurement | **Clean** for every run in this file. It went dirty *after* the last measurement — see §1a |
| Binary provenance | `build-release/kds_server` relinked at 2026-08-29 11:02:39 UTC (a 3.4 s incremental build — the `kds` library object files were already current for `9eae44a`; only the final link ran), **before** every run below and before the tree went dirty. Copied to `/home/cdkbs/rs_run/kds_server` and never touched again; `sha256sum` = `52123d2c47cfd1b9ef67aadef8607687326b53306590a3f49fad02ff17c9a986`. Every server in this file started from that copy, per `.claude/agents/ck-tester.md` rule 5 |
| Device | `/home/cdkbs` on `/dev/root`, **ext4** (`df -T`) — a real block device, not tmpfs |
| Build type | `CMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG`, `KDS_WITH_TLS=ON` |
| Host | AMD EPYC 9V74, **2 logical CPUs = 2 physical cores, 1 thread/core** (no SMT; `lscpu` reports `Thread(s) per core: 1`), 1 socket, 1 NUMA node, Microsoft Hyper-V guest, 15 GiB RAM, Ubuntu 24.04.4 LTS, Linux 6.17.0-1022-azure |
| Host quiet | `uptime` load average 0.02–0.33 on a 2-core host throughout; `pgrep cc1plus`/`pgrep cmake` empty before every run |
| Server config (S-a, S-e) | `cores = 2`, `placement = creating` (S-e's `rotate` cell noted separately), `range_size_ids = 512`, `durability = relaxed`, `peer_listeners = on` |
| Suite / sim harness | Not re-run in this session — no engine source changed. **Two different ancestor claims, not to be read as one**: `9eae44a` (RS)'s own message states **suite 3040/3040** in `build-release` (80 s) and lists `scripts/sim.sh` under *"Not done in this commit"*, explicitly not implied green there. `scripts/sim.sh` **171 runs, 0 failures** is `5b62ac3` (R4-R)'s claim, an ancestor of `9eae44a` on this branch, not re-run by RS. Both cited from the commit graph, neither re-measured this session |

### 1a. The tree went dirty mid-session, and the binary predates it

Partway through this run `git status --short` on `spread-relation` began
reporting eight modified files — `bench/parked_coroutine_probe.py`,
`docs/inflight/in-progress/workplan-insert-spreading.md`,
`include/kds/server/core_affinity.hpp`,
`include/kds/server/remote_step_service.hpp`,
`src/server/command_dispatcher.cpp`, `src/server/session_step_client.cpp`,
`tests/core_affinity_test.cpp`, `tests/core_runtime_test.cpp` — with `HEAD`
unchanged at `9eae44a`. This is a concurrent agent editing the same
worktree, exactly the case `.claude/agents/ck-tester.md` warns about.

Checked rather than assumed:

- **`build-release/kds_server`'s mtime never moved** (`stat -c %y`,
  checked before the dirty state appeared, again afterward, again just
  before writing this file — all three read `2026-08-29 11:02:39.449573985
  +0000`). Nobody rebuilt while these edits landed, so the binary this file
  cites — and the copy at `/home/cdkbs/rs_run/kds_server` — reflects
  `9eae44a` exactly and none of the dirty edits.
- **`git diff -- <those eight files>`** is comment and message-text only:
  `remote_step_service.hpp`'s `kMaxFanInUpstreams` comment is reworded
  (no line touches the constant or a code path), `core_affinity.hpp`'s
  `CrossCoreReadUnsupported` doc-comment is reworded, `session_step_client.cpp`
  gains **no new guard lines** (`OnStepBatch`/`OnStepEof`/`OnStepError`'s
  three `if (reads_.empty()) return;` lines from RS0-RS5 are untouched;
  only the comment above `OnStepBatch`'s changed), `command_dispatcher.cpp`'s
  diff against `9eae44a` is entirely inside a comment block above the stage
  loop (the `ServableBy` conditional and the loop body are byte-identical),
  and the two test files change an `EXPECT_*` string literal to match the
  reworded refusal text, not a fixture or an assertion's target. No number
  in this file is affected either way, since the binary predates all of
  it — stated for the record, per the instruction that flagged the
  possibility.

## 2. Confirmed first: the host refuses `cores` above 2

`build-release/kds_server --config <cores=3>` on this host:

```
startup failed: cores 3 exceeds the 2 this machine reports; reactors are
pinned one per core and never block, so overcommitting them serializes
whole workloads behind each other
```

(`src/server/expeditor.cpp`'s `CheckCoreCount`, exit code 1.) So `cores =
3`, `4` and `8` are unavailable on this host for every cell in this file,
not merely noisy — the refusal is `hardware_concurrency() = 2` compared
against the configured value, and no amount of quieting the box changes
it.

## 3. Cell S-a — what a self-directed stage costs (CS1)

### 3.1 Rig

One server, `cores = 2`, `placement = creating`, `range_size_ids = 512`,
`durability = relaxed`, `peer_listeners = on`. Two relations, `id int64, v
int64`, 600 rows each:

- `spread` — 600 rows, round-robin from both cores' sessions (id =
  `2r + core`, `r` = 0..299). `SHOW META`: `split_relation_detail=4000:2@2`
  — two ranges, two stages, confirmed on every run in this file (this
  matches the operator's own earlier verification of the same string
  exactly, at oid 4000).
- `twin` — 600 rows, from core 0's session alone. No entry in
  `split_relation_detail` — never split.

Four arms, `SELECT * FROM <relation>` from a session pinned to a chosen
core (`tools/multicore_benchmark.collect_connections`, the only way to
choose a core under `SO_REUSEPORT`), 300 reps, arms rep-interleaved (one
rep runs `B, A, C, D` in that order before the next rep starts, so drift
lands on all four rather than on whichever ran last — RD9(a)'s standing
rule). New driver: `bench/self_directed_stage_probe.py`, documented at
`bench/docs/README.md` under "The self-directed-stage probe".

| arm | statement | core | stages |
|---|---|---|---|
| B | `SELECT * FROM twin` | 0 | 0 — `ServableBy(0)`, plain local walk |
| A | `SELECT * FROM twin` | 1 | 1, remote |
| C | `SELECT * FROM spread` | 1 | 2 — one remote, one self-directed |
| D | `SELECT * FROM spread` | 0 | 2 — the same two stages, opposite roles |

### 3.2 The numbers

Measured on the copied binary described in §1, three independent runs
(the reported table, plus a same-binary repeat and a build-tree-direct
run for reproducibility — §3.4). All 1,200 queries across the four arms
returned 0 errors.

| arm | relation | core | stages | ops | p0 | p25 | p50 | p90 | p95 | p99 | mean |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| B | twin | 0 | 0 | 300 | 111.0 | 125.0 | **128.8** | 137.8 | 145.1 | 164.3 | 129.5 |
| A | twin | 1 | 1 | 300 | 268.0 | 300.8 | **309.3** | 330.1 | 345.9 | 529.1 | 316.5 |
| C | spread | 1 | 2 | 300 | 207.6 | 218.2 | **221.7** | 234.2 | 238.2 | 282.9 | 224.1 |
| D | spread | 0 | 2 | 300 | 266.6 | 310.1 | **317.7** | 344.9 | 352.6 | 368.4 | 321.5 |

*(all times in µs; p50 in bold, the value the derived rows below use.)*

**Rows in = rows out.** Every one of the 1,200 replies carried exactly 600
data rows (`reply.count("\n")` on the wire's literal row separator — the
protocol packs a `SELECT *` reply as `header\nrow1\nrow2\n...`, one
physical line per `docs/spec/client-manual.md` §2, confirmed by hand
against a scratch server before writing the driver). Reply size was
identical across both relations and every arm: **5,186 bytes** for 600
rows of `(id int64, v int64)`, measured separately (§3.5) since the timed
loop records latency and a row count, not a byte count, per query.

**The correctness check.** `A` and `B` read `twin` over two different
routes (remote fan-in, local walk) and must be byte-identical; so must `C`
and `D`, since the fan-in's concatenation is `lo`-range order and does not
depend on which core opened it (`command_dispatcher.cpp`'s stage-building
comment). Both held on every reply in every run: `A_B_byte_identical =
true`, `C_D_byte_identical = true`, and every one of the 300 `A` replies
(and every one of the 300 `C` replies) hashed identically to itself
(`A_distinct_replies = 1`, `C_distinct_replies = 1` — SHA-256 over the raw
wire reply). A spread relation's remote-plus-self-directed answer is the
same answer a local walk gives the unsplit twin, which is R4-R's
byte-identical claim holding under an actual measurement rather than only
under the equivalence suite's fixtures.

### 3.3 The two differences CS1 asks for

| | p50 (µs) | mean (µs) |
|---|---:|---:|
| **A − B** (remote over none) | **180.5** | 187.0 |
| **C − A** (self-directed over remote) | **-87.6** | -92.4 |
| C − D (same two stages, opposite roles) | -96.0 | -97.4 |

**`C − A` is not close to `A − B`; it is the opposite sign.** Adding a
self-directed stage on top of a remote one does not cost what a second
remote hop would — it *reduces* the total, because the two stages of a
fan-in run concurrently and the statement finishes when the slower one
does (`command_dispatcher.cpp`'s wait: *"Every stage, one park (RD7): ...
One `WaitUntil` over a predicate that ands them rather than k sequential
parks - k parks would serialise on whichever stage the loop happened to
name first, turning a fan-out into a fan-out-then-queue"*). Since `C`'s
self-directed stage does not need a cross-reactor wake (§3.3a), it does
not add to the wall-clock total the way a second remote stage would — it
runs alongside the small remote fetch of core 0's one-row anchor range and
finishes first.

**`C < A` — a two-stage read of `spread` costs less than a one-stage read
of the smaller-looking `twin`.** This is explained, not just observed, by
where the rows actually sit. Traced separately (§3.5): `spread`'s split
happens at `id = 1` — core 1's very first foreign write is refused once
(`SHOW META`'s `cross_core_write_refusal_detail=0>1:4000=1`, exactly one
refusal for the whole 600-row load), the demand it records is granted
almost immediately, and every subsequent grant is a **contiguous top-up**
of the same range row rather than a new one (`command_dispatcher.cpp`'s
own comment: *"a contiguous top-up extends the same core's own run"*). So
`spread`'s anchor range on core 0 holds **one row**, and core 1's range
holds essentially all 599 of the rest — `SHOW META`'s `split_relation_detail`
already reads `4000:2@2` after the very first round of the load (traced
at r = 0, 1, 2, 3, 5, 10, ... 299; it never changes). Arm `C`'s dominant
stage is therefore a **self-directed walk of ~599 rows**, and it beats
arm `A`'s **remote fetch of 600 rows** — a self-directed walk of nearly
the whole relation is cheaper than a remote fetch of the whole relation.

**C − D is a large, real gap, and it is the mechanical mirror of the
paragraph above, not an inconsistency.** `D`'s dominant stage is the
*opposite* pairing — a **remote** fetch of core 1's ~599-row range plus a
**self-directed** walk of core 0's 1-row anchor — so `D`'s total tracks
arm `A`'s remote-hop cost almost exactly (317.7 µs vs. 309.3 µs), while
`C`'s total tracks a self-directed walk of the same size (221.7 µs). The
two arms are "the same two stages in opposite roles" exactly as the rig
says, and what they show is that **which stage is remote and which is
self-directed matters far more than the row split itself** — the
self-directed stage is nearly free per row and the remote one is not.

### 3.3a Measured, not just source-read: where the cost is

A companion probe (`sched_wakes_sent`/`sched_wakes_received` on `SHOW
META`, per-core, before/after 200 queries of one arm in isolation —
archived at `bench/v2.6.0/archive/self-directed-stage-v2.2.1-135-g9eae44a/wake-delta.log`)
turns the p50 story above into a wait that has a name:

| arm | dominant stage | wakes sent / 200 queries (each core) | wakes / query |
|---|---|---:|---:|
| B | none (0 stages) | 0 | 0 |
| A | remote (600 rows) | 6,369 | ~31.8 |
| C | self-directed (~599 rows) + remote (1 row) | 400 | 2.0 |
| D | remote (~599 rows) + self-directed (1 row) | 6,339–6,377 | ~31.7–31.9 |

**A remote stage costs on the order of 32 wake events per query on each
side of the hop, regardless of the small row-count difference between
`twin` and `spread`'s big range; a self-directed stage costs exactly 2 —
one to open, one at EOF — whatever it walks.** That is the fixed cost
`§10b`'s "opened, credited and closed through the same protocol" already
named without a number: a self-directed run still pays the STEP_OPEN/EOF
bookkeeping (2 wakes, matching `D`'s tiny self-directed leg riding beside
its dominant remote one at effectively no extra wake cost — `D`'s wake
count is `A`'s within noise), but it never crosses to a different
reactor's run queue the way a remote stage's producer and consumer do.
`B`'s 0 wakes is the floor: a plain local walk never touches the ring at
all.

This is the direct answer to CS1's question. **A self-send is not "about
as expensive as a real hop" — it is close to free relative to one**,
because what a remote hop costs is overwhelmingly the cross-reactor wake,
not the protocol envelope self-directed and remote share. §10b's decision
not to build a second, message-free path for the self-directed case is,
on this number, **better supported than it looked from the source
argument alone**: the wake a message-free path would additionally avoid
is already avoided by staying on one reactor, and what is left — the
STEP_OPEN/EOF pair — is 2 wakes against a remote stage's ~32, not a
meaningful fraction of what a second protocol spelling would be built to
save.

### 3.4 Reproducibility

Three independent 300-rep runs (Δt a few minutes apart, one against the
build tree's own binary and two against the copied one) agree within
noise:

| run | B p50 | A p50 | C p50 | D p50 | A−B | C−A |
|---|---:|---:|---:|---:|---:|---:|
| build-tree binary, run 1 | 129.7 | 297.0 | 222.8 | 315.6 | 167.3 | -74.2 |
| build-tree binary, run 2 | 130.2 | 298.7 | 222.8 | 323.6 | 168.5 | -75.9 |
| copied binary (reported above) | 128.8 | 309.3 | 221.7 | 317.7 | 180.5 | -87.6 |

The sign and rough magnitude of both differences hold across all three;
the ~10-15% spread between runs is this two-CPU host's own noise floor for
a sub-millisecond query (no dedicated noise-floor control was run beyond
this repeat, standing in for a dedicated noise-floor control — the three
independent full runs serve as one).
None of the three differences crosses zero, which is the only thing the
finding above depends on.

### 3.5 How the row split and reply size were established

Two small instrumented loads (not part of the timed measurement, run
separately against the same binary and rig, JSON/logs not archived — they
answer a structural question, not a latency one, per `.claude/agents/ck-tester.md`
rule 1b's "narrower measurements archive nothing"):

- **Split timing.** `SHOW META`'s `split_relation_detail` was read after
  rounds 0, 1, 2, 3, 5, 10, 20, 50, 100, 150, 200, 250 and 299 of the
  600-row load; it reads `4000:2@2` at every one of them, including the
  first. `cross_core_write_refusal_detail=0>1:4000=1` confirms exactly one
  refusal occurred for the whole load — core 1's very first write into
  `spread` — consistent with `catalog.hpp`'s `PeekRowId`/`NoteRowIdDemand`
  (core 0 has no `RowIdLeaseTable`, so its own `PeekRowId` always answers
  from `sys.tables.next_id` directly and the demand-recording branch is
  simply never reached from the system core) making core 0's own writes
  the ones that keep getting shipped to core 1 once the boundary exists,
  never the ones that open a second boundary.
- **Reply size.** One `SELECT *` per relation per core, outside the timed
  loop: 5,186 bytes for 600 rows on both `spread` and `twin`, and `A`'s
  raw reply string equals `B`'s, `C`'s equals `D`'s, byte for byte.

## 4. Cell S-e — the read surface, re-verified on `9eae44a`

`bench/spread_read_surface.py --cores 2 --range-size-ids 512 --rounds 300`,
re-run independently against the copied binary from §1 (the operator's
own run was against the build tree). Both placements; archived at
`bench/v2.6.0/archive/read-surface-v2.2.1-135-g9eae44a/`.

### 4.1 `--placement creating`

`placed=600`, `split_relation_detail=4000:2@2` — reproduces the operator's
figure exactly. **5 shapes reachable from every core**, **11 refused BY
THE SPLIT**:

| verdict | shapes |
|---|---|
| reachable everywhere (5) | `star`, `star+where-pk`, `star+where-nonpk`, `star+between`, `star+order-pk-asc` |
| refused BY THE SPLIT (11) | `star+order-pk-desc`, `star+order-nonpk`, `projection`, `projection-multi`, `limit`, `limit+offset`, `order+limit`, `count`, `sum`, `count-distinct`, `group-by` |

This confirms the 5/11 split `docs/inflight/known-gaps.md` and `CLAUDE.md`
carry from R4-R (drafted in a parallel session hours before RS, per
`9eae44a`'s own commit message) — the fan-in route in `HandleSelect`
admits a single-step, non-aggregated, unsorted-or-pk-ascending, no
`LIMIT`/`OFFSET`, no sub-chain star read, and refuses everything wider by
the same shape test whether the relation is split or not.

### 4.2 `--placement rotate` — not a valid cell at this core count, and why

`placed=600`, `split_relation_detail=(absent)` — **nothing split**, and
the grid answers `spread` identically to the unsplit `whole` control on
every shape, at every core. This is **not** evidence that the two
placements produce an identical surface; it is evidence that `rotate`
never produced a *split* relation to test at `cores = 2`, so the cell is
reported as not run rather than as confirming anything about placement.

**Why, confirmed from source rather than guessed:**

1. **`rotate` places every relation on core 1 at this core count.**
   `core_placement.hpp`'s `AssignOwnerCore`:
   ```cpp
   if (policy == PlacementPolicy::kRotate && core_count > 1) {
       return kSystemCore + 1 + static_cast<std::uint32_t>(relation_seq % (core_count - 1));
   }
   ```
   At `core_count = 2`, `core_count - 1 = 1`, so `relation_seq % 1 = 0` for
   every relation — the owner is always `kSystemCore + 1 = 1`. `rotate`
   only distributes relations across peer cores when there is more than
   one peer to distribute across; at two cores there is exactly one.
2. **Core 0's foreign writes into that relation never open a second
   range**, and the reason is `catalog.hpp`'s `PeekRowId`/`NoteRowIdDemand`
   pair, not a placement-specific gate. `PeekRowId` is core 0's own
   question "what id would `AllocateRowId` issue next" — and core 0's
   `row_id_leases_` is always null (*"Null (the default) is core 0's
   arrangement and the path that always existed"*), so `PeekRowId` always
   answers from `sys.tables.next_id` directly and **never returns
   `nullopt`**. The only branch that calls `catalog_.NoteRowIdDemand`
   — the call that records a core's demand for a range of its own — is
   guarded by `!target_id.has_value()`, which is unreachable from core 0.
   So core 0 writing into a relation core 1 owns takes the ordinary
   pre-R4 statement-shipping route (`ShipStatement`, since `target_core
   (1) != core_id_ (0)`) every time, and never becomes a second owner.

Both points were checked against `9eae44a`'s actual source (`core_placement.hpp`
lines 94-106, `catalog.hpp` lines 275-303), not inferred from the
`SHOW META` output alone. The operator's reading in the brief — *"`rotate`
never places on core 0... and core 0's foreign INSERTs take a route that
does not leave core 1's relation with a second owner"* — is **confirmed**,
with the second half's precise mechanism being that core 0 structurally
cannot record the row-id demand the spreading pump depends on, rather than
a placement-specific exclusion.

## 5. Not run, and why — each confirmed rather than assumed

### 5.1 S-b — the ceiling re-measured at k = 4 and k = 8

Two independent blockers stop this cell, both confirmed this session:

- **The host refuses `cores > 2`** (§2), so k = 4 and k = 8 cannot start
  at all.
- **Even at `cores = 2` the ceiling is structurally unreachable.**
  `bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md`
  §6's HK4 measured that with exactly one contending peer, IS5's
  top-owner suppression fires on every carve after the first and a
  relation settles at **two ranges forever** (no ceiling reached after two
  million rows there). This run's own S-a load reproduces the same
  structural signature at a much smaller scale: `spread` settles at
  `4000:2@2` after a single round and never grows a third range across
  600 rows (§3.5) — the same suppression, the same two-range ceiling. So
  this cell needs a host with ≥ 3 CPUs and is not a matter of tolerating
  noise; not run.

### 5.2 The kill −9 matrix (12 cells × 3 passes, `txn_2pc_kill_matrix_probe.py`)

**Independently re-confirmed this session**, at `--repeat 1` (12 cells,
not the full 36, since the result does not depend on the repeat count):
`1/12 cells PASS`, the pass being `fastpath.cores1` (D1's one-owner path,
which asks for no peer at all). Every other cell's `s.stderr` reads
exactly the §2 refusal: `startup failed: cores 3 exceeds the 2 this
machine reports; ...`. `1/12` is the same proportion the operator's own
`3/36` reports. **Reported as not run, never as a pass rate** — the probe
needs two relations on two distinct peer cores under `rotate`
(`txn_2pc_kill_matrix_probe.py`'s `write_conf(..., placement='rotate')`,
`--cores` defaulting to 3), and §4.2's `AssignOwnerCore` reading applies
here too: at `core_count = 2` every relation would land on the same
single peer, so even a host that tolerated `cores = 3` in name would still
need the third core to get two distinct owners. Not run.

### 5.3 S-c — a peer's per-`kStepBatch` cost with no fan-in open (CS3, absent vs. zeroed)

Not measured, and not reported as zero. `session_step_client.cpp`'s
`OnStepBatch`/`OnStepEof`/`OnStepError` each return on `reads_.empty()`
before decoding — work removed is one bounds check and a ~24-byte header
`memcpy` per message (source-read, `session_step_client.cpp:82-131` at
`9eae44a`). §3.3a's wake-delta probe shows this host resolves scheduler
counters cleanly at the hundreds-of-events scale, but a single skipped
bounds check inside an already-scheduled handler is well below what a
Python-driven, socket-timed harness on this host can separate from
reactor variance — the same argument `workplan-insert-spreading.md` §11c
makes.
Carried here rather than re-derived.

### 5.4 S-d — RD9(a) re-run, both of CD1's paths

Not run: the operator suspended interleaved A/B per-statement overhead
measurement for v2-stage development, 2026-08-24 (`CLAUDE.md`, Session
Workflow §3). Confirmed from the source this session (§1a): nothing in
`9eae44a`'s own diff against `7eeb7b5` touches the unsplit path — the
`session_step_client.cpp` guards are on a peer's *inbound* step message
only, `command_dispatcher.cpp`'s `ServableBy` conditional and stage loop
are unchanged by `9eae44a` (diffed directly, §1a), and the rest of
`9eae44a`'s diff is comments, a refusal string and tests. So even were the
suspension lifted, this commit is not expected to move an unsplit-relation
overhead number — stated as a source-read expectation, not a measurement.

## 6. Versus PostgreSQL

**No counterpart concept exists, and none is claimed.** A self-directed
stage is an engine-internal routing decision inside `HandleSelect`'s
fan-in over a relation split across this engine's own core-owned page
ranges — PostgreSQL has no analogue to a relation owned by one of several
backend-partitioned execution units the way this engine's cores are
(the same reasoning `bench/v2.5.0/results-r6b-cross-owner-cost-v2.4.0-28-g53f6aae.md`
§10 gives for the cross-owner-transaction line, cited there and here
because it is the same shape of question). §4's read-surface enumeration
is likewise about which shapes a *split* relation answers, which
PostgreSQL relations are not. Nothing here has a PostgreSQL twin to build;
this is stated rather than the section omitted.

## 7. What this run does not measure

- **k > 2 for anything.** Every cell that needs a third core — S-b's
  ceiling, the kill matrix, `rotate` actually distributing two relations
  over two peers — is not run, and the host cannot run it (§2).
- **A message-free self-directed path's actual cost**, since none exists
  to measure. §3.3a's wake-delta number bounds what it would have to beat
  (2 wakes against ~32 for remote), not what it would cost.
- **S-c's per-`kStepBatch` guard cost as a number.** Argued from source,
  stated as below this host's measurement floor, never reported as zero
  (§5.3).
- **Per-statement overhead A/B for `9eae44a`'s diff generally.** Suspended
  by operator amendment since 2026-08-24; §5.4 gives the source-read
  reason to expect no movement on the unsplit path specifically, which is
  not the same as measuring it.
- **Whether the row split at `id = 1` (§3.5) is typical of round-robin
  loading at other `range_size_ids` or row counts.** It is what this
  specific 600-row, 512-block rig produced, traced once; the general
  shape of how early a peer's demand gets granted relative to the load's
  progress is not swept here.
- **Any comparison to a pre-`9eae44a` build of this mechanism.** Every
  number in this file is `9eae44a`'s engine only, per
  `.claude/agents/ck-tester.md` rule 1's "current state only".
