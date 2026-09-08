# AN-S5, scenario0 half — blocked before the first statement, by SUS-1

**No TPS number in this document. This is a report of why, not a results
table with a caveat attached** — per the rule that governs this file
("do not report a number you did not measure in this session, and never
predict what a run would produce"). AN-S5 asks for `tools/`'s AL-S8
scenario matrix re-run at `13b6b55` beside AL-S8's own
`results-scenario0-stockmarket-v2.7.0-157-gf6ed10c.md` numbers. It could
not be attempted past schema creation, on any of the four cells, for a
reason that has nothing to do with AN-S2's or AN-S3's own code: **SUS-1
(`instructions/v3.0.0/workorder-as-sus1-heap-suspended.md`), landed on
`main` 2026-09-05, refuses `CREATE TABLE … HEAP`, and
`tools/scenario0_stockmarket.py` declares two of its five relations
(`trades`, `user_periodic_profit`) `HEAP` in its own fixed schema, with no
flag to override it.**

## 1. Stamp

| Field | Value |
|---|---|
| Date/time attempted | 2026-09-07, 11:32–11:34 UTC |
| Worktree | `an-s3-snapshot-adoption` (branch `an-s3-snapshot-adoption`) |
| Engine commit measured | `13b6b55`, `git describe --tags` = `v2.7.0-265-g13b6b55`. `git diff --stat 13b6b55 HEAD -- src include tools CMakeLists.txt` is empty at this worktree's `HEAD` (`451022b`), verified this session |
| Tree cleanliness | Clean |
| Binary provenance | `/home/cdkbs/bench-runs/an-s5-v2.7.0-265-g13b6b55/kds_server`, `sha256 5bf18b6d0851facdf0e03463d8c0ed4c0def32020d33c4348173bf4c43088a3b`. Source `build-release/kds_server` mtime `2026-09-07 11:27:43.32 UTC`, copy mtime `11:28:26 UTC`, both after `13b6b55`'s commit time `11:14:21 UTC`. Started from the copy in every attempt below, never from `build-release/` directly |
| Device | `/home/cdkbs`, `/dev/root`, ext4 (`df -T`), 51% used |
| Build type | `build-release` (Release), not rebuilt this session |
| Host | 8 logical CPUs, AMD EPYC 9V74, 1 socket × 4 cores × 2 threads/core — same shape AL-S8 recorded |
| Precheck (`s0-c1-g`) | 11:33:19 UTC, `/proc/loadavg` 1.20 / 1.97 / 2.67; `pgrep -a -f 'cc1plus\|cmake --build\|ctest'` matched a `ctest` invocation in an unrelated worktree (`AU+AV-S0S1/build`, pids 151091/151092, <2% CPU each) — a different session's test-suite run, not a build, and not in this worktree or this run's build tree. Noted rather than waited out: it is CPU-light and touches no file this run reads or writes |

## 2. What was attempted, and what happened

The server started clean from the copy, at `cores = 1`, `durability =
group`, port 15590, fresh data file. `tools/scenario0_stockmarket.py`
(unmodified) was invoked exactly as specified:

```
python3 tools/scenario0_stockmarket.py --port 15590 --suffix s0c1g \
  --users 100 --accounts-per-user 3 --assets 30 --traders 8 \
  --txn-per-user 50 --verify 200 --seed 1 --sync \
  --json .../json/s0-c1-g.json
```

It created `accounts_s0c1g`, `users_s0c1g`, `assets_s0c1g` (all declared
`BTREE` in the driver's schema) and then aborted on the fourth relation:

```
loading: users=100 assets=30 accounts~300  (tables suffixed _s0c1g)
stress aborted: could not create trades_s0c1g
  server said: ERR HEAP storage is suspended (SUS-1) and no new heap
  relation is created (byte 124); BTREE is the default and every existing
  heap relation still mounts and serves. The suspension, its rulings and
  the condition that lifts it are
  instructions/v3.0.0/workorder-as-sus1-heap-suspended.md
```

The server's own log confirms the same refusal server-side
(`logs/s0-c1-g.log`):

```
WARN [query] "CREATE TABLE trades_s0c1g (id int64, account_id int64,
asset_id int64, side int32, qty int64, price int64, trade_day int32)
HEAP" -> ERR UNSUPPORTED retryable=0 HEAP storage is suspended (SUS-1)
and no new heap relation is created (byte 124); ... in 8us
```

The server was then stopped cleanly (`SIGTERM`, exited before the next
check), and the partial data file removed. **The same three remaining
cells (`s0-c1-s`, `s0-c8-g`, `s0-c8-s`) were not attempted separately**:
the failure is in `tools/scenario0_stockmarket.py`'s fixed schema
(`SCHEMA["trades"]` and `SCHEMA["user_periodic_profit"]` both name
`"HEAP"` at `tools/scenario0_stockmarket.py:104`, `:178-181`), which does
not vary with `cores` or `durability` — every cell would fail identically
at the same `CREATE TABLE trades_<suffix>` statement, for the same
reason. Re-running it three more times to watch it fail the same way
three more times would not be a measurement.

## 3. Why this is not worked around

Two rules this session runs under both forbid the two ways around this:

- **`tools/` stays unmodified.** A driver change inside a measurement
  stage measures the driver, not the engine — dropping the `HEAP` keyword
  from `scenario0_stockmarket.py`'s schema would change what workload is
  being priced (an all-`BTREE` schema descends a tree for every insert
  scenario0 was written to make append-only) and would no longer be "AL-S8's
  driver arguments verbatim" this stage asks for.
- **Engine code is not edited to make a benchmark pass.** SUS-1 is an
  operator-ordered suspension (`instructions/v3.0.0/workorder-as-sus1-heap-suspended.md`
  AS-1), not a bug; bypassing it (the test-only seam
  `parser::SetHeapStorageAllowedForTest` exists, but is reachable from no
  wire path — AS-S1's own row says so) would measure an engine
  configuration no client can reach.

This is also not a surprise the session had to discover cold: AS-Q6 in
the same work order names it directly — *"three [tools] emit explicit
`HEAP` and will simply be refused"* — and records that `bench/`'s rules
are the operator's to resolve, left open rather than decided by the
author of SUS-1's own stage. `scenario0_stockmarket.py` is one of those
three (`scenario2_freight.py` is a second — see the sibling document).
AS-Q6 was written 2026-09-05 and is still unratified at `13b6b55` and at
this worktree's `HEAD` (`451022b`); this run is the first time it was
exercised against the AL-S8 scenario matrix specifically.

## 4. What this means for AL-S8's numbers

AL-S8's own baseline (`results-scenario0-stockmarket-v2.7.0-157-gf6ed10c.md`,
commit `f6ed10c` = `v2.7.0-157-gf6ed10c`, measured 2026-09-03) predates
SUS-1 (2026-09-05) and is unaffected by it — those numbers (700.9 / 754.7
/ 192.6 / 207.2 TPS across the four cells) stand as a record of the
engine at that commit and are not superseded by this document, which
carries no numbers of its own to supersede them with. **`bench/README.md`'s
rule that a re-run deletes what it supersedes does not apply here**: no
run happened, so there is nothing to replace AL-S8's file with, and it is
left as the standing baseline for scenario0 until either SUS-1 lifts
(the resume condition is stated in AS-1) or `tools/scenario0_stockmarket.py`
gains a schema variant that does not require a heap relation.

## 5. What would unblock this

Either of two things, neither of which is this session's to decide:

1. **SUS-1 resumes** (`AS-R6`'s stated condition: `CREATE TABLE … HEAP`
   succeeds again without the test-only bypass, and `parser-v2.md` §9
   item 8's three-way corpus is green at `cores = 4` under tsan) — then
   the driver runs unmodified exactly as AL-S8 ran it.
2. **A BTREE-schema variant of the driver is written** (a new file or a
   flag under `tools/`, not a silent edit of the existing one) so an
   AN-S5-shaped run can measure the read view's cost on a schema this
   engine's current state can actually create — at the cost of no longer
   being "the same workload AL-S8 measured," which is exactly why this
   document does not take that step on its own initiative.

No archive directory entries exist for this document beyond the aborted
attempt's own logs (§2), which are kept at
`bench/v3.0.0/archive/an-s5-v2.7.0-265-g13b6b55/` for the reproduction —
not a results archive, since no cell produced a result.

**Superseded on the comparator, 2026-09-08.** The operator's mark on AS-Q6
(`instructions/v3.0.0/raft-marks-2026-09-08.md`) took the step §5 above
declined to take on its own: the driver's `trades` and
`user_periodic_profit` are `BTREE`, `f6ed10c` is to be re-measured with the
changed driver as the new baseline, and measurement is BTREE only for now.
The AL-S8 file this document names as "the standing baseline" is history
from that mark on and is compared against nothing (`bench/README.md`); the
driver lines this document cites now say `BTREE`. Nothing above is
re-measured or edited.
