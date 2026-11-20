# Work order DM - a leaf that knows its own upper bound: the right sibling's low key remembered in memory, or kept on the page, measured before either is built

Status: **WRITTEN 2026-11-20, not opened.** Written in the main checkout on
`main` at `b0a824db` (`v3.4.16-2-gb0a824db`) on the operator's words of
2026-11-20 (`raft-marks-2026-11-20.md` §12). **Exploratory is the
operator's word** (DM-Q0), and so is that the upper bound's two forms are
candidates. The letter DM, that it is an order of its own beside DK
(DM-Q1), the candidates' shapes, the rules and the stages are CLA's
proposals. No DM-Q item but DM-Q0 is marked. No stage is started. Every
`[source-read]` claim below was read at `b0a824db`. No suite was run: the
order changes no build input.

**Exploratory, and the measurement comes first.** DM-S0 and DM-S1 can each
close the order with nothing built, and that is a result, not a failure.
One candidate is a format event (U2); nothing in this order licenses one
without the operator's word for it (DM-R6).

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` where a wrong choice turns a
correct answer into a wrong one with no error.

Related: `instructions/v3.2.0/workorder-dk-descent-low-key.md` (the
descent; its D1 and D2, and DK-R5's premise for the secondary tree);
`instructions/v3.2.0/workorder-cw-optimistic-page-read.md` (C3b, a point
lookup's leaf read with no latch);
`instructions/v3.2.0/workorder-dd-index-entry-purge.md` (what would move a
leaf's first entry);
`instructions/v3.2.0/workorder-cv-ordered-index-walk.md` (the seek that
already walks right);
`instructions/v3.2.0/workorder-da-access-stats-memory-resident.md` (DA-S3,
a format event waiting for the next superblock bump);
`docs/spec/heap-and-tuple.md` §5, §8 (invariants 2 and 3);
`docs/spec/index.md`; `docs/spec/page.md` §3, §6, §12.

## 0. What DM is

`[operator]` *"And what if it were managed by a `max_key` instead of the
`min_key`?"*, then *"The pros and cons, and the cost and effect, of adding
the upper bound."*, and, after CLA's account, *"For now add them as
candidates, and write it as an exploratory milestone."* (given in Korean;
recorded in English.)

`[design]` A leaf covers `[min_key, right sibling's min_key)`. The lower
end is on the page and never moves. **The upper end is on another page**:
every coverage check fetches the right sibling to read it (§1.2). DM asks
whether a leaf should know its upper end without that fetch, in one of two
forms, and what either is worth:

- **U1 - remembered in memory**: a core-local memo of the sibling's low
  key, checked against the link the leaf holds. Nothing on disk moves.
- **U2 - kept on the page**: an upper bound beside `min_key`, written with
  the sibling link whenever the link changes. A format event.

**What CLA told the operator, and what DM takes from it.**

1. *Replacing* `min_key` with an immutable `max_key` is not a candidate.
   Which end of a leaf's range is immutable is decided by the side a split
   puts its new page on. Splits here go right, so the low end is fixed and
   the high end shrinks. A fixed high end needs splits that go left, and
   that costs the append split that moves no row - the shape every issued
   key takes - a write to the left neighbour's link, which no path may ask
   for while holding a page, and invariants 2 and 3. It buys the mirror
   image of today. Recorded as rejected in §2.
2. *Adding* an upper bound is the B-link tree's high key. It need not be
   immutable: it changes exactly when the sibling link changes, under the
   same hold.
3. The CPU it returns is small - one resident page fetch on the operations
   §1.2 lists, which CLA put at a few tenths of a microsecond against a
   statement of about 23 us, an estimate and not a measurement. **What may
   matter more is the tail**: the sibling can be out of the pool, and the
   fetch is then a device read under the leaf's exclusive hold.
4. It is also a premise of two later things: an index entry purge (DD),
   which today's bound cannot survive, and a latch-free read of a leaf
   (CW's C3b), which cannot decide a miss from one page while the bound is
   on another.

## 1. The tree as read

### 1.1 What is measured `[measured]`

Nothing about the sibling fetch: how often it happens, what it costs, or
how often the sibling is not resident. The numbers beside it:

| measurement | reading |
|---|---|
| DA's close, `v3.4.14-32-g89c26252`, `cores = 1` (`bench/v3.4.14/results-da-close-v3.4.14-32-g89c26252.md`) | a child `INSERT` with one foreign key costs 22.76-23.21 us of server CPU; the cell has no secondary index |
| DB's close, `v3.4.14-5-gec057c06` (`bench/v3.4.14/results-db-close-v3.4.14-5-gec057c06.md`) | a table three times the pool (1M rows, 2,048 frames) is a measured regime, so a cold sibling is not a contrived case; that file prices the page image there, not a fetch |

### 1.2 What the tree does `[source-read]` at `b0a824db`

- **The check.** `LeafStillCoversKey` fetches the right sibling shared and
  compares the key with its `min_key`; the rightmost leaf has no sibling
  and costs nothing (`src/storage/btree/btree.cpp:98-114`, the account at
  `:73-97`). The secondary tree's does the same against **the sibling's
  first entry**, since an index leaf stores no low key
  (`src/storage/index/index_tree.cpp:75-95`), sound only "because nothing
  removes an index entry today" (`:60-64`).
- **Who pays it.**

  | operation | where | when |
  |---|---|---|
  | a secondary index entry's insert | `index_tree.cpp:372`, `:166` | every insert that lands off the rightmost leaf - entries arrive in no key order, so nearly all |
  | a named-key `INSERT` | `btree.cpp:972`, `:197` | when it lands mid-chain |
  | an issued-key `INSERT` | the same | never: the rightmost leaf |
  | a tuple key lookup | `btree.cpp:1195-1215` | on a miss only; a hit is authoritative |
  | a write lookup (`PURGE`, the dropped-column clean) | `btree.cpp:1244`, `src/exec/clean_dropped.cpp:47` | every one off the rightmost leaf |
  | an ordered walk's seek (CV) | `BtreeFetchLeafAtOrBelow`, `btree.cpp:1316-1334`; `IndexFetchLeafAtOrBelow`, `index_tree.cpp:604-630` | once a hop: it already walks right along the chain under each leaf's hold, asking the sibling at every step |

- **The fetch is a whole page fetch**, with the leaf held: the frame
  table's latch, a pin, the sibling's latch word, and their release
  (CW §1.2 counted three holds of the frame table's latch and two
  compare-and-swaps a resident page). The comment calls it "one
  resident-page read" (`btree.cpp:90`, `:1187`); nothing makes the sibling
  resident. A miss is "a synchronous `pread` on the reactor's thread for
  every caller but two" (`docs/spec/page.md` §3, the miss path), and a
  write descent is neither of the two - so a cold sibling is a device read
  under the leaf's exclusive hold, on the statement's reactor.
- **A range walk reads one leaf past its range.** The stop is "the first
  leaf whose `min_key` exceeds the high bound", tested on the leaf already
  fetched (`src/exec/step_vm.cpp:2299-2302`, `:2376`).
- **Where a bound could live.**
  - A clustered leaf's header is `flags`, `nr_slots`, `lower`, `upper`,
    `min_key` - 16 bytes, no reserved field
    (`include/kds/storage/heap/heap_page.hpp:59-65`) - shared by every
    heap-bodied page; the sibling link is the page's last four bytes
    (`:174`).
  - An index leaf's header is 16 bytes with `right_sibling` in it
    (`include/kds/storage/index/index_page.hpp:146`, `:158`); an entry's
    sort key is the whole entry, of a width the index's layout fixes.
- **A split already writes the page whose bound would change.** The old
  leaf's link is rewritten by both split shapes, and a split is logged as
  full images of every page it writes, in one record
  (`include/kds/wal/payload.hpp:418`). A bound on the page rides in that
  image; no record kind and no record field is owed.
- **The low key and the link are stable in the ways a memo needs.**
  `min_key` is immutable until its page is freed (invariant 2); a division
  and an in-place compaction keep it (`btree.cpp:742-746`, `:656-702`);
  nothing turns a leaf into another kind of page, and "a freed id is
  reused only once its relation is dropped and no statement can reach it"
  (`btree.cpp:186-191`). An index tree's pages are freed by an index job
  after a drop, a rollback or a rebuild, under an oid that is never
  reissued (`src/exec/index_ddl.cpp:177-194`; CLAUDE.md's index row).
  DM-S0 lists every free of a tree page; this is a first reading.
- **The tree's own argument against a second boundary.** A separator is a
  leaf's `min_key` copied up, "and the two can never disagree about where
  a key belongs", and "adding a second, differently derived boundary is
  how a descent lands one page off"
  (`include/kds/storage/btree/btree_page.hpp:30-36`). DM-R2 is written
  against that sentence.
- **The superblock is 37** (`include/kds/server/superblock.hpp:247`), and
  DA-S3 waits for the next bump any order makes.

## 2. The candidates `[design]`

Priced by DM-S0 and DM-S1; none is chosen here.

- **U0 - nothing.**
- **U1 - the bound remembered: a core-local memo.** Keyed by the owning
  oid and the leaf's page id; it holds the sibling's page id and the
  sibling's low key (the clustered tree: 8 bytes; the secondary tree: the
  sibling's first entry, a sort key of the layout's width).
  - *Use.* Under the leaf's hold, the check reads the leaf's link. If the
    memo's sibling id equals it, the memo's key is the bound and no page
    is fetched. Otherwise the sibling is fetched as today and the memo
    filled.
  - *Why it is sound.* The link under the hold names the sibling; the same
    id within one owner is the same page, because a tree's pages are not
    freed while the owner lives; that page's low key cannot move. The
    owner's oid is never reissued, so a reused page id under another owner
    never matches. For the secondary tree the last step is "no entry is
    removed" - today's premise, no weaker and no stronger (DM-R7).
  - *What it costs.* Memory a core, fixed; a lookup and a compare on every
    check; nothing shared, nothing on disk, nothing at a split.
  - *What it does not give.* The range walk's extra leaf only on a memo
    hit; nothing to DD; nothing to CW's C3b, which reads from any core
    with no hold.
- **U2 - the bound on the page.** An upper bound stored in the leaf,
  **defined as the right sibling's low key and nothing else**; a leaf with
  no sibling stores none.
  - *Written* wherever the link is: both split shapes, under the hold that
    writes the link, in the same image (§1.2). A compaction keeps it. A
    new leaf takes its own from the leaf it was split from.
  - *Read* under the leaf's hold, in place of the sibling fetch, by every
    row of §1.2's table; and by a range walk, which stops on the leaf it
    is in.
  - *Format.* The clustered leaf gains eight bytes (DM-Q5 places them);
    the index leaf gains a sort key's width, and keeps its bound when a
    first entry goes. A superblock bump: an older volume does not mount.
  - **U2a** - the clustered tree only. **U2b** - both trees.
- **U3 - an upper bound and a right link on internal nodes: the B-link
  tree.** Named so that it is not reached by drift. It would let a descent
  move right at every level and is what DK's D3 would lean on. It changes
  every internal page, the split's write side and `SecureParents`. CLA
  proposes it is not DM's (DM-Q6).
- **Rejected on paper: the low key replaced by an immutable high key.**
  §0, item 1. It needs a split that puts its new page on the left; every
  append split then copies a leaf where today it moves no row, the left
  neighbour's link has to be found and written, invariants 2 and 3 fall,
  and the result is today's tree mirrored, worse for ascending keys.

## 3. Rules

Binding from DM-S2 on, if the order reaches it.

- **DM-R1 `[quiet-wrong]` - a bound a check uses is never above the right
  sibling's low key as the chain stands under the leaf's hold.** A bound
  that is too low costs a hop or a restart. One that is too high admits a
  key to a leaf that does not cover it: a row no lookup finds, or a second
  row under one key.
- **DM-R2 - one authority.** `min_key` stays the only boundary a page owns
  and invariants 2 and 3 stand as written. The upper bound is a copy of
  another page's `min_key`, derived from it and from nothing else: a memo
  that disagrees with the link is discarded (U1), and a stored bound that
  disagrees with the sibling it names is `Corruption` wherever both are in
  hand (U2). No path computes a bound.
- **DM-R3 `[quiet-wrong]` (U2) - the link and the bound change together**:
  one hold, one image, one record. No state on disk or in the log has one
  without the other, and a crash between them does not exist.
- **DM-R4 (U1) - the memo is the core's own, validated at every use
  against the link read under the hold, and a miss is today's path.**
  Nothing invalidates it from outside and nothing needs to.
- **DM-R5 - no answer moves.** With the candidate on and off, every
  statement returns the same rows and refusals; a contract cell compares
  them, and the armed two-core rigs run the split races with both.
- **DM-R6 - no format event without the operator's word for it.** U2 is
  built only on that word, bundled with whatever else waits for the bump
  (DA-S3), and DM-S0 to DM-S3 change no page byte.
- **DM-R7 - the secondary tree's premise is stated where a later order
  meets it.** U1's memo of a first entry, like today's check, holds only
  while no index entry is removed; `index.md` says so, and DD reopens it
  by name. U2b removes the premise, and that is one of its prices to
  weigh.
- **DM-R8 - at every core count.** Neither candidate is armed by `cores`.

## 4. Items for the operator (DM-Q)

(a) is CLA's proposal in each; only DM-Q0 is marked.

| | item | (a) | other |
|---|---|---|---|
| DM-Q0 | Is DM exploratory - may DM-S0 or DM-S1 close it with nothing built? | **yes - marked, the operator's word** (`raft-marks-2026-11-20.md` §12) | |
| DM-Q1 | DM and DK. | an order of its own: DK's are descent changes with no format in reach, DM's question ends at a format event; DK's §2 names U1 and U2 as its D5 and D6 and points here; DM-S0's M1 and DK-S0's M1 are one run | (b) fold U1 and U2 into DK as D5 and D6 and delete this order; (c) U1 into DK, U2 alone here |
| DM-Q2 | The bars. | **V0** (after S0): go on if a sibling fetch is made by at least half the write statements of the indexed-insert cell, or M3 finds a sibling read from the device under an exclusive hold in at least 1 % of write statements under pool pressure. **V1** (after S1): build U1 if the spike returns at least 3 % of server CPU a statement in that cell at any core count, or removes M3's reads with the other sessions' p99 moving outside its spread | (b) the operator's numbers |
| DM-Q3 | Which candidate. | U1 first, when V1 passes. U2 only on the operator's word for a format event (DM-R6), and CLA would bring it when DD or CW's C3b opens, or if M4 prices the range walk's extra leaf above the cell's floor | (b) U2 at once, at the next bump; (c) U1 only, U2 struck |
| DM-Q4 | U2's reach. | both trees (U2b): the secondary tree pays the fetch most often and is the one whose bound DD threatens | (b) the clustered tree only |
| DM-Q5 | U2's place on a clustered leaf. | eight bytes at the page's tail beside the link, on btree leaves only; the heap header every heap-bodied page shares is not widened | (b) the header widened for every heap-bodied page |
| DM-Q6 | U3, the B-link internal node. | not DM's; a concept note if the operator wants it kept | (b) a candidate here; (c) its own order |
| DM-Q7 | U1's size. | 1,024 entries a core for the clustered tree, direct-mapped by leaf id; the secondary tree's the same count with keys up to a bound S0 proposes from the layouts in use, wider keys not remembered; S1 measures the hit rate | (b) other numbers |
| DM-Q8 | The range walk's stop on a remembered bound. | U2 only; under U1 the walk stops as today | (b) U1 too, on a memo hit |
| DM-Q9 | The measurement budget. | the standing one: 20 minutes a stage, about an hour at the close, cuts named in the results file | (b) lifted for DM-S0 |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DM-S0 - census and premise** | No engine code on `main`; counters live on a branch that is not landed. **Census**: every caller of both `LeafStillCoversKey`s and what it holds at the call; every write of a leaf's link and of what a bound would copy; every free of a tree page and the owner it is freed under; the index layouts' sort-key widths in the scenarios; every reader of a leaf's header bytes that a tail field or a wider header would meet. **Measurement** M1-M4 below, `build-release`. | **V0**: go to S1, or close with the numbers |
| **DM-S1 - the ceiling, a spike that stays on the branch** | U1 as a measurement-only patch, never landed: the memo, the check, the hit rate; interleaved against the base on M1's cells and on M3's. It is also what U2 can return on those cells, less the range walk. | **V1**: U0 or U1; what U2 would add, stated |
| **DM-S2 - the model, on paper** | U1 against the code: the key, the validation, the fill, the bound on a key's width, the argument of DM-R4 against S0's list of frees, the contract cell's design, the amendments owed (`heap-and-tuple.md` §5, `index.md`, the two trees' concurrency accounts). If the operator has given the word for U2: its layout, every write site, the `Corruption` checks, the crash cells, the bump. Items back to the operator before any code. | marks |
| **DM-S3 - U1 built** | Both trees; review, the full suite, the armed rigs; `btree_race_test.cpp` and its index counterpart run with the memo warm across a split. | a branch at the push gate |
| **DM-S4 - U2 built** | Only on the operator's word (DM-R6), with the bump's other riders. | a branch at the push gate |
| **DM-S5 - the close** | Interleaved A/B over what was built: `cores = 1` and the largest clean cell, M1's cells, M3's pool-pressure cell, the range walk if DM-Q8 put it in; the memo's hit rate and memory. | closed, or closed narrow |

**DM-S0's cells.**

- **M1** - sibling fetches a statement, counted: an `INSERT` into a
  relation with zero, one and four secondary indexes; a named-key
  `INSERT` mid-chain; a tuple key lookup that misses; a `PURGE`; an
  ordered walk's seek; a range walk's leaf past its range.
- **M2** - what a statement spends in the check: a sampled profile of the
  indexed-insert cell, attributed to `LeafStillCoversKey` and the fetch
  under it, at `cores` 1 and the largest clean cell.
- **M3** - the cold sibling: the indexed-insert cell on a table three
  times the pool (DB's regime); how many sibling fetches miss the pool,
  under which hold, and the p99 and p99.9 of sessions on the same core
  and of sessions waiting for the held leaf.
- **M4** - the range walk: leaves fetched past the range a statement, and
  how often that leaf misses the pool, on a short-range shape.

## 6. What DM does not do

- No change to `min_key`, to invariants 2 and 3, or to a separator.
- No split that goes left, and no change to which page a split creates.
- No internal-node bound or link (DM-Q6).
- No format event in DM-S0 to DM-S3; none at all without the operator's
  word (DM-R6).
- No change to DK's D1 or D2, to a scan's or a walk's order, to
  visibility or to the lock family.
- No index entry purge (DD's) and no latch-free leaf read (CW's); DM
  states what each would need from it and builds neither.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-20

Written in the main checkout on `main` at `b0a824db`
(`v3.4.16-2-gb0a824db`), documents only. The letter DM and its standing
beside DK are CLA's proposals; the exploratory standing and the two
candidates are the operator's word. No review was run and no suite was
executed: the commit changes no build input.

### Landed 2026-11-20 over `1cd689ae`

`main` moved four commits between the writing and the push (the audit's
dead-code removal, merged at `1cd689ae`). Read against it: `AsPage` is
gone from both trees and each `out.Record` lost its arguments, so
`btree.cpp` is seven lines shorter after `:61` and `index_tree.cpp` six
after `:27`; nothing §1.2 cites changed in substance. The citations
stand at `b0a824db`, as the header says. That merge took
`raft-marks-2026-11-20.md` §11, so this order's record is §12.
