# The FK reverse check answers "no children" from an exhausted per-core Cabin set

**Found** 2026-09-09 by the `critics-developer` pass on AT-S5
(`091be8c` on `m3-at`), as its D4. **Not fixed.** Verified at `091be8c`.
The Cabin half is small enough to ride AT-S5b or AT-S5f; the store becomes
one for the instance at **AT-S7** (AT-0 item 9), which is what restores the
fast path rather than removing it.

**Cost: a quiet wrong answer, and it is the exact one `foreign-keys.md` §1
says a constraint may not have** — a `DELETE` of a parent succeeds while a
child still references it, leaving a dangling foreign key with RESTRICT
reporting success.

## The wrong behaviour

`CheckNoChildReferences` (`src/exec/fk_check.cpp:152`) has a Cabin fast path
at `:213-322`. It reads `options.cabins`, re-checks every entry the set
holds for the parent's pk, and — if the loop drains without finding a live
matching child — returns at `:316-319`:

```cpp
outcome.verdict = FkVerdict::kPass;
outcome.served_from_cabin = true;
```

with no walk at all. The argument, stated at `:223-229`, is that an
exhausted superset scan is an authoritative "no children": every child row
that exists is in the set, so a set with no live match proves there is no
child.

**The set is per core.** `stats::CabinStore` is a member of `CoreRuntime`
(`include/kds/server/core_runtime.hpp:642`) and a second one of `Expeditor`
(`include/kds/server/expeditor.hpp:780`), and the write path files an entry
into the **inserting** core's store (`NoteCabinWrite` →
`cabins_->NoteWrite`, `src/server/command_dispatcher.cpp:10302`, through
the per-dispatcher `cabins_` at `command_dispatcher.hpp:2882`).

What made "exhausted" mean "no child exists" was the owner refusal AT-S5
struck — a child relation this core did not own was not answered here at
all, and `fk_check.cpp:200-201` is now the comment recording the strike.
**Read those two lines with care**: a live refusal still stands directly
below them, at `:202-203`, for a child whose *ranges* this core cannot
serve (`!child.ServableBy`), so the surviving one is easily mistaken for
the struck one. The Cabin arm was left standing on the struck one, and
since then a child row inserted on core 1 never reaches core 0's set — so
core 0's set is exhausted *and* stale, and the drained loop clears a parent
that has a child.

One level down, the walk fallback at `:342-409` covers only the chains this
core owns (RD6), so even the miss path is a per-core answer — that half is
AT-S5f's.

## Reproduction

Two cores, a parent on one and a child relation with an active Cabin on its
fk column (`CREATE CABIN ON child(fk_col)`, F6). Insert the child row on
core 1; delete the parent on core 0, whose set holds no entry for it; the
`DELETE` is cleared where it must answer `kFkViolation`.

## The fix, in two halves

**Now (small):** the drained loop falls through to the walk instead of
returning `kPass` — a Cabin set may *find* a child and may not *clear* one
while the store is per core. `served_from_cabin` stays true only on a hit.
This costs the fast path on the clear case and answers correctly.

**At AT-S7 (the real one):** one store for the instance, under AR1 §11's
partitioning (AT-0 item 9), after which the exhausted-superset argument is
true again and the `kPass` comes back with it.

## Owner

`instructions/v3.0.0/workorder-at-m3-uniformity.md`, AT-S5f for the walk
half and AT-S7 for the store. `docs/spec/foreign-keys.md` F6 and §3a, and
`docs/spec/cabin.md` §4b, are the rules involved.
