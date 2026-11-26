# Work order DX - the buffer pool under pressure: a full pool that waits for its writeback, and an abort that cannot be refused a frame

Status: **WRITTEN 2026-11-26, not opened.** Written on worktree
`known-gaps-milestone-orders` (branch
`worktree-known-gaps-milestone-orders`) from `8274c951`
(`v3.4.17-148-g8274c951e`) on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §3). No stage is started; DX-S0 waits for the
operator's word and for the marks on §4's items. A `[source-read]` claim
with a `path:line` was read at `8274c951`; a claim taken from
`known-gaps.md` carries that entry's own verification commit and is not
re-verified here.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/eviction.md` §3.1-§3.3, EV6, EV8; `docs/spec/page.md`
§3, §8; `docs/spec/txn.md` (abort); `docs/inflight/known-gaps.md`,
"Eviction"; `instructions/v3.0.0/workorder-be-bounded-pool.md` (BE);
`instructions/v3.0.0/workorder-bg-scan-queue.md` (BG, closed narrow on
2026-11-23 with its scan queue not built).

## 0. What DX is

`[design]` Three behaviours of a pool that is full, in the order of what
each can cost:

1. **A rollback's compensation is never refused a frame.** Today one
   path is argued safe and not enforced.
2. **A pool full of dirty frames makes a statement wait for writeback**
   where it has nothing to undo, instead of refusing it.
3. **A scan larger than the pool** and the hot set it displaces - only
   if the operator reopens what BG closed (DX-Q4).

## 1. The tree as read

`[source-read]` from `docs/inflight/known-gaps.md` at `8274c951`:

- **A rollback compensation that meets a full pool while its thread
  holds a pin is refused, and that leaves the write un-undone** (`:112`,
  verified at `2f08b71c`): drain mode writes back only when the thread
  holds no pin (`DevicePageStore::ReserveFrame`), because a writeback's
  shared try re-enters the thread's own exclusive hold. "No path into
  `TransactionManager::Abort` holding a pin was found, and nothing
  enforces that either."
- **A mostly dirty pool refuses point updates** (`:40`, `[measured]`
  `bench/v3.5.0/results-dm-s1-ceiling-v3.5.0-89-gc003e7fe.md` §3): on
  2,048 frames over 1,000,000 rows, four sessions at `cores = 1`,
  143-220 `ResourceExhausted` in 6 s against about 40,000 updates, and
  557-596 with 78 % of the right sibling's reads removed. The likely
  cause as written there: a dirty frame is queued for writeback by a
  reclaim walk that passes over it, and the built retry re-walks without
  yielding. Any change that removes reads from a write path meets this
  cell.
- **Writers outside a statement meet the ordinary refusal** (`:122`),
  and no cell drives one into a full pool.
- **Three of BE's rules are argued, not tested** (`:99`): the reclaim
  walk's total bound, `FailStop`, drain mode under recovery and `Abort`
  at the cap.
- **The cold scan protects only a hot set touched more often than the
  scan laps the pool** (`:29`, `[measured]`): a hot set of half a
  16,384-frame pool is 92 % re-faulted after a scan four times the pool.
- **Writeback stamps the checksum into the live frame under a shared
  page latch** (`:68`): value-benign, a data race by the language's
  rule.

`[source-read]` in the code at `8274c951`: the reservation is
`DevicePageStore::ReserveFrame`
(`src/storage/device_page_store.cpp:901`, declared
`include/kds/storage/device_page_store.hpp:1498`); the CLOCK cap is
`kClockUsageCap = 5` (`device_page_store.hpp:856`); `OpenScanRing`
(`device_page_store.cpp:1376`) has one caller, the Cabin controller
(`src/exec/cabin_optimizer_exec.cpp:165`).

## 2. The mechanism `[design]`

1. **The abort.** `TransactionManager::Abort` and every compensation
   under it assert, in every build, that the thread holds no pin at
   entry; a path found holding one is reordered. Beside the assertion, a
   reservation made for a compensation drains by a writeback that does
   not need the caller's own page - the copy-under-latch writeback of a
   frame the thread does not hold - so the refusal has no path left.
   DX-S0 decides whether the assertion alone closes it.
2. **The wait.** A reservation refused outside a no-refuse window, by a
   statement that has written nothing or can park, parks under a wait
   event until writeback has cleaned a batch, then asks again, inside
   the statement's deadline. Writeback under pressure is driven by the
   pressure, not only by the reclaim walk's passing: a refused
   reservation hands a batch of dirty frames to the writeback path
   before it parks.
3. **The refusal stays** where a park is impossible - a synchronous
   dispatcher, a statement past its deadline - with the same code.
4. **The checksum stamp** moves to the scratch copy (`:68`'s named
   remedy), one line of §2.2's stage.

## 3. Rules

- **DX-R1. No refusal and no park after a mutation's first page write**
  `[quiet-wrong]`: EV8's no-refuse window is unchanged, and the wait is
  taken only before it or outside it.
- **DX-R2. An abort's compensation is never refused a frame**, and a
  cell fills the pool with dirty frames under a rollback and reboots.
- **DX-R3. A waiting reservation holds no latch and no pin**, is inside
  a `WaitScope`, and ends at the statement's deadline with today's
  `ResourceExhausted`.
- **DX-R4. `buffer_pool_frames` stays the hard ceiling**: resident
  frames never exceed it, and no reserve is carved that a setting does
  not name.
- **DX-R5. A pool that is not full pays nothing**: the hit path and the
  free-list path execute no new instruction. Measured at the close.
- The standing rules, as DT's order lists them.

## 4. Items for the operator (DX-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| DX-Q0 | Is DX opened, and with what scope? | (a) §2's 1, 2 and 4; (b) the abort only; (c) with the scan queue | (a) |
| DX-Q1 | Does DX-S0 gate? | (a) S0 reproduces `:40`'s cell on the current tree and searches every path into `Abort` for a pin; the wait is built only if the refusals reproduce. The abort's assertion is built either way. (b) all built regardless | (a) |
| DX-Q2 | The abort's guarantee | (a) the assertion, plus the drain that needs no page of the caller's; (b) the assertion only; (c) a per-transaction frame reserve | (a); (c) spends frames a setting does not name |
| DX-Q3 | What the wait is bounded by | (a) the statement's existing deadline (`lock_wait_fault_net_ms`); (b) its own key | (a) |
| DX-Q4 | The scan queue (BG-S2..S4) | (a) stays closed, as the operator closed it on 2026-11-23; (b) reopened inside DX | (a) |
| DX-Q5 | BE's three untested rules | (a) each gets a cell in the stage that touches it, with a seam between reclaim batches; (b) outside DX | (a) |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DX-S0** | The census: every caller of `Abort` and of a compensation, with what it pins; the dirty-pool cell re-measured at the current tree; where writeback is queued from. No behaviour change | a results file; DX-Q1's bar read |
| **DX-S1** | The abort: the assertion, any reorder S0 finds, the drain as DX-Q2 marks | DX-R2's cell; the sim's allocation-failure sweep over rollbacks |
| **DX-S2** | The wait, pressure-driven writeback, the scratch-copy stamp | cells: the dirty-pool cell refuses nothing inside its deadline; DX-R1's window; a `tsan` cell over the stamp |
| **DX-S3** | The close: the dirty-pool cell and a resident control A/B, `eviction.md` §3.3, `known-gaps.md` | the operator's word on landing |

## 6. What DX does not do

- No change to the CLOCK hand, the usage cap or the cold-fault rule.
- No second pool, no per-core pool, no background writer thread beyond
  what the checkpoint and writeback already run (DX-S0 states if one is
  needed, and it is then the operator's item).
- Nothing for the 2.5-3.7 % resident-scan cost (`known-gaps.md:17`),
  whose cause is not found.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.
