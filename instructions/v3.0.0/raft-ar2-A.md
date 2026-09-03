# Ratification AR2-A — The borrow model: intent, the mover's unit, and what is ratified now

2026-09-03, operator (verbal, chat session), recorded by CLA against
`worktree-ar2-borrow-model` at `92cb654` (AR2 draft + AR2-V) and `main`
at `f710b3d`. Applies to `instructions/v3.0.0/ar2-architecture-revision-borrow-model.md`.
Nothing here is code. Every item names the AR2 rule or E-item it moves.

## 1. Intent, restated as the evaluation axis

AR2 is evaluated on three axes, in this order, and not on throughput:

1. **Refusal → wait.** Every place the engine today refuses because a
   permanent owner is elsewhere becomes a wait on a borrow.
2. **Flexibility.** No unit has a permanent writer; borrows are as fine
   as the operation needs and no finer than it can name.
3. **Physical-optimizer foundation.** A physical move is legal exactly
   when it is a borrow the model can check, and the model's statistics
   are the optimizer's input.

Consequence for §9: C1/C2 remain as measurements and price R12/E7; they
are **not** a gate on AR2. M2 opens when M1 (AM) and AN-S2 close.

## 2. Revision — the mover borrows the unit it moves

AR2 §3's mover row ("relayout mover (future) — the **relation**, `X`") is
struck. Replaced by:

> **AR2-R13 — A move borrows the unit whose key-space assignment it
> changes, in `X`, for the maintenance task's run.** The unit may be a
> range (split, merge, migration), a slice (relayout of a key interval),
> or — for a move that changes **no** key assignment, only where bytes
> live — a page. A lock-family unit (range, slice) is borrowed with `IX`
> on its ancestors, so a reader's `IS` at any enclosed unit (R14) is what
> the move waits for, by the ordinary compatibility table. A page-unit
> move is latch-family only (R2): it holds the frame `X` for its critical
> section, changes no key's unit, and bumps the Waystone epoch
> (`heap-and-tuple.md:28-34`'s one epoch-bumping operation gains a
> second, listed) because recorded `(page, slot)` locations move.
> A move that changes key assignment **and** relocates pages borrows the
> lock unit; the page latch is taken inside it, as for any write.

> **AR2-R14 — The read borrow is at the position's finest lock unit.**
> E12's `IS` is taken on the **slice** a positioned statement is walking
> (the page's `[min_key, next.min_key)` when it is on a page, the range
> when it is between pages), with `IS` on the range and relation by the
> intention rule. A relation-level move still waits for it through the
> hierarchy; a slice-level move waits only for readers in that slice.
> R3's "one compatibility check" is unchanged; what changes is that the
> check has a unit finer than the relation.

## 3. Ratified now (operator, this session)

| item | rule | ratified as | why now |
|---|---|---|---|
| E11 | R1 | A borrow ends with its scope, never with a clock. No expiry, renewal or revocation. | Structural; measurement cannot change it |
| E4 | R6 | A slice is keyed `(rel_oid, [lo, hi))` in key space; the page is a hint only. | Structural; it is what lets R13's moves keep fences alive |
| E12 | R3, §5.4 | **Adopted**, at the unit R14 states, not at the relation. Its price is C3's to report, not to decide. | Without it no move can wait; it is axis 1 meeting axis 3 |
| E1 | R2 | `IS`, `IX`, `S`, `X`; no `SIX`, no update mode in v1. | Listed default, confirmed |
| — | R4 | R4's cap refusal is declared **the one refusal the borrow model keeps**, because a cap cannot be waited out. Its value is E2 and stays open. | Axis 1 requires the remaining refusals to be named |
| — | R13, R14 | As written in §2 above. | The operator's revision |

## 4. Deferred, with the reason and the gate

| item | gate | note |
|---|---|---|
| E2 (cap value, code) | M2 opening | `[constant]`; 65,536 stays CLA's proposal; the refusal code's comment at `status.hpp:69` widens |
| E3 (FK reverse with no covering structure) | AR1/AR2 unification, M3 | `[quiet-wrong]`; the relation `S` fence is correct and is the coarse arm; the covering structure that narrows it to a slice is AR1-8's supporting Cabin, i.e. a legal move (§6 item 3) |
| E5 (affinity collector) | M3, **amended** — see §5 item 2 | grant counts alone are not the optimizer's signal |
| E7 (execution default) | C1/C2 numbers | measurement-gated; CLA's "local" proposal stands |
| E8, E9 (NS10 verb, `core_count` pin) | M3 | user-visible; no reason to move them before the code they describe |
| E10 (AE re-ratify or retire) | M3 work order, §5.7 gate-by-gate | as AR2 proposes |
| D12 priority | M2 opening | see §5 item 1 |

## 5. Amendments requested to the AR2 body (CLA to apply on the branch)

1. **D12**: state the priority — wait-for-graph detection is the
   mechanism, an abort is issued only on a detected cycle; the timeout is
   a safety net for a detector fault, logged as such, never the normal
   end of a wait. A wait that ends by clock reintroduces a refusal after
   the work was done, which is worse than the refusal it replaced.
2. **E5 / R8**: the collector exports, per lock unit, **wait counts and
   wait time** beside grant counts. Waits are the optimizer's signal for
   the moves R13 admits: waits concentrated on one range → split; fence
   waits on a relation → create the covering structure (E3, AR1-8).
   Grant counts say where is warm; wait counts say where is blocked.
3. **§3 mover row**: replaced by R13; **§3 `SELECT` row**: unit per R14.
4. **R5, named pk**: add to §7 as **E13 `[OPEN, M3]`** — whether core 0's
   catalog relation becomes borrowable at tuple unit (`X` on the
   `sys.tables` row) so a named-key `INSERT` waits instead of ships.
   CC11 is the only ground for the ship, and AR2 does not argue it.
5. **§9**: C1/C2 lose their gating sentence; M2's opening condition is
   "M1 and AN-S2 closed".
6. **§5.1 / AR1 crossing**: name the supporting Cabin of AR1-8 as the
   structure E3's slice arm requires, so the two drafts ask for one thing
   under one name.

## 6. What this ratification does not do

It does not merge the branch, open M2, start a lock-manager prototype,
or set any constant. It fixes the axis, the mover's unit, and five
structural items so that AM and AN proceed against a stated M2 rather
than an unstated one.
