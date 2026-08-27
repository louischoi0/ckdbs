# Bulk ingestion — workplan

Tasks `BLK01`-`BLK08` for `docs/spec/bulkinsert.md` (BI1-BI14). **T1 is
built (BLK01-BLK05, 2026-08-10); T2 is gated** on a substrate the engine
does not have, and the gate is named rather than worked around.

Two deviations from the spec's letter, each recorded with its reason:

- **BI3's cap is enforced at the dispatcher, not the parser.** The spec
  says "refused at parse"; the parser here is a pure syntax layer with no
  config access (its own standing rule), and a config key enforced by a
  config-blind layer is a contradiction. The refusal still names the cap
  and the count and still inserts nothing; only the layer moved.
- **BI4's ordinal is appended, not prefixed.** The spec's example spells
  `row 37: expected 4 value(s)`; the wire's leading error tokens
  (`TXN_CONFLICT`, `FK_VIOLATION`, `ASSERTION_VIOLATION` with their
  `retryable=` bits) are a compatibility surface clients switch on, and a
  prefix would move token 2 of every one of them. ` (row 37)` at the end
  carries the same fact and moves nothing.

## BLK01 — Parser: multi-row VALUES  **[DONE 2026-08-10]**

`InsertStmt.values` became `rows` (`vector<vector<AstValue>>`); the
single-row statement is a rows vector of one and its parse is
byte-identical in behavior. A trailing comma or a bare value after a row
is the truthful syntax error it always was. Files: `parser.cpp`,
`ast.hpp`, `tests/bulk_insert_test.cpp`.

## BLK02 — BI5: the fingerprint never sees the row count  **[DONE 2026-08-10]**

The suppression lives in `FingerprintAccumulator::Feed` - the **one**
implementation both the parse-time fold and `FingerprintOf` share, which
is what keeps the differential test meaningful. An INSERT-headed stream
tracks top-level paren depth; past the first group's close, literals and
params fold into `arg_hash` (a value is an argument wherever it sits) and
nothing folds into the shape. So `VALUES (1)` and `VALUES (1), (2)` are
one `pattern_id` - corpus-pinned at `a2266ff85c0a8df6` - and every stored
hash is byte-stable, because a *storable* 1-row statement has nothing but
`;`/EOF after its group. The two pre-existing hashes that moved
(`... (1) extra`, the column-list form) were never storable; the corpus
carries the argument where they sit.

## BLK03 — Dispatcher: the bulk loop and `InsertOneRow`  **[DONE 2026-08-10]**

§4's refactor, not a second write path: `InsertInner`'s per-row body is
now `InsertOneRow` - arity (hoisted above the id so a refused row burns
nothing, BI9's rule made literal; the codec's deeper check unchanged),
FK, admission, id, encode + spill, placement, Cabin witness, index
maintenance, reservation, rollback trail, WAL, root repoint - and the
statement is one resolution, one affinity check, one loop. A btree level
growth invalidates the cached `TableAccess`, so the row that repoints the
root **refreshes the borrow** before the next row - the one bulk-only
hazard the loop added, and the reason the pointer passes by reference.
Admission at row k sees rows 1..k-1's reservations (§2.3's argument,
pinned: a 3-row statement into a `COUNT(*) <= 2` group fails at row 3
with nothing inserted). A multi-row statement in a configuration without
the transaction manager is **refused upfront**: BI4's unwind replays the
manager's trail, and placing rows that cannot be taken back is a wrong
answer with a right answer's shape. Production always builds the manager.

## BLK04 — The cap and the config key  **[DONE 2026-08-10]**

`max_insert_rows` (default `parser::kDefaultMaxInsertRows` = 1024
`[PROPOSED]`, must be ≥ 1) through the expeditor's allow-list into the
dispatcher; `kds.conf.sample` documents it. A refusal naming cap and
count, never a truncation.

## BLK05 — Tests (§7's unblocked obligations)  **[DONE 2026-08-10]**

`tests/bulk_insert_test.cpp`: pipeline equivalence (bulk vs single-row
twin, heap and btree, byte-identical replies); the fingerprint pin plus
the differential-path agreement; atomicity with ordinals (arity, supplied
pk, FK at row 2, assertion at row 3 - zero rows visible after each); BI9
id-burn semantics pinned exactly (a pre-id refusal burns nothing, an
aborted statement burns precisely its placed rows' ids); the cap; index
maintenance riding the loop. Gated obligations deliberately absent:
crash-mid-load (`[GATED: recovery]`), T2 flow control (T2 unbuilt),
EV12-style pool profile (eviction off engine-wide).

## BLK06 — T2 frames and load session  **[GATED: KWP server]**

`C_LOAD_*`/`S_LOAD_*` and the modal load session presuppose a server that
speaks KWP - and per `docs/spec/protocol.md` only the frame codec exists, with
nothing calling it; the server speaks the newline text protocol. Building
T2's frames without a session layer would be codecs with no caller.
When the KWP server lands, T2's engine half is already waiting:
`InsertOneRow` is the one place a row becomes durable state, and the load
session is its second caller by construction.

## BLK07 — BI12 buffer conduct  **[GATED: eviction enablement]**

The EV6 ring exists (EVT06) but nothing evicts engine-wide, so "must not
displace the foreground working set" is vacuously true and unenforceable
until the `PageRef` migration turns eviction on. Revisit with EVT04/05.

## BLK08 — Bench  **[DONE 2026-08-10]**

Run twice, and the two runs are the record (`bench/results-bulk-insert.md`):
Part I at `9ee04e4` measured T1 against the 21 µs claim and found the two
engine defects that mattered more than T2 (the ChainTail walk, the WAL
segment-boundary wedge); Part II at `926f422`, after both fixes, inverted
the verdict - ckdbs beats PostgreSQL in every cell of the matrix, up to
2.94x at batch 1000, and per-row cost is flat at ~3.5 µs with the
resident-rows term dead (re-fit slope -0.009 ns, R² 0.169).
