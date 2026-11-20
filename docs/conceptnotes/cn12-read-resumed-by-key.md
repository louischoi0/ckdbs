# CN-12 — Concept Note: A read resumed by key — "after tuple key `k`, `n` rows", under a snapshot the client carries

Status: CONCEPT, not a work order, not a CIP. Nothing here opens a stage.
**No work starts on this note until the operator rules on §7.**
Author: CLA, 2026-11-20, against `15cc1c0a` (worktree
`dj-connections-sessions-cursors`)
Origin: `instructions/v3.2.0/workorder-dj-connections-sessions-cursors.md`
§2 (P5b) and DJ-Q11, whose (a) - adopted on the operator's standing
go-ahead of 2026-11-20, not the operator's own mark - reads: "a byte bound
on a portal's buffer ...; the resumable read written up as a concept note,
not built in DJ, because it gives up '`SnapshotTooOld` is never raised'".
This is that note. The operator's words that opened DJ are in the order's
§0; nothing in this note is the operator's.
Claim tags: `[source-read]` with `path:line` at `15cc1c0a`; `[measured]`
with the results file; everything else `[design]`, including every
statement about PostgreSQL, Oracle and SQL Server, which comes from CLA's
training knowledge and is to be re-checked against each vendor's current
documentation before a work order cites it.
Relation to `docs/spec/protocol.md`: this note reserves no frame, bit or
code. §3 names what it would need.

---

## 0. The concept

`[design]` A client that wants a large result a page at a time asks for it
as a sequence of independent statements, each "the next `n` rows after
tuple key `k`", and carries between them the two things that make the
pages one result: **the key it stopped at** and **the snapshot the first
page was read under**. The server holds nothing between two pages - no
portal, no buffer, no read view, no borrow - so any connection, on any
core, may fetch the next one, and a client that walks away costs nothing.

The second half is the whole difficulty. Without it this is keyset
pagination, which a client can already write (`WHERE id > k ORDER BY id
LIMIT n`) and which reads each page under its own snapshot. With it, the
pages are one consistent result - and the engine must then answer a
question it has never had to: **what if that snapshot can no longer be
read?**

## 1. What this tree has

- `[source-read]` **A capped portal runs its statement whole.** "`max_rows`
  bounds delivery, not execution" (`docs/spec/protocol.md` §7); the sink
  keeps every sealed batch (`include/kds/server/kwp_session.hpp:180`) and a
  capped portal never streams (`:176`).
- `[measured]` **What that costs**
  (`bench/v3.4.17/results-dj-s0-premise-v3.4.17-1-g054dbc5e.md` §5):
  `C_EXECUTE {max_rows = 100}` of a 100,000-row `SELECT` answers its first
  hundred rows after 19.8 ms where `LIMIT 100` answers in 92 us, and the
  suspended portal holds 4.4 MiB until it is closed or the 60 s portal
  timeout releases it.
- `[source-read]` **An uncapped plain `SELECT` streams**, paced by the
  socket, and a client that stops reading parks the walk with its read
  view, its borrow and its core's reader-lease slot held
  (`protocol.md` §7).
- `[source-read]` **Every reader is a commit-LSN snapshot over the
  instance read view, registered with the horizon** (`docs/spec/txn.md`
  §4.1). "Nothing a live view can reach is ever freed, so `SnapshotTooOld`
  is never raised", and what bounds the price is the transaction's lifetime
  ceiling, which ends the holder "as an ordinary abort" rather than failing
  a read. That sentence is the contract this concept touches.
- `[source-read]` **A tuple key is never rebound except by `PURGE`**
  (invariant 11), and a btree leaf is in key order by placement
  (invariant 4): "the rows after key `k`" is a well-defined, stable
  position in a relation, whatever splits happen between two pages
  (`session.hpp`'s `WalkCursor` resumes a write walk the same way).
- `[source-read]` **`ORDER BY <tuple key>` ascending is elided and
  `DESC` is a reverse walk** (CV): the ordered page by key exists as an
  access path.

## 2. What other systems ship `[design]`

- PostgreSQL: a cursor lives in its session and its transaction (or is
  held `WITH HOLD`, materialised at commit); a snapshot can be exported
  and imported by another session of the same server only while the
  exporting transaction lives. A read under a snapshot older than the
  vacuum horizon is not offered at all (`old_snapshot_threshold`, when it
  existed, raised "snapshot too old").
- Oracle: flashback query (`AS OF SCN`) reads any past SCN the undo still
  covers, and raises ORA-01555 when it does not. That is this concept's
  shape exactly, with the refusal this tree has declined.
- SQL Server: keyset cursors and `OFFSET ... FETCH`, each page under its
  own statement's snapshot; snapshot isolation is per transaction.

The three agree on the trade: a snapshot nobody holds can be reclaimed, so
either someone holds it (and the server pays), or reading it can fail.

## 3. The mechanism, were it built `[design]`

- **A read names its snapshot.** A statement form, or a `C_EXECUTE` field
  behind a capability bit, carrying the commit LSN the first page reported.
  The first page's reply would have to report it - a new field of
  `S_COMPLETE` or `S_ROW_DESC` - and the page's last tuple key is already
  in the rows.
- **Admission.** The engine mints a view at the named LSN only where every
  version that LSN could see is still present: the LSN is at or above the
  purge floor (`ReadHorizon()`), on every core. Below it the read is
  refused.
- **Between pages nothing is held**, which is the point - and is why the
  floor can pass the LSN while the client thinks.
- **Scope.** A single relation walked in tuple key order, or by an index
  the ordered walk serves (CV), since "after `k`" must be a position. A
  join, an aggregate or a sort has no such position and would re-run whole
  per page.

## 4. The price `[design]`

One of three things must be true, and each is a contract change:

1. **The resumed read is refused when its snapshot has been passed** -
   `SnapshotTooOld` by another name. `txn.md` §4.1's "never raised" becomes
   "raised only to a read that named its own snapshot". A client that pages
   slowly through a busy relation loses its result part-way and must start
   again; how often is a function of the purge cadence and is unmeasured.
2. **The server remembers the snapshot for the client** - a registered
   view with no session, held until a timeout. That is a portal's cost
   again (the horizon held, every core's undo with it) without the
   portal's buffer, and it needs its own bound, which would be the
   lifetime ceiling's reasoning applied to something that is not a
   transaction.
3. **The resumed read silently continues at a newer snapshot** - rows the
   first page's view never held, or a row seen twice, with no error.
   `[quiet-wrong]`, and not offered: it is keyset pagination, which a
   client can write today and knows it is writing.

## 5. Relation to other notes and orders

- **DJ** takes the other candidate of P5b: a byte bound on a portal's
  buffer, which changes no contract (the statement is refused by name) and
  leaves the 215-times cost of §1 where it is for a client that stays
  under the bound.
- **CN-10** (unbounded sessions, bounded transactions) wants sessions that
  hold nothing between transactions; this note is the read-side
  counterpart, a result that holds nothing between pages.
- **CE** (streaming replies) is the server-paced answer to the same need:
  it holds a view for as long as the client reads, which a stateless page
  does not.

## 6. What this note does not license

- No statement form, frame field, capability bit or error code.
- No change to `txn.md` §4.1, to the purge's floor, or to what a view
  registers.
- No claim that the refusal of §4.1 would be rare: nothing here is
  measured but §1's two numbers, which price what exists.
- No view held on a client's behalf outside a session.

## 7. Operator decisions this note leaves open

| | item | options |
|---|---|---|
| O1 | Whether a snapshot may be named by a client at all. | (a) no: `SnapshotTooOld` stays unreachable, and a paged read is keyset pagination with a snapshot a page; (b) yes, refused by name when the floor has passed it (§4.1); (c) yes, with the server holding it to a timeout (§4.2) |
| O2 | If O1 is (b): what a refused page costs the client, and whether a page reports how far the floor is from its snapshot so a client can pace itself. | to be proposed after a measurement of how fast the floor moves under a write load |
| O3 | If O1 is (b) or (c): the scope - tuple key order only, or every order CV's walks serve. | - |
| O4 | Whether the first page is an ordinary statement that reports its snapshot LSN to every client, or a form a client must ask for. | - |
