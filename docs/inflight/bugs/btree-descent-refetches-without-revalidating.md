# A btree insert's descent re-fetches its leaf for write and re-validates nothing

**Found** 2026-09-09 by the `critics-developer` pass on AT-S5
(`091be8c` on `m3-at`), as its D3. **Not fixed.** Verified at `091be8c`;
the fix's stage is **AT-S5c**, the first sub-stage after AT-S5b, because
this is the defect with the widest reach.

**Cost: a quiet wrong answer, and the widest exposure of the seven.** Any
two sessions on two cores inserting into one btree relation reach it; no
DDL, no assertion, no foreign key is needed. A row lands on a page that no
longer covers its key, which is invariant 3's statement (`no tuple with
id < min_key(page)` — and, on the other side, no tuple above the range the
parent's separator routes to that page) read from the direction the spec
does not spell out.

## The wrong behaviour

`DescendTo` (`src/storage/btree/btree.cpp:73-116`) drops and re-takes the
leaf on exactly one condition: it has arrived at a leaf and `leaf_for_write`
is true (`:87-88`) — every `BtreeInsert` descent, never a lookup. It
releases the shared read handle at `:103`, because the page latch is never
upgraded (AM-S1) and asking exclusive while holding share would self-
deadlock, then re-`Get`s the same page for write at `:104` and moves the
new ref straight into `d.leaf` at `:106`.

**Between those two lines the page is unheld.** After them nothing is
checked: not `IsLeafPage`, not `RequireType`, not `min_key`, not
`next_page_id`, not the separator that routed the descent here. The only
coverage test on the insert path is `BtreeInsert`'s lower bound at
`:625-630`, which catches a backwards id and says nothing about a page that
split its upper half away.

The justification is written at `:100-102`: *"Nothing can evict between the
release and the re-fetch on a single-threaded core, which is the argument
the comment above already makes."* AT-S5 removed that premise — a write
runs where the session is, so two cores' writes reach one relation — and
the header still states the old one: `include/kds/storage/btree/btree.hpp:75-80`,
*"there is no latch coupling and no B-link right-link protocol, because
there is no concurrent mutation to protect against yet - the server is one
cooperative thread and nothing here suspends"* (weaker form at
`btree_page.hpp:46`).

## Reproduction

Two cores, one btree relation, interleaved caller-named keys chosen so one
core's insert divides the leaf the other core's descent has just released.
Not yet written as a cell: it needs the barrier-plus-rounds shape, because
one run of an interleaving proves nothing.

## Related, and resting on the same premise

`btree.cpp:745-756` deliberately deleted a second re-fetch on the split
path — *"The re-fetch that used to stand here guarded against a frame move
a held pin already forbids"* — and writes `set_next_page_id` through the
descent's own handle. That is sound under a held pin and still assumes the
leaf is the leaf.

## Owner

`instructions/v3.0.0/workorder-at-m3-uniformity.md`, AT-S5c.
`docs/spec/heap-and-tuple.md` §5 (the separator-then-link publication order)
and §6 (the page-latch contract, which AM-S1 already made cross-core) are
the two sections the fix has to agree with.
