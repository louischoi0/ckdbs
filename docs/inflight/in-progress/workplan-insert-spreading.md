# Workplan — R4, id-block-aligned insert spreading

**The mechanism is built and has no producer.** `docs/spec/crosscore.md`
§6b states the answer to the tail problem — *each core inserts from its
own leased id block, and ranges align to block boundaries, so every core
appends to its own range's tail, fully locally* — and R3 built every
piece of it except the one that makes a second **owner** exist. Drafted
in worktree `v2.6.0-insert-spreading-1` at `86f2052`, which is R3's last
commit (RB6) and is on `origin/main`.

Owning spec: `docs/spec/crosscore.md` §6b (the mechanism), §6a (the
gates), CC8 (per-range sub-structures), §8 test 12 (what this must
prove). Blueprint row: `docs/inflight/in-progress/blueprint-range-ownership.md`
§11's **R4** — *"Writes: single-range statement shipping; id-block-aligned
insert spreading (`crosscore.md` §6b, per-range chains included)"*, gate
**R3, PW1b**, both satisfied.

Numbering is **IS1-IS7** and collides with nothing; cite the file, never
the bare number.

---

## 1. The gate, and why it is open

R3 is complete and on `main` (RB0-RB6, `b7bc72e`..`86f2052`). PW1b — the
row-id lease's asking half — has been built since the peer-writer series.
Both of R4's gates are therefore satisfied, and the phase is open with no
decision owed first.

**D1 is still not taken**, and it bounds this phase the same way it
bounded R3: every btree relation is declined by `RangeEligible`, so
spreading applies to heap relations only. §6b says this is the right
scope anyway — *"a btree relation whose caller names its keys spreads
naturally and needs none of this"* — so unlike R3, where D1 left the
measurement subject unrepresentative, here it removes a case that does
not need the mechanism. Stated so the absence is not read as an
oversight.

## 2. The loop this phase breaks

Three sites in R3's build say the same thing in different words, and each
is a comment on code that cannot fire:

- `src/server/command_dispatcher.cpp:5340` (`VisitRelation`'s ownership
  pass): *"Every range of a relation is owned by the core that asked for
  the lease, which is the relation's owner, so today this cannot fire -
  it fires when R4 starts handing blocks to **other** cores."*
- `src/server/command_dispatcher.cpp:6283` (the fan-in): *"even armed,
  RD5 opens a range for the core that asked - the owner - so a second
  **owner** arrives only with R4's spreading."*
- `tests/core_runtime_test.cpp:1385`: *"the directory is written by hand
  because **nothing can produce this state yet**."*

The loop is exact and small:

1. `Catalog::AllocateRowId` draws from `RowIdLeaseTable` on any core that
   has one, and a miss **records the demand** (`row_id_lease.hpp`'s
   `Next()`).
2. `CoreRuntime::MaybeRefillRowIds` answers recorded demand, and when
   `range_size_ids` is armed it asks core 0 for a range as well as a
   block.
3. `RegisterRowIdGrantHandler` opens that range **owned by
   `header.src_core`** — the core that asked. It has always been written
   this way; nothing needs changing there.
4. But nothing on a non-owner core ever calls `AllocateRowId` for a
   foreign relation, because `CheckWriteAffinity` refuses the statement
   before `InsertOneRow` runs. **So the demand is never recorded, the
   request is never made, and every range core 0 opens belongs to the
   relation's own owner.**

Step 4 is the loop. R4 breaks it at the one place it can be broken
without writing anything: a peer that is *about to give a foreign INSERT
away* records the demand first. Everything downstream of that is already
built.

## 3. The ceiling this design runs into, priced before it is built

**Interleaved id blocks produce one range per block, and the read fan-in
admits 64 stages.** The arithmetic is not subtle and it is the most
important number in this phase, so it is stated before any code:

- A range is a lease grant (D6, `server/range_alloc.hpp`), so a relation
  taking `n` blocks has `n` ranges.
- The fan-in opens **one stage per maximal contiguous run of ranges on
  one core** (RD7, `command_dispatcher.cpp:6288`). Spreading is exactly
  the case where consecutive ranges have **different** owners, so a run
  is one range and `stages == ranges`.
- `kMaxFanInUpstreams` is **64** (`remote_step_service.hpp:151`), and
  above it a read of the relation is refused, not degraded.

So a spread relation is readable up to `64 × range_size_ids` rows and
refuses every `SELECT *` after that. At the sweep's centre value of
4,096 that is **262,144 rows** — small enough that R4 would ship a
mechanism whose benefit expires inside one benchmark. The number is a
product of two ratified choices (D6's range = grant, RD7's run-shaped
stage) and of nothing this phase introduces, which is why it is priced
here rather than discovered in RD9's successor.

**Three ways out, none taken here, each named with its owner:**

- **Raise `range_size_ids`.** Costs nothing structurally and is a config
  value: at 2^20 the ceiling is 67 M rows. What it costs is burn — a
  core that stops inserting burns the remainder of its block, and a
  restart burns every live block — so the knob trades the ceiling
  against 40-bit space consumed per (relation, core, mount). This is the
  D6 sweep's axis and it now has a hard constraint on one side of it that
  §10b's table did not carry. **IS7 measures it; the operator takes it.**
- **Raise `kMaxFanInUpstreams`.** The wire carries the upstream index in
  one byte (`remote_step_service.cpp:149`), so 255 is reachable without
  a format change — a 4× ceiling for the cost of per-stage state on the
  session core. Not taken because 4× does not change the shape of the
  problem, and because the constant is `crosscore.md` §9's sizing item,
  not this plan's.
- **A per-core stripe of the id space** — each core's ranges become one
  interval per relation instead of one per block, so ranges stay at
  `cores` forever and the ceiling disappears. This is the real answer and
  it **reverses D6**: `sys.tables.next_id` stops being one ascending
  high-water mark, which is invariant 11's own wording. A decision of
  that size is the operator's, and it is recorded in §4 as one this plan
  does not take.

The mover's **merge** would also collapse spent ranges, and §14c of
`workplan-range-directory.md` already places merge with R5 for its own
reasons. It is not a way out for R4 because R5 does not exist.

## 4. Decisions this plan does not take

- **D1** (btree relations splittable) — `crosscore.md` §9's. Untouched;
  §1 states what its absence bounds.
- **D6's final value** — the operator's, on IS7's numbers. This plan
  ships `range_size_ids` still defaulting to `kRangeSizeOff`, for the
  reason RD5 gave and one more of its own (§5's IS6).
- **The stripe alternative to D6** (§3) — a reversal, the operator's.
- **`kMaxFanInUpstreams`** — `crosscore.md` §9's sizing item.
- **Multi-range write statements** — R6's, and this plan refuses them by
  name rather than answering them partially (IS4).
- **Whether the multi-row INSERT straddle refusal should partition
  instead** — `workplan-range-directory.md` §14f handed it to the
  operator and it is still theirs. IS3 does not change it.

## 5. Task series

| # | Task | Gate |
|---|---|---|
| **IS1** | **The pump.** A core that is about to ship or refuse a foreign relation's **omitted-key** INSERT records row-id lease demand for it first (`Catalog::NoteRowIdDemand` → `RowIdLeaseTable`), so the drain tick asks and core 0 opens a range **owned by this core**. Nothing else changes: the statement is still shipped, or still refused with the exact spelling and wire bit it had, so this row is invisible to a client and to an unarmed instance. This is the whole of §2's step 4 | none |
| **IS2** | **A range's owner may write it.** `CheckWriteAffinity` and the creation-page rights probe ask **which ranges this core owns** rather than whether it owns the relation: a core holding a range is funded for rows landing in *its own* ranges, and the pages it must be able to write are its ranges' entry pages, not `desc_page_id` (which is the lo = 0 range's, and another core's). `InsertIntoRelation` refuses by name when the resolved chain's range is not this core's, so the store's `MayWrite` backstop stops being the thing that catches it | IS1 |
| **IS3** | **The insert lands locally, and the invariant is checked rather than assumed.** An omitted-key INSERT on a core holding a live lease for the relation takes the local path — no ship — because the id it will issue is inside its own block, which is its own range. The "by construction" is verified at placement (IS2's refusal), which is what makes the claim a check | IS2 |
| **IS4** | **Single-range statement shipping, and the multi-range refusal.** A write whose pk predicate resolves to ranges on exactly one **other** core is shipped there rather than to `sys.tables.owner_core`; one spanning several cores — including a predicate that names no pk at all — is refused **retryably by name** as R6's. This is the cost of arming spreading and it is stated to the operator rather than discovered: on a spread relation, `UPDATE`/`DELETE` with a non-pk predicate stops working until R6 | IS2 |
| **IS5** | **A contiguous refill opens no boundary.** Core 0 opens a range at every carve today, so a *single-writer* relation accumulates one boundary per lease block for no benefit — its own chain cut in two, and one fan-in stage spent, per 4,096 rows. Suppressed when the carve continues the requesting core's own top range (same owner, `lo == hi`). Bounds §3's ceiling to relations that are actually contended | IS2 |
| **IS6** | **§8 test 12, end to end.** k cores inserting concurrently each land in their own range's tail; ids ascend per range; ids stay globally unique (K1 across cores); invariant 3 holds per range. Plus what IS4 owes: a pk-named write shipped to its range's owner, and a multi-range write refused with the bit. `range_size_ids` stays defaulted off and the tests arm it, so an unarmed instance is byte-identical | IS3, IS4, IS5 |
| **IS7** | **Measure, and hand D6 its constraint.** Insert throughput at k = 1..4 writer cores, spread against concentrated (today's shipping), `build-release`, interleaved A/B per `ck-tester`; plus the range count and stage count the run produced, which is §3's ceiling as a measured number rather than an arithmetic one. Results to `bench/v2.6.0/` naming `git describe --tags` | IS6 |
| **IS8** | **The documentation §6b asks for by name.** *"Per-relation id monotonicity becomes per-range monotonicity — invariant 11's 2026-08-11 amendment one level down, and it needs the same loud documentation when built (R4)."* `heap-and-tuple.md` §4.1, invariant 11 in `CLAUDE.md`, `crosscore.md` §6b's status, and `known-gaps.md` for §3's ceiling and IS4's cost | IS6 |

**Not in this phase**: the mover (R5); multi-range transactions (R6);
merge; the stripe alternative to D6; raising `kMaxFanInUpstreams`;
`CREATE INDEX`/Cabin/assertion/FK on a split relation (each declines, and
the converse gates RD5 built are what decline them).

## 6. Where to pick this up

At `86f2052`, nothing of IS1-IS8 is built.
