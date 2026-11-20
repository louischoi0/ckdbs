# Work order DK - a btree descent that leans on the immutable low key: one fetch fewer for a write, a leaf that moves right instead of restarting, and the internal levels read optimistically only under CW's protocol

Status: **WRITTEN 2026-11-20, not opened.** Written in the main checkout on
`main` at `1c7bb774` (`v3.4.15-4-g1c7bb774`) on the operator's words of
2026-11-20 (`raft-marks-2026-11-20.md` §7). **Exploratory is the
operator's word** (DK-Q0). The letter DK and every candidate are CLA's
proposals. No stage is started and no DK-Q item but DK-Q0 is marked. Every
`[source-read]` claim below was read at `1c7bb774`. No suite was run: the
order changes no build input.

**Exploratory, and the measurement comes first.** DK-S0 can close the
order with nothing built, and that is a result, not a failure.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` where a wrong choice turns a
correct answer into a wrong one with no error.

Related: `instructions/v3.2.0/workorder-cw-optimistic-page-read.md` (the
page-read protocol DK's D3 depends on, its C3a, its rules CW-R1 to CW-R9);
`instructions/v3.2.0/workorder-dd-index-entry-purge.md` (the one order
that would remove what the secondary tree's bound rests on);
`docs/spec/heap-and-tuple.md` §5 (the descent and its coverage check);
`docs/spec/index.md`; `docs/spec/page.md` §3, §6; `docs/rules/rules.md` §3;
`include/kds/storage/btree/btree.hpp` (the concurrency account).

## 0. What DK is

`[operator]` *"Is there room to optimize, using the property that a
page's `min_key` is immutable, so that a btree descent goes down without
holding a latch or a lock? When the page has been split it would have to
retry."* and, after CLA's reading of the tree, *"For now, put it as an
exploratory milestone and write the work order."* (given in Korean;
recorded in English.)

`[design]` What CLA told the operator, and what DK takes from it:

1. **The immutable low key is already the descent's arbiter.** There is
   no latch coupling: an internal node is read under a shared hold and
   released before its child is asked for, and what makes the leaf right
   is the coverage check `[min_key, right sibling's min_key)` asked of the
   chain as it stands (§1). So the internal levels' holds protect the
   *bytes being read*, not the route. That is the premise an optimistic
   read of the internal levels needs.
2. **The low key alone does not make a latch-free read sound.** A node
   being divided can be read torn; an id read from it can name any page;
   a plain read beside a writer is a data race by the language's rule.
   A version that validates the read is still owed. CW is the order that
   owns that protocol, and its C3a is exactly "a descent's internal nodes
   read with no pin and no latch". **DK does not write a second
   protocol** (DK-Q1).
3. **The holds are not where the descent's cost is likely to be.** A
   fetch takes the instance's one structure latch, and a page's latch
   word is touched by compare-and-swap; CW §1.2 counted three structure
   latch holds and two compare-and-swaps a page. Taking the page latch
   away and keeping the rest returns little; that is CW's C1 against C2
   and C3, and its S0 and S1 price them.
4. **What the immutable low key gives that CW does not use**:
   - a split moves keys only to the right and never rewrites a low key,
     so a descent that reaches a leaf too far left can **move right along
     the chain** instead of descending again from the root, and needs no
     coupling to do it - the lower bound it proved at the left page stays
     proved after the left page is released (D2);
   - with D2, an optimistic descent whose validation fails can **restart
     at the node that failed** rather than at the root, since any
     resulting stray to the left is caught and corrected at the leaf (D3's
     restart granularity, DK-Q6).
5. **One saving needs no optimism at all**: a write descent fetches its
   leaf shared, releases it and fetches it again exclusive, because it
   learns the page is a leaf only by reading it. The parent's stored
   `level` already says so (D1).

What DK is not: a B-link tree. No internal node gains a right link or a
low key, nothing on disk moves, and no structure change becomes visible
across levels (§6).

## 1. The tree as read `[source-read]` at `1c7bb774`

- **The concurrency account.** *"There is still no latch coupling and no
  B-link right-link protocol: a descent never holds two pages at once,
  and an internal node is read and let go before its child is asked
  for"* (`include/kds/storage/btree/btree.hpp:94-100`). A merge or
  rebalance would need the coupling the file does not have, *"and none
  exists"* (`:104-110`).
- **The descent.** `DescendTo` (`src/storage/btree/btree.cpp:155-234`)
  fetches every level by `GetForRead` (`:170`), and the internal node's
  `ChildFor(key)` picks the child (`:210`). At a leaf, a read descent
  returns the shared hold (`:173-176`); a write descent **releases it and
  fetches the same page again exclusive** (`:177-185`) - the latch is
  never upgraded - then asks coverage under the exclusive hold
  (`:196-201`). On a failed check it starts again **from the root**, at
  most `kMaxDescentRestarts + 1` times
  (`include/kds/storage/insert_placement.hpp:66`, `= 4`), then refuses
  `TxnConflict` (`btree.cpp:225-233`, tally `kRefusalBtreeDescend`).
- **The coverage check** (`btree.cpp:73-113`). A leaf covers
  `[min_key, right sibling's min_key)`; both ends are immutable, so the
  interval changes in one way only - a page spliced in on the right.
  Only the upper end is asked; a key below `min_key` is a caller whose
  id sequence went backwards, reported `OutOfRange`, not a race
  (`:82-85`). The rightmost leaf costs nothing (`:107`); a mid-chain
  leaf pays one resident-page read of its right sibling, taken shared
  and released in the order leaf-then-right (`:91-97`).
- **The read lookup.** `BtreeLookup` (`btree.cpp:1195-1233`): a hit is
  authoritative whatever the chain did since (the row is there); a miss
  asks coverage, and a failed one descends again from the root, with its
  own bound and tally (`kRefusalBtreeLookup`). `BtreeSeekLeaf` asks no
  coverage on purpose: a seek outrun by a split lands to the left of the
  key and the forward scan reaches the right page next (`:1258-1268`).
- **The stale root.** The refusal's own text names it as the everyday
  second cause: a descent from a root read before the tree grew a level
  lands in what is now the leftmost subtree every time, so a key outside
  that subtree exhausts the bound with nothing racing (`btree.cpp:217-224`).
  A restart from the same root cannot cure it; only a task boundary's
  `Revalidate()` does.
- **The low key never moves.** A division moves the upper part out and
  leaves the old page's `min_key` untouched; the new leaf's `min_key` is
  the split key (`btree.cpp:742-746`, `:841-843`). A compaction before a
  split keeps `min_key`, the sibling link and `grown_over` (`:656-702`).
  A separator is the low key of the subtree it points at, literally the
  leaf's `min_key` copied up (`include/kds/storage/btree/btree_page.hpp:28-36`).
- **Nothing removes a leaf.** Nothing turns a leaf into an internal node,
  and a freed id is reused only once its relation is dropped and no
  statement can reach it (BF; `btree.cpp:186-191`).
- **An internal node has no low key and no right link.** Its header is
  `flags`, `nr_entries`, `level`, `leftmost_child` (`btree_page.hpp:54-70`).
  So a move right exists at the leaf level only.
- **`level` is stored so that a descent need not read the child to know
  it is a leaf** - *"reading the child's header byte to find out costs
  the page fetch this field avoids being wrong about"*
  (`btree_page.hpp:57-63`). `DescendTo` does not use it: it reads the
  fetched page's type (`btree.cpp:42-44`, `:170`).
- **The secondary tree** has its own `DescendTo`
  (`src/storage/index/index_tree.cpp:130-199`, tally
  `kRefusalIndexDescend`) with the same release-and-refetch and the same
  restart. **Its leaf stores no low key**: coverage compares against the
  right sibling's first entry (`index_tree.cpp:75-95`), and that is
  sound *"because nothing removes an index entry today"*
  (`index_tree.cpp:60-64`). Its internal header also stores `level`
  (`include/kds/storage/index/index_page.hpp:172-177`). DD's proposal
  keeps a leaf's first entry (`workorder-dd-index-entry-purge.md` §0).
- **A fetch's cost.** `FetchAndPin` holds the structure latch for the
  frame lookup and the pin (`src/storage/device_page_store.cpp:2595`
  onward), the page latch is waited for after it (`AcquirePageLatch`,
  `:2896`), and `UnpinFrame` gives both back (`:2971`). CW §1.2 counted
  three structure latch holds and two compare-and-swaps a resident page
  at `7aa75760`; DK-S0 re-reads that count at its own commit.
- **Reach.** 24 lines outside `src/storage/btree/` name `DescendTo`,
  `BtreeLookup` or `BtreeSeekLeaf`; the secondary tree's descent has its
  own callers in `index_tree.cpp` (`:372`, `:585`). DK-S0 lists both.

**Not measured, and it is the premise:** how often a descent restarts at
all (only the refusals are tallied, not the restarts below the bound);
how often a stale root is the cause; what share of a `point` and an
`insert1` statement the descent's fetches are; whether a restart's
second descent costs anything a statement can feel.

## 2. The candidates `[design]`

Priced by DK-S0; none is chosen here.

- **D0 - nothing.**
- **D1 - the write descent fetches its leaf exclusive at once.** At an
  internal node whose `level` is 1, the child is a leaf, so a write
  descent asks for it with `Get` directly and never takes it shared. The
  self-deadlock the release exists to avoid (`btree.cpp:178-183`) cannot
  arise, since no share is held. The coverage check is unchanged. The
  type check stays, now as a check that `level` did not lie. Both trees.
  Removes one fetch and one release from every write descent. **No
  optimism, nothing validated late, and it applies at every core count.**
- **D2 - a leaf that moves right instead of restarting.** When the
  coverage check fails at a leaf - a write descent under its exclusive
  hold, or a read lookup's miss - the descent releases the leaf and takes
  its right sibling in the same mode, and asks coverage again, up to a
  hop budget (DK-Q4); past it, today's restart from the root.
  - *Why no coupling is needed.* The descent reached the right sibling
    because `key >= right.min_key`, and `right.min_key` cannot move, so
    the lower bound is still proved after the left page is released. The
    upper bound is asked under the hold on the page it ends at. So the
    leaf returned satisfies `min_key <= key < right.min_key` under its
    own hold, which is what `DescendTo` promises today.
  - *Progress.* Each hop strictly raises `min_key` and nothing removes a
    leaf, so a walk right ends at the rightmost leaf at the latest.
  - *Order.* A hop holds nothing while it asks for the next page, and
    never asks for a page to the left or below; the coverage read keeps
    its leaf-then-right order. DK-S2 writes the latch-order argument
    against `page.md` §6 and against `SecureParents`' bottom-up holds
    before any code.
  - *The stale root.* A descent from a pre-growth root lands in the
    leftmost subtree and today exhausts its bound; under D2 it walks
    right, and the budget decides how far (DK-Q4). Within the budget the
    statement is answered where today it is refused.
  - *The secondary tree.* Its bound is the sibling's first entry, which
    is fixed only while no entry is removed (DK-Q5).
  - *What does not change.* A read hit stays authoritative; a seek still
    asks no coverage; a split's write side and its `SecureParents` are
    untouched.
- **D3 - the internal levels read optimistically: CW's C3a, with the
  restart moved down.** The protocol - the version word, the sites that
  move it, the pin-less slot lookup, the copy routine, the `tsan`
  treatment, the fallback - is CW's (CW-S2) and is not restated here.
  DK adds two things:
  - *The leaf remains the arbiter.* A validated read of an internal node
    is exact only at the instant it was validated; a split after it can
    still leave the descent one page to the left. That case exists today
    under the latched descent and is closed the same way: by the leaf's
    coverage check under its hold, or by D2's move.
  - *Where a failed validation restarts.* A node that failed can be
    re-read in place, in either case: re-read after a division of its
    own, it routes the key at worst to a child left of the right one, and
    the leaf's check catches that. Without D2 the catch is a restart from
    the root, so re-reading in place saves nothing over CW's restart from
    the root; with D2 the leaf level walks right instead (DK-Q6).
  D3 is built only through CW: if CW does not open, or its V0 or V1 is
  not met, D3 closes with it (DK-Q1).
- **D4 - rejected on paper: the low key as the only validation.** Read
  the internal levels with no latch and no version, and trust the leaf's
  coverage check to catch whatever a torn read produced. Recorded so it
  is not proposed again. It breaks CW-R1 (an id read from an unvalidated
  node is fetched) and CW-R2; it reads beside a writer with no
  synchronisation, which is undefined behaviour whatever is checked
  after; a torn `ChildFor` can name an unallocated page (a device read on
  the statement's reactor) or another relation's page, and the type,
  owner and level checks that would reject those turn a race into a
  `Corruption` that cannot be told from a real one. The coverage check
  proves a leaf right; it does not make a wrong fetch harmless.

## 3. Rules

Binding from DK-S1 on, if the order reaches it.

- **DK-R1 `[quiet-wrong]` - the leaf a descent returns covers the key,
  asked under that leaf's own hold.** `min_key <= key < right.min_key`
  for a write descent and for a read lookup's miss; a read hit stays
  authoritative as today. No candidate weakens this to "covered when it
  was routed".
- **DK-R2 `[quiet-wrong]` - no bound and no child id is used from a page
  that is neither held nor validated.** CW-R1 for the descent, stated
  for D2's hops as well: a hop reads `next_page_id` and `min_key` only
  under a hold.
- **DK-R3 - a move goes right only.** No candidate asks for a page to the
  left or below while it holds one, and a hop holds nothing while it asks
  for the next page.
- **DK-R4 - a key below the low key stays a caller's error.** Under
  latched or validated routing, a descent cannot arrive at a leaf whose
  `min_key` is above the key, so `OutOfRange` keeps its meaning and is
  not made retryable.
- **DK-R5 - the premise is written where a later order will meet it.**
  D2 and D3 hold only while no leaf is removed, no `min_key` is
  rewritten, and - for the secondary tree - no leaf's first entry is
  removed. `heap-and-tuple.md` §5 and `index.md` say so, so that a merge,
  a rebalance or DD reopens DK's parts by name.
- **DK-R6 - the latched descent stays and is the fallback** (CW-R4). D2's
  budget spent, the restart; D3's failures spent, the latched read.
- **DK-R7 - D3 is armed only above one core** (CW-R5). D1 and D2 run at
  every core count; D2 fires only on a failed coverage check.

## 4. Items for the operator (DK-Q)

(a) is CLA's proposal in each; only DK-Q0 is marked.

| | item | (a) | other |
|---|---|---|---|
| DK-Q0 | Is DK exploratory - may DK-S0 close it with nothing built? | **yes - marked, the operator's word** (`raft-marks-2026-11-20.md` §7) | |
| DK-Q1 | DK and CW. | DK stands apart; D3 *is* CW's C3a and is built only under CW's protocol, its gates and its stages; DK-S0's descent cells are the ones CW-S0's M1 and M4 need, and either order's run may serve both | (b) fold DK into CW as an amendment; (c) DK takes C3a out of CW |
| DK-Q2 | D1's bar. | built if DK-S0 confirms the second fetch on every write descent; no speed bar, since it removes work; the close must show no cell slower | (b) the same bar as D2 |
| DK-Q3 | D2's bar. | built if DK-S0 finds restarts at or above 0.1 % of descents in any multi-core cell, or any `kRefusalBtreeDescend`, `kRefusalBtreeLookup` or `kRefusalIndexDescend` refusal in the standard cells; else D2 closes with its numbers | (b) built on the stale-root argument alone; (c) the operator's numbers |
| DK-Q4 | D2's hop budget. | 8 hops, then today's restart from the root, then today's refusal | (b) another number; (c) bounded by `base/cycle_guard.hpp` alone |
| DK-Q5 | D2 on the secondary tree. | yes, in the same stage, while DD stays unbuilt or keeps a leaf's first entry; DK-R5 names it | (b) the clustered tree only |
| DK-Q6 | D3's restart granularity. | the node that failed validation, when D2 is built; the root otherwise | (b) the root always, as CW proposes |
| DK-Q7 | D4. | rejected on paper, recorded in §2 | (b) spiked as an upper bound only, never landed (CW-S1 already prices that ceiling) |
| DK-Q8 | `BtreeSeekLeaf`. | unchanged: it asks no coverage and needs none | (b) given D2's move for symmetry |
| DK-Q9 | The measurement budget. | the standing one: 20 minutes a stage, about an hour at the close, cuts named in the results file | (b) lifted for DK-S0 |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DK-S0 - census and premise** | No engine code on `main`; counters for the cells live on a branch that is not landed. **Census**: every caller of both descents, classed read lookup / write descent / seek; every site that writes `min_key`, `next_page_id`, a right sibling or a leaf's first entry; whether `level` is written at every root growth and division (both trees). **Measurement**: M1-M4 below, `build-release`, inside the budget. | D1 go or not (DK-Q2); D2 go or close (DK-Q3); D3 handed to CW with M1 and M4 |
| **DK-S1 - D1** | Both trees. Review and the full suite; the armed two-core rigs beside it. | a branch at the push gate |
| **DK-S2 - D2, on paper then built** | Paper first: the hop, the budget, the latch-order argument against `page.md` §6 and `SecureParents`, the amendments owed (`btree.hpp`'s concurrency account, `heap-and-tuple.md` §5, `index.md`, DK-R5's sentence). Items back to the operator before any code. Then the build, with `btree_race_test.cpp` and its index counterpart extended: a split between a parent's read and the leaf's arrival, a split while a hop is between pages, a stale root after a growth. | a branch at the push gate |
| **DK-S3 - D3** | Only as CW-S3's C3a stage; DK records DK-Q6's choice there and adds the restart-at-node case to CW's contract suite. | CW's close |
| **DK-S4 - the close** | Interleaved A/B over what was built: `cores = 1` (nil is the claim for D2; D1's one fetch fewer is measured), `point`, `insert1`, named-key inserts mid-chain, a secondary-index insert, at the clean multi-core cell; restarts and refusals before and after; the specs amended, the row flipped. | closed, or closed narrow |

**DK-S0's cells.**

- **M1** - fetches a descent, counted, not inferred: `point`, `insert1`,
  a named-key insert mid-chain, a secondary-index probe and insert; the
  write descent's second fetch shown apart.
- **M2** - restarts a million descents, by tree and by caller, at
  `cores` 1, 2 and the largest clean cell, for `insert1` on one relation
  from every session and on a relation each; with the refusal tallies.
- **M3** - the stale root: restarts and refusals in the window after a
  root grows a level, on a relation grown from one leaf under load.
- **M4** - where the descent's cycles are: a sampled profile of a
  `point` and an `insert1` statement at the clean cell, attributed to
  `FetchAndPin`, `AcquirePageLatch` and `UnpinFrame` under `DescendTo`,
  apart from the leaf's own work.

## 6. What DK does not do

- No B-link: no right link or low key on an internal node, no move right
  above the leaf level.
- No merge, no rebalance, no leaf removal, and no change to a split's
  write side or to `SecureParents`.
- No page format, record kind or superblock change.
- No optimistic leaf read (CW's C3b) and no optimistic writer.
- No version protocol of its own (DK-Q1; CW's).
- No change to scans, walks, `BtreeSeekLeaf` (DK-Q8), visibility or the
  lock family.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-20

Written in the main checkout on `main` at `1c7bb774`
(`v3.4.15-4-g1c7bb774`), documents only. The letter DK is CLA's proposal;
the exploratory standing is the operator's word (DK-Q0). No review was
run and no suite was executed: the commit changes no build input.
