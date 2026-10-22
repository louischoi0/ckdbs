# Waystone — Concept & Technical Specification

How KDS remembers where a repeated query found its rows. `[PROPOSED]` marks a default that has not been confirmed.

**Naming:** the **Keystone** word is each tuple's structural identity; a *waystone* guides travelers without being the road. The advisory role is what the name is for.

**Status: recording and replay both work, for the patterns an operator opted in** (CF-S1b, CF-S1c; `instructions/v3.2.0/workorder-cf-waystone-premise.md` §8). `WAYSTONE ON <select>` opts a shape in and `WAYSTONE OFF` takes it out (§4a); a shape nobody opted records nothing, replays nothing, and pays one atomic load per `SELECT` while nothing on the instance is opted (§4b). No `SELECT` writes a catalog row or appends to the log: there is no auto-registration and no run-time directory claim. A repeated instance of an opted pattern is served from a validated recorded location instead of a descent or a walk. What holds today, each a fact and not a plan: trails are never retired, decayed or evicted; trails are unlogged, and **every mount discards them** — every directory root cleared, each opted pattern given a fresh empty directory (§4c), and `sys.access_stats` emptied after recovery (`server::DiscardAdvisoryState`), because a crash can leave a directory page naming a child whose allocation bit never reached the device, which the cursor would hand to a logged page; a mount cannot tell a crash from a clean stop, so a restart costs every trail — replays, never results — and the pages the old roots reached stay allocated and unreached; the opt-in itself is logged and survives every mount; a trail is one page, never continued — more entries than fit means no trail rather than a truncated one (§9); the page epoch is recorded and checked (§2 rule 2); and `rel_oid` is checked against the *query*'s step, while the verifier checks the page's class and `owner_oid` against the step's relation - so an entry naming a page a dropped relation's reclaim freed and another relation reused is a miss (`exec/tuple_verify.hpp`; `instructions/v3.0.0/workorder-bf-drop-table-page-reclaim.md` BF-R10). Trails of a dropped relation are not retired (§9); they answer nothing (`drop-table.md` DT4).

---

## 1. Concept

A **waystone is the recorded trail of one pattern instance.**

A *pattern* is the shape of a query or procedure, identified when it is parsed and reduced to function form — `patternX(a, b)`. A *pattern instance* is that shape with its arguments bound: the pair `(fetch_id, arg_hash)`, where `fetch_id` is the shape with the leading `SELECT`'s select list left out (§3). Executing an instance touches some set of tuples, possibly across several relations. The waystone for that instance records their **Keystones**, with where each one was last seen.

Three properties follow, and they define the structure:

- **A relation holds no Keystone map.** Nothing addresses a tuple by arithmetic on its tuple key, so there is no per-relation directory, no coverage guarantee, and no per-relation enable flag. A relation stores tuples; that is all it does.
- **A waystone spans relations.** One page holds the Keystones of a customer row, the account rows under it, and the instrument rows those reference — because that is what one execution of `patternX(a, b)` touched. Every entry therefore carries its own `rel_oid` (§6).
- **TupleKey values may be arbitrary.** Nothing here requires a dense, monotonically issued id sequence, so a btree leaf may hold any tuple key the rest of the engine permits. See §8: this *permits* a relaxation, it does not perform one.

Waystone lives outside the executor. The executor emits a trail through a one-method seam and asks for one through another; all storage and policy belong to Waystone, which is what keeps the advisory contract structurally enforceable.

## 2. What a waystone is not — the trust model

**A waystone is a trail, not an answer.** It records where a previous execution *found* rows. It never asserts that the set it holds is complete.

This is not a preference. Invariant 9 says Waystone is never authoritative, and a stored set trusted as the complete answer to `patternX(a, b)` *is* authoritative — it would be the sole reason the engine believes no other row qualifies. The failure mode is severe in a particular way: a stale entry that points at the wrong place is caught by the Keystone-id check, but a stored set missing a row inserted since it was recorded is wrong in a way **no per-tuple validation can detect**, because there is no tuple to validate. Absence has no witness.

So the normative rule, which every consumer is bound by:

> **A waystone may replace a lookup. It may never replace a search.**

A pattern *step* whose authoritative work is a keyed lookup — a tuple key equality, or a chain step probing the next relation by tuple key (`docs/spec/parser-v2.md` I12: the chain is the plan) — may be served from the trail, because completeness for that step follows from tuple key uniqueness, not from the trail. A step that must *search* — a non-tuple-key predicate, a range, a scan — runs authoritatively no matter what the trail says; the trail may only prefetch for it.

Replay contract, normative, per entry:

0. **Re-derive the step's key from the current execution and require it to equal the entry's `tuple_key`** - from the statement's own values for the driving step, from the current outer row for a probe. Mandatory before **any** replay, the driving step included (`docs/spec/parser-v2.md` I17): every other rule below validates the trail against storage and none looks at the query, so an outer row whose join column changed since recording, or a driving key that is not the one the trail was recorded under, would otherwise pass all of them and emit the wrong row. **Two mechanisms hold it, neither a separate step a caller must remember.** The replay index (`include/kds/exec/trail_replay.hpp`) is keyed on `(step_id, tuple key)`, and both replay sites look it up by the key their step just derived (`step_vm.cpp`'s `RunPointStep`), so an entry is found only by matching that key. And rule 1's identity check is made against that same derived key: the location the index hands back carries no `tuple_key`, so the replay can only pass `VerifyTupleAt` the key. (AR1's rule 0′; D8 as AR1-V1 amends it.)
1. Read the tuple at the recorded `(page_id, slot)` and check that the Keystone id there equals the key the step derived (rule 0; for an entry found by that key, that is its `tuple_key`) **and** that the page belongs to the entry's `rel_oid`. A mismatch is a stale entry, not corruption: fall through. The id half is checked against the tuple actually there; the relation half against the *query*'s step rather than against storage, because nothing can ask a page which relation owns it.
2. Check the recorded `page_epoch` against the page's current epoch. A mismatch is a miss — for every location recorded against that page at once, and checked before the slot is read. The entry's `page_epoch` is the page's `page_epoch` observed at access, narrowed to the entry's `u32`; the comparison is `exec/tuple_verify.hpp`'s, shared with the Cabin's location hint so there is one verifier. An epoch match never *accepts* a location on its own — rule 1 still runs.
3. Apply MVCC visibility exactly as the authoritative path would. The trail chooses *where to look*, never *what is visible*. A validated location is handed to the same `AcceptTupleAt()` a descent feeds, so whatever the authoritative path does about visibility, replay does identically because it is the same call.
4. On any miss, fall through to the authoritative path **for that step alone** — a btree descent, every user relation being a btree (`heap-and-tuple.md` §5). A missing waystone, a dropped one, or the whole structure deleted changes no result; it costs the descents the trail would have saved.

Nothing caches a set as complete. Doing so would require amending invariant 9 and a completeness mechanism to go with it — a per-relation change stamp bumped at *commit*, not at write.

## 3. Pattern identity

`pattern_id` is a fingerprint of the statement's *shape*, computed **at parse time** and never per execution (`docs/spec/parser-v2.md` I1). Literals are parameterized as they are lexed: the shape stream hashes to `pattern_id`, and the ordered literal values hash to `arg_hash`. `WHERE id = 42` and `WHERE id = ?` therefore converge on one `pattern_id`, which is the property that makes the whole structure work — a client that inlines literals and one that binds parameters share a waystone.

**The trail is keyed by `fetch_id`, not `pattern_id`, since AP-S2** (`instructions/v3.0.0/workorder-ap-function-catalog-fetch-id.md` AP-R2). `fetch_id` is `pattern_id`'s hash with the tokens between the leading `SELECT` and the `FROM` that ends its select list left out (`fingerprint.hpp`; for every other leading word the two are equal). Two statements differing only in what they project read the same tuples by the same path - the compiled chain does not read the projection, and an aggregated statement compiles to its unaggregated twin's chain (`aggregate.md` AG1) - and a trail records where tuples were, never what was projected, so they share one trail. `pattern_id` stays the statement's identity everywhere else: the cabin optimizer counts by it and `ANALYZE` prints it beside `fetch_id`. A shared trail can cost a miss and cannot produce a row: replay finds an entry only by `(step_id, derived key)` and rule 1 checks its `rel_oid` against the step (§2).

Two obligations follow:

- **Stability.** `fetch_id` is persisted in `sys.patterns` and in every waystone header and is the key to stored waystones, and `pattern_id` is the cabin optimizer's key, so neither may depend on pointer values, hash-map iteration order, or anything else that varies between runs of the same binary.
- **Versioning.** Every pattern row carries a `fingerprint_version`; a row whose version does not match the running build is ignored, and its waystones with it. This is the cheap alternative to a migration that would have to re-parse stored SQL the engine no longer keeps. `kFingerprintVersion` moves only per `fingerprint.hpp`'s bump rule: an additive shape that changes no existing hash does not move it, and the golden corpus is the witness. **It is 2 since AP-S2**, the one bump in which no hash moved: version-1 rows are keyed by `pattern_id`, version-2 rows by `fetch_id`, and the bump is what keeps a version-1 row from being read as a version-2 one (AP-Q2 (b)).

## 4. Catalog — `sys.patterns`

Patterns are catalog objects, in a relation named `patterns` in the `sys` namespace, bootstrapped on its own fixed page (page 9) alongside `sys.tables` and friends. **Since CF-S1b `sys.patterns` is the opted set**: one row per shape an operator has ever opted in, with bit 0 of `flags` saying which are opted now. `WAYSTONE ON` is its only inserter, `WAYSTONE OFF` clears a row and keeps it, and the only other writer is the mount's renewal of each directory (§4c). On a superblock-33 volume no row was registered by traffic.

| Field | Type | Meaning |
|---|---|---|
| `oid` | `Oid` | the pattern object's oid, from `AllocateRowId(kSysPatternsTable)` |
| `fetch_id` | `uint64` | the parse-time fetch fingerprint (§3); the lookup key. A `fingerprint_version` 1 row holds a `pattern_id` here |
| `fingerprint_version` | `uint32` | §3; a mismatch retires the row's waystones and lapses its opt-in (§4c) |
| `stmt_class` | `uint8` | the parser's execution-class tag of the example `WAYSTONE ON` compiled (`docs/spec/parser-v2.md` I2; every step-chain statement carries `kJoinSelect`, per J3) |
| `waystone_root` | `PageId` | root of this pattern's `arg_hash` directory, `kInvalidPageId` when none |
| `dir_depth` | `uint8` | levels the directory walk traverses; persisted, never derived |
| `use_count` | `uint32` | reserved: written 0 at insert, advanced by nothing |
| `last_seen` | `uint64` | reserved: written 0 at insert, advanced by nothing |
| `flags` | `uint16` | bit 0 `kPatternWaystone`, the opt-in: set by `WAYSTONE ON`, cleared by `WAYSTONE OFF`. Every other bit is written 0. Re-scoped from `kPatternPinned`, which had no writer since 2026-08-31 |
| `origin` | `uint8` | **reserved since CF-S1b**, written 0 and read by nothing. It said who created a row (traffic or the withdrawn `CREATE PATTERN`); `WAYSTONE ON` is now the only creator, so it has no second value to carry |

The row is 41 bytes and no offset moved with the re-scoping; superblock 33 is what keeps a version-32 binary from reading the bit as `kPatternPinned` (`include/kds/server/superblock.hpp`). **The bit is a recording-policy bit on the fingerprint's own row** and carries no second identity: `fetch_id` is the only name a pattern has, and no statement text is stored, which is what separates it from the withdrawn declaration (`create-pattern-user-defined-patterns-v1.md` §1). A row is opted when bit 0 is set **and** its `fingerprint_version` is the running build's (`catalog::IsCurrentOptIn`, `rows.hpp`); the opted count, the opted map, the renewal and the view's `waystone` column all ask that one predicate.

**A recorded execution writes its trail and no catalog row.** `use_count` and `last_seen` were the pattern's heat, advanced by every recorded execution through a logged rewrite of the row; that write put a repeated point `SELECT` on the one WAL behind any bulk writer, and it was deleted with `Catalog::TouchPattern` on BL-S4's ruling 1 (`instructions/v3.0.0/raft-marks-2026-10-12.md` §11, W11). The two fields stay on disk, and `SHOW PATTERNS` and the `sys.patterns` view print them as zero. `sys.patterns` stays wholly logged: every field a writer moves is written through the WAL, and `sys.access_stats` remains the one unlogged catalog relation `docs/rules/rules.md` §5 admits.

`waystone_root` and `dir_depth` are written as one unit and validated as a pair: a root without its depth is unwalkable, and a depth disagreeing with the root sends every walk to the wrong leaf. They have two writers, `Catalog::SetPatternWaystone` (`WAYSTONE ON` and `OFF`, under page 9's hold) and `Catalog::RenewPatternDirectories` (the mount, §4c), and neither replaces a root a run is recording into. `dir_depth == 0` is the authority on "no directory" — a row read out of a zeroed page decodes its root as page 0, which looks like a valid `PageId`, so the question is keyed on the field whose zero value already means none.

Why a catalog relation rather than an in-memory table: an opt-in is the durable, inspectable statement of *what this database is asked to remember*, the rows are few (the shapes operators opted, never every shape that repeated), and making them catalog objects gives `SHOW PATTERNS` and the opt-in one place to live. The unbounded axis is not patterns but *instances per pattern*, and nothing bounds instances today.

**An opt-in moves the schema word; a `SELECT` writes nothing.** `SetPatternWaystone` calls `BumpVersion("waystone opt-in")` after it releases page 9 and after its write, never before, so a peer that revalidates against the new word reads the new row. Every core drops its pattern entries at its next statement head and refills from the page (§4b); the mount's renewal drops core 0's cache instead (§4c). Nothing on the statement path writes the row, so no pattern change can dangle a `const TableAccess*` a `SELECT` holds: the write is the `WAYSTONE` statement's own, and it releases its compiled chain and every catalog handle before it writes.

## 4a. The opt-in — `WAYSTONE ON` / `WAYSTONE OFF`

```
WAYSTONE ON  <select>
WAYSTONE OFF <select>
WAYSTONE OFF <fetch_id>        -- decimal, or 0x and 1-16 hex digits; fits 64 bits
```

- **A dispatcher prefix verb**, beside `ANALYZE` (`CommandDispatcher::HandleWaystone`): no grammar production and no reserved word. It is not a patternable leading word, so `kFingerprintVersion` did not move. The example is parsed as the whole command's text with the prefix blanked, so every byte a refusal names is the command's own, and the fingerprint is the bare `SELECT`'s, as `ANALYZE`'s is.
- **The gates, in the dispatcher's order**: the role check (`WAYSTONE` is unclassified in `RequiredRole`, so **admin's**), the WAL archive's fail-closed gate (`WAYSTONE` is on its list, `backup.md` B13), the failed-transaction gate, then the handler. `sys.statement_stats` counts it under the kind `WAYSTONE`.
- **Autocommit only** (CF-O-Q7 (a)): inside an explicit transaction it is refused `NotImplemented` at byte 0 before anything is read, so the transaction is not poisoned. The writes are outside any user transaction - an insert at `kBootstrapXid`, an overwrite at `kNoTxnId` - and an advisory bit gains nothing from atomicity with user writes.
- **`ON` names its pattern by example** (CF-O-Q1 (a)). It parses and compiles the `SELECT` through the native path `SelectBody` takes, under the session's namespace and with the bind's read borrow declared (AT-S1), and **does not execute it**. It takes `fetch_id` from the parse's fingerprint and `stmt_class` from the compiled chain, refuses a shape no trail could serve, releases the chain and the borrow, writes, and discards the text. A borrow the bind cannot take fails nothing, as for every read borrow (`read_borrow.hpp`), so `ON` compiles beside a DDL's `X` as the same `SELECT` runs beside it.
- **`OFF <select>` only parses and fingerprints**, never compiles, so none of `ON`'s shape, determinism or borrow refusals apply, and `OFF` can always clear what `ON` set - after a `DROP TABLE` of every relation it names included. `OFF` accepts a `?` (the text is fingerprinted as written and parsed with each `?` read as a literal). **`OFF <fetch_id>`** parses the number only; `ANALYZE` prints `fetch_id` in hex and `SHOW PATTERNS` in decimal, and `OFF` takes both.
- **Refusals.** Each names the offending token's byte in the whole command, and nothing is written on a refusal.

| Case | Applies to | Code | Byte |
|---|---|---|---|
| A session below admin | both | the role gate's `ERR permission:` text, which answers before every other gate | none |
| The WAL archive gate engaged | both | `ResourceExhausted` | 0 |
| Inside a failed explicit transaction | both | the failed-transaction gate's own refusal | none |
| Inside an explicit transaction | both | `NotImplemented` | 0 |
| A missing or unknown second word | both | `InvalidArgument` | that word, or the end |
| Nothing after `ON` / `OFF` | both | `InvalidArgument` | the end |
| `ON` followed by a number | ON | `InvalidArgument` | the number |
| `OFF <fetch_id>` malformed, or past 64 bits | OFF | `InvalidArgument` | the number |
| `INSERT`, `UPDATE` or `DELETE` | both | `NotImplemented` | the leading word |
| `WITH`, or another statement the query layer reads (`parser-v2.md` I19) | both | `NotImplemented` | the leading word |
| Any other leading word, `ANALYZE` included | both | `InvalidArgument` | the leading word |
| A `?` placeholder (CF-O-Q11 (a)) | ON | `NotImplemented` | the `?` |
| An example that does not parse | both | the parser's own code; for `ON`, `NotImplemented` where the query layer reads it | the parser's byte |
| A statement that is not one `SELECT`, or has no fingerprint | both | `InvalidArgument` | the example |
| An example that does not compile | ON | the compiler's own code; `NotImplemented` where the native compile declined a statement the query layer reads | its byte, else the example |
| A statement calling a row-volatile (D2) function | ON | `Unsupported` | the call |
| A shape with no tuple key equality: no `=` conjunct, in the example or any subquery, equates a relation's tuple key with a literal, another relation's column or an enclosing row's (`exec::ShapeAdmitsDescent`) | ON | `Unsupported` | the example's `SELECT` |
| `sys.patterns` unreadable, no page for the directory (a full pool is `ResourceExhausted`), or the write failing | ON / both | that status | 0 |

  **The recordability refusal is drawn at the shape, never at this plan** (`status.hpp`'s test for `Unsupported`): a shape with no tuple key equality has no step Waystone records under any plan, for good (invariant 9). Whether the chain *as compiled now* has a `kLookup` or `kProbe` step is not a property of the shape - join order is chosen per `arg_hash` and per plan statistics, and a negative tuple key literal compiles to a scan under a `fetch_id` a positive one shares - so a shape that admits a descent and compiles to none now is accepted, and the gate's per-execution `HasReplayableStep` keeps such a run from recording (§4b).
- **The writer** is `Catalog::SetPatternWaystone(fetch_id, stmt_class, on, root)`, holding page 9 exclusive across the decision and the write, so two concurrent `ON`s of one shape serialize on the hold and the existence check and the insert are one act across cores.
  - **ON with no current-version row**: `AllocateRowId`, then an insert with `flags = kPatternWaystone`, the passed root at depth 1 (no root, depth 0, where none was passed), logged at `kBootstrapXid`.
  - **ON with a row**: set the bit, and write the passed root if the row has no directory; one logged overwrite at `kNoTxnId`. A row already opted that needs no root is `unchanged`.
  - **OFF**, under either spelling: one whole-chain walk under the hold clearing the bit **and** the root of **every** row carrying that `fetch_id`, of any fingerprint version. The rows are kept; their directories become unreachable, as at a mount. Every version, because a `fetch_id` is not salted by `kFingerprintVersion`: after a bump a lapsed opted row and the current row a re-issued `ON` inserted commonly share one `fetch_id`, and a first-match walk would clear one and leave the other recording. `OFF` answers `unchanged` when none of the rows it met had bit 0 set.
  - **Then** page 9 is released and the schema word moves (§4). An `OFF` that fails part-way still moves the word for the rows it already cleared.
- **The directory is made before page 9's hold, never under it.** A directory page is a headerless create (`stats::CreateDirPage`), which on an id the device still holds writes zeros and waits on the data-sync thread's sync (`device_page_store.cpp`); a catalog page latch is never held across that. So `ON` reads its row with no hold and, where `waystone_recording` is on and the row is absent or has no directory, makes the page first and passes it in. A passed page the write does not use - a concurrent `ON` rooted the row first, the row was found opted with a root, or the write failed - is logged `WARN [waystone] directory page <id> leaked by WAYSTONE ON`. **With `waystone_recording` off** `ON` sets the bit and makes no directory: there is no recorder, so nothing would record into it whatever `waystone_replay` says, and replay against a row with no directory is a plain miss. The next mount with recording on makes it (§4c).
- **Replies**: `OK WAYSTONE ON fetch_id=<decimal> waystone_recording=<on|off> waystone_replay=<on|off>` - the instance's two boot ceilings under their keys' own names, since an opt-in under an off ceiling is accepted and records nothing - and `OK WAYSTONE OFF fetch_id=<decimal>`; each ends ` unchanged` when nothing was written.
- **What the answer promises, by the session's durability rung.** The hold is released before any wait. The answer rests on a record: for a write, `appended_lsn() - 1` read after it (a later bound, never an earlier one); for `unchanged`, the same read **under page 9's hold**, which bounds whichever earlier statement set the state it found, since every writer of the bit writes under that hold. Under `strict` and `group` the statement requests a sync and parks in the dispatcher's post-statement park, as a `group` commit does, so the state answered is durable; a fail-stopped log turns into the refusal, and a failed sync request is refused with a message saying the state is in effect for this run but may be lost at a restart. Under `relaxed` the answer comes at once and the bit is durable within the relaxed interval, as a relaxed commit is; a crash inside it can lose the opt-in, which costs performance only (invariant 8).

## 4b. The gate — what a `SELECT` pays

- **The opted count** (CF-S1c): one atomic for the instance, `InstanceWords::opted_count` (`catalog.md` CT6), of the current-version rows with bit 0 set. `Catalog::NoOptedPatterns()` is one load. It moves only after a successful write under page 9's hold, and only on a 0->1 or 1->0 change of bit 0 on a current-version row; clearing a lapsed row's bit leaves it alone, and the decrement saturates at 0. It is set from the chain at every boot: by the renewal's walk (§4c), or by `RecountOptedPatterns` on a boot that runs no renewal (the sim's `skip_recovery`).
- **The opted map**: each core's catalog cache holds the opted patterns, `fetch_id -> PatternAccess`, **complete per cache generation** (`catalog_cache.hpp`). `Catalog::FindOptedPattern` fills it at the first ask after a cache drop with one `ScanAll` of `sys.patterns`, keeping every current-version row with bit 0 set and nothing else; every later ask in the generation reads no page, and a miss is `NotFound` with no scan. While the count is 0 nothing fills.
- **The soundness rule**, stated in `catalog_cache.hpp` and pinned by `PatternCatalogTest.TheOptedMapIsCompletePerGenerationAndMissesWithNoScan`: every write that changes the bit or an opted row's root either moves the schema word or is followed by a drop of the writing catalog's cache before that catalog serves a statement. Given it and the count's rule, a stale answer lasts at most one statement after a successful `ON` or `OFF` - a zero read during a peer's `ON` misses one statement, a non-zero read just after the last `OFF` pays one fill - and costs performance only (invariant 8). A peer still holding a root `OFF` cleared writes into a directory that is unreachable but allocated, which is safe because no Waystone page is freed during a run.
- **In `SelectBody`**: with either key on, a statement that is not D2, has a fingerprint, finds the count non-zero and whose chain has a `kLookup`/`kProbe` step (`HasReplayableStep`, asked before the map so a scan-only run never triggers the fill) asks `FindOptedPattern(fetch_id)`. Only a found entry is Waystone-usable: replay reads the trail from that entry's directory, with no second lookup, and the trail collector is armed only where a recorder exists and the entry was found, so a shape nobody opted is never sighted and collects nothing. A lookup that cannot read is a pattern that is not opted, counted in the recorder's `write_failures`. The statement's identity (`pattern_id`, `fetch_id`) is still built where the cabin optimizer's signals are wired or the statement is an `ANALYZE`, so `ANALYZE` prints `fetch_id=` for a shape nobody opted - which is how an operator names one.

| Statement | Pays | Does not pay |
|---|---|---|
| Nothing opted on the instance | one atomic load | `HasReplayableStep`, the map, the collector, `Observe`, the directory, the trail |
| Something opted, not this shape | the load, `HasReplayableStep`, one hash miss, and a share of the generation's fill | the collector, `Observe`, the directory, the trail |
| Opted | the load, the hash hit, the trail read and replay index, the collector and the recording | - |

**The refill.** While the count is non-zero, every core refills its map by one whole-chain scan of `sys.patterns` once per schema word move: a DDL, a clustered or index root growth (on every core but the growing one; a root growth comes from DML), a `CABIN AUTO` create or drop, a catalog-writing resolve, or an opt-in. The chain holds only the rows ever opted, so the scan is bounded by what operators opted, not by what repeated.

**Cross-core.** The bit reaches every core through the word, and a peer answers from its previous generation for at most one statement. Every core's fill already sees the root, because the directory exists before the word moves; no core makes a directory at run time, so there is no root-claim race. Sightings stay per core; trails stay the instance's.

## 4c. The mount — renewal and the lapse

- **`Catalog::RenewPatternDirectories(waystone_recording)`** runs where the old clear of every root ran: core 0, inside `server::DiscardAdvisoryState`, after recovery (and on a restore after the preparation's `ForgetUnwrittenPages`), before the listener binds; the sim's `Boot` makes the same call. `waystone_replay` does not enter, for the reason `ON`'s directory rule gives (§4a).
- **Two passes, so no page is created under a catalog page latch.** Pass 1 reads the chain and collects the current-version opted rows. Then, with no latch held and only where `waystone_recording` is on, one `CreateDirPage` per collected row. Pass 2 is one `kWrite` walk writing each collected row's fresh root at depth 1 as a logged overwrite and clearing **every other root, whatever its fingerprint version**, so a lapsed row keeps no stale root. A fresh root names no stale child, so the discard's reason holds. With recording off every root is cleared and none is made.
- **A creation that fails** leaves the row's root cleared and its bit kept, logs `WARN [waystone] pattern oid <oid> has no directory this run: ...`, and the mount still opens; nothing records for that row until the next mount or an `OFF` and `ON`. A page made for a row that changed between the passes is logged `WARN [waystone] directory page <id> leaked by the mount's renewal`.
- **The walk ends by dropping core 0's catalog cache, then setting the opted count** from pass 2's count. Without the drop, a pattern entry filled before the walk would keep the previous run's root, whose children can be ids the cursor has since handed to logged pages, and a trail written through it would overwrite them with no record. `[quiet-wrong]` without the drop; a cell pins it.
- **The boot log line**, after the discard: `INFO [waystone] waystone_recording=<on|off> waystone_replay=<on|off> waystone_opted=<n>`, under the keys' own names; `sys.meta` carries the same three fields in its `waystone` section (`sys-views.md` §7).
- **The mount's added cost**, per opted row: one device read of the new page's id and, on an id the device still holds, a zero write and a sync on the data-sync thread, plus one logged overwrite. Paid once per opted row before the listener binds; not measured.
- **The lapse.** A `kFingerprintVersion` bump makes every opted row of the old version another version's: it stops counting, records nothing, keeps no root, and reads `waystone = 0` in the view with its `fingerprint_version` saying why. The mount logs `INFO [waystone] <n> opt-ins lapsed at fingerprint version <v>` (at `log_level = info` only), counting a lapsed row only while no current-version row with its `fetch_id` is opted, so a lapse the operator has re-issued is not reported again. Carrying the bit forward would need stored text, which is excluded by design (CF-O-Q12 (a)); the operator re-issues `ON`, which inserts a current row (commonly under the lapsed row's `fetch_id`), and `OFF` clears both.
- **Warm-up after every mount.** Trails are discarded at every mount (CF-O-Q2 (a)), so an opted instance records again on its second execution on one core and replays thereafter on any core.

## 4d. The opt-in is by text

`fetch_id` is a hash of the statement's text with no namespace and no oid in it. So:
- **One text is one opt-in in every namespace.** `SELECT ... FROM t WHERE id = 1` opted under `USE a` is opted under `USE b`, over whichever `t` that resolves to. Replay stays correct, because every trail entry carries its `rel_oid` and replay drops one recorded against another relation (`exec/trail_replay.cpp`), but the other relation's statements pay recording too.
- **An opt-in survives `DROP TABLE`** and a re-create under the same name: no `sys.patterns` writer runs on a drop. It must be turned off explicitly, with `OFF <select>` (which only fingerprints, so it works with the relation gone) or `OFF <fetch_id>`. The renewal cannot tell an example that no longer resolves, so each mount renews a directory for it.

## 4e. What it buys and costs — measured

CF-S0 (`bench/v3.3.0/results-cf-s0-premise-v3.3.0-97-g3d628df4.md`, at `v3.3.0-97-g3d628df4`, engine source `a1809988`; `build-release`, `cores = 1`, `durability = relaxed`, three arms rotated, 6 reps each) measured the engine as it stood before the opt-in, with every repeated shape recording:
- **Point reads and tuple-key join chains** (`pk`, `chain3`, `chain4`): replay gained nothing (-0.2 % to -1.1 %, inside the spread), and recording cost 2.7-3.9 % of server CPU per statement against both keys off, about 1.2-1.5 us, outside the spread on `pk` and `chain3`. Under the opt-in a shape nobody opted does not record.
- **A scan-driven chain with many tuple-key probes** (`scanprobe`: a `FilterScan`, then 200 tuple-key probes per statement): replay paid +5.7 % throughput and 7.2 % less server CPU against recording alone, and +5.0 % and 6.4 % less against both keys off, outside the spread; recording cost it nothing measurable. This is the shape an opt-in is for.
- Every reply digest matched across all arms (invariant 8).

Not measured yet, and owed by CF-S2's close: the opt-in's own steady state against CF-S0's arms, `cores > 1`, a cold pool, the refill under a moving word, and the mount's per-opted-row directory create. The opted path's own costs - the exclusive, dirtying directory `Get` on every re-record and `TrailReplay::Build`'s per-statement map - are left as built (CF-O-Q13 (a), `known-gaps.md`).

## 5. Addressing — two levels

```
fetch_id    --> opted sys.patterns row    (the core's opted map, complete per generation, §4b)
arg_hash    --> waystone for that instance (directory walk under waystone_root)
```

The second level is an inode-style page directory: interior pages of 2048 `PageId` children, walked by digits of the `arg_hash`. The root is made by `WAYSTONE ON` or the mount's renewal (§4a, §4c), never at run time; the trail pages under it are allocated lazily by the recorder. A directory could be deepened by relinking the root, but **nothing deepens one**: `GrowPatternDirectory` has no caller, so every directory is depth 1 and a colliding instance displaces; a deepening would be a third writer of the root, needing page 9's hold and a move of the schema word (`waystone_dir.hpp`). Depth is bounded at `kMaxPatternDirDepth` = 6, derived rather than chosen — ceil(64 / 11) levels address a 64-bit key at a fanout of 2^11.

It is a hash directory, not a radix index over a dense key, so **collisions are possible**. An `arg_hash` collision must be resolved by the waystone's own header, which stores the `arg_hash` it was recorded for; a mismatch is a miss, never a wrong trail. A colliding instance **displaces** the trail already there (`TrailDisplacesOnCollision()`, `[PROPOSED]`); nothing chains. A walk at depth *d* consumes the low 11*d* bits and ignores the rest: no key is ever out of range — the structure this replaced refused a tuple key past its coverage, and a hash has no coverage to exceed.

**Growth is a cache flush, not a rehash.** Relinking the root is O(1) and preserves prior mappings *only* for keys whose new top digit — bits [11*d*, 11*d*+11) of the `arg_hash` — is zero, which is 1 in 2048 of them. On the dense tuple key this design replaced, every stored key had that zero by construction; a hash does not, and no O(1) growth can give it one. Everything else is cooled, not corrupted: the old subtree stays reachable under slot 0, a key that now addresses one of its pages gets a header mismatch and a miss, and the next execution re-records the trail at the new address. Safe by invariant 8, and the reason growth must be paid for by capacity — each level multiplies addressable instances by 2048 — rather than performed routinely.

**Interior pages are headerless.** 2048 × 4 bytes tiles 8 KiB exactly, which is why the fanout is 2048; a common header would cost a child slot, and `DevicePageStore` stamps a checksum at byte offset 4 of every headered frame — child 1. They are allocated through `PageStore::CreateNewHeaderless()`, which records the fact durably in the `kHeaderlessMap` bitmap page. The cost is that a damaged interior page carries no checksum to catch it; it leads a walk to a page that is not the instance's waystone, which the header check turns into the same miss a cold directory gives.

## 6. Page format

Waystone pages are **headered** — `PageType::kWaystone`, carrying the common page header like every other page class. Nothing here is addressed by shift and mask (a trail is read sequentially), so the payload need not tile the page exactly, and the pages keep checksums and a `page_lsn`.

Page body: a waystone header, then entries in **execution order**; an entry's `step_id` is its relation's written position (CB-Q3), so a trail recorded under one join order is read by `(step_id, tuple key)` under another.

Waystone header:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `fetch_id` — self-identifying, checked on read; a page written before AP-S2 holds a `pattern_id`, and a `SELECT`'s never equals its `fetch_id`, so it reads as a miss |
| 8 | 8 | `arg_hash` — resolves directory collisions (§5) |
| 16 | 2 | `entry_count` |
| 18 | 2 | `flags` |
| 20 | 4 | `next_page_id` — reserved; a trail is never continued (§9) |
| 24 | 8 | `recorded_ts` |
| 32 | 4 | `use_count` — written 0, advanced by nothing |
| 36 | 4 | `reserved` |

Entry — 32 bytes, one per Keystone in the trail:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | `tuple_key` | zero-extended Keystone id, upper 16 bits 0 (invariant 7) |
| 8 | 8 | `rel_oid` | **the field a per-relation structure did not need.** One page spans relations, so the entry must say which. A full 64-bit `catalog::Oid`: oids are `uint64` at the source, and a narrowed copy that happens to fit today aliases two relations onto one value the day it does not |
| 16 | 4 | `page_id` | last observed location; advisory |
| 20 | 4 | `page_epoch` | location trust (§2 rule 2); the page's `page_epoch` narrowed to 32 bits |
| 24 | 2 | `slot` | last observed slot |
| 26 | 2 | `flags` | `kWaystoneEntryValid`, rest reserved |
| 28 | 2 | `step_id` | which step of the pattern produced it — the join position. 16 bits counts relations in a join chain, not rows |
| 30 | 2 | `reserved` | 0 |

`step_id` is what makes a cross-table trail replayable rather than merely descriptive: the executor needs to know that entry 0 is the driving relation and entries 1–17 are the probe results, or the trail is an unordered bag of tuples from three tables.

Entries per page is derived, not chosen: `(8192 − 32 common header − 40 waystone header) / 32 = 253`, with 24 bytes of tail slack. Not a power of two, and it does not need to be — the exact tiling a tuple-key-addressed structure required is precisely what cost it the page header.

Format rules as ever (`docs/rules/rules.md` §§2, 5): field-wise `memcpy` through named offsets, `static_assert` on every size and offset, fixed-width little-endian, no bitfields, no `reinterpret_cast` onto page bytes.

## 7. What this buys

Per pattern step served from a trail, against the authoritative path it replaces:

- **`kJoinSelect`, the case this design exists for.** Written-order nested-loop over 3 relations is 3 keyed descents, ~3 page touches each. A validated trail replay is 3 direct page reads. The saving grows with the join's length, and join length is exactly what a financial procedure has.
- **`kPointSelect`.** One descent becomes one read. Modest, and not the reason to build this.
- **Any step that searches.** Nothing. By §2 it still searches. Prefetching its pages is the only permitted use, and prefetch is advisory twice over.

Measured against the `bench/` tree at `1769487`: replay is 3-7% slower than a descent on a B+ tree relation. CF-S0 (§4e, at `v3.3.0-97-g3d628df4`) measured no gain on point reads and short tuple-key join chains, and a gain on a scan-driven chain with 200 tuple-key probes per statement; that finding is why recording and replay run only for the shapes an operator opts in.

## 8. Invariants — what this touches

Numbered as `docs/spec/heap-and-tuple.md` §8 numbers them.

- **Invariant 8** (advisory; deleting it never changes results) — unchanged, and still the one that matters.
- **Invariant 9** (never authoritative) — unchanged, and load-bearing: it is what forces §2's trail model.
- **Invariant 7** (ids outside the tuple header are zero-extended `uint64`) — unchanged; entry `tuple_key` obeys it.
- **Invariant 3** (`min_key`) and **invariant 11** (tuple key provenance and ordering) — **not changed by this document.** §1 notes that pattern-keying removes the *only* structural reason the engine needed dense monotonic tuple keys. A caller may name a relation's tuple keys (`docs/spec/heap-and-tuple.md` §4.1), on the storage layer's terms - a named key is placed where it sorts and refused only as a duplicate or exhausted, its uniqueness proved by the descent (BD-R5) - not on pattern-keying's, and it changes nothing here: trail entries key on a tuple key's *value*, never on its order or its provenance. `min_key` remains immutable on every heap page; a btree leaf division keeps the old page's bound and gives the new page the split key, so invariants 2 and 3 survive the one operation that moves tuples between pages. What that operation costs Waystone is one epoch bump: the divided page's `page_epoch` advances and every trail entry into it becomes untrusted at once. A placement mid-leaf shifts the slots after it and bumps nothing; replay's Keystone check turns an entry naming a shifted slot into a miss and a descent. Invariant 8 absorbs both as a performance event, never a correctness one.

## 9. Open decisions — do not assume

None is recorded here. What holds today, each a fact and not a plan:

- **Only an opted pattern records or replays** (§4a, §4b). Nothing records until an operator opts a shape in; `waystone_recording` and `waystone_replay` are the instance's boot-only ceilings over the opted patterns, each default on.
- **Recording policy is `n = 2`, for opted patterns** (`docs/spec/parser-v2.md` J5 owns it; `TrailRecorder::kRecordThreshold`, CF-O-Q8 (a)): the first execution of an opted instance only counts; the second records. A shape nobody opted is never sighted. An execution that *collects nothing* is not counted at all, since it can never produce a trail however often it repeats. Sightings live in a bounded core-local in-memory table of `[PROPOSED]` 4096 entries (`TrailRecorder::kMaxSightings`), cleared wholesale on overflow; eviction from it merely restarts the count — a performance event, never a correctness one.
- **A trail is one page (253 entries), and a trail that would exceed it is not recorded at all.** A truncated trail covers only the first rows of an execution and **no reader can tell it from a complete one**, so replay would serve a partial answer believing it whole. Not recording leaves the instance in the state every instance starts in, which every consumer already handles. `next_page_id` stays reserved and unused.
- **Re-recording an instance replaces its trail wholesale, never merges.** A merge would accumulate rows from earlier executions that no longer qualify, and nothing at this layer can tell those from rows that still do.
- **Only lookup-class steps are recorded.** §2 already says a trail may never replace a search, so a `Scan` step's rows could only ever be prefetched — and nothing prefetches. The entry format does not depend on this.
- **Trails are unlogged**, never retired, never decayed, never evicted; an `arg_hash` collision displaces (§5). The opt-in is logged; the trails under it are discarded at every mount (§4c).
- **A directory `OFF` or a mount made unreachable stays allocated**: no Waystone page is freed during a run or reclaimed at a mount, so the leak grows with mounts and with toggles (`known-gaps.md`).

## 10. Why not a tuple-key-direct index

A per-relation structure mapping every tuple key to its location by arithmetic was built and removed (before 2026-09-02). It was a radix tree over the key the clustered B+ tree already indexes, reaching a leaf in the same handful of page touches at a fanout of 2048, while being non-authoritative, answering no range, costing space per *issued id* rather than per live row, and forcing a dense, monotonically issued id sequence on the whole engine. **No index maps a pattern instance to a cross-relation tuple set**; that gap is what Waystone fills. Its headerless-page facility (`CreateNewHeaderless()`, `kHeaderlessMap`) now serves the directory's interior pages (§5).

## 11. Testing requirements

All deterministic (injected clock, simulated scheduling; `docs/rules/rules.md` §4):

1. **Fingerprint:** inline-literal and bound-parameter forms of one statement yield one `pattern_id`; different shapes differ; stable across runs and processes; `fingerprint_version` mismatch retires stored waystones.
2. **Codec & directory:** header and entry round-trips; offset/size asserts; walk correctness including lazy allocation and depth growth; `arg_hash` collision resolves to a miss via the header check, never to a foreign trail.
3. **The advisory contract — the test that must never be allowed to fail.** For a fixed query set, results are **byte-identical** across five configurations: recording on, recording off, replay off, all waystones deleted mid-run, and a deliberately corrupted trail whose entries name valid pages and slots holding *different* tuples. The last case proves the §2 rule-1 identity check is load-bearing. Since CF-S1c the suite (`tests/waystone_contract_test.cpp`) also compares the query set opted in against not opted under every ceiling, and with the bit flipped mid-run; **every recordable opted query asserts `replays > 0`**, so a fixture that opted nothing cannot pass vacuously.
8. **The opt-in** (`tests/waystone_opt_in_test.cpp`, with `PatternCatalogTest` in `tests/catalog_test.cpp`, `ExpeditorTest.EveryMountRenewsTheWaystoneDirectoryItFinds` and `BackupControlTest.ARestoreKeepsAWaystoneOptInAndNotItsTrail`): one cell per refusal with its code and byte, the ON/OFF round trip and `unchanged`, hex and decimal `OFF`, `OFF` after a `DROP TABLE`, a lapsed row under a re-issued `ON`, the bit surviving a crash at `group`, a clean restart and a restore while the trail does not, a peer core seeing the bit after one statement, the mount's renewal ordered after recovery and before the listener, and the renewal's cache drop (`[quiet-wrong]` without it).
4. **Lookup-not-search:** a pattern with a non-tuple-key predicate must produce identical results and must be shown (instrumented) to still perform its search. A trail that shortcuts a search is a correctness bug, not an optimization, and this is the test that catches it.
5. **Cross-relation replay:** a `kJoinSelect` trail spanning three relations replays by `step_id` and produces the same rows as the authoritative path - in the same order for one plan; across plans the rows match as a multiset, since without `ORDER BY` the order is the planner's (`parser-v2.md` I12).
6. **MVCC:** a trail recorded under one read view, replayed under another, returns exactly what the authoritative path returns — including a row deleted since recording (invisible) and a row inserted since (present via the authoritative search step, absent from the trail).
7. **Epoch validation:** a page whose epoch is bumped renders its entries untrusted; re-recording restores them.
