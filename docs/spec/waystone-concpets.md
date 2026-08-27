# Waystone — Concept & Technical Specification

How KDS remembers where a repeated query found its rows. Task breakdown: `waystone-workplan.md`. `[OPEN]` marks a decision that has not been made; `[PROPOSED]` marks a default to confirm or amend before the affected part is built.

**Naming:** the **Keystone** word is each tuple's structural identity; a *waystone* guides travelers without being the road. The advisory role is what the name is for.

**Status: recording and replay both work (`waystone-workplan.md` P01-P13).** A repeated pattern instance is served from a validated recorded location instead of a descent or a chain scan. What is not built: retention and eviction (P15), decay (P16), and epoch bump sites (P17).

**Measured (P14, `bench/results-waystone-v2.md`): 26-34x faster on a relation with no pk index, 3-7% slower on a B+ tree one.** Spec §7 predicted both directions and the numbers now match it closely. The first measurement showed the btree case at 13-15% slower, but most of that was not replay: it was `FingerprintOf` lexing the statement a second time, plus a `std::string` per token. Both are gone (2026-08-03 — the fingerprint rides the parse, and token text views the source), which cut the btree overhead to a few percent and made the parse itself 3-8% faster for *every* statement, Waystone or not.

**Two gaps replay now depends on, and the first is the serious one.** There is **no page epoch** (§6's entry field is recorded as 0), so §2's replay rule 2 is unenforceable. It is safe only because a tuple's address is currently stable for life — the fixed-length rule stops an UPDATE migrating one and nothing relayouts — so ***the epoch must land with relayout, whichever comes first***: replay is the consumer that would return a wrong row rather than merely a stale hint. Second, **nothing verifies that a page still belongs to the relation it was recorded from** — `rel_oid` is checked against the *query*'s step, not against storage — which is sufficient only until pages can be freed and reallocated between relations (`DROP TABLE`, page reuse). And a trail is **one page, never continued**: more entries than fit means no trail rather than a truncated one (§9).

---

## 1. Concept

A **waystone is the recorded trail of one pattern instance.**

A *pattern* is the shape of a query or procedure, identified when it is parsed and reduced to function form — `patternX(a, b)`. A *pattern instance* is that shape with its arguments bound: the pair `(pattern_id, arg_hash)`. Executing an instance touches some set of tuples, possibly across several relations. The waystone for that instance records their **Keystones**, with where each one was last seen.

Three properties follow, and they define the structure:

- **A relation holds no Keystone map.** Nothing addresses a tuple by arithmetic on its pk, so there is no per-relation directory, no coverage guarantee, and no per-relation enable flag. A relation stores tuples; that is all it does.
- **A waystone spans relations.** One page holds the Keystones of a customer row, the account rows under it, and the instrument rows those reference — because that is what one execution of `patternX(a, b)` touched. Every entry therefore carries its own `rel_oid` (§6).
- **Pk values may be arbitrary.** Nothing here requires a dense, monotonically issued id sequence, so a heap page or a btree leaf may hold any pk the rest of the engine permits. See §8: this *permits* a relaxation, it does not perform one — and as of 2026-08-11 the engine performs part of it for its own reasons, which cost this document nothing.

Waystone lives outside the executor. The executor emits a trail through a one-method seam and asks for one through another; all storage and policy belong to Waystone, which is what keeps the advisory contract structurally enforceable.

## 2. What a waystone is not — the trust model

**A waystone is a trail, not an answer.** It records where a previous execution *found* rows. It never asserts that the set it holds is complete.

This is not a preference. Invariant 9 says Waystone is never authoritative, and a stored set trusted as the complete answer to `patternX(a, b)` *is* authoritative — it would be the sole reason the engine believes no other row qualifies. The failure mode is severe in a particular way: a stale entry that points at the wrong place is caught by the Keystone-id check, but a stored set missing a row inserted since it was recorded is wrong in a way **no per-tuple validation can detect**, because there is no tuple to validate. Absence has no witness.

So the normative rule, which every consumer is bound by:

> **A waystone may replace a lookup. It may never replace a search.**

A pattern *step* whose authoritative work is a keyed lookup — a pk equality, or a chain step probing the next relation by pk (`docs/spec/parser-v2.md` I12: the statement is the chain) — may be served from the trail, because completeness for that step follows from pk uniqueness, not from the trail. A step that must *search* — a non-pk predicate, a range, a scan — runs authoritatively no matter what the trail says; the trail may only prefetch for it.

Replay contract, normative, per entry:

0. **Re-derive the probe key from the current outer row and require it to equal the entry's `pk`.** Added 2026-08-01 by `docs/spec/parser-v2.md` I17 and *mandatory before any join replay*: every other rule below validates the trail against storage and none looks at the query, so a driving row whose join column changed since recording would otherwise pass all of them and emit the wrong row. **Built as the lookup key** (`include/kds/exec/trail_replay.hpp`): the index is keyed on `(step_id, pk)`, so an entry can only be found by matching the freshly derived key and there is no separate check to forget.
1. Read the tuple at the recorded `(page_id, slot)` and check that the Keystone id there equals the entry's `pk` **and** that the page belongs to the entry's `rel_oid`. A mismatch is a stale entry, not corruption: fall through. **Built**, with one caveat: the id half is checked against the tuple actually there, the relation half against the *query*'s step rather than against storage, because nothing can ask a page which relation owns it. `tests/waystone_contract_test.cpp`'s corrupted-trail case is what proves this rule load-bearing — without the id check a poisoned trail silently returns the wrong rows.
2. Check the recorded `page_epoch` against the page's current epoch. A mismatch is a miss. **Not built — there is no epoch**, see the status note above.
3. Apply MVCC visibility exactly as the authoritative path would. The trail chooses *where to look*, never *what is visible*. **Free by construction**: a validated location is handed to the same `AcceptTupleAt()` a descent feeds, so whatever the authoritative path does about visibility, replay does identically because it is the same call.
4. On any miss, fall through to the authoritative path **for that step alone** — a btree descent on a btree relation, a chain scan on a heap one. A missing waystone, a dropped one, or the whole structure deleted changes no result; it costs the descents the trail would have saved.

Trusting a set as complete would require amending invariant 9 and a completeness mechanism to go with it — a per-relation change stamp bumped at commit, so "nothing has changed under this set" is provable. That is `[OPEN]` (§9) and deliberately out of scope.

## 3. Pattern identity

`pattern_id` is a fingerprint of the statement's *shape*, computed **at parse time** and never per execution (`docs/spec/parser-v2.md` I1). Literals are parameterized as they are lexed: the shape stream hashes to `pattern_id`, and the ordered literal values hash to `arg_hash`. `WHERE id = 42` and `WHERE id = ?` therefore converge on one `pattern_id`, which is the property that makes the whole structure work — a client that inlines literals and one that binds parameters share a waystone.

Two obligations follow:

- **Stability.** `pattern_id` is persisted in `sys.patterns` and is the key to stored waystones, so it must not depend on pointer values, hash-map iteration order, or anything else that varies between runs of the same binary.
- **Versioning.** Every pattern row carries a `fingerprint_version`; a row whose version does not match the running build is ignored, and its waystones with it. This is the cheap alternative to a migration that would have to re-parse stored SQL the engine no longer keeps. **Amended 2026-08-01:** the parser replacement is now planned to be *hash-preserving* — the joins-and-subqueries language is additive shape, and folding the fingerprint into the parse pass must not change one hash (`docs/inflight/in-progress/parser-v2-workplan.md` V01 pins every pre-existing `pattern_id`, V29 must not move them). So the version constant stays the seam, but no bump is expected from that work; it is there for the day the algorithm itself changes.

## 4. Catalog — `sys.patterns`

Patterns are catalog objects, in a relation named `patterns` in the `sys` namespace, bootstrapped on its own fixed page alongside `sys.tables` and friends.

| Field | Type | Meaning |
|---|---|---|
| `oid` | `Oid` | the pattern object's oid |
| `pattern_id` | `uint64` | the parse-time fingerprint; the lookup key |
| `fingerprint_version` | `uint32` | §3; a mismatch retires the row's waystones |
| `stmt_class` | `uint8` | the parser's execution-class tag (`docs/spec/parser-v2.md` I2; every step-chain statement carries `kJoinSelect`, per J3) |
| `waystone_root` | `PageId` | root of this pattern's `arg_hash` directory, `kInvalidPageId` when none |
| `dir_depth` | `uint8` | levels the directory walk traverses; persisted, never derived |
| `use_count` | `uint32` | executions observed; best-effort, drives retention |
| `last_seen` | `uint64` | truncated logical timestamp; best-effort |

`waystone_root` and `dir_depth` are written as one unit by `Catalog::SetPatternWaystoneRoot()` and validated as a pair: a root without its depth is unwalkable, and a depth disagreeing with the root sends every walk to the wrong leaf. `dir_depth == 0` is the authority on "no directory" — a row read out of a zeroed page decodes its root as page 0, which looks like a valid `PageId`, so the question is keyed on the field whose zero value already means none.

Why a catalog relation rather than an in-memory table: patterns are the durable, inspectable statement of *what this database is asked to do*, they are few (an application's distinct query shapes — dozens to hundreds, not millions), and making them catalog objects gives `SHOW PATTERNS` and every future policy surface one place to live. The unbounded axis is not patterns but *instances per pattern*, and that is bounded by eviction inside the directory (§9), not by the catalog.

**Registering a pattern bumps no catalog version**, so it is safe on the statement path. Nothing cached can go stale from a pattern appearing: absences are never cached, so no entry claims the pattern is missing, and no other cached fact mentions it. This is what lets a first execution register its own pattern mid-statement without dangling the `const TableAccess*` that statement is holding. Pointing a pattern at a new directory updates the cached `PatternAccess` in place rather than invalidating, for the same reason — the fact belongs to one pattern and is read by nothing else.

## 5. Addressing — two levels

```
pattern_id  --> sys.patterns row          (catalog lookup, cached)
arg_hash    --> waystone for that instance (directory walk under waystone_root)
```

The second level is an inode-style page directory: interior pages of 2048 `PageId` children, walked by digits of the `arg_hash`, lazily allocated, deepened by relinking the root. Depth is bounded at `kMaxPatternDirDepth` = 6, derived rather than chosen — ceil(64 / 11) levels address a 64-bit key at a fanout of 2^11.

It is a hash directory, not a radix index over a dense key, so **collisions are possible**. An `arg_hash` collision must be resolved by the waystone's own header, which stores the `arg_hash` it was recorded for; a mismatch is a miss, never a wrong trail. Handling repeated collisions (chain vs. displace vs. drop) is `[OPEN]`. A walk at depth *d* consumes the low 11*d* bits and ignores the rest: no key is ever out of range — the structure this replaced refused a pk past its coverage, and a hash has no coverage to exceed.

**Amended 2026-07-31 (P07): growth is a cache flush, not a rehash.** Relinking the root is O(1) and preserves prior mappings *only* for keys whose new top digit — bits [11*d*, 11*d*+11) of the `arg_hash` — is zero, which is 1 in 2048 of them. On the dense pk key this design replaced, every stored key had that zero by construction; a hash does not, and no O(1) growth can give it one. Everything else is cooled, not corrupted: the old subtree stays reachable under slot 0, a key that now addresses one of its pages gets a header mismatch and a miss, and the next execution re-records the trail at the new address. Safe by invariant 8, and the reason growth must be paid for by capacity — each level multiplies addressable instances by 2048 — rather than performed routinely.

**Interior pages are headerless.** 2048 × 4 bytes tiles 8 KiB exactly, which is why the fanout is 2048; a common header would cost a child slot, and `DevicePageStore` stamps a checksum at byte offset 4 of every headered frame — child 1. They are allocated through `PageStore::CreateNewHeaderless()`, which records the fact durably. The cost is that a damaged interior page carries no checksum to catch it; it leads a walk to a page that is not the instance's waystone, which the header check turns into the same miss a cold directory gives.

## 6. Page format `[PROPOSED]`

Waystone pages are **headered** — `PageType::kWaystone`, carrying the common page header like every other page class. Nothing here is addressed by shift and mask (a trail is read sequentially), so the payload need not tile the page exactly, and the pages keep checksums and a `page_lsn`.

Page body: a waystone header, then entries in **execution order**.

Waystone header:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `pattern_id` — self-identifying, checked on read |
| 8 | 8 | `arg_hash` — resolves directory collisions (§5) |
| 16 | 2 | `entry_count` |
| 18 | 2 | `flags` |
| 20 | 4 | `next_page_id` — a trail longer than one page continues here |
| 24 | 8 | `recorded_ts` |
| 32 | 4 | `use_count` |
| 36 | 4 | `reserved` |

Entry — 32 bytes, one per Keystone in the trail:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | `pk` | zero-extended Keystone id, upper 24 bits 0 (invariant 7) |
| 8 | 8 | `rel_oid` | **the field a per-relation structure did not need.** One page spans relations, so the entry must say which. A full 64-bit `catalog::Oid`: oids are `uint64` at the source, and a narrowed copy that happens to fit today aliases two relations onto one value the day it does not |
| 16 | 4 | `page_id` | last observed location; advisory |
| 20 | 4 | `page_epoch` | location trust (§2 rule 2) |
| 24 | 2 | `slot` | last observed slot |
| 26 | 2 | `flags` | `kWaystoneEntryValid`, rest reserved |
| 28 | 2 | `step_id` | which step of the pattern produced it — the join position. 16 bits counts relations in a join chain, not rows |
| 30 | 2 | `reserved` | 0 |

`step_id` is what makes a cross-table trail replayable rather than merely descriptive: the executor needs to know that entry 0 is the driving relation and entries 1–17 are the probe results, or the trail is an unordered bag of tuples from three tables.

Entries per page is derived, not chosen: `(8192 − 32 common header − 40 waystone header) / 32 = 253`, with 24 bytes of tail slack. Not a power of two, and it does not need to be — the exact tiling a pk-addressed structure required is precisely what cost it the page header.

Format rules as ever (`docs/rules/rules.md` §§2, 5): field-wise `memcpy` through named offsets, `static_assert` on every size and offset, fixed-width little-endian, no bitfields, no `reinterpret_cast` onto page bytes.

## 7. What this buys

Per pattern step served from a trail, against the authoritative path it replaces:

- **`kJoinSelect`, the case this design exists for.** Written-order nested-loop over 3 relations is 3 keyed descents, ~3 page touches each. A validated trail replay is 3 direct page reads. The saving grows with the join's length, and join length is exactly what a financial procedure has.
- **`kPointSelect` on a btree relation.** One descent becomes one read. Modest, and honestly not the reason to build this.
- **`kPointSelect` on a heap relation.** A full chain scan becomes one read. Large — and this is where §1's third consequence pays off: a heap relation with arbitrary pks gets pk-keyed acceleration for *observed* patterns without carrying an index at all.
- **Any step that searches.** Nothing. By §2 it still searches. Prefetching its pages is the only permitted use, and prefetch is advisory twice over.

The bar to clear, measured on this engine: a validated point lookup ran 8,417 qps / 11 µs server-side against 311 qps / 2,582 µs for a chain scan at 5,000 rows, and stayed flat in row count. A pattern trail should match that on the single-relation case and beat a btree descent chain on the join case, or it has not earned its complexity.

## 8. Invariants — what this touches

- **Invariant 7** (advisory; deleting it never changes results) — unchanged, and still the one that matters.
- **Invariant 8** (never authoritative) — unchanged, and now load-bearing in a new way: it is what forces §2's trail model.
- **Invariant 6** (ids outside the tuple header are zero-extended `uint64`) — unchanged; entry `pk` obeys it.
- **Invariant 3** (`min_key`) and **invariant 11** (pk provenance and ordering) — **not changed by this document.** §1 notes that pattern-keying removes the *only* structural reason the engine needed dense monotonic pks, so arbitrary pk values become possible. Whether to actually permit a caller-supplied pk or to retire `min_key` pruning are separate decisions with their own blast radius — `min_key` exists for lock-free range pruning and `next_id` for tuple identity, neither of which was ever about Waystone.

  **Settled 2026-08-11, and settled by something else** (`docs/spec/heap-and-tuple.md` §4.1): a caller may name a relation's pks, and on a btree relation they **need not ascend** (a per-relation `EXPLICIT` mode then; per row since 2026-08-25). Both halves of this bullet's "arbitrary pk values" are therefore permitted — on btree-clustered relations, which is where the descent can prove a named key unique. Two things worth stating plainly, because the earlier note here half-claimed the decision:

  - **This document did not win the argument, and did not need to.** The relaxation was granted on the storage layer's terms — uniqueness moved from the cursor to the btree descent — not on pattern-keying's. Waystone's claim that it removes the *need* for dense monotonic pks stands unused: nothing in §4.1's argument cites it.
  - **The engine's answer for a heap relation is unchanged.** Every heap chain is still fed a monotonic sequence, because a chain has no descent. (Amended 2026-08-25: a heap relation may be told its keys, but only at or above its high-water mark - the sequence it sees is monotonic either way.) So the "retire `min_key` pruning" half of this bullet is fully open, and `min_key` remains immutable in both storage forms — a btree leaf division keeps the old page's bound and gives the new page the split key, precisely so invariants 2 and 3 survive the one operation that moves tuples.
  - **What it cost Waystone: one epoch bump.** A leaf division moves tuples, so the old page's `relayout_epoch` advances and every trail entry into it becomes untrusted at once. That is §3.1a's pairing rule working exactly as designed — invariant 8's advisory contract absorbs it as a performance event, never a correctness one.

## 9. Open decisions — do not assume

- **Retention and eviction per pattern.** Instances per pattern are unbounded; the catalog bounds patterns, nothing yet bounds instances. Admission/eviction returns here, confined to one directory.
- ~~**Recording policy.**~~ **Decided 2026-08-01 — `n = 2`** (`docs/spec/parser-v2.md` J5, which owns it now). The first execution of an instance only counts; the second records. Recording on first sight pays the write for one-shot queries, and waiting longer misses short-lived hot instances; two is the smallest *n* that excludes the one-shot case. Sightings live in a bounded core-local in-memory table, and eviction from it merely restarts the count — a performance event, never a correctness one. **Built 2026-08-02** with two refinements the implementation forced: a **declared** pattern records at `n = 1` (a declaration is the evidence n=2 gathers — `create-pattern-user-defined-patterns-v1.md` §7), and an execution that *collects nothing* is not counted at all, since it can never produce a trail however often it repeats. The sighting table's size is `[PROPOSED]` 4096, cleared wholesale on overflow.
- ~~**Per-instance trail cap.**~~ **Decided 2026-08-02 — one page (253 entries), and a trail that would exceed it is not recorded at all.** Truncation was the alternative and is worse in a way that matters: a truncated trail covers only the first rows of an execution and **no reader can tell it from a complete one**, so replay would serve a partial answer believing it whole. Not recording leaves the instance in the state every instance starts in, which every consumer already handles. `next_page_id` stays reserved for the day a cap decision permits more.
- **Only lookup-class steps are recorded** (decided 2026-08-02). §2 already says a trail may never replace a search, so a `Scan` step's rows could only ever be prefetched — and nothing prefetches. Recording them would pay a write per scanned row for a read nobody makes. The entry format is unchanged, so recording them for a future prefetcher stays available without a format break.
- **No page epoch exists**, so entries record `page_epoch = 0` and §2's rule 2 cannot be enforced. Not a decision so much as a debt: it is safe only while a tuple's address is stable for life. See the status note at the top.
- **Persistence class** of waystone pages (WAL-logged vs unlogged). Unlogged loss costs replays, never results.
- **Completeness / set caching**, and with it whether invariant 8 is ever amended. Needs a per-relation change stamp bumped at *commit*, not at write — a row inserted-then-committed by another transaction would otherwise slip past a stamp taken between the two.
- **`arg_hash` collision handling** beyond the header check (§5): chain, displace, or drop.
- **Pattern registration on the statement path** vs. lazily off it (§4 hazard).
- **Invariant 3 relaxation** (§8) — still fully open. ~~Invariant 11's~~ pk half is **settled 2026-08-11**: caller-supplied ids yes, and descending ids yes, on btree-clustered relations (`docs/spec/heap-and-tuple.md` §4.1). It was settled on the storage layer's argument rather than this document's, and it changed nothing here — trail entries key on a pk's *value*, never on its order or its provenance, and the leaf division it introduced invalidates trails through the epoch counter that already existed for relayout.
- Inherited and still open: heap-page epoch storage and width; decay function and cadence; ring sampling policy under pressure; hint-index per-template trust classification.

## 10. Why not a pk-direct index

The obvious alternative — a per-relation structure mapping every pk to its location, addressed by arithmetic — was built and then removed. The argument against it is what shapes this design, so it is worth stating.

A pk-direct waystone is a radix tree over the same key the clustered B+ tree already indexes. At a fanout of 2048 both reach a leaf in the same handful of page touches, so the O(1)-versus-O(log n) distinction does not survive contact with real fanout. What the tree has and the radix structure does not: it is authoritative, it answers ranges, and it costs space proportional to *live rows* rather than to *issued ids*. Addressing entries by pk also forces a dense, monotonically issued id sequence on the whole engine — a constraint paid across every layer to buy a duplicate of an index that already exists.

This design duplicates nothing. **No index maps a pattern instance to a cross-relation tuple set**, and that gap is what Waystone fills.

One artifact was left by that removal and has since been reclaimed: `DevicePageStore::CreateNewHeaderless()` and the durable `kHeaderlessMap` bitmap page were caller-less once §6 made waystone pages headered, and P07 gave them one — the directory's interior pages, which tile the page exactly for the same reason the old entry pages did (§5). The waystone pages themselves stay headered; it is only the structure that finds them that gives up its checksum.

## 11. Testing requirements

All deterministic (injected clock, simulated scheduling; `docs/rules/rules.md` §4):

1. **Fingerprint:** inline-literal and bound-parameter forms of one statement yield one `pattern_id`; different shapes differ; stable across runs and processes; `fingerprint_version` mismatch retires stored waystones.
2. **Codec & directory:** header and entry round-trips; offset/size asserts; walk correctness including lazy allocation and depth growth; `arg_hash` collision resolves to a miss via the header check, never to a foreign trail.
3. **The advisory contract — the test that must never be allowed to fail.** For a fixed query set, results are **byte-identical** across five configurations: recording on, recording off, replay off, all waystones deleted mid-run, and a deliberately corrupted trail whose entries name valid pages and slots holding *different* tuples. The last case proves the §2 rule-1 identity check is load-bearing.
4. **Lookup-not-search:** a pattern with a non-pk predicate must produce identical results and must be shown (instrumented) to still perform its search. A trail that shortcuts a search is a correctness bug, not an optimization, and this is the test that catches it.
5. **Cross-relation replay:** a `kJoinSelect` trail spanning three relations replays in `step_id` order and produces the same rows in the same order as the authoritative path.
6. **MVCC:** a trail recorded under one read view, replayed under another, returns exactly what the authoritative path returns — including a row deleted since recording (invisible) and a row inserted since (present via the authoritative search step, absent from the trail).
7. **Epoch validation:** a page whose epoch is bumped renders its entries untrusted; re-recording restores them.
