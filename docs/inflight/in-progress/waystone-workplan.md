# Waystone — Workplan

Work instructions, companion to `waystone-concpets.md`.

Execution rules:
- Do tasks in numeric order unless "needs" says otherwise.
- Each task ships with its listed tests in the same change; `bash test.sh` green is part of "done".
- If a task turns out to touch an `[OPEN]` item in the spec — stop, flag, do not decide.
- The advisory-contract test (`P12`) is regression-mandatory from the moment it exists.

---

## The gate: pattern identity

Every task here depends on a statement having a stable identity, so Phase A comes first and is built as a bolt-on over the existing lexer rather than waiting for the blueprint parser (`docs/inflight/in-progress/parser-v2-workplan.md` phase V-6). The fingerprint is a pure function of the token stream, so it can be rewritten against the flat AST later without any consumer changing — the output contract is two integers. **Amended 2026-08-01:** that rewrite is now required to be *hash-preserving* rather than merely versioned — V01 pins every pre-existing `pattern_id` and V29 folds the pass in without moving one — so `kFingerprintVersion` is not expected to bump. If it ever does, it absorbs the change the same way: stored patterns retire, the engine re-learns, a performance event and not a correctness one.

## Phase A — pattern identity

**P01 — Fingerprint over the token stream.** — **done.**
Files: `include/kds/parser/fingerprint.hpp`, `src/parser/fingerprint.cpp`, `tests/fingerprint_test.cpp`.
Walk the lexer's tokens once. Emit `pattern_id` (hash of the shape stream, with every literal replaced by a parameter marker) and `arg_hash` (hash of the literal values in order). No second pass and no separate normalization step — the shape stream is what the lexer already produces. Hashing must be value-based: no pointers, no addresses, no container iteration order.
Tests: `WHERE id = 42` and `WHERE id = ?` yield one `pattern_id` and different `arg_hash`es; different shapes differ; identical input hashes identically across processes; `SET`/DDL statements are excluded from fingerprinting.
Needs: nothing.

Decisions worth carrying forward:

- **`TokenType::kParam` was added to the lexer.** `?` used to lex as `kError`, and without a token type there is no way to tell a placeholder from a lexing failure — which makes the convergence property untestable and unimplementable. It is lexed and accepted by no production; `tests/parser_test.cpp` pins that it still rejects, so giving it a type did not make it executable.
- **Identifiers are shape, values are arguments, NULL is shape.** A different relation is a different pattern. Int and string literals emit the *same* shape marker as `?` — a bind parameter's type is unknown at parse, so distinguishing int-shaped from string-shaped holes would break convergence; the type moves to the argument stream instead. NULL carries no value to bind and gets its own marker.
- **Literal *text* is hashed, not the decoded value.** Hashing `int_val` would let two distinct literals collide, because the lexer's decode silently wraps past 64 bits. The cost is that `42` and `042` miss each other, which costs a replay; a collision would cost a wrong location.
- **ASCII case folding by hand, not `std::tolower`.** The latter is locale-dependent, and this hash goes on disk.
- **Fields are length-prefixed** in the hash stream, so `FROM ab WHERE c` and `FROM a WHERE bc` cannot flatten to the same bytes. Tested directly.
- **Collisions are survivable by construction, not by hash strength.** FNV-1a/64 is not cryptographic, but a colliding pattern's trail names tuples that fail the replay validation in spec §2, so it degrades to a miss. A collision can cost performance; it cannot produce a row.
- **The golden-value tests were cross-checked against an independent implementation** of the same byte stream rather than recording whatever the C++ emitted. They pin a value persisted in `sys.patterns`; changing the algorithm is a format change and is P02's business.
- ~~Known cost of the bolt-on: this lexes the statement a second time, separately from `Parser`.~~ **Fixed 2026-08-03**: the lexer feeds a `FingerprintAccumulator` as it produces tokens, so a parse yields the fingerprint with no second pass, and `FingerprintOf()` — still there for callers with a string and no parse — is implemented on the same accumulator. `kFingerprintVersion` did not move; the golden corpus pins that, and a differential test compares both paths over every statement in it. The second lex had measured ~13% of a point join's latency and was the whole of replay's B+ tree regression (`bench/results-waystone-v2.md`). The contract — text in, two integers out — is what survives the blueprint parser regardless.

**P02 — Fingerprint versioning.** — **done.**
Files: same header.
A `kFingerprintVersion` constant with the rule stated at its definition: bump it whenever the token stream or the hash changes, and stored patterns whose version differs are ignored along with their waystones. This is the seam that lets the blueprint parser replace `P01` without a migration.
Tests: a pattern row with a foreign version resolves as "no pattern", not as an error.
Needs: P01.

Decisions worth carrying forward:

- **The bump rule is narrower than "the algorithm changed".** Bump when an *already fingerprintable* statement would hash differently — a change to the token stream a statement reduces to, the shape or argument tag values, a hashed field's framing, the case-folding rule, or the hash function. Making a statement fingerprintable that previously was not (adding `DELETE` to the patternable leading words, say) **does not** need a bump: nothing already stored changes meaning, since a shape that hashed to X still hashes to X.
- **`IsCurrentFingerprintVersion()` is an exact comparison, never `>=`.** There is no ordering between versions, only identity: an older row's `pattern_id`s were computed under different rules and name shapes that are not the ones they claim, and an inequality would silently resurrect the trails the constant exists to retire. Tested in both directions.
- **0 is reserved and `static_assert`ed against.** A `sys.patterns` row read out of a zeroed or never-written page decodes to version 0, and that must never pass for current.
- **A mismatch is a miss, not an error** — the predicate answers and cannot fail, so there is no error path for P04 to propagate by mistake. The catalog-level form of that test (a stored row with a foreign version resolving as "no pattern") lands with the `sys.patterns` lookup in P04; what is pinned here is the decision it rests on.
- **The version is asserted beside the golden hashes**, not in isolation. Whoever changes the algorithm sees the golden values fail, and that failure is the reminder to bump. Proximity is the enforcement mechanism; nothing mechanical can check the pairing.

## Phase B — the catalog relation

**P03 — `sys.patterns` row format.** — **done.**
Files: `include/kds/catalog/rows.hpp`, `src/catalog/rows.cpp`, `include/kds/catalog/well_known.hpp`, ~~`tests/row_codec_test.cpp`~~ → **`tests/catalog_row_test.cpp`** (new). The named file is the `exec` row codec, a different subsystem; no catalog-row codec test file existed at all, since the other rows are only exercised indirectly through a live `Catalog` in `catalog_test.cpp`.
`SysPatternRow` per spec §4 — `{oid, pattern_id, fingerprint_version, stmt_class, waystone_root, dir_depth, use_count, last_seen}` — with the usual codec discipline: named offsets, `static_assert`s, field-wise memcpy, exact-size `Decode`. Add `kSysPatternsTable` oid and `kCatalogPagePatterns` fixed page id below `kFirstUserPageId`.
Note: another `SysTableRow`-class format break is *not* involved — this is a new relation on a new page. Bootstrap changes, so old data files still will not open, but they already do not.
Tests: round-trip; defaults; exact-size decode refusal.
Needs: nothing.

Decisions worth carrying forward:

- **`dir_depth == 0` is the authority on whether a directory exists**, not `waystone_root == kInvalidPageId`. A row read out of a zeroed or never-written page decodes every field to 0, which would make its root look like page 0 — a valid-looking `PageId`, and the superblock's. Keying the question on the field whose zero value already means "none" leaves no way to spell the state wrong. `HasWaystoneDirectory()` is the only test any reader should use; writers should still store `kInvalidPageId` when clearing, but nothing may depend on it.
- **`stmt_class` is a raw `std::uint8_t`, not an enum.** The v1 statement-class list is `[PROPOSED]` in `docs/spec/parser-v2.md` I2 and its ratification is an open decision in `CLAUDE.md` — defining the enum here would be deciding it. The field exists now because this is an on-disk format and adding one later is a format break; `kStmtClassUnclassified = 0` names the value every row carries until the parser can classify anything.
- **Fields are ordered by descending alignment**, so the on-disk offsets and the struct's own offsets coincide and every field carries an `offsetof` `static_assert`. `SysTableRow` gave that up past `next_id` and has to be read more carefully as a result; there was no reason to repeat it.
- **`Decode()` validates size and nothing else** — not the version, not the root/depth pair. It is a pure decode like every other row's; whether a version is current or a pair is coherent are questions for the layer that can act on the answer (P04).
- **A field-independence test earns its keep here.** An offset collision between two fields survives any round-trip that writes one row and reads it back through the same code; the test encodes a row that is zero except for one field and asserts nothing else comes back non-zero. The byte-layout test pins the format now, while there is still no data to lose by reordering it.

**P04 — Bootstrap and catalog API.** — **done.**
Files: `src/catalog/catalog.cpp`, `include/kds/catalog/catalog.hpp`, `src/bootstrap/bootstrap.cpp`, `tests/catalog_test.cpp`.
Bootstrap `sys.patterns` alongside the other system relations. Add `FindPattern(pattern_id) -> StatusOr<const PatternAccess*>` and `RegisterPattern(pattern_id, version, stmt_class)`, plus `SetPatternWaystoneRoot(oid, root, depth)` writing root and depth **as one unit** with the same validation the deleted `SetWaystoneDirectory()` had, and for the same reason.
Cache patterns through `CatalogCache` on the same three rules as `TableAccess`: sequences never cached, absences never cached, one invalidation choke point at `BumpVersion()`.
Tests: register/find round-trip; version mismatch resolves as absent; root+depth validated as a pair; ~~cache invalidation on registration~~ → **registration invalidates nothing**, see below.
Needs: P03.

Decisions worth carrying forward:

- **`PatternAccess` exists to *exclude* fields, not to wrap the row.** `use_count`/`last_seen` change on every execution, which is not DDL, so by `catalog_cache.hpp`'s one rule they are not cacheable and are absent from the struct — the same reason `TableAccess` carries no `next_id`. Heat is read through `GetSysPatternRow()`, off the page.
- **`RegisterPattern` bumps no catalog version**, which reverses the deleted design. Nothing cached can go stale from a pattern appearing: absences are never cached, so no entry claims the pattern is missing, and no other cached fact mentions it. This **resolves the hazard `waystone-concpets.md` §4 flagged for this task** — registering mid-statement cannot dangle the `const TableAccess*` the statement holds, and the lazy-registration alternative the spec floated is not needed. Tested directly.
- **`SetPatternWaystoneRoot` updates the cached entry in place** rather than invalidating — the one departure from "drop everything at one choke point". The fact belongs to exactly one pattern and is read by nothing else, so a global drop would be collateral damage of precisely the kind that dangled the deleted Waystone's `TableAccess*`. The held pointer stays valid and sees the new root; tested.
- **The version is stamped by `RegisterPattern`, not passed in** — a change from this workplan's original signature. No caller has business recording a pattern under a fingerprint version other than the one that computed its `pattern_id`, so the parameter could only ever be passed wrong, and passing it wrong writes a row no build will resolve. Removing it makes "the cache holds current-version entries only" true by construction, and deletes the version-0 validation as unreachable.
- **Version filtering lives in `GetSysPatternRow()`, the row lookup itself** — not in each caller. Found by a failing test: the first cut filtered in `FindPattern` only, so `RegisterPattern` cached a stale row it had just written, and a stale row on the page shadowed the current one because the scan took the first `pattern_id` match. A version bump leaves old rows in place (nothing rewrites them), so both states are reachable in production. `AStaleRowDoesNotShadowTheCurrentOne` pins it with the stale row written first.
- **A stale row does not block re-registration.** It is invisible to `GetSysPatternRow()`, so `AlreadyExists` cannot fire on it, and a shape stays learnable across a version bump instead of being permanently blocked. The old row is left where it is; reclaiming it is P15's.
- **Pattern oids come from `AllocateRowId(kSysPatternsTable)`** — a repurposing worth naming, since catalog rows carry no Keystone word. It is the sequence used as an oid source. `GenerateUserOid()` is in-memory and restarts at `kUserOidStart` every boot, which for a *persisted* row means two patterns sharing an oid across a restart.
- **`sys.patterns` gets no `sys.columns` rows**, like the other five catalog relations: they are read through typed row codecs, never through a schema.
- Test-only: a stale row is fabricated by writing straight onto the catalog page, because **no API can produce one** now that the version is stamped internally — which is also how a stale row really appears (an older build wrote it, then the version moved).

**P05 — `SHOW PATTERNS`.** — **done.** 5 tests.
Files: `src/server/command_dispatcher.cpp`, `tests/command_dispatcher_test.cpp`, plus `Catalog::ListPatterns()`.
A dispatcher command listing `pattern_id`, class, `use_count`, `last_seen`, and whether a waystone root exists. Development surface on a protocol already documented as one; KWP/1 decides the real spelling.
Tests: empty catalog; after registration.
Needs: P04.

Decisions worth carrying forward:

- **`ListPatterns()` is unfiltered, unlike `GetSysPatternRow()`**, and stale rows are listed with a `stale=v<n>` marker rather than hidden. That is not a hole in the version rule: the rule protects *lookup by pattern_id*, so a stale row still cannot resolve as the pattern it names — which the test asserts alongside the listing. An inspection surface that hides the garbage a version bump left behind is an inspection surface that cannot answer the question you opened it for.
- **`waystone=` is derived from `HasWaystoneDirectory()`, never from the root.** Printing the root directly would report page 0 for a pattern that has no directory, which is the exact confusion `dir_depth` was made the authority to prevent.
- **`pattern_id` prints in hex.** The decimal form of a 64-bit hash is 20 undifferentiated digits, and the only thing anyone does with the value is compare it to another one.

## Phase C — waystone storage

**P06 — Page type, header, and entry codec.** — **done.** 15 tests.
Files: `include/kds/stats/waystone.hpp`, `src/stats/waystone_page.cpp`, `tests/waystone_page_test.cpp`, plus `PageType::kWaystone` in `common.hpp` and its `MaxSupportedFormatVersion` entry.
`PageType::kWaystone` (a **headered** class — spec §6), the waystone header, and the 32-byte entry with `rel_oid` and `step_id`. Entries per page derived from the real header size in a named `constexpr` with the derivation in a comment; it need not be a power of two and the comment should say why that is now allowed.
Tests: header and entry round-trips; offset/size asserts; `entry_count` bounds; a page whose `pattern_id`/`arg_hash` do not match what was asked for reads as a miss.
Needs: nothing.

Decisions worth carrying forward:

- **`rel_oid` is 8 bytes, not the 4 the spec sketched** — `catalog::Oid` is `uint64_t`, and a narrowed copy that happens to fit today aliases two relations onto one value the day it does not. `step_id` shrank to 2 bytes to pay for it (it counts relations in a join chain, not rows), so the entry is still exactly 32 bytes with every field naturally aligned and no padding. Spec §6 amended to match.
- **Fields are ordered by descending alignment** in both structs, so on-disk offsets and struct offsets coincide and every field carries an `offsetof` `static_assert`.
- **253 entries per page, with 24 bytes of slack.** Derived from the real header sizes rather than picked. The exact power-of-two tiling a pk-addressed structure needed is what cost it the common page header; giving that up buys back the checksum and the `page_lsn`.
- **`WaystonePageHolds()` folds four reasons into one bool** — unformatted, wrong type, unparseable format version, wrong instance. A caller does the same thing in all four (miss, fall through), and four distinct returns would invite handling three. The wrong-instance case is load-bearing: the directory is keyed by a hash, so a collision leads a reader to a real, valid, *wrong* trail.
- **`kWaystoneEntryValid` is the liveness test, never `pk != 0`.** A never-written entry decodes to pk 0 and page_id 0, both of which look like real values.
- The tests that earn their keep beyond round-trips: adjacent entries not overlapping (catches a stride off-by-one, which a single round-trip cannot), the tail slack staying zero after the highest legal write, and a page of another type or a newer format version holding nothing.

**P07 — `arg_hash` directory.** — **done.** 19 tests.
Files: `include/kds/stats/waystone_dir.hpp` (new), `src/stats/waystone_dir.cpp`, `tests/waystone_dir_test.cpp`.
The interior-page walk, rekeyed from pk digits to `arg_hash` digits: fanout 2048, lazy allocation, depth growth by root relink, root handed in and out as a plain `PageId` (catalog wiring is P04's). Collisions resolve against the waystone header from P06 — a foreign trail is a miss, never a result.
Tests: walk correctness across level boundaries; lazy-allocation sparseness by page count; ~~depth growth preserves prior mappings~~ → **depth growth preserves the mappings it can and cools the rest**, see below; a synthetic collision resolves to a miss.
Needs: P06.

Decisions worth carrying forward:

- **Growth is a cache flush, and the test says so.** Root relink preserves a prior mapping only when the new top digit — `arg_hash` bits [11*d*, 11*d*+11) — is zero, i.e. for 1 key in 2048. The dense pk key had that zero by construction; a hash does not, and no O(1) growth can give it one. This is the one place the spec's inherited wording was wrong rather than merely incomplete, so §5 was amended instead of the code being bent to it: the workplan's original "preserves prior mappings" is now two tests, one for the surviving digit and one — `GrowthCoolsEveryOtherInstanceWithoutCorruptingOne` — for the honest majority case. Cooling is safe by invariant 8 and self-healing, since the next execution re-records; the old subtree is not leaked either, it hangs off slot 0 and a key that now addresses one of its pages gets a header mismatch.
- **No key is ever out of range.** The pk walk refused an id past the directory's coverage, because a folded digit would have aliased two real tuples. There is no analogue here: a hash has no coverage, the bits above 11*d* simply are not consumed, and the aliasing that produces *is* the collision the header check exists for. `CheckAddressable()` has no successor.
- **The directory resolves an address; it never displaces.** `LookupOrCreateWaystonePage()` returns the page at the address even when it already holds a foreign instance, and a page it does allocate is left **zeroed, not formatted** — `PageType::kInvalid`, so `WaystonePageHolds()` reports false and a reader falls through exactly as for an empty slot. Formatting and the choice of what to do about an occupant belong to the writer (P08) and to the `[OPEN]` collision policy in spec §9; deciding either here would have closed it by accident.
- **Interior pages are headerless, which reclaims `CreateNewHeaderless()`.** 2048 × 4 tiles the page exactly, and a headered frame gets a checksum stamped at byte 4 — child 1. `IsHeaderless()` is asserted on the root, `false` on the waystone it points at, and the pair is round-tripped through a `Flush()` and a reopen so the hazard is tested rather than described. Spec §10's "no caller" note is retired.
- **Lookup reads through `GetForRead()`**, create through `Get()`. A replay walks the directory on the statement path and must not dirty every interior page it touches; the walk that allocates is writing anyway.
- **A dangling child id is reported, not followed.** Interior pages carry no checksum, so damage surfaces as the store's `NotFound` rather than as a walk into a page that was never created. That is an error for this layer, and a miss for the replay path above it (P11) — the distinction is deliberate: this function cannot tell damage from a store failure, and the layer that falls through can afford not to care.

**P08 — Trail write and read. DONE 2026-08-02.**
Files: `include/kds/stats/trail_store.hpp`, `src/stats/trail_store.cpp`, `tests/trail_store_test.cpp` (named `trail_store`, not `waystone_store`, to match the thing it stores).
`WriteTrail` / `ReadTrail`, keyed on `stats::InstanceKey`. Overwrite is wholesale as specified.

**No continuation was built, and the cap decision is why.** Spec §9's open per-instance cap was settled at one page, so `next_page_id` is always `kInvalidPageId` and a trail that would exceed 253 entries is refused with `OutOfSpace` and **not recorded at all** — a truncated trail is indistinguishable from a complete one to every reader, which is the one failure mode a replayer cannot defend against. The field stays reserved for the day a larger cap is wanted.

Two more things the implementation settled that the task did not name. Re-recording **compares before writing** and skips an unchanged trail: nothing moves a tuple today, so the steady state is rewriting byte-identical entries, and a comparison through `GetForRead()` avoids dirtying the page. And the `[OPEN]` collision policy had to be chosen to write anything at all — `TrailDisplacesOnCollision()` is `[PROPOSED]` **displace**, isolated in one function so drop and chain stay one-line changes.
Tests: round-trip with every field; overwrite replaces rather than merges; a 254-entry trail is refused whole and leaves the previous one intact; a colliding `arg_hash` reads as a miss via the header check and never as a foreign trail; an absent trail is empty, not an error.

## Phase D — recording

**P09 — The executor seam. DONE 2026-08-02.**
Files: `include/kds/exec/trail_collector.hpp`, `src/exec/step_vm.cpp`.
Split differently from the task, and the reason is the dependency arrow: `stats/` already depends on `exec/`, so an executor that included a Waystone header would close a cycle. The executor therefore writes `exec::TouchedTuple` — five integers, no page format, no mention of a trail — into a bounded droppable `TrailCollector`, and `stats::TrailRecorder` is the observer that turns those into entries. Spec §1's "Waystone lives outside the executor" is structural rather than a convention.

`epoch` is **not** collected: none exists (see §9). The `PageId` was the one thing the executor had and threw away — `RunWalkStep`'s visitor took it as an unnamed parameter — so threading it into `AcceptTupleAt` is the whole structural change.
Tests: covered through `trail_recorder_test.cpp`; the null configuration is the default everywhere, which is what keeps every pre-existing test recorder-free.

**P10 — Recording wired into the dispatcher. DONE 2026-08-02.**
Files: `include/kds/stats/trail_recorder.hpp`, `src/stats/trail_recorder.cpp`, `src/server/command_dispatcher.cpp`, `tests/trail_recorder_test.cpp`.
Policy is not the `[PROPOSED]` record-every-execution this task named — §9 decided `n = 2` after it was written, and `WouldRecord()` is the one function that holds it (n=1 for a declared pattern). Auto-registration happens **at the moment recording is decided**, not on first sight, so a database of one-shot queries accumulates no `sys.patterns` rows at all. `waystone_recording` (config, default on) is the off switch.
Tests: n=2 for observed and n=1 for declared; two instances of one pattern count separately; a failed statement records nothing; a scan-only statement records nothing; a join records one entry per replayable step in `step_id` order; the sighting table clearing only restarts counting; **replies are byte-identical with recording on and off**.

**Measured cost** (`tools/join_benchmark.py`, 2000 rows): +19% p50 on a 333 µs point join, of which ~13pp is `FingerprintOf` lexing the statement a *second* time — the bolt-on cost `fingerprint.hpp` already names — and only ~4pp is the recording itself. On an 11 ms join it is +1%. A statement that collects nothing skips the fingerprint entirely. **Both planned parser changes have since landed (2026-08-03)** — the fingerprint is accumulated during the parse, and `Token::text` views the statement instead of copying it — which took the recording overhead down with them; see P14.

## Phase E — replay

**P11 — Point replay. DONE 2026-08-02.**
Files: `include/kds/exec/trail_replay.hpp`, `src/exec/trail_replay.cpp`, `src/exec/step_vm.cpp`, `src/server/command_dispatcher.cpp`, `tests/waystone_replay_test.cpp`.

Landed in the **step VM**, not the dispatcher, per I17: replay is driven by the bound step list, so `Lookup` and `Probe` are one mechanism and P13 came with it. The validation lives in one function, `ChainRunner::TryReplay()`, and rule 0 is the *lookup key* rather than a check - the index is keyed on `(step_id, pk)`, so an entry is only found by matching the freshly derived probe key.

The fingerprint had to move ahead of execution (replay needs the instance before the run), so one `FingerprintOf` now serves replay and recording. A chain with no replayable step skips it entirely.
Tests: hit path equals the descent's answer *and* is shown by `replays=1` to have actually replayed; each miss cause falls through and still returns the right row - no trail, wrong Keystone at the target, page gone, slot past the end, an entry recorded against another relation, an entry for a different key.
For `kPointSelect`-shaped statements: look the instance up, replay the single entry through the spec §2 validation chain, fall through to `LocateByPk` on any miss. The validation must live in **one** function shared by point and join replay — a second copy is how one path forgets the `rel_oid` check.
Tests: hit path returns the same row as the scan/descent; each of the four miss causes (no trail, invalid entry, epoch mismatch, wrong Keystone at the target) falls through and still returns the right row.
Needs: P10.

**P12 — The advisory-contract test. DONE 2026-08-02.**
Files: `tests/waystone_contract_test.cpp`.

All five configurations compared byte for byte, plus §11-4's instrumented half (a `Scan` step reports zero replays, with a keyed step reporting non-zero as the control - without which "no replays on a scan" would be satisfied by replay never working). **Verified by removing rule 1 and watching it fail**: a poisoned trail then makes `WHERE id = 3` return *no rows*, because the engine follows the bad pointer to a different tuple which fails the residual. That is the silent-wrong-answer mode invariant 8 exists to forbid.
Spec §11-3 and §11-4 in full: byte-identical results across recording-on, recording-off, replay-off, waystones-deleted-mid-run, and corrupted-trail; plus the instrumented proof that a pattern with a non-pk predicate still performs its search. Regression-mandatory from here on.
Needs: P11.

**P13 — Join replay. DONE 2026-08-02**, folded into P11.
Files: `src/exec/step_vm.cpp`, `tests/waystone_replay_test.cpp`.

No separate mechanism and no fixture executor: the join path exists (V28) and a `Probe` is just a step, so step-list-driven replay covered it for free. Rule 0 shipped with it, as the amendment below requires.
Replay a cross-relation trail in `step_id` order for the written-order nested-loop join. This is the case the design exists for (spec §7) and the first one whose measurement is worth quoting.
Blocked on the executor having a join path at all — that lands in `docs/inflight/in-progress/parser-v2-workplan.md` phase V-4 (V28), with the bound step list this task replays from in V36 and the mandatory probe-key check in V37. Until then this task's deliverable is a fixture executor replaying scripted multi-relation trails, which is also what P12's cross-relation cases run against.

**Amended 2026-08-01 by `docs/spec/parser-v2.md` I17, and it is a correctness amendment, not a scheduling one.** Replay must be driven by the bound plan's **step list**, never by `stmt_class` — a statement whose root is `kPointSelect` and whose subquery is a join would otherwise have a trail nobody replays. And §2's validation chain is **not sufficient for a join step**: every rule in it validates the trail against storage and none looks at the query, so if the driving row's join column is updated between recording and replay, an entry for the old key passes `rel_oid`, Keystone id, epoch and visibility, and the join emits the wrong row. Rule 0 — re-derive the probe key from the current outer row and require it to equal the entry's `pk` — is mandatory before any join replay ships. V37 lands it together with the §2 and §11 amendments.
Needs: P11, P12.

**P14 — Measure. DONE 2026-08-02.**
Files: `bench/results-waystone-v2.md`, `tools/join_benchmark.py`.

**The finding, recorded rather than tuned away as this task requires.** First measurement: replay 22-31x faster on a relation with no pk index, 13-15% *slower* on a B+ tree one — against §7's bar, cleared on heap and **failed on btree joins**.

The diagnosis was that the btree loss belonged to the parser, not to replay, and **fixing it confirmed that**: folding the fingerprint into the parse and then making tokens zero-copy took the btree join from +15.2% to +2.8% and the point lookup from +13.1% to +6.7%, while the heap cases were unchanged at 26-34x. The parser changes also made every statement 3-8% faster on their own, which is a wider win than this task was scoped to find. What remains of the btree overhead is replay's own — a trail page read, an index build, and the recorder's write-side work.
Point replay vs. btree descent vs. heap chain scan, and join replay vs. descent chain, at several row counts. Quote against the bar in spec §7 (8,417 qps / 11 µs for a validated point lookup vs. 311 qps / 2,582 µs for a chain scan at 5,000 rows). If join replay does not beat a descent chain, that is a finding and it goes in the file.
Needs: P13.

## Phase F — lifecycle

**P15 — Invalidation and eviction.** Retention per pattern and instance eviction (spec §9, `[OPEN]` — surface a policy seam, do not pick one). Needs: P11.
**P16 — Decay and `use_count` maintenance.** Behind a `DecayPolicy` interface; halving as the default *implementation*, which is not deciding the `[OPEN]`. Needs: P15.
**P17 — Epoch bump sites.** `EpochProvider::BumpFor(page)` gains real callers when relayout exists. Interface and tests now, call sites later. Needs: P06.

## Standing instructions

- Every consumer of a trail validates per spec §2. There is one implementation of that check (P11) and everything calls it.
- No allocation on the observer path; all timing via the injected clock; all page access via `PageStore`.
- Update the spec and this file together when an `[OPEN]` lands — move it into the spec body with the date, and mirror it in `CLAUDE.md`.

## Out of scope, explicitly

- **Set caching / completeness.** Requires amending invariant 9 and a commit-time change stamp (spec §9).
- **Invariant 3 / invariant 10 relaxation.** Permitted by this design, not performed by it (spec §8).
- ~~**Removing `CreateNewHeaderless()` and `kHeaderlessMap`.**~~ Retired by P07: the directory's interior pages are headerless, so both have a caller again (spec §5, §10).
