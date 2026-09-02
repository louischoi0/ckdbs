# Waystone — Concept & Technical Specification

How KDS remembers where a repeated query found its rows. `[PROPOSED]` marks a default that has not been confirmed.

**Naming:** the **Keystone** word is each tuple's structural identity; a *waystone* guides travelers without being the road. The advisory role is what the name is for.

**Status: recording and replay both work.** A repeated pattern instance is served from a validated recorded location instead of a descent or a chain scan. What holds today, each a fact and not a plan: trails are never retired, decayed or evicted; trails are unlogged, so a crash loses what the last checkpoint did not carry — replays, never results; a trail is one page, never continued — more entries than fit means no trail rather than a truncated one (§9); the page epoch is recorded and checked (§2 rule 2); and nothing verifies that a page still belongs to the relation it was recorded from — `rel_oid` is checked against the *query*'s step, not against storage, which is sufficient only while pages are never freed and reallocated between relations.

---

## 1. Concept

A **waystone is the recorded trail of one pattern instance.**

A *pattern* is the shape of a query or procedure, identified when it is parsed and reduced to function form — `patternX(a, b)`. A *pattern instance* is that shape with its arguments bound: the pair `(pattern_id, arg_hash)`. Executing an instance touches some set of tuples, possibly across several relations. The waystone for that instance records their **Keystones**, with where each one was last seen.

Three properties follow, and they define the structure:

- **A relation holds no Keystone map.** Nothing addresses a tuple by arithmetic on its pk, so there is no per-relation directory, no coverage guarantee, and no per-relation enable flag. A relation stores tuples; that is all it does.
- **A waystone spans relations.** One page holds the Keystones of a customer row, the account rows under it, and the instrument rows those reference — because that is what one execution of `patternX(a, b)` touched. Every entry therefore carries its own `rel_oid` (§6).
- **Pk values may be arbitrary.** Nothing here requires a dense, monotonically issued id sequence, so a heap page or a btree leaf may hold any pk the rest of the engine permits. See §8: this *permits* a relaxation, it does not perform one.

Waystone lives outside the executor. The executor emits a trail through a one-method seam and asks for one through another; all storage and policy belong to Waystone, which is what keeps the advisory contract structurally enforceable.

## 2. What a waystone is not — the trust model

**A waystone is a trail, not an answer.** It records where a previous execution *found* rows. It never asserts that the set it holds is complete.

This is not a preference. Invariant 9 says Waystone is never authoritative, and a stored set trusted as the complete answer to `patternX(a, b)` *is* authoritative — it would be the sole reason the engine believes no other row qualifies. The failure mode is severe in a particular way: a stale entry that points at the wrong place is caught by the Keystone-id check, but a stored set missing a row inserted since it was recorded is wrong in a way **no per-tuple validation can detect**, because there is no tuple to validate. Absence has no witness.

So the normative rule, which every consumer is bound by:

> **A waystone may replace a lookup. It may never replace a search.**

A pattern *step* whose authoritative work is a keyed lookup — a pk equality, or a chain step probing the next relation by pk (`docs/spec/parser-v2.md` I12: the statement is the chain) — may be served from the trail, because completeness for that step follows from pk uniqueness, not from the trail. A step that must *search* — a non-pk predicate, a range, a scan — runs authoritatively no matter what the trail says; the trail may only prefetch for it.

Replay contract, normative, per entry:

0. **Re-derive the probe key from the current outer row and require it to equal the entry's `pk`.** Mandatory before any join replay (`docs/spec/parser-v2.md` I17): every other rule below validates the trail against storage and none looks at the query, so a driving row whose join column changed since recording would otherwise pass all of them and emit the wrong row. The replay index (`include/kds/exec/trail_replay.hpp`) is keyed on `(step_id, pk)`, so an entry can only be found by matching the freshly derived key and there is no separate check to forget.
1. Read the tuple at the recorded `(page_id, slot)` and check that the Keystone id there equals the entry's `pk` **and** that the page belongs to the entry's `rel_oid`. A mismatch is a stale entry, not corruption: fall through. The id half is checked against the tuple actually there; the relation half against the *query*'s step rather than against storage, because nothing can ask a page which relation owns it.
2. Check the recorded `page_epoch` against the page's current epoch. A mismatch is a miss — for every location recorded against that page at once, and checked before the slot is read. The entry's `page_epoch` is the page's `relayout_epoch` observed at access, narrowed to the entry's `u32`; the comparison is `exec/tuple_verify.hpp`'s, shared with the Cabin's location hint so there is one verifier. An epoch match never *accepts* a location on its own — rule 1 still runs.
3. Apply MVCC visibility exactly as the authoritative path would. The trail chooses *where to look*, never *what is visible*. A validated location is handed to the same `AcceptTupleAt()` a descent feeds, so whatever the authoritative path does about visibility, replay does identically because it is the same call.
4. On any miss, fall through to the authoritative path **for that step alone** — a btree descent on a btree relation, a chain scan on a heap one. A missing waystone, a dropped one, or the whole structure deleted changes no result; it costs the descents the trail would have saved.

Nothing caches a set as complete. Doing so would require amending invariant 9 and a completeness mechanism to go with it — a per-relation change stamp bumped at *commit*, not at write.

## 3. Pattern identity

`pattern_id` is a fingerprint of the statement's *shape*, computed **at parse time** and never per execution (`docs/spec/parser-v2.md` I1). Literals are parameterized as they are lexed: the shape stream hashes to `pattern_id`, and the ordered literal values hash to `arg_hash`. `WHERE id = 42` and `WHERE id = ?` therefore converge on one `pattern_id`, which is the property that makes the whole structure work — a client that inlines literals and one that binds parameters share a waystone.

Two obligations follow:

- **Stability.** `pattern_id` is persisted in `sys.patterns` and is the key to stored waystones, so it must not depend on pointer values, hash-map iteration order, or anything else that varies between runs of the same binary.
- **Versioning.** Every pattern row carries a `fingerprint_version`; a row whose version does not match the running build is ignored, and its waystones with it. This is the cheap alternative to a migration that would have to re-parse stored SQL the engine no longer keeps. `kFingerprintVersion` moves only per `fingerprint.hpp`'s bump rule: an additive shape that changes no existing hash does not move it, and the golden corpus is the witness.

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
| `use_count` | `uint32` | executions observed; best-effort |
| `last_seen` | `uint64` | truncated logical timestamp; best-effort |

The row also carries `flags` (`u16`) and `origin` (`u8`), retained from the withdrawn declared-pattern design (`docs/spec/create-pattern-user-defined-patterns-v1.md`): every row is `kOriginAuto` and nothing writes `kPatternPinned`.

`waystone_root` and `dir_depth` are written as one unit by `Catalog::SetPatternWaystoneRoot()` and validated as a pair: a root without its depth is unwalkable, and a depth disagreeing with the root sends every walk to the wrong leaf. `dir_depth == 0` is the authority on "no directory" — a row read out of a zeroed page decodes its root as page 0, which looks like a valid `PageId`, so the question is keyed on the field whose zero value already means none.

Why a catalog relation rather than an in-memory table: patterns are the durable, inspectable statement of *what this database is asked to do*, they are few (an application's distinct query shapes — dozens to hundreds, not millions), and making them catalog objects gives `SHOW PATTERNS` and every policy surface one place to live. The unbounded axis is not patterns but *instances per pattern*, and nothing bounds instances today.

**Registering a pattern bumps no catalog version**, so it is safe on the statement path. Nothing cached can go stale from a pattern appearing: absences are never cached, so no entry claims the pattern is missing, and no other cached fact mentions it. This is what lets a first execution register its own pattern mid-statement without dangling the `const TableAccess*` that statement is holding. Pointing a pattern at a new directory updates the cached `PatternAccess` in place rather than invalidating, for the same reason — the fact belongs to one pattern and is read by nothing else.

## 5. Addressing — two levels

```
pattern_id  --> sys.patterns row          (catalog lookup, cached)
arg_hash    --> waystone for that instance (directory walk under waystone_root)
```

The second level is an inode-style page directory: interior pages of 2048 `PageId` children, walked by digits of the `arg_hash`, lazily allocated, deepened by relinking the root. Depth is bounded at `kMaxPatternDirDepth` = 6, derived rather than chosen — ceil(64 / 11) levels address a 64-bit key at a fanout of 2^11.

It is a hash directory, not a radix index over a dense key, so **collisions are possible**. An `arg_hash` collision must be resolved by the waystone's own header, which stores the `arg_hash` it was recorded for; a mismatch is a miss, never a wrong trail. A colliding instance **displaces** the trail already there (`TrailDisplacesOnCollision()`, `[PROPOSED]`); nothing chains. A walk at depth *d* consumes the low 11*d* bits and ignores the rest: no key is ever out of range — the structure this replaced refused a pk past its coverage, and a hash has no coverage to exceed.

**Growth is a cache flush, not a rehash.** Relinking the root is O(1) and preserves prior mappings *only* for keys whose new top digit — bits [11*d*, 11*d*+11) of the `arg_hash` — is zero, which is 1 in 2048 of them. On the dense pk key this design replaced, every stored key had that zero by construction; a hash does not, and no O(1) growth can give it one. Everything else is cooled, not corrupted: the old subtree stays reachable under slot 0, a key that now addresses one of its pages gets a header mismatch and a miss, and the next execution re-records the trail at the new address. Safe by invariant 8, and the reason growth must be paid for by capacity — each level multiplies addressable instances by 2048 — rather than performed routinely.

**Interior pages are headerless.** 2048 × 4 bytes tiles 8 KiB exactly, which is why the fanout is 2048; a common header would cost a child slot, and `DevicePageStore` stamps a checksum at byte offset 4 of every headered frame — child 1. They are allocated through `PageStore::CreateNewHeaderless()`, which records the fact durably in the `kHeaderlessMap` bitmap page. The cost is that a damaged interior page carries no checksum to catch it; it leads a walk to a page that is not the instance's waystone, which the header check turns into the same miss a cold directory gives.

## 6. Page format

Waystone pages are **headered** — `PageType::kWaystone`, carrying the common page header like every other page class. Nothing here is addressed by shift and mask (a trail is read sequentially), so the payload need not tile the page exactly, and the pages keep checksums and a `page_lsn`.

Page body: a waystone header, then entries in **execution order**.

Waystone header:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `pattern_id` — self-identifying, checked on read |
| 8 | 8 | `arg_hash` — resolves directory collisions (§5) |
| 16 | 2 | `entry_count` |
| 18 | 2 | `flags` |
| 20 | 4 | `next_page_id` — reserved; a trail is never continued (§9) |
| 24 | 8 | `recorded_ts` |
| 32 | 4 | `use_count` |
| 36 | 4 | `reserved` |

Entry — 32 bytes, one per Keystone in the trail:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | `pk` | zero-extended Keystone id, upper 24 bits 0 (invariant 7) |
| 8 | 8 | `rel_oid` | **the field a per-relation structure did not need.** One page spans relations, so the entry must say which. A full 64-bit `catalog::Oid`: oids are `uint64` at the source, and a narrowed copy that happens to fit today aliases two relations onto one value the day it does not |
| 16 | 4 | `page_id` | last observed location; advisory |
| 20 | 4 | `page_epoch` | location trust (§2 rule 2); the page's `relayout_epoch` narrowed to 32 bits |
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
- **`kPointSelect` on a btree relation.** One descent becomes one read. Modest, and not the reason to build this.
- **`kPointSelect` on a heap relation.** A full chain scan becomes one read. Large — and this is where §1's third consequence pays off: a heap relation with arbitrary pks gets pk-keyed acceleration for *observed* patterns without carrying an index at all.
- **Any step that searches.** Nothing. By §2 it still searches. Prefetching its pages is the only permitted use, and prefetch is advisory twice over.

Measured against the `bench/` tree at `1769487`: replay is 26-34x faster than a chain scan on a relation with no pk index, and 3-7% slower than a descent on a B+ tree one — both directions as this section predicts.

## 8. Invariants — what this touches

Numbered as `docs/spec/heap-and-tuple.md` §8 numbers them.

- **Invariant 8** (advisory; deleting it never changes results) — unchanged, and still the one that matters.
- **Invariant 9** (never authoritative) — unchanged, and load-bearing: it is what forces §2's trail model.
- **Invariant 7** (ids outside the tuple header are zero-extended `uint64`) — unchanged; entry `pk` obeys it.
- **Invariant 3** (`min_key`) and **invariant 11** (pk provenance and ordering) — **not changed by this document.** §1 notes that pattern-keying removes the *only* structural reason the engine needed dense monotonic pks. A caller may name a relation's pks (`docs/spec/heap-and-tuple.md` §4.1), but that relaxation was granted on the storage layer's terms — uniqueness moved from the cursor to the btree descent — not on pattern-keying's, and it changed nothing here: trail entries key on a pk's *value*, never on its order or its provenance. `min_key` remains immutable in both storage forms; a btree leaf division keeps the old page's bound and gives the new page the split key, so invariants 2 and 3 survive the one operation that moves tuples. What that operation costs Waystone is one epoch bump: the divided page's `relayout_epoch` advances and every trail entry into it becomes untrusted at once — invariant 8 absorbs it as a performance event, never a correctness one.

## 9. Open decisions — do not assume

The decisions this section once listed — retention and eviction, persistence class, completeness caching, collision policy, decay — are unrecorded here. What holds today, each a fact and not a plan:

- **Recording policy is `n = 2`** (`docs/spec/parser-v2.md` J5 owns it): the first execution of an instance only counts; the second records. An execution that *collects nothing* is not counted at all, since it can never produce a trail however often it repeats. Sightings live in a bounded core-local in-memory table of `[PROPOSED]` 4096 entries (`TrailRecorder::kMaxSightings`), cleared wholesale on overflow; eviction from it merely restarts the count — a performance event, never a correctness one.
- **A trail is one page (253 entries), and a trail that would exceed it is not recorded at all.** A truncated trail covers only the first rows of an execution and **no reader can tell it from a complete one**, so replay would serve a partial answer believing it whole. Not recording leaves the instance in the state every instance starts in, which every consumer already handles. `next_page_id` stays reserved and unused.
- **Re-recording an instance replaces its trail wholesale, never merges.** A merge would accumulate rows from earlier executions that no longer qualify, and nothing at this layer can tell those from rows that still do.
- **Only lookup-class steps are recorded.** §2 already says a trail may never replace a search, so a `Scan` step's rows could only ever be prefetched — and nothing prefetches. The entry format does not depend on this.
- **Trails are unlogged**, never retired, never decayed, never evicted; an `arg_hash` collision displaces (§5).

## 10. Why not a pk-direct index

The obvious alternative — a per-relation structure mapping every pk to its location, addressed by arithmetic — was built and then removed. The argument against it is what shapes this design, so it is worth stating.

A pk-direct waystone is a radix tree over the same key the clustered B+ tree already indexes. At a fanout of 2048 both reach a leaf in the same handful of page touches, so the O(1)-versus-O(log n) distinction does not survive contact with real fanout. What the tree has and the radix structure does not: it is authoritative, it answers ranges, and it costs space proportional to *live rows* rather than to *issued ids*. Addressing entries by pk also forces a dense, monotonically issued id sequence on the whole engine — a constraint paid across every layer to buy a duplicate of an index that already exists.

This design duplicates nothing. **No index maps a pattern instance to a cross-relation tuple set**, and that gap is what Waystone fills.

The headerless-page facility that structure left behind — `DevicePageStore::CreateNewHeaderless()` and the durable `kHeaderlessMap` bitmap page — now serves the directory's interior pages (§5). The waystone pages themselves stay headered; it is only the structure that finds them that gives up its checksum.

## 11. Testing requirements

All deterministic (injected clock, simulated scheduling; `docs/rules/rules.md` §4):

1. **Fingerprint:** inline-literal and bound-parameter forms of one statement yield one `pattern_id`; different shapes differ; stable across runs and processes; `fingerprint_version` mismatch retires stored waystones.
2. **Codec & directory:** header and entry round-trips; offset/size asserts; walk correctness including lazy allocation and depth growth; `arg_hash` collision resolves to a miss via the header check, never to a foreign trail.
3. **The advisory contract — the test that must never be allowed to fail.** For a fixed query set, results are **byte-identical** across five configurations: recording on, recording off, replay off, all waystones deleted mid-run, and a deliberately corrupted trail whose entries name valid pages and slots holding *different* tuples. The last case proves the §2 rule-1 identity check is load-bearing.
4. **Lookup-not-search:** a pattern with a non-pk predicate must produce identical results and must be shown (instrumented) to still perform its search. A trail that shortcuts a search is a correctness bug, not an optimization, and this is the test that catches it.
5. **Cross-relation replay:** a `kJoinSelect` trail spanning three relations replays in `step_id` order and produces the same rows in the same order as the authoritative path.
6. **MVCC:** a trail recorded under one read view, replayed under another, returns exactly what the authoritative path returns — including a row deleted since recording (invisible) and a row inserted since (present via the authoritative search step, absent from the trail).
7. **Epoch validation:** a page whose epoch is bumped renders its entries untrusted; re-recording restores them.
