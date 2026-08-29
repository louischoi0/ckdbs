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
| **IS7** | **Measure, and hand D6 its constraint.** Insert throughput at k writer cores, spread against concentrated (today's shipping), crossed with `durability` per §8, `build-release`, per `ck-tester`; plus the range count the run produced, which is §3's ceiling as a measured number rather than an arithmetic one. Driver: `bench/spread_insert_probe.py`. Results to `bench/v2.6.0/` naming `git describe --tags`. **k is bounded by the host**: this box has 2 CPUs, so a 1..4 writer-core sweep is not honestly runnable on it and what cannot be run is reported as not run, never as a pass | IS6 |
| **IS8** | **The documentation §6b asks for by name.** *"Per-relation id monotonicity becomes per-range monotonicity — invariant 11's 2026-08-11 amendment one level down, and it needs the same loud documentation when built (R4)."* `heap-and-tuple.md` §4.1, invariant 11 in `CLAUDE.md`, `crosscore.md` §6b's status, and `known-gaps.md` for §3's ceiling and IS4's cost | IS6 |

**Not in this phase**: the mover (R5); multi-range transactions (R6);
merge; the stripe alternative to D6; raising `kMaxFanInUpstreams`;
`CREATE INDEX`/Cabin/assertion/FK on a split relation (each declines, and
the converse gates RD5 built are what decline them).

## 6. Where to pick this up

At `86f2052`, nothing of IS1-IS8 was built. **IS1-IS3 landed at `7b0ba61`**
in worktree `v2.6.0-insert-spreading-1` (§7). **IS4-IS6 and IS8's doc half
follow it in the same tree.** What remains is **IS7**, the measurement,
whose design constraint is §8 — the group-commit finding bounds the
headline number before the run starts, so the run has two durability arms
or it reports v2.1.0's result under R4's name.

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
