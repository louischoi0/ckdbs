# Known Gaps

The engine-wide list of what is missing, what does not survive a restart,
and what the code does differently from what a spec or older doc claims.
Verified against code 2026-08-10. Each entry names the owning doc — the
full argument and any workplan live there, not here. Manuals link here
instead of carrying their own copies.

Scope note: an entry here is a *known, accepted* state, usually with a
named owner. It is not a bug list; a gap whose fix is decided belongs in
the owner's workplan.

## Durability and recovery

- **WAL recovery is not implemented.** The log is written and never read
  back; a crash is protected only by the last checkpoint / `SYNC` / clean
  shutdown. Do not attempt a partial recovery (`docs/wal.md`,
  `docs/txn.md` §8's instruction). **Planned 2026-08-10:**
  `docs/workplan-wal-recovery.md` (RV1-RV9, RC01-RC10) — and its survey
  corrects the impression this entry gives, that the substrate is missing
  too. It is not: the scan with torn-tail detection, both checkpoint
  tables, the per-core anchors, the `page_lsn` gate, FPI and the
  acceptance tests all exist. What is missing is the phases, the
  per-record appliers, and **a durable record of an INSERT** — rollback
  walks an in-memory trail that a crash destroys, so a loser's inserts
  cannot be undone from the log today.
- **MVCC ships before recovery** (`docs/txn.md` §8): an uncommitted row
  surviving a crash reads as **committed** on the next boot — its writer id
  is below the new high-water mark and in no live set. No cheap mitigation
  exists; closing it requires a persisted commit watermark, i.e. recovery.
- **DDL and catalog writes are unlogged**, and DDL is not transactional
  (`docs/txn.md` §7): `CREATE TABLE` inside a transaction is not rolled
  back.
- **Keystone K1 does not hold across a crash**
  (`docs/keystoneid-k0-findings.md`): the durable log names ids the
  unlogged `sys.tables.next_id` has forgotten. K-M2a/K-M2 own it.
- **The assertion checkpoint-genesis gap** (`docs/feat-assertion.md` §7):
  the group-directory fold needs records from the Bound Cabin's birth, and
  nothing durable holds headers for a checkpoint-bounded replay to start
  from. Unowned, like recovery itself.

## What a restart loses (without a crash)

- **Cabin entry sets** are memory-resident by design
  (`docs/feat-cabin.md` §9): the `sys.cabins` row survives, the sets
  re-observe from traffic.
- **Assertion enforcement**: the registry/directory is memory-resident, so
  a surviving assertion honestly reports `enforcing=0` until recovery can
  replay the directory (`docs/feat-assertion.md`). The durable Bound Cabin
  pages and the catalog row survive.
- **Waystone sighting counts** restart (a performance event, never a
  correctness one — invariant 8).

## Reclamation — nothing purges, anywhere

There is no purge pass, and readers are deliberately unregistered
(`docs/txn.md` §9), so:

- undo pages grow monotonically; `SnapshotTooOld` is structurally
  unreachable;
- delete-marked tuples keep their slots; var-heap bytes of superseded
  values stay; superseded index and Cabin entries stay
  (`docs/feat-index.md` §13);
- catalog rows are never reclaimed (the column ceiling is on columns ever
  created); pages, extents and Keystone ids are never reused;
- `DROP TABLE` exists (`docs/spec-drop-table.md`) but is **catalog-scoped**:
  the relation's pages, var-heap chain and index pages orphan — leaked
  space, deliberately, because free-map reuse is gated (a reallocated page
  breaks trail validation, `feat-physical-optimizer.md` §6 gate 3) and no
  reader horizon exists. The oid is tombstoned in `sys.objects` and never
  reissued, which is what keeps dead-oid advisory structures harmless.
  `ALTER TABLE` is catalog-only renames (`docs/spec-alter.md` AL1). Both
  RESTRICT on assertions; DROP also RESTRICTs on referencing foreign keys.
  Every one of these is an unlogged catalog write like all DDL: a crash
  after it can lose it.

## Concurrency and multicore

- `cores > 1` buys parallel WAL streams only: **core 0 serves every
  statement**, peers come up alive and idle (`docs/crosscore.md`). The
  P6 ownership question is decided **and built** (CC7 + P6b handoff + P6c
  `placement` key, 2026-08-10): a rotated relation's pages are grantable
  and readable by its owner — but `placement = rotate` stays non-default,
  because a rotated relation's *statements* are still refused retryably
  until dispatch lands. Row-id leasing for peer INSERT is also built
  (P5-shape, 2026-08-10). **The one remaining piece is the step pipeline
  itself** — statement dispatch plus the executor's coroutine conversion,
  the workplan's largest single change left.
- **REPEATABLE READ is knowingly weakened across cores** (CC4): no
  cross-core ReadView; RR holds per core. Client-facing docs must say so.
- Cross-core writes are refused retryably (CC3): a transaction's writes
  bind to one home core. 2PC is an open decision, to be designed from the
  refusal counters.
- **Buffer-pool eviction is built but disarmed**: nothing calls the sweep,
  because `Get()` hands out raw spans safe only while nothing evicts — the
  `PageRef` migration (~257 call sites) is a hard prerequisite
  (`docs/spec-eviction.md`, `docs/page.md` §3).

## SQL surface and protocol

- **No NULL storage**: `NULL` parses as a literal; rows holding one are
  not storable today (`docs/client-manual.md`).
- **Pagination is LIMIT/OFFSET only** (V09, built 2026-08-10):
  `ORDER BY` accepts the primary key alone (a validated no-op) and no
  `DESC`; there are no cursors, and KWP/1 portal suspension is still
  unbuilt — only the frame codec exists (`docs/protocol.md`).
- **`IN (value list)`** is unbuilt — the open half of parser workplan V08;
  it currently reports "expected a subquery".
- **Per-transaction durability class** is a KWP/1 protocol field; the text
  protocol offers only the instance-wide `durability` config key.
- **No auth, no TLS, loopback only** — by design until KWP/1's handshake
  and auth stages exist.
- **`float`** stays refused at `CREATE TABLE`: nothing settled its
  encoding (`docs/rule-fixed-length-tuple.md`).

## Advisory and optimizer structures

- **Waystone retention, decay and epoch-bump sites are unbuilt**
  (P15-P17, `docs/waystone-workplan.md`); trails grow until then.
  One validation gap remains: nothing verifies a page still belongs to the
  relation a trail recorded it from — holds until pages can be reallocated
  between relations (`docs/feat-physical-optimizer.md` §6 gate 3 owns it).
- **`CABIN AUTO` acts only under `cabin_optimizer = on`, default `off`**:
  the controller runs end to end since PHY04 and is observable since
  PHY06 (`SHOW CABIN_OPTIMIZER`, both 2026-08-10), but with the key at
  its default a column declared `auto` still behaves exactly as an
  undeclared one (`docs/feat-physical-optimizer.md` Part II). Its managed
  state and decision log are memory-resident: a restart forgets what the
  controller was managing, and re-observation rebuilds it — the stated
  crash posture, not a bug.
- **The physical optimizer is shadow-only as a finding**
  (`docs/feat-physical-optimizer.md` §6): every candidate move is blocked
  by a named gate; `physical_optimizer = on` is refused at startup naming
  all three.

## Stale claims found in docs (fix at the source when touched)

- `docs/client-manual.md` §5: "exactly one accepted client connection
  served at a time" — stale; many clients are served concurrently,
  cooperatively on one thread (`include/kds/server/tcp_server.hpp`).
- Any doc or task brief claiming **there is no SQL DELETE** or that
  **assertions enforce nothing** predates the transaction work and AST07
  respectively; both are built (verified in
  `src/server/command_dispatcher.cpp`).
