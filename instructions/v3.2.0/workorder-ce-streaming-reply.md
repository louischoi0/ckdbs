# Work order CE — a result leaves while its statement runs: streaming the KWP reply

Written 2026-10-20 on `ce-streaming-reply` from `b98271a9`
(`v3.3.0-47-gb98271a9`), on the operator's words, recorded in English
(`raft-marks-2026-10-20.md` §4):

- **W0**: *"BN-R7: client transport while executing - plan a new
  milestone."* (written "RN7", read as BN-R7, the rule CE amends). It
  follows the row-cost attribution of the same day.

**Not opened.** W0 asks for a plan. No stage starts until CE-Q0 is marked,
and every proposal in §3 is CLA's, unmarked.

**Claim tags**: `[source-read]` with `path:line` at `b98271a9`;
`[measured]` with its results file; `[design]` for reasoning about code not
yet written; `[quiet-wrong]` where a wrong choice turns a correct answer
into a wrong one with no error.

**The letter.** `[source-read]` CD is the last work order under
`instructions/v3.2.0/`; no `CE` token names a milestone on `origin/main`
at `b98271a9`. CLA's choice.

**CE against other rules.** CE amends `docs/spec/protocol.md` §7's
"`max_rows` bounds delivery, not execution - the statement runs whole into
the portal's buffered batches" for the rows of a plain `SELECT`, and the
timing half of BN-R7 (§5's "a pipelined statement's reply leaves in one
write"). It leaves standing: the frames, their order and their encoding;
§5's skip-to-sync; `S_PORTAL_SUSPENDED`/`C_CONTINUE` as the only flow
control a client asks for; invariants 8 and 9 (a trail is recorded only on
success, whatever left the server).

## 1. Why: what the measurement found

`[measured]` `bench/v3.3.0/results-row-cost-attribution-v3.3.0-37-gf829964c.md`
(and its review, `64428271`) and
`bench/v3.3.0/results-scenario1-backtest-v3.3.0-38-ga40882af.md` §10:

- With the Python client decoder fixed (`a40882af`), KDS returns a row at
  4.84 us where PostgreSQL 18.6 takes 4.08 us (`symbol-history`, 7,560
  rows); the many-row shapes stand at 0.85x-0.96x of PostgreSQL.
- **Wall time minus client CPU is 0.41-0.43 us per row for KDS - equal to
  its server CPU per row - and 0.01 us for PostgreSQL.** The client waits
  for every microsecond of KDS's server work and for almost none of
  PostgreSQL's: PostgreSQL's backend writes rows while it produces them,
  and the client decodes the first batches while the backend is still
  walking.
- That non-overlap is 56 % to 84 % of the remaining gap (a derived bound:
  0.43 us measured on a key range, up to about 0.64 us on a filtered walk
  whose unreturned rows are also server time the client waits through).
- First-row latency is the whole statement's: a client sees nothing of a
  60,480-row answer until the last row is encoded.

## 2. What the tree does now

`[source-read]` at `b98271a9`:

- **The sink buffers the whole result.** `WireResultSink::Emit`
  (`src/server/kwp_session.cpp:263`) appends each row to an open batch and
  `Seal` (`:282`) moves a sealed batch into `batches_`, a vector the sink
  owns; nothing leaves it while the statement runs.
- **The reply is assembled at completion.** `KwpSession::OnStatementComplete`
  (`:671`) runs once the dispatch ends: it calls `sink.Finish()`, sends
  `S_ROW_DESC` only then, and `Deliver` (`:864`) sends every batch (all of
  them for an uncapped portal, one per `C_EXECUTE`/`C_CONTINUE` for a
  capped one), then `S_PORTAL_SUSPENDED` or `S_COMPLETE`.
- **The socket is written at completion** (BN-R7): `TcpServer` holds the
  outbox while `conn.in_flight` (`src/server/tcp_server.cpp:577`) and
  `OnStatementComplete` (`:912`) flushes it in one write, `S_PARSE_OK` and
  `S_BIND_OK` of a pipelined statement included.
- **A plain `SELECT`'s walk already suspends.** `SelectRun`
  (`src/server/command_dispatcher.cpp:5019`) owns everything the walk reads
  and `Execute` (`:5097`) awaits `exec::ExecuteAsync`, which yields every
  `SlicePolicy::pages_per_slice` (64) pages (BA-S15, `sched.md` §3),
  holding its view, its borrow, its core's statement epoch and a memo pin,
  and **no page pin**. That yield is the one place a reply could leave
  mid-statement without a new suspension point. Rows reach the sink inside
  the walk's visitor (`:5113-5138`); a sorted statement's rows reach it
  only in `Finish`'s drain (`:5184-5192`), after the walk.
- **A failure discards the rows.** On an error `OnStatementComplete` erases
  the portal and answers `S_ERROR` alone (`:715-741`), so today a client
  never sees a row of a statement that failed.
- **A structural refusal re-runs the statement** (BA-S14):
  `StructuralRerunDue` (`command_dispatcher.cpp:883`) runs a statement that
  wrote nothing again from the top after a yield when the clustered tree
  refused a stale descent `TxnConflict`. A `SELECT` writes nothing, so a
  walk refused part way is re-run, and today its rows from the first
  attempt are discarded with the sink's reset.
- **Cancel** is observed at the slice boundary and ends the statement
  `CANCELLED` (`protocol.md` §10); a connection closed mid-statement sets
  the same flag, and `CloseClient` defers the teardown while `in_flight`.

## 3. Proposals and questions

**CE-Q0** - open CE. *(a)* open it with §3 as marked; *(b)* hold it.
CLA proposes (a).

**CE-R1 / CE-Q1 - what streams.** CLA proposes: **a plain `SELECT` run by
`SelectRun`, unsorted, with no aggregate** - the shape whose rows reach the
sink inside the walk. A sorted statement, an aggregate, the query layer
(BU-S4..S7) and a write's `RETURNING` keep today's buffered reply: their
rows exist only after the walk (the sort's drain, the fold, the trail), so
streaming them gains nothing but the drain's encode. *(a)* that scope;
*(b)* also stream the sorted drain (it overlaps the drain's encode with the
client's decode, a smaller gain); *(c)* every result set.

**CE-R2 / CE-Q2 - when bytes leave** `[design]`. CLA proposes: at each
slice boundary, the sink's sealed batches are written to the connection -
`S_PARSE_OK`/`S_BIND_OK` and `S_ROW_DESC` ahead of the first - and the open
batch stays open; the rest leaves at completion as today. A slice is 64
pages, so a walk writes about once per 64 pages walked and never inside a
page. *(a)* at slice boundaries only; *(b)* also as each batch seals, which
needs a write from inside the visitor under the walk's page read hold - a
`send()` under a page latch, refused by CLA.

**CE-R3 / CE-Q3 - a failure after rows have left** `[quiet-wrong]`. Today
a reply is all or nothing. Once a batch has left, a failure (a cancel, the
budget, an encode refusal, a structural refusal) can only follow the rows
with `S_ERROR`. CLA proposes: **rows followed by `S_ERROR` are no result**,
stated in `protocol.md` §7 as the client's contract (PostgreSQL's own:
`DataRow`s then `ErrorResponse`), the portal erased as today. *(a)* that;
*(b)* gate streaming behind a negotiated capability bit (§3's
"capability bits gate optional behavior"), so a client that did not ask
keeps the all-or-nothing reply - CLA's recommendation **if** any deployed
client counts rows as they arrive, which the operator knows and CLA does
not; *(c)* buffer until the walk ends, which is no streaming.

**CE-R4 - no re-run after the first byte** `[quiet-wrong]`. A BA-S14
re-run of a statement whose rows have left would send its rows twice.
CLA proposes: the sink records whether a batch has left, and
`StructuralRerunDue` declines a statement that streamed, which answers the
`TxnConflict` (`retryable = 1`) instead. Not a question: the alternative is
a duplicated answer.

**CE-R5 / CE-Q5 - a slow client** `[design]`. A walk that streams faster
than its client reads fills the socket. Today the statement finishes, drops
its view and borrow, and the reply waits in memory. CLA proposes: the
outbox carries at most `stream_outbox_bytes` (proposed 1 MiB, an engine
constant) of unsent batches; past it the walk does not take its next
slice, parked on the socket's writability under the deadline the
connection already has (TCP keepalive's user timeout, 60 s, `protocol.md`
§10). While parked it holds its read view (bounding the purge horizon),
its borrow (a `DROP TABLE` waits on it) and its epoch. *(a)* park, as
proposed; *(b)* never park - keep buffering in memory past the cap as
today, so a slow client costs memory, not horizon; *(c)* park up to a
bound, then fall back to (b).

**CE-R6 / CE-Q6 - `max_rows`.** CLA proposes: unchanged - a capped portal
keeps today's "bounds delivery, not execution" and does not stream (a
capped client asked to be fed one batch per frame). *(a)* that; *(b)* a
capped portal suspends its walk at the quota and resumes it on
`C_CONTINUE` - a live walk held across client frames, the view and borrow
held as long as the client waits, and a session that can do nothing else
meanwhile. CLA recommends against (b) in CE.

**CE-R7 - what does not change**: the frames and their encoding, the
skip-to-sync rule, the trail (recorded in `Finish`, on success only), the
newline debug protocol (one reply per command), TLS (the channel takes the
bytes as it does at completion), a session's core moves (at a
transaction's head only).

## 4. Stages

- **CE-S0 - the premise, measured**: the row-cost sweep and the many-row
  scenario cells at `b98271a9`, with server CPU per slice and the time to
  first batch, as the base. Stage budget.
- **CE-S1 - red cells**: a `SELECT` whose first `S_ROW_BATCH` arrives
  before the walk ends (a walk parked on a test seam after slice 1); a
  failure after a sent batch answered `S_ERROR` with the portal gone; a
  structural refusal after a sent batch answered, not re-run; a closed
  connection mid-stream ending the walk at its next slice; a byte-identical
  reply for every shape CE-R1 does not stream.
- **CE-S2 - the sink and the session**: `WireResultSink` hands sealed
  batches out as they seal (a sent mark per batch), `KwpSession` sends
  `S_ROW_DESC` at the first hand-out, `Deliver` sends what is left.
- **CE-S3 - the slice-boundary write**: `SelectRun` asks the connection to
  flush at each slice boundary through a callback the server installs with
  the sink; `TcpServer` writes the outbox while `in_flight`; CE-R4 in
  `StructuralRerunDue`.
- **CE-S4 - backpressure** per CE-Q5's mark, with its cells (a client that
  stops reading; the horizon held while parked; the user timeout).
- **CE-S5 - the close**: `protocol.md` §5 and §7, `client-manual.md`,
  `sched.md` §3, the known gaps; the row-cost sweep and the scenario
  cells against CE-S0, server CPU interleaved; the full suite.

## 5. Out of scope

Streaming a sorted, aggregated, query-layer or `RETURNING` reply (CE-Q1
(a)); a capped portal's walk suspended at its quota (CE-Q6 (a)); a
credit or window scheme beyond the socket; the Python client (fixed at
`a40882af`); the remaining text-rendering cost of `send_command`, which is
the client's.
