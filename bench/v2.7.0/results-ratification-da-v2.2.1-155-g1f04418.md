# Ratification DA-b/DA-c, and what DA1's default costs where it can't help

`instructions/v2.7.0/ratification-da.md` asks two things of this run: is a
255-stage fan-in affordable in session-core *state* (DA-b), and does the
16.7M-row read-ceiling arithmetic hold under the new `range_size_ids =
65,536` / `kMaxFanInUpstreams = 255` pair (DA-c). The order also asked for
an empirical check of a premise before either question is answered.

**The premise holds, confirmed twice, directly from the engine's own
instrument.** On this 2-CPU host, `cores` cannot exceed 2, and a relation
can therefore never carry more than **2 ranges**: `expeditor.cpp` gives a
`CoreRuntime` — the only thing that ever asks core 0 to open a range — to
cores `1..cores-1`, which is exactly one core at `cores = 2`, and
`OpenRangeOnSystemCore`'s top-owner suppression then holds that one peer's
ranges at its first boundary forever. §1 below drives 100,000 inserts
through that one peer at `range_size_ids = 256` (390 lease blocks' worth,
had suppression not fired) and reads `SHOW META`'s `split_relation_detail`
directly: **2 ranges, 2 stages, unmoved**, both times. **DA-b's and DA-c's
production forms are therefore not runnable here**, exactly as the task
predicted, and this file says what could be measured instead and what it
does and does not cover — never a number this session did not measure.

What follows: §1 the two-range confirmation, §2 DA-b's state-cost
microbenchmark (in isolation from the two-range limit, and what it does
not cover), §3 DA-c stated as unrunnable with the reason, §4 the one thing
DA1's flip should cost at `cores = 1` (nothing) measured as an A/B across
three row-set sizes, §5 the read surface re-confirmed unchanged under the
armed default, §6 correctness (suite + sim), §7 what this run does not
answer.

## 0. Provenance

| | |
|---|---|
| Date/time (UTC) | 2026-08-31, 01:30–02:10 |
| Worktree | `v2.7.0-ratification-da` (`/home/cdkbs/ckdbs/.claude/worktrees/v2.7.0-ratification-da`) |
| Branch | `worktree-v2.7.0-ratification-da` |
| Commit measured | `1f04418` (`git describe --tags` → **`v2.2.1-155-g1f04418`**) |
| Tree state | **Dirty with this session's own additions only**: `CMakeLists.txt` (+10 lines, one new `add_executable` block), `bench/session_step_state_bench.cpp` (new, DA-b's microbenchmark), `bench/v2.7.0/` (this file and its archive). No engine source (`src/`, `include/`) touched — confirmed by `git status --short` before writing this file |
| Binary provenance | `build-release/kds_server`, linked 2026-08-31 01:24:20 UTC. That **predates** HEAD's commit timestamp (01:28:14) — checked per rule 4 — but **not** HEAD's source: `git log -- src include` shows HEAD's own edits landed 01:09–01:13 (`git show --stat HEAD` touches `range_alloc.hpp`, `remote_step_service.hpp`, `expeditor.cpp` etc.), and `expeditor.cpp.o`'s object-file mtime is 01:24:01 — after those edits, before the commit. The binary was built and tested locally, then committed with no further source edits; content matches HEAD. Copied to `/home/cdkbs/bench-v2.7.0-da/run/kds_server` before the first cell (rule 5) and never touched again; `sha256sum` = `4e06a773cd7e07e71eb163a52635fbd10c8f25873d8008846d231c5589e6c222` |
| Device | `/home/cdkbs` on `/dev/root`, **ext4** (`df -T`) — checked, not assumed; `/tmp` is on the same ext4 mount on this host today |
| Build type | `CMAKE_BUILD_TYPE=Release`, configured with `-DOPENSSL_ROOT_DIR=<scratchpad>/ossl` per this session's environment note |
| Host | AMD EPYC 9V74, **2 logical CPUs = 2 physical cores, 1 thread/core** (no SMT), 1 socket, 1 NUMA node, Hyper-V guest, 15 GiB RAM, Ubuntu 24.04.4 LTS, Linux 6.17.0-1022-azure |
| Host quiet | Checked before every timed cell with `uptime` and `pgrep -af 'cc1plus\|cmake --build\|kds_tests\|ctest'`; a second worktree (`err-not-implemented`) ran its own build+ctest cycle repeatedly through this session and every timed cell here waited it out first (§4's load-average detail is in that section) |
| Suite / sim | Debug `build/` suite: **3018 tests green at this HEAD**, established before this session (task's own stated fact, not re-derived here). Release `build-release/tests/kds_tests`, run this session after the CMake addition: **3018/3018 passed** (83.6s). `scripts/sim.sh`: **190 runs, 0 failures** — §6 |
| Raw output | `bench/v2.7.0/archive/da-two-range-check-v2.2.1-155-g1f04418/`, `da-b-state-cost-v2.2.1-155-g1f04418/`, `da-cores1-ab-v2.2.1-155-g1f04418/`, `da-read-surface-v2.2.1-155-g1f04418/`, `da-sim-v2.2.1-155-g1f04418/` |

## 1. The two-range ceiling, confirmed empirically

**Method.** `cores = 2`, `placement = creating`, `peer_listeners = on`,
`range_size_ids = 256` (deliberately small — 100,000 rows is ~390 lease
blocks' worth, so if IS5's suppression did *not* hold, dozens of new range
boundaries would show up well inside this row count). `CREATE TABLE` and
one seed row from the core-0 connection, then every other row —
**100,000 of them** — inserted one at a time from the one connection the
kernel placed on core 1 (found by opening connections and asking each
`SHOW META` until one answers `core=1`, `SO_REUSEPORT`'s only way to pick
a core). `SHOW META`'s `split_relation_detail=<oid>:<ranges>@<stages>`
read from the core-0 connection afterward — the engine's own count, not
an estimate (`command_dispatcher.cpp`'s comment on that field: *"R4-M
could measure the count only where the fan-in refusal names it… and had
to estimate it… wrong by construction wherever IS5's suppression
fires"*). Run twice, independently, on a quiet host both times.

| run | rows attempted from the peer | `split_relation_detail` | ranges | stages | transient refusals | rows placed |
|---|---:|---|---:|---:|---:|---:|
| 1 | 100,000 | `4000:2@2` | **2** | **2** | 3 | 99,998 |
| 2 | 100,000 | `4000:2@2` | **2** | **2** | 2 | 99,999 |

**The relation never exceeded two ranges, in either run.** `4000` is the
relation's oid (not a row count — `command_dispatcher.cpp`'s own key is
`oid:ranges@stages`), and it repeats because the bootstrap catalog assigns
the same first user oid on a fresh file both times. The 2–3 transient
refusals are the documented one-time cost — *"the first INSERT into a
relation on a given peer fails retryably… no later one does"*
(`core_runtime.cpp`) — plus one further race of the same shape later in
the stream; my driver did not retry them, which is why 99,998/99,999
placed rather than 100,000, and is a driver choice, not an engine defect.

This directly confirms, from the engine's own instrument rather than an
inference, what the task's derivation and R4-M's HK4 both said: **at
`cores = 2` under `creating`, exactly one peer ever asks to open a range,
and once it owns the top range every further lease refill lands inside
that same range** (`range_alloc.cpp`'s comment: *"the block lands in the
top range either way… so when this core already owns the top range there
is nothing to open and nothing lost by not opening it"*). Raw driver
output: `bench/v2.7.0/archive/da-two-range-check-v2.2.1-155-g1f04418/`.

**Consequence for DA-b and DA-c.** A live 255-stage fan-in needs 255
distinct range owners contending for one relation (or 255 ranges under
one owner, which the same suppression rules out even harder). Two
ranges is the ceiling this host can produce in the production write
path, full stop. §2 and §3 are what is left to measure honestly.

## 2. DA-b — session-core state, priced in isolation from the range limit

**What is and is not being measured, stated before the numbers.**
`SessionStepClient::reads_` (`session_step_client.hpp`) is a
`std::vector<RemoteRead>` keyed by `PipelineTag{request_id, session_core,
step_id}`. A tag is minted once per `Open()` call **regardless of how
many distinct ranges the stages address** — so N loopback reads against
one relation, opened with N distinct `request_id`s, build exactly the
vector a 255-stage fan-in would, without needing 255 range owners to
exist. `bench/session_step_state_bench.cpp` does this: a `RemoteStepServer`
on core 1 and a `SessionStepClient` on core 0, wired the same synchronous
loopback `tests/session_step_client_test.cpp`'s fixture uses (no reactor,
no ring, no transport — the same isolation `bench/crosscore_pipeline_bench.cpp`
already uses for the same reason: a peer-owned relation cannot be written
or fanned into over the wire on this host in the first place). It prices
the two O(N²) candidates the order named by name:

1. **The park predicate.** `command_dispatcher.cpp`'s `finished` lambda —
   the predicate `co_await sched::WaitUntil{&finished}` polls on every
   reactor tick while a fan-in statement is parked — calls
   `SessionStepClient::Find` once per tag, and `Find` is a linear scan of
   `reads_`. Priced here as **one full poll**: N `Find` calls in the
   statement's own tag order.
2. **Teardown.** `FinishRemoteReads`'s `CloseAll` destructor calls
   `SessionStepClient::Close` once per tag, each a linear scan plus an
   erase-shift. Priced here as **one `CloseAll`**: N `Close` calls in the
   same order.

Not covered by this harness, and said plainly rather than folded into a
number that looks more complete than it is: the wire, the ring, and
backpressure (`kInitialCreditsPerEdge` against `kCoreRingSlots`, the
order's candidate #3) — all three need a real multi-core ring this host
cannot build past two ranges (§1). §7 names what a k ≥ 3 host would need
to measure instead.

### 2a. The numbers

Median of 21 independent rebuild-open-poll-close cycles per N (a fresh
`Bed` each rep, so no state leaks between reps); `poll_us`/`close_us` are
per-*single* poll or per-*single* teardown, not summed over the reps.

| N | open_us | open_us/tag | poll_us | poll_us/tag | close_us | close_us/tag |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 4.05 | 4.0460 | 0.003 | 0.0027 | 0.23 | 0.2300 |
| 2 | 6.17 | 3.0845 | 0.005 | 0.0023 | 0.32 | 0.1605 |
| 4 | 10.51 | 2.6265 | 0.009 | 0.0023 | 0.45 | 0.1128 |
| 8 | 20.63 | 2.5789 | 0.022 | 0.0027 | 0.79 | 0.0989 |
| 16 | 40.81 | 2.5507 | 0.066 | 0.0041 | 1.62 | 0.1014 |
| 32 | 81.99 | 2.5623 | 0.209 | 0.0065 | 5.08 | 0.1587 |
| 64 (old ceiling) | 166.01 | 2.5939 | 0.718 | 0.0112 | 17.94 | 0.2803 |
| 128 | 334.45 | 2.6129 | 2.954 | 0.0231 | 68.44 | 0.5347 |
| 200 | 531.49 | 2.6574 | 6.929 | 0.0346 | 165.40 | 0.8270 |
| **255 (new ceiling)** | 689.87 | 2.7054 | **10.900** | 0.0427 | **268.28** | 1.0521 |

*(This is a fitted per-operation cost, not a statement's QPS/TPS — rule
5a's own carve-out for a shape with no throughput form: there is no
"poll per second" a client experiences, only a per-poll and a one-time
teardown cost paid on the session core. Labelled in microseconds
throughout rather than inverted into a rate that does not exist.)*

**Percentiles over the 21 reps** (rule 6; n=21 per cell — thin by that
rule's own standard for a p99, which collapses to the max at this sample
size, said plainly rather than dressed up):

| N | poll p0 | poll p25 | poll p50 | poll p95 | poll p99 | close p0 | close p25 | close p50 | close p95 | close p99 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 0.003 | 0.003 | 0.003 | 0.003 | 0.004 | 0.14 | 0.19 | 0.23 | 0.44 | 0.60 |
| 2 | 0.005 | 0.005 | 0.005 | 0.005 | 0.005 | 0.19 | 0.30 | 0.32 | 0.53 | 0.57 |
| 4 | 0.009 | 0.009 | 0.009 | 0.009 | 0.009 | 0.31 | 0.42 | 0.45 | 0.74 | 0.95 |
| 8 | 0.022 | 0.022 | 0.022 | 0.022 | 0.022 | 0.62 | 0.72 | 0.79 | 1.11 | 1.20 |
| 16 | 0.066 | 0.066 | 0.066 | 0.066 | 0.075 | 1.46 | 1.57 | 1.62 | 1.79 | 2.54 |
| 32 | 0.207 | 0.209 | 0.209 | 0.211 | 0.211 | 4.93 | 5.03 | 5.08 | 5.36 | 5.46 |
| 64 | 0.697 | 0.711 | 0.718 | 0.726 | 0.761 | 17.72 | 17.86 | 17.94 | 18.56 | 25.79 |
| 128 | 2.721 | 2.785 | 2.954 | 2.985 | 3.021 | 68.02 | 68.25 | 68.44 | 76.34 | 77.68 |
| 200 | 6.646 | 6.679 | 6.929 | 6.980 | 7.268 | 164.63 | 165.13 | 165.40 | 172.01 | 175.48 |
| 255 | 10.763 | 10.884 | 10.900 | 11.910 | 13.027 | 267.21 | 267.90 | 268.28 | 277.46 | 278.44 |

Full driver output (both `-fdatasync`-quiet reps and this reproduction):
`bench/v2.7.0/archive/da-b-state-cost-v2.2.1-155-g1f04418/`.

### 2b. Reading it: superlinear, confirmed — and small enough not to matter here

**The scaling check says both mechanisms are genuinely superlinear, not
just "big-O on paper."** Consecutive-N ratios climb toward the quadratic
prediction as fixed overhead stops dominating: `poll_ratio` at N: 64→128
is 4.11 against a predicted 4.00, and `close_ratio` at 32→64 is 3.53
against 4.00 — both essentially exact quadratic scaling once N is large
enough that constant-factor overhead is a rounding error. At small N
(1→2, 2→4) the ratios sit below 4.00 because a fixed per-call cost (the
loop's own overhead, cache effects) still matters at that scale — the
same shape any O(N²) algorithm shows before N dominates.

**And the absolute magnitude at N = 255 is trivial.** One park-predicate
poll costs **10.9 µs**; one full teardown costs **268 µs**, paid once per
statement. Both numbers are two to three orders of magnitude below what
this engine's own numbers say a real network hop or a durability wait
costs: CLAUDE.md's cross-core milestone row states shipping's flat wire
cost at *"~20 µs… once the reactor-wake fix removed the idle-block
penalty"* for **one** hop, and `bench/v2.1.0/results-multicore-writers-v2.1.0.md`
measured this class of device's single-stream `fdatasync` **latency at
~0.94 ms** (from its independently measured 1,066/s single-stream rate) —
the wait a `group`-durability commit pays on the critical path. A 255-stage fan-in pays
the 10.9 µs poll cost **once per reactor tick it is parked**, and even a
pessimistic few hundred ticks before a slow remote leg answers would sum
to single-digit milliseconds — still under one synchronous `fsync`.

**So DA-b's answer is not "affordable because it's actually linear" — it
is not linear — but "affordable because O(N²) of a few-nanosecond unit is
still small at N = 255."** The order's instruction was *"if the state
cost is superlinear, report and stop"*; both mechanisms are, confirmed
directly rather than assumed, and reported above. What stops here is the
inference from "superlinear" to "a problem": at this N the practical
answer is that state is not what would make 255 stages unaffordable — the
range limit in §1 is what makes them unbuildable on this host, which is a
different and unrelated ceiling. A future host that can actually reach
N = 255 in production should still watch the `close_us` column, because
it is the term growing fastest in absolute terms (268 µs and rising) and
the one this microbenchmark cannot bound past 255 without exceeding
`kMaxFanInUpstreams` itself.

**What is left open, honestly.** The order's candidate #3 — credit
(`kInitialCreditsPerEdge = 4`) against ring capacity (`kCoreRingSlots =
256`) self-throttling near 64 stages per peer — needs real batches in
flight over a real ring, which needs the range limit in §1 not to hold.
**Not measured here, and not measurable on this host**; a k ≥ 3 writer-
core host is what would answer it (§7). Candidate #4 — unbounded session
memory holding every stage's raw batches — is a function of reply *size*
more than stage *count*, and `bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md`
§6c already found reply bytes dominate stage count there; not re-measured
in this run.

## 3. DA-c — the read-ceiling cell, unrunnable here, and why precisely

DA1 × DA3's arithmetic is 255 × 65,536 ≈ 16.7M rows for the refusal
boundary `bench/spread_ceiling_probe.py` would need to find. §1 already
shows the precondition for *any* refusal boundary above 2 ranges does not
hold on this host: the harness cannot make the relation split a third
time, let alone 255 times, because only one peer core exists and IS5
suppresses every further boundary it would open. There is no row count at
which `spread_ceiling_probe.py` would report a refusal here — it would
run to `kIdSpaceEnd` on two ranges and never see one, which is exactly
what the prior line's HK4 already found at the smaller sizes (*"at k = 2
the ceiling is not reached after two million rows at any size"*,
`bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md`
§6) and what §1 above reconfirms directly at the new default.

**Not attempted, and not a hole this file apologises for**: running
`spread_ceiling_probe.py` for however many hours it would take to place
16.7M rows on two permanently-open ranges would measure the harness's own
throughput ceiling (`bench/spread_client_ceiling.py`'s subject, §4b of the
cited doc), not the fan-in's. **A k ≥ 3 host is the precondition for this
cell**, named plainly rather than guessed at (§7).

## 4. `cores = 1` unmoved by the armed default — an A/B across three row-set sizes

DA1 flips `range_size_ids`'s *default* from off to 65,536. R4-M measured
`cores = 1` unmoved with spreading armed at a fixed `range_size_ids =
4096` and one row count
(`bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md`
§4a, HK5: 0.981 group / 1.002 relaxed at 4,000 rows/core). This section
is the same shape at the value DA1 actually ships (65,536) and swept
across the three row-set sizes rule 9 asks for, using
`bench/spread_insert_probe.py`'s own arms — `C-concentrated`
(`range_size_ids = 0`) against `S-spread` (`65,536`) — at `cores = 1`,
where the engine omits `peer_listeners` entirely (*"has no peer to
listen"*) and there is no `CoreRuntime`, no lease table, and no range to
open on either arm. Both arms are therefore the same server binary
running the same code path; any measured delta is host noise, and the
spread across cells **is** this shape's noise floor (rule 8) rather than
a finding to explain away.

Fresh server and data file per arm, three interleaved reps per cell
(RD9(a)'s rule), medians reported. Host quiet checked before each row-set
size; a second worktree's own build+ctest cycle interrupted this run
twice (load average 1.23 and then 2.75–2.08 for several minutes) and both
row-set-200/1000 cells completed before either interruption (the driver's
own `check_host` gate, load > 1.0 on 2 cores, refused to start otherwise);
the 10,000-row cell was blocked by the gate once and re-run after a
confirmed-quiet wait — the wait and the retry are in the raw log.

| rows/core | durability | concentrated ips | spread ips | **S/C** | reps |
|---:|---|---:|---:|---:|---:|
| 200 | group | 808 | 757 | 0.936 | 3 |
| 200 | relaxed | 30,351 | 29,853 | 0.984 | 3 |
| 1,000 | group | 788 | 782 | 0.992 | 3 |
| 1,000 | relaxed | 36,433 | 37,067 | 1.017 | 3 |
| 10,000 | group | 771 | 768 | 0.996 | 3 |
| 10,000 | relaxed | 31,188 | 36,877 | 1.182 | 3 |

*(inserts/s, `bench/spread_insert_probe.py`'s own unit; a QPS/TPS column
per rule 5a, not a latency one. `ids_burnt = 0` and `retries = 0` on
every cell of both arms, confirming structurally — not just by ratio —
that spreading never activated: HK5's *"the spread arm is not a
configuration so much as the same configuration under a different
name"* holds again at 65,536.)*

**`group`'s ratios are tight — 0.936, 0.992, 0.996 — and that is the
trustworthy read: unmoved, within a few percent, at every row count.**
`relaxed`'s ratios are wider (0.984, 1.017, **1.182**) because its
absolute wall times are two orders of magnitude smaller (10,000 rows
completes in ~0.3s under `relaxed` against ~13s under `group`), which
exposes it far more to the host's own scheduling jitter — and the
10,000-row `relaxed` cell is the one cell measured while the neighboring
worktree's contention was rising (load climbed to 2.75 immediately after
this cell finished; the gate had passed at its start). **1.182 at 10,000
rows/relaxed is this shape's noise floor, named rather than smoothed**:
a null-by-construction A/B swings nearly 20% on the fast arm under this
host's contention, and a future reading of a *real* spread-vs-concentrated
delta on this host should not be trusted below that width on `relaxed`.
Raw per-rep numbers, including the load-gate refusal and its retry:
`bench/v2.7.0/archive/da-cores1-ab-v2.2.1-155-g1f04418/`.

*Wait breakdown (rule 3): does not apply.* This driver reports one
whole-INSERT throughput number per cell; it has no per-phase
instrumentation (`bench_common.Phase` is not wired into
`spread_insert_probe.py`), so there is nothing under this row to
decompose into fsync/write/read/round-trip shares. Said here rather than
omitted silently.

## 5. The read surface, re-confirmed unchanged under the armed default

`bench/spread_read_surface.py` enumerated 11/16 shapes reachable
everywhere at `3446666`
(`bench/v2.6.0/results-ag3-read-surface-v2.2.1-140-g3446666.md`), with
`range_size_ids = 512` explicitly (the script's own default at the time,
unrelated to DA1's engine default). Re-run here at HEAD with
`--range-size-ids 65536` — the value DA1 now ships as default — to
confirm the widened AG3 surface is what a reader actually meets, not an
artifact of the smaller size that run happened to use.

```
bench/spread_read_surface.py --server <staged binary> --workdir <dir> \
    --cores 2 --range-size-ids 65536 --rounds 300 --placement creating
```

`placed=600`, `split_relation_detail=4000:2@2` (two peers, round-robin —
the read-surface driver's own shape, distinct from §1's single-peer
one-owner shape).

| shape | reachable everywhere? |
|---|---|
| star, star+where-pk, star+where-nonpk, star+between, star+order-pk-asc | ok |
| projection, projection-multi | ok |
| count, sum, count-distinct, group-by | ok |
| star+order-pk-desc, star+order-nonpk | refused BY THE SPLIT |
| limit, limit+offset, order+limit | refused BY THE SPLIT |

**11/16 reachable, 5/16 refused — identical to `3446666`, shape for
shape.** Nothing about the read route changed with the default flip,
which is the expected result stated plainly rather than left implicit:
DA1 changes the range *size* and DA3 the fan-in *ceiling*, and neither
touches `TableAccess::ServableBy` or the shape gate that decides
reachability. Raw grid and JSON:
`bench/v2.7.0/archive/da-read-surface-v2.2.1-155-g1f04418/`.

*Wait breakdown: does not apply.* This is a reachability enumeration
(ok/refused per shape per core), not a latency measurement.

## 6. Correctness

- **Debug `build/` suite**: 3018 tests green at this HEAD, established
  before this session (task's stated fact — not re-derived here since
  this session's own change touches no engine source).
- **Release `build-release/tests/kds_tests`**: run this session after
  adding `bench/session_step_state_bench.cpp` and its `CMakeLists.txt`
  target (a new, independent benchmark binary; no existing target
  modified) — **3018/3018 passed**, 83.6s wall. Confirms the addition
  changed no behavior and the full Release tree still links and runs.
- **`scripts/sim.sh`** (default: 19 committed seeds + 4 fresh,
  date-derived): **190 runs, 0 failures**. Log:
  `bench/v2.7.0/archive/da-sim-v2.2.1-155-g1f04418/sim_run.log`.

No engine code changed this session — the only additions are
`bench/session_step_state_bench.cpp` and its `CMakeLists.txt` block — so
this is confirmation that the addition is inert, not a regression check
against a code change.

## 7. What this run does not answer, and what host would

- **DA-b's credit/ring backpressure candidate** (`kInitialCreditsPerEdge`
  against `kCoreRingSlots`) — needs real batches in flight over a real
  ring between distinct owner cores, which needs more than 2 ranges to
  exist. Unmeasurable on a 2-CPU host by §1's own finding.
- **DA-c's refusal boundary itself** — where the 16.7M-row prediction
  actually breaks, if it does. Needs the same precondition.
- **A live 255-stage fan-in's wire cost** — `bench/crosscore_pipeline_bench.cpp`-
  style loopback pricing exists for a *shipped statement*, not for a
  many-stage fan-in; nobody has built or run that loopback at N > 2 stages,
  on any host, because until DA3 nothing needed more than the old 64-stage
  ceiling to be representable at all.

**All three need a host with `hardware_concurrency() >= 3`, so at least
one *additional* peer core can independently win the top-owner race in
`OpenRangeOnSystemCore`** — the exact mechanism §1 confirms suppresses a
second boundary at k = 2. R4-M's own k-sweep (§6 of the cited v2.6.0 file)
already shows k = 3 is sufficient in principle (a real, if imperfect,
third-range transition); k ≥ 4 is where suppression is "essentially
absent" and `ids/stage` tracks the block size to within 4%. A future run
on such a host should drive `spread_insert_probe.py`'s k up until
`SHOW META`'s `split_relation_detail` stage count approaches 255, and
read `spread_ceiling_probe.py`'s refusal boundary at whatever k gets
there fastest — the harness for both already exists in `bench/`; only the
CPU count is missing here.

## 8. What this teaches about the engine

**The two-range ceiling is a host-topology fact, not a tuning outcome.**
No config key on this instance moves it: `range_size_ids`, `placement`,
`peer_listeners` are all exhausted by §1's setup, and the number that
actually bounds a relation's range count here is `hardware_concurrency()`
minus one — a fact about the VM, not about D6 or DA1. This is itself the
loop rule 4b asks for (*"find the limit, say what bound it"*): the limit
found is the host, not the engine, and no further optimize-and-re-measure
step exists on *this* host because there is nothing left to tune — the
next number needs a different machine, not a different value.

**DA-b's superlinear-but-small result is a data point for the standing
open question in `crosscore.md` §9** (batch/credit/ring sizing): a
255-stage fan-in's session-side bookkeeping is provably not the
bottleneck at this N, which narrows where a future host's measurement
should look first — the wire and the ring, not `SessionStepClient`. That
sharpens rather than closes the open decision; nothing here says the ring
is fine, only that the vector scan is.

**And the read-surface confirmation is a small but real piece of
evidence for the milestone table's claim that D6's value and D2/D4's
gates are orthogonal to the route logic**: DA1 rewrote a size constant
that changes how *often* a relation splits, and the reachable-shape set
did not move by one entry. Where a future change to `range_size_ids` or
`kMaxFanInUpstreams` ever does move the read-surface grid, that would be
the surprise worth a workplan entry — this run is the confirmed-unmoved
baseline it would be read against.
