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

Numbering is **IS1-IS8** and collides with nothing; cite the file, never
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
is a comment on code that cannot fire. Quoted from `86f2052`, before this
phase amended all three in place:

- `VisitRelation`'s ownership pass: *"Every range of a relation is owned
  by the core that asked for the lease, which is the relation's owner, so
  today this cannot fire - it fires when R4 starts handing blocks to
  **other** cores."*
- The read fan-in: *"even armed, RD5 opens a range for the core that
  asked - the owner - so a second **owner** arrives only with R4's
  spreading."*
- `tests/core_runtime_test.cpp`'s RD7 test: *"the directory is written by
  hand because **nothing can produce this state yet**."*

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
  one core** (RD7, the stage loop in `command_dispatcher.cpp`). Spreading
  is exactly the case where consecutive ranges have **different** owners,
  so a run is one range and `stages == ranges`.
- `kMaxFanInUpstreams` is **64** (`remote_step_service.hpp:151`), and
  above it a read of the relation is refused, not degraded.

So a spread relation is readable up to `64 × range_size_ids` rows and
refuses every `SELECT *` after that. At the sweep's centre value of
4,096 that is **262,144 rows** — small enough that R4 would ship a
mechanism whose benefit expires inside one benchmark. The number is a
product of two ratified choices (D6's range = grant, RD7's run-shaped
stage) and of nothing this phase introduces, which is why it is priced
here rather than discovered in RD9's successor.

### 3a. Measured, and the arithmetic was answering the wrong question

**R4-M (`instructions/v2.6.0/r4-k-sweep.md`), worktree `v2.6.0-ksweep` at
`03b815b`; `bench/v2.6.0/` §6a.** The arithmetic above is right and the
ceiling is real. It is also **not what stops a spread relation being
read**, and §3 could not have known that because it priced the fan-in and
nothing else. Two limits sit in front of it, and a relation meets both at
its **second** range:

- **The fan-in client is core 0's alone.** `expeditor.cpp` builds it as
  `remote_reads_.emplace(/*core_id=*/0, …)` and calls `SetRemoteReads` on
  that one dispatcher. `CoreRuntime` — every peer — has `remote_steps_`,
  the **server** half, and no client member at all. Every peer can serve a
  stage; none can open one. So a session on a peer meets
  `CheckReadAffinity`'s not-`WhollyOwnedBy` refusal for any relation with a
  range elsewhere, whatever the count.
- **The route requires the reader not to be the relation's `owner_core`**,
  which under `placement = creating` is core 0 for every relation. So
  under `creating` — the default, and the arrangement this section's
  spreading exists to produce — **no core can read a spread relation at
  all**.

Where it is readable (a core-0 session, `placement = rotate`) the surface
is one shape: `SELECT *`, optionally with a `WHERE`, optionally with a free
`ORDER BY <pk> ASC`. A projection, an aggregate or a `LIMIT` is refused,
because the route tests `chain.star()`, not aggregated, not sorted, no
`LIMIT`/`OFFSET`, no sub-chain. Measured from every core under both
placements; the table is in the results file.

**What this costs the scenario benchmarks** (CK4, same file §5): six of
this repository's twenty-four scenario relations spread — the heap ledgers
`trades`, `user_periodic_profit`, `daily_stats`, `model_results`,
`freights`, `charges` — and **all six lost all five read shapes their
drivers use, at ~395 rows.** The other eighteen never spread, and not
because a gate declined them: **the pump is heap-only**
(`command_dispatcher.cpp`'s `heap_omitting_pk`), so a btree relation never
records demand and `RangeEligible` is never asked. One consequence worth
its own line: `SHOW META`'s `range_split_decline_detail` — RD5's C3,
whose stated purpose is naming which gate to lift first — **cannot report
D1**, the gate that blocks those eighteen, because the decline never
happens.

**Three ways out, none taken here, each named with its owner — and none of
them moves the limit above.** The re-pricing R4-M's §8 performs: a larger
`range_size_ids`, a larger `kMaxFanInUpstreams` and the per-core stripe
all move a ceiling that is not what binds. What would move it is the
**self-directed stage** `workplan-range-directory.md` §15d already names
and defers (*"a design question and not this row's"*), plus a fan-in
client on every core. Neither has an owner named anywhere, and that is
this line's largest open hand-off.

- **Raise `range_size_ids`.** Costs nothing structurally and is a config
  value: at 2^20 the ceiling is 67 M rows. What it costs is burn — a
  core that stops inserting burns the remainder of its block, and a
  restart burns every live block — so the knob trades the ceiling
  against 40-bit space consumed per (relation, core, mount). This is the
  D6 sweep's axis and it now has a hard constraint on one side of it that
  §10b's table did not carry. **IS7 measures it; the operator takes it.**
  **Measured 2026-08-29 by R4-M** (§9a): the sweep runs 256..65,536 at
  k = 4, and the knob turns out to have a *third* axis - below 4,096 a
  block is spent as fast as it is granted and the relaxed arm falls to
  0.434x at 256, so a small size costs throughput as well as ceiling.
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
| **IS7** | **Measure, and hand D6 its constraint.** Insert throughput at k writer cores, spread against concentrated (today's shipping), crossed with `durability` per §8, `build-release`, per `ck-tester`; plus the range count the run produced, which is §3's ceiling as a measured number rather than an arithmetic one. Driver: `bench/spread_insert_probe.py`. Results to `bench/v2.6.0/` naming `git describe --tags`. **k is bounded by the host**: this box has 2 CPUs, so a 1..4 writer-core sweep is not honestly runnable on it and what cannot be run is reported as not run, never as a pass | IS6 |
| **IS8** | **The documentation §6b asks for by name.** *"Per-relation id monotonicity becomes per-range monotonicity — invariant 11's 2026-08-11 amendment one level down, and it needs the same loud documentation when built (R4)."* `heap-and-tuple.md` §4.1, invariant 11 in `CLAUDE.md`, `crosscore.md` §6b's status, and `known-gaps.md` for §3's ceiling and IS4's cost | IS6 |

**Not in this phase**: the mover (R5); multi-range transactions (R6);
merge; the stripe alternative to D6; raising `kMaxFanInUpstreams`;
`CREATE INDEX`/Cabin/assertion/FK on a split relation (each declines, and
the converse gates RD5 built are what decline them).

## 6. Where to pick this up

At `86f2052`, nothing of IS1-IS8 was built. **IS1-IS3 landed at `7b0ba61`**
in worktree `v2.6.0-insert-spreading-1` (§7); **IS4-IS6 and IS8's doc half
follow it in the same tree**, and **IS7 landed at `a135a59`** — honestly,
at the only k a 2-CPU host allowed.

**IS1-IS8 are built and R4-M has measured them** (§9), on hardware that
runs the sweep IS7 could not: k = 1..8, both durability arms, three
interleaved reps, plus the ceiling, the burn and the `range_size_ids`
sweep. **Nothing of this plan remains open as work.** What it hands on is
in §3a and §9:

- **The read surface, which is this line's real ceiling** and belonged to
  nobody — a self-directed stage and a fan-in client on every core.
  **Both built 2026-08-29**, R4-R and RS: §10 and §11.
- **D6's value**, to the operator, on §9a's curve.
- **CK5 unanswered end to end**, because a spread relation could not be
  read back by the workloads that write it — which is why R5's mover should
  not be designed against a scenario premise yet. **The impossibility is
  gone** (§10, §11) and the aggregate becomes runnable; it is the next
  order's, and until it runs CK5 stays unanswered rather than answered.

## 7. What IS1-IS5 concluded, what the build found, and what the review found

Built in worktree `v2.6.0-insert-spreading-1`, IS1-IS3 at `7b0ba61`.

**The loop was one call wide, and that is the phase's main finding.**
Everything §6b describes existed: core 0 opens a range owned by
`header.src_core`, the lease supplies ids, RD6 gives each range its own
chain, RD7 reads across owners. The single missing fact was a core saying
it wanted ids for a relation it did not own — `RowIdLeaseTable::NoteDemand`,
called once, from the site where a foreign INSERT is given away. R3 built
the mechanism and R4 turns out to be mostly *routing*: deciding, before
anything is encoded, that this statement belongs here.

**Routing is by id and never by "this core holds a lease."** Core 0 grants
ids whether or not it opens a range — a gated relation gets its block and
no boundary, and so does a carve at `first_id == 0` — so a lease can name
ids that fall inside another core's range. Asking the directory sends those
where they belong; asking the lease would have written them here. The
distinction cost one line and would have been a wrong-page write.

### 7a. Six findings, each recorded where it belongs

Two of these came from the `critics-developer` review and one from running
the probe rather than from reading; they are marked.

- **The refill callback's invalidation was a no-op for its stated
  purpose** (fixed with IS3). It called `Catalog::InvalidateFromPeer`,
  which drops the memo and leaves the resident catalog frames the memo was
  derived from — `CoreRuntime::InvalidateCatalog`'s own comment says a
  pairing that does one without the other "reads the same stale bytes back
  and reaches the same conclusion". So the core that had just been granted
  a range could not see it until core 0's broadcast arrived, which is
  exactly the window IS3's routing reads. Found by the end-to-end test, not
  by inspection.
- **Core 0 never takes a range of a relation it does not own**, and this
  bounds spreading rather than breaking it. `CoreRuntime::Open` installs a
  row-id lease table on peers only (core 0 bumps the mark directly, M5), so
  `NoteRowIdDemand` is a no-op there. Under `placement = rotate` a relation
  owned by core 2 spreads over every peer but core 0, which ships to the
  owner as before. That is M5's asymmetry and blueprint **R1**'s to retire,
  not this phase's — recorded so a k-core measurement is not read as k-way
  when the relation is not core 0's.
- **A named key that resolves to a foreign range burns the mark.**
  `AdmitExplicitRowId` advances `sys.tables.next_id` past the supplied key
  *before* IS2's placement check runs, so a refused explicit INSERT above a
  foreign boundary leaves the relation's next *omitted* key above it too,
  and the next statement meets the same refusal. K3 calls a burnt id free,
  so this is a burn and not a leak — but on a heap relation whose upper
  range is another core's it means this core has no further ids to issue,
  which is precisely the state IS3's routing keeps a statement out of.
  Pinned by `RangeChainTest.AnInsertWhoseIdFallsInAnotherCoresRangeIsRefusedByName`.
- **The sorted fill reached a chain head without the placement check**
  (found by the review, fixed with it). `SortedFillInner` does not go
  through `InsertIntoRelation`, so IS2's refusal did not cover it — and the
  case is reachable rather than theoretical: that path is core-0-only
  (`!catalog_read_only_`), and `AllocateRowIdRange` draws from
  `sys.tables.next_id`, which sits **above** every block core 0 has leased
  out, so on a spread relation the batch lands in the *top* range, owned by
  whichever core asked last. Core 0 holds no extent lease, so `MayWrite`
  admits the write: two cores with divergent images of one page and nothing
  logged. Both paths now ask through one `CheckRangePlacement`, because two
  spellings of one refusal is two chances to forget one of them — which is
  what had happened.
- **The relation's own owner could not insert into a spread relation at
  all**, and this follows from the finding above rather than standing apart
  from it. Core 0 has no lease table, so `PeekRowId` answered "no id" and
  the router sent the statement to core 0 itself; the placement check then
  refused the row, retryably and forever, because core 0's next id *is* the
  mark and the mark is in the top range. The fix is that both allocators
  answer the peek: a leased core reads its block's cursor, core 0 reads
  `sys.tables.next_id`. Core 0 then routes to the top range's owner and
  ships, like any other non-owner of that range. The page read is paid only
  when ranges are armed **and** the relation already has a directory, so no
  unsplit relation reaches it.
- **The multi-row straddle refusal §14f handed to the operator is
  untouched**, and IS4 does not change what it refuses. It is still an
  implementation limit (`ChainAppendBatch` takes one head) surfacing as a
  user-visible error, and partitioning the run is still the proposal. What
  *did* change is the order it is asked in: ownership is answered first, so
  a run that both straddles a boundary and starts in a foreign range names
  the ownership rather than the straddle.
- **`peer_listeners = on` was refused with `placement = creating`, and R4
  falsifies the reason** (found by running the probe, not by reading). The
  gate's own words were *"every relation is core 0's, so a peer-accepted
  session could serve nothing"* — true while a peer's only answers were to
  ship or refuse, and false with ranges armed, where such a session takes a
  range of its own and serves it locally. That pairing is not a
  misconfiguration under spreading, it is the arrangement spreading exists
  to produce and the one IS7 has to be able to configure. The gate now
  narrows on `range_size_ids`; unarmed, and at `cores = 1`, the refusals are
  unchanged.

### 7b. What IS4 costs, stated rather than discovered

On a relation whose ranges have **different owners**, a write that names no
primary key is refused — it could touch every range, so it spans owners,
and that is R6's. A write naming a pk still runs, on the core owning that
key's range, and the walk narrows to that one range rather than meeting the
refusal over ranges it was never going to touch.

**The pk test is `PkEqualityTarget`'s, which admits exactly one condition**
— the point-statement fast path's own rule, reused rather than re-derived
so a statement cannot be routed one way and then scanned another. The
consequence is conservative in the safe direction: `WHERE id = 5 AND v = 3`
names a pk but is not a *bare* pk equality, so it is refused on a
multi-owner relation even though it can touch only one range. Widening it
means a predicate → `PkSpan` reduction that does not exist yet and that
`ResolveRanges` was originally specified to take (RD3's signature said
`predicate`, the build took `PkSpan`); it belongs with that reduction, not
here.

So arming spreading on a relation costs `UPDATE`/`DELETE` with a non-pk
predicate on it. That is a real loss against the unsplit relation, it is
the reason `range_size_ids` stays defaulted off, and it lifts at R6 with no
further design. A split relation whose ranges are all one core's keeps
every write it had — a split is not by itself a restriction, two owners
are.

**And it costs the caller-named key on that relation**, which is the
second half of the same statement and is not routing's to fix (§7c's C2).
Admitting a named key writes `sys.tables` — the high-water mark, or the
`key_order` flip — which is core 0's page; placing the row writes the range
owner's chain. A heap relation refuses a key below the mark, and a key at
or above it falls in the **top** range, which is whichever core leased
last. So on a multi-owner heap relation a named-key `INSERT` needs a write
on two cores at once, which is R6's shape rather than a destination a
router could choose. It is refused, and the refusal is honest.

### 7c. The review, and the one finding taken as documentation rather than code

`critics-developer` over `7b0ba61` plus the tree. Two live bugs, fixed
with it: **the sorted fill wrote a whole batch into another core's chain**
(§7a), and **`OwnsAnyRange` guarding the pump livelocked a spread relation
across a restart** — owning a range is durable (`sys.ranges`) and holding a
lease is not (`RowIdLeaseTable`), so after a mount a core owned a range,
held no block, recorded no demand because it "already owned a range", and
every INSERT from every core was refused forever. `NoteDemand` is
idempotent, so the guard bought nothing and cost that; it is gone, and
`OwnsAnyRange` with it, having no other caller.

Five further findings, four taken:

- **C1** (core 0 cannot insert into its own spread relation) — taken, and
  §7a records it. Found independently while writing this row; the review
  reached the same fix, which is the peek answering on both allocators.
- **C3** (disarming `range_size_ids` after ranges exist reproduces the
  livelock) — **taken**, where the review left it for a decision. It is not
  a D6 question: `HeapChainFor` and `VisitRelation` already honour a
  durable directory whatever the knob says, so a router that stopped
  honouring it would be the odd one out. The gates are now two and say
  different things — the *pump* asks the knob, because what it decides is
  whether a grant opens a new boundary; the *routing* asks
  `!ranges.empty()`, because that is a fact on disk. Disarming the key
  stops new boundaries; it cannot un-split what is written.
- **C4** (a pk literal above the 40-bit space became an error where it
  answered `UPDATED 0`) — taken. Such an id names no row, so it names no
  range; it falls through to the sole-owner rule instead of reaching
  `ResolveRanges`, whose refusal is aimed at a caller that computed a bound
  wrong rather than a user who typed one.
- **C5** (the rights probe asked the creation pages *or* the range head)
  — taken. They are not alternatives: a core can own the relation *and* a
  higher range, and a row landing there was admitted on a probe of a page
  it would not write. Both now, at most four page ids.
- **C2** (an `INSERT` naming its keys is never routed by them) — **not
  taken as code, taken as documentation**, and the reason is that routing
  cannot fix it. Admitting a caller-named key writes `sys.tables` (the
  mark, or the `key_order` flip), which is core 0's page; placing the row
  writes the range owner's chain. On a spread heap relation those are two
  different cores, because a heap relation refuses a key below the mark and
  a key at or above it lands in the top range, which is whoever leased
  last. So the statement needs a write on two cores — R6's shape, not a
  destination this row could pick. §7b names it as a cost.

**One cost C1's fix introduces, stated because IS7 will see it.** Core 0's
peek is a `sys.tables` read, and `AllocateRowId` is about to make the same
read — so a spread relation's INSERT on core 0 walks the catalog chain
twice. It is paid only where the relation already has a directory (an
unsplit one never reaches the call), and only on core 0, where a peer reads
its lease cursor instead. Collapsing the two would mean an allocator that
returns the id it is about to issue *and* the relation's row, which is a
wider change than restoring a route; IS7's numbers decide whether it is
worth making.

Of the simplifications: the pump/peek fold (S1) is taken and is what makes
the demand condition agree with `RowIdLeaseTable::Next()`'s; IS5's second
`RangesOf` scan (S2) is gone, reading `TableAccess::ranges` the fill has
already built; the two dead default arguments (S3) are dropped, so a fourth
write path cannot get "the whole relation" or "no row id" by omission; and
`SetRangeSizeIds` became `set_range_size_ids`, which is this file's
convention for a scalar knob. The comment-volume note (S5) is accepted in
substance — the review trimmed the one block that had become a second
telling after its own refactor.

## 8. What IS7 must measure, and the finding it will run into

**The headline number this phase exists for is already bounded by a
measured result, and IS7 must be designed around it rather than into it.**
`known-gaps.md`'s *"Rotation divides the group-commit batch"*
(`bench/v2.1.0/results-multicore-writers-v2.1.0.md` §6-§7, at `v2.1.0`)
found that spreading N writing sessions over W cores divides the group
committer's batch by W, and each core is then independently capped at the
volume's **single-stream** `fdatasync` rate — 965-1,071 commits/s per
writer core against a probe-measured 1,066/s. At two sessions per writer
core, rotation ran **0.989×** of not rotating at all.

Insert spreading moves the *work* to the core that owns the range; it does
not move the `fdatasync`. So on a durably-committed insert workload the
prediction is **no throughput gain, and possibly a small loss** — for a
reason that has nothing to do with ranges and everything to do with where
the commit batch forms. Reporting that as R4's number would be true and
useless.

The run therefore has to separate the two costs, which means at least two
durability arms:

- **Group-committed** (the shipped default): the honest end-to-end number,
  expected flat-to-slightly-down, and the finding it reproduces is
  v2.1.0's rather than this phase's.
- **Relaxed or unlogged**: what spreading actually buys, because it is the
  arm where the per-statement CPU and the shipping round trip are the cost
  and the `fdatasync` is not. Concentrated inserts on one owner pay
  statement shipping's flat ~20 µs of wire plus the owner's serialisation;
  spread inserts pay neither. That difference is R4's subject.

Both arms against the same concentrated control (today's shipping to the
relation's owner), `build-release`, interleaved A/B per `ck-tester`, at
k = 1..4 writer cores. The run must also report **the range count and the
stage count it produced**, which is §3's ceiling measured rather than
derived, and it must note whether the relation under test is core 0's —
§7a's third finding makes a peer-owned relation (k-1)-way, not k-way.

**The driver is `bench/spread_insert_probe.py`.** The existing
`tools/multicore_benchmark.py` measures N *non-interfering* relations, one
per core, which is the isolation question and is already answered; R4's
subject is several cores writing **one** relation. The arms differ in one
config key (`range_size_ids`) on one binary, crossed with `durability`, and
the probe also reports the **pump's client-visible cost**: how many retries
a peer's first INSERT takes before its range exists. That number is worth
having because `multicore_benchmark.py` classifies the cross-core write
refusal as *permanent* (`PERMANENT_TEXTS`) — which was correct and stops
being correct with spreading armed, since the refusal now leaves a demand
behind it.

**Two range counts are not observable from outside the process**, and that
is a gap this row hits rather than closes: nothing exposes how many ranges
a relation has (`sys.ranges` has no catalog view, and `SHOW META` carries
only the *decline* counters). The unit tests assert it directly; the probe
reports `rowid_refill_grants` per core as the proxy. Worth an observability
field if the range count ever needs to be read operationally.

## 9. R4-M's conclusions — CK1-CK5, measured

Work order `instructions/v2.6.0/r4-k-sweep.md`, run in worktree
`v2.6.0-ksweep` across `03b815b` and `5b37fec`; the file is
`bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md`, with
raw driver output beside it under `archive/r4m-ksweep/`. Host: Xeon
8488C, 8 logical CPUs on 4 physical cores, SMT active, ext4. Suite
3037/3037 and `scripts/sim.sh` 171/0 at the commit measured.

**CK1 — the shape, and what binds. Two constraints, not one.** Insert
throughput at k = 1..8, both durability arms, three interleaved reps:

  k  group C  group S   S/C   relaxed C  relaxed S   S/C
  1      890      873  0.981     40,234     40,315  1.002
  4    1,361    1,986  1.460     60,398     60,365  0.999
  5    1,727    2,606  1.509     69,859     71,246  1.020
  8    2,880    2,784  0.967     78,073     93,371  1.196

The **group** arm peaks at k = 5 and returns to parity by k = 8 - the
device plus this host's SMT pairing, reactors being pinned one per CPU
index (`expeditor.cpp`'s `CPU_SET(core_id)`) so k >= 5 puts each new one on
an existing physical core. The **relaxed** arm rises monotonically past
k = 5 to 1.196, and what it lifts is core 0 serialising every shipped
statement - measured independently at ~80k inserts/s by the control. **The
fan-in stage count, the order's third candidate, binds neither**: it binds
reads, and it is not what closes the read surface (§3a).

The control is `bench/spread_client_ceiling.py`, the sweep's own process
model: `PING` reaches 596k ops/s at k = 8, six to seven times the engine's
plateau, with `errors=0` on all 24 arms. No cell sits on the harness.

**CK2 — the group arm was not flat, and §8's prediction is refuted.** §8
expected flat-to-down on v2.1.0's commit-batching finding. The
concentrated group arm rises 890 -> 2,880 on its own and spreading adds up
to 1.51x on top. v2.1.0's result is not contradicted - it measured rotation
of *independent* sessions and this is one relation - but the inference §8
drew from it, that a durably-committed insert workload cannot gain from
spreading, does not survive.

**And the two arms do not track each other**, which corrects IS7 directly.
IS7 found `group` and `relaxed` agreeing to 0.2% at k = 2 and called that
the finding; across the sweep they diverge in both directions (k = 4: 1.460
against 0.999; k = 8: 0.967 against 1.196). **The agreement was a
coincidence of k = 2.**

**CK3 — the ceiling, measured, and §3's arithmetic holds.** The refusal
fires at 65-72 stages, at 32,025 / 64,730 / 234,776 rows for block sizes
512 / 1,024 / 4,096 against an arithmetic of 32,768 / 65,536 / 262,144.
`ids/stage` is 512 / 1,056 / 4,277 at k = 4, so **stages equal blocks to
within 4%**.

**With a precondition §3 did not state: two or more peers must contend.**
At one peer (k = 2 under `rotate`) IS5's top-owner suppression fires on
every carve after the first, the relation settles at two ranges, and the
ceiling was **not reached after two million rows** at any block size. HK4
is refuted in its strong form - suppression *does* bound a contended
relation, when exactly one peer contends.

**And the ceiling is not what bounds a spread relation's readability**;
§3a is that finding, and it is this milestone's largest. **Superseded
2026-08-29 by R4-R and RS** (§10, §11): both limits §3a names are closed,
so the 64-stage arithmetic above is once again what binds — and it now
counts the reading core's **own** run as a stage like any other (§10b).
`docs/inflight/known-gaps.md` carries the same correction; the numbers in
this section were measured before it and stand as measured.

**CK3's other side, the burn.** `next_id - 1 - rows placed` is one to two
lease blocks per contending core per mount: 3,678 ids at k = 8 / 512
(7.2 blocks, seven peers), and about thirteen blocks at 4,096 nearly
independent of k, consistent with a refill landing before its predecessor
is spent and not attributed further. **A restart adds nothing to it** - a
live block's remainder was charged to the mark when it was carved, so §3's
*"a restart burns every live block"* double-counts; what a restart costs is
the re-carving after it, which is the same one to two blocks again.

**CK4 - the scenario benches do not fit, and not for a size reason.** Six
of twenty-four scenario relations spread, and all six lost **all five**
read shapes their drivers use, at ~395 rows. §3a carries it. The aggregate
cell is **reported as not run**.

**CK5 - the aggregate says nothing, because it could not be run.** The
question the line has been deferring - does the engine go faster with more
cores - is answered for *writes* by CK1 and is **unanswered end to end**,
because a spread relation cannot be read back by the workloads that write
it. That is a stronger reason than "the number was bad", and it is the
reason the mover (R5) should not be designed against a scenario premise
yet.

### 9a. D6's value, handed over with a sweep behind it

RD9(b) re-run at k = 4 with the large end reachable, ceiling from §6's
measurement where measured and arithmetic above 4,096:

  range_size_ids   group S/C   relaxed S/C   read ceiling      burnt (k=4)
             256       1.333         0.434   ~16,384 (arith)             -
           1,024       1.401         0.809   64,730 (measured)       3,814
           4,096       1.470         1.025   234,776 (measured)     56,018
          16,384       1.296         1.001   ~1,048,576 (arith)          -
          65,536       1.339         1.031   ~4,194,304 (arith)          -

**The new finding is at the small end.** A small block does not only lower
the ceiling, it **costs throughput outright** - 0.434x at 256 and 0.809x at
1,024 on the relaxed arm, because a block that small is spent as fast as it
is granted and every exhaustion is a round trip to core 0 plus a client
retry. §3 framed the knob as ceiling against burn; **there is a third axis
and below 4,096 it dominates both.** So: below 4,096 is a loss on every
axis; 4,096 is the throughput optimum on both arms; above it costs ~10% of
the group arm's gain per 4x of ceiling. The value is the operator's; this
is the curve it is taken on.

## 10. RR0 — the design answer §15d deferred

Work order `instructions/v2.6.0/r4-r-readable-surface.md`, written in
worktree `v2.6.0-ksweep` at `949a7d4` **before RR1's code**, and grounded
in a throwaway experiment rather than in reasoning alone — the experiment
is named below because it is what makes this a design *answer* rather than
a design *proposal*.

`workplan-range-directory.md` §15d deferred the self-directed stage as *"a
design question and not this row's."* The answer is smaller than the
deferral implies.

### 10a. A self-directed stage is not a new mechanism, and the message stays

HR1 guessed it would be *"the fan-in's existing shape minus the message."*
**Half right, and the wrong half is the interesting one: the message does
not go away, and it does not need to.** The ring already carries a stage a
core opens against itself — `core_runtime_test.cpp`'s two-step pipeline
forwards an enclosed leaf open to its own core and its comment says so
outright (*"self-sends are the same protocol"*), and `expeditor.cpp`
constructs core 0's own `RemoteStepServer` for exactly this, with the
comment *"a stage placed on a relation core 0 owns is served here, like any
peer serves its own … producers and consumers land on core 0's one
reactor."*

So the producing half of a self-directed stage was built and wired at
P4d-4b-3. What has never been reachable is a **plan that asks for one**.

**The whole of it is one predicate.** `HandleSelect`'s fan-in route is
guarded by, among the shape tests, `owner_access->owner_core != core_id_`
— *"is this relation someone else's"*, which is the question the route was
born asking when it meant "ship this read to the owner". RD7 generalised
the route to many stages and left that predicate alone. The question it
should ask is **"can this read be served by a purely local walk"**, and
under `placement = creating` — where every relation is core 0's — the two
answers differ for exactly the relation this milestone is about.

*Verified before writing this section*: replacing that one predicate with
`!WhollyOwnedBy(core_id_)`, on a 4-core instance with `range_size_ids =
512`, `placement = creating` and 1,200 rows spread across the boundary, a
**core-0 session reads the relation** — `SELECT *` (10,855 bytes),
`WHERE id = 1`, `WHERE v = 1` and `ORDER BY id ASC` all answer, where every
one of them was refused before. No other line changed.

### 10b. What it does to the stage count (HR4's subject)

**Nothing.** A self-directed run is still one stage: `stages` is built
from the range list before any owner is consulted, and
`stages.size() > kMaxFanInUpstreams` is tested on that list. A local run
costs an upstream slot exactly as a remote one does, because it *is* an
upstream — opened, credited and closed through the same protocol, with the
ring hop being a self-send.

So HR4's falsifier does not fire and §8's first two ways out are **not**
re-priced. Making the local run message-free would change that — it is a
real optimisation and it is **not this order's**, because it would replace
a protocol path with a second one and RB4's whole lesson was that two
spellings of one thing is two chances to forget one of them.

### 10c. The predicate has to be written as a disjunction, not as `WhollyOwnedBy`

The probe above is not the shipping form, and the difference is a
correctness one. `WhollyOwnedBy(me)` is `owner_core == me` when the range
list is empty and *"every range is mine"* when it is not. So a relation
whose `owner_core` is another core but whose ranges had all become this
core's would answer **true** and be sent down the local path — where
`CheckReadAffinity`'s `owner_core != core_id_` arm refuses it outright.
That state is unreachable today (CC9 gives the `lo = 0` anchor to the
owner, and no mover exists to move it), but a route predicate that is
correct only because of a neighbouring invariant is the shape this
milestone has been caught by twice. RR1 writes the question it means:

    servable_locally = owner_core == core_id_ && WhollyOwnedBy(core_id_)

and takes the route when it is false.

### 10d. What this does not answer

**The peer half is a different problem and RR2 owns it.** With the
predicate fixed, a core-0 session reads a spread relation and a **peer
session still cannot** — it has no `remote_reads_` at all, so it falls
through to statement shipping, ships the read to core 0, and the reply is
lost above some size: measured, a peer's `SELECT * FROM spread` over 1,200
rows answers `ERR UNKNOWN_OUTCOME retryable=0 statement shipping: the
statement executed on its owner but its reply …` while the same session's
`WHERE id = 1` answers normally. That failure is worth its own line
regardless of RR2: **`UNKNOWN_OUTCOME` is the wrong category for a read.**
It exists because a write whose reply is lost may or may not have
committed; a read mutates nothing, so there is no outcome to be unknown
about and the honest answer is a retryable refusal or a bigger reply.

## 11. RS0-RS5 — the residue of R4-R, and the conclusions it did not owe

Work order `instructions/v2.6.0/spread-realation.md` (rows **RS0-RS5**),
opened in worktree `spread-relation` against `main` at `949a7d4` and worked
on top of `7eeb7b5`.

**Most of its scope had already landed when it opened, and that is recorded
rather than re-derived.** R4-R (`instructions/v2.6.0/r4-r-readable-surface.md`,
rows RR0-RR5 at `5b62ac3` with its review at `7eeb7b5`) is the same
milestone under a different order, drafted a few hours earlier. The two
overlap almost exactly:

| RS row | what it asks | state on `7eeb7b5` |
|---|---|---|
| RS0 | §15d's design answer in the workplan, before code | **landed** as §10 (RR0) |
| RS0 | the width comment (§2) and `core_affinity.cpp`'s stale message corrected | **not done** — §11a, §11b below |
| RS1 | the fan-in client on every core (HS1, HS2) | **landed** as RR2 |
| RS1 | **CS3**, absent versus zeroed | **not asked by R4-R** — §11c |
| RS2 | the self-directed stage (HS3), **CS2** | **landed** as RR1; CS2 answered at §10b |
| RS3 | `CheckReadAffinity`'s refusal narrowed | **landed** at `7eeb7b5` as `TableAccess::ServableBy` |
| RS4 | **CS4**'s enumeration against the equivalence suite | **landed** as RR3 (`bench/spread_read_surface.py`) |
| RS5 | the equivalence gate **from a non-zero core**, with its vacuity matrix | **not done** — §11d |
| §9 | the ceiling arithmetic updated wherever it is carried | **not done** in `known-gaps.md` — §11e |

So this section is the residue: two stale sentences, one conclusion nobody
asked for, one gate, and the register entry. Nothing here rebuilds a
mechanism R4-R built, and where RS's own hypotheses were already settled by
RR0-RR3 they are cited rather than re-run.

### 11a. The width comment was the argument for a constant that had become a limit

`remote_step_service.hpp`'s note on `kMaxFanInUpstreams` read:

> the width is the number of distinct **owner cores**, never the number of
> boundaries. A 10 M-row relation at D6's size has ~2,441 ranges and at
> most `cores` stages, which is the difference between a plan and an
> absurdity.

**False under R4, and R4-M measured it false**: interleaved spreading makes
a maximal run one range, so the width is the range count, and the refusal
fired at **65-72 stages** with `ids/stage` matching the block size to
within 4% (§9's CK3). The bound of 64 is still safe for the one-byte wire
count; what is gone is the sentence that made it comfortable, which is the
kind a later reader reasons from — and would have reasoned from to conclude
that no realistic relation can reach it.

Corrected in place to state the width as **the number of maximal same-owner
runs** — the core count only under contiguous ownership, the range count
under interleaved — with R4-M's measurement and R4-R §10b's slot cost for a
self-directed run named beside it.

### 11b. And `core_affinity.cpp` said a built pipeline was not built

`CrossCoreReadUnsupported` — the refusal a client actually sees — ended
*"cross-core reads need the step pipeline, which is not built"*. The
pipeline has existed since RD7 and, since RR2, on every core. What is true
is narrower, and the message now says it: the route serves **a whole-row
read of one relation outside a transaction**, and a statement reaching that
line is not that shape.

Two live drivers matched on the old spelling and move with it:
`bench/refusal_baseline_probe.py`'s `cross_core_read` class key, and
`bench/parked_coroutine_probe.py`'s explanatory quote — which keeps its
argument and gains a note that the run it reports met the older wording.
A results file is never edited; the two changed files are drivers.

### 11c. CS3 — absent, not zeroed

**The question R4-R did not ask.** Since RR2 every core holds a client, and
`WireStepEndpoints` hands it every `kStepBatch` and `kStepEof` the core
receives — *including the ones its own server half is producing for someone
else's fan-in*, which is the entire step traffic of a peer that has opened
nothing. Core 0 has always paid this; the order's question is whether a
peer should pay it too, or skip.

**It skips.** `SessionStepClient::OnStepBatch`/`OnStepEof`/`OnStepError`
each return immediately when `reads_` is empty. This is behaviour-identical
by construction — `Find` iterates `reads_` and every handler returns on
`nullptr` with no side effect before the lookup — so it removes a decode
whose answer is known, and nothing else. The repository's own
absent-rather-than-zeroed discipline, applied where it had not been.

**Its cost is below this host's measurement floor and is reported as not
measured, not as zero** (§7's S-c): the work removed is one bounds check
and a ~24-byte header `memcpy` per message, and the two-CPU host these rows
were worked on cannot resolve that against a reactor's own variance. The
argument for the change is the source-read one above, which does not need a
number.

### 11d. RS5 — the gate from a non-zero core, and what the vacuity matrix found

RR5's `ACoreReadsARelationItOwnsButDoesNotWhollyHold` reads from **core 0**,
which is where the fan-in client has always lived. RS5's case is the one RR2
created and nothing covered: a **peer** opening a fan-in of its own, one
stage self-directed (core 2 asking core 2) and one remote.

Two tests, because one could not gate both halves:

- **`APeerReadsASpreadRelationThroughItsOwnFanIn`** — the loopback rig, a
  peer dispatcher, byte-identical against the same rows unsplit and
  straddling the boundary, plus a count of opens to self versus to the
  owner. It gates the *plan and the answer* from a non-zero core.
- **`APeersOwnDispatcherPlansAFanInRatherThanRefusing`** — because the rig
  above hands the dispatcher a client it built itself, so it would pass on a
  tree where `CoreRuntime` never constructs one. This one uses a peer's own
  transport and own client, and reads the verdict off **which refusal comes
  back**: the synchronous `Dispatch` cannot finish a fan-in either way, so
  it closes its stages and answers `TxnConflict("remote read needs the
  reactor path")` — a refusal reached only *after* the route resolved the
  ranges and opened the stages. Without RR2's wiring the answer is
  `CheckReadAffinity`'s `Unsupported` instead, which is what every peer
  answered before it.

**The vacuity matrix** (§6's requirement: revert each mechanism, count what
catches it, report the count). Four reversions, each rebuilt in
`build-release` and run against the four tests that could plausibly catch
one — RR5's, RS5's two, and RD7's `AFanInOverInterleavedOwnershipStillAnswersInRangeOrder`:

| reversion | what it undoes | caught by |
|---|---|---|
| **R1** | `ServableBy` loses its conjunct: back to `owner_core == core_id` | **1** — RR5's `ACoreReadsARelationItOwnsButDoesNotWhollyHold` |
| **R2** | the stage loop skips runs this core owns: **no self-directed stage** | **2** — RR5's *and* RS5's `APeerReadsASpreadRelationThroughItsOwnFanIn` |
| **R3** | a peer's dispatcher never learns about its client (pre-RR2) | **1** — RS5's `APeersOwnDispatcherPlansAFanInRatherThanRefusing` |
| **R4** | CS3's `reads_.empty()` guards removed | **0** |

Four readings, and the fourth is the one worth stating:

- **R1 is caught by RR5's test alone, and RS5's do not catch it** — which is
  correct rather than a hole. RS5's fixture reads from a core that is not
  the relation's `owner_core`, so `owner_core != core_id_` is already true
  and the route is taken under either predicate. RR5's fixture is the only
  one where the two predicates disagree, exactly as its own note says.
- **R2 is the reversion both equivalence tests catch**, each losing the half
  of the rows on its own side of the cut — RR5's the low range, RS5's the
  high one. That is the straddle discipline doing what it is for.
- **R3 is caught only by the test written for it**, and by construction: the
  other two hand the dispatcher a client they built, so a tree where
  `CoreRuntime` builds none is invisible to them. This is why RS5 is two
  tests and not one.
- **R4 is caught by nothing, and that is the correct count, not a gap.** The
  guard is an early return on a condition under which every later line is a
  no-op, so no behaviour can distinguish the two trees. Reported as a
  measured zero for a reversion whose zero was predicted from the source —
  which is the one case where "nothing caught it" is a pass.

`AFanInOverInterleavedOwnershipStillAnswersInRangeOrder` caught none of the
four: its reading core owns no run of the relation, so it exercises the
all-remote fan-in and nothing this order touches.
