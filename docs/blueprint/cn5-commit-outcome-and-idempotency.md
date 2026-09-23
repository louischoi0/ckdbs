# CN-5 — Concept Note: Commit Outcome and Idempotency — a client token claimed on arrival and carried by the commit record

Status: CONCEPT, not a work order, not a CIP. Nothing here opens a stage.
**No work starts on this note until the operator rules on §8.** Ordering
relative to CN-1 to CN-4 is not fixed.
Author: CLA, 2026-09-23, against `7f35688`
Origin: operator's request of 2026-09-23 to write up *result lookup* and
*idempotency keys* as one note, after CLA's survey of what other databases
ship (§1). The operator has answered none of §8.
Claim tags: `[operator]` for the operator's statements; `[source-read]`
with `path:line` at `7f35688`; everything else `[design]`. Nothing is
`[measured]`. §1's survey of other products is from CLA's training
knowledge, tagged `[design]` because it is not a source read of this tree,
and is to be re-checked against each vendor's current documentation
before a work order cites it.
Relation to `docs/spec/protocol.md`: this note adds nothing to KWP/1 and
reserves no frame, bit or code.

---

## 0. The problem, and the two answers

`[operator]` A client whose connection drops after it sent `COMMIT`, or an
autocommit `EXECUTE`, cannot tell whether the work committed. Two answers
exist in the field, and the operator asked for both:

- **Result lookup** — after the drop, ask *"did it commit?"*
- **Idempotency key** — resend with the same key; the engine runs the
  work at most once and answers the resend with the first result.

`[design]` The note's claim is that on this engine **they are one
mechanism**: a client-chosen token, claimed atomically when the work
arrives and carried by its commit record. For a financial OLTP client this
is the question after every timeout — money moved twice, or not at all.

## 1. What other systems ship

`[design]` (see the header on this section's tag).

| system | shape | mechanism |
|---|---|---|
| MongoDB 3.6+ | dedup on retry | retryable writes: the driver stamps a logical session id and a `txnNumber`; the server keeps the outcome and answers a retry from it; `commitTransaction` is retryable |
| DynamoDB | dedup on retry | `ClientRequestToken` on `TransactWriteItems`, ten-minute window, a differing retry refused |
| Stripe (an API, not a database) | dedup on retry | `Idempotency-Key` header, kept for a day, a differing retry refused |
| TigerBeetle | dedup on retry, permanent | every transfer carries a client-chosen 128-bit id; a resend answers `exists`, a differing one `exists_with_different_*` |
| Oracle 12c+ | lookup, then fence | Transaction Guard: a logical transaction id per session **and per commit attempt**; `GET_LTXID_OUTCOME` after an outage also blocks a late commit of that attempt, so the answer cannot go stale |
| PostgreSQL 10+ | lookup | `pg_xact_status(xid8)`; the client must learn the xid before it commits |
| CockroachDB | neither | an explicit ambiguous-result error; idempotency is the application's |

Three lessons carry into §3: a lookup is only final if it **fences** what
it answered about, and the fence must name an **attempt** or it also
fences the retry (Oracle); a key the client chooses before sending covers
autocommit, which a server-issued id cannot; and a retention bound needs a
way to tell "expired" from "never seen" (§3's retention).

## 2. What this tree has

`[source-read]` unless marked.

- **The read view cannot answer "did trx N commit?"** Below its floor it
  answers *"committed" with no lookup*, *"only because an aborted
  transaction's page changes are physically undone"*
  (`docs/spec/txn.md:385-391`). The view never records an abort, so a
  lookup by `trx_id` served from it would call an old aborted transaction
  committed. `[design]` Abort records do persist in the WAL until it is
  recycled, but no structure indexes them; CLA's earlier remark in
  conversation that lookup was "nearly free" was wrong on this point.
- **Trx ids are never reissued across a restart**: the sequence reopens at
  the superblock's `next_trx_id` and no core issues below it again
  (`include/kds/txn/manager.hpp:353-356`). A trx id is a stable name — of
  one attempt, which is why it is still the wrong key (O6).
- **A dropped connection neither stops nor keeps anything.** *"There is no
  cancellation in this engine, so a statement already running runs to
  completion whatever happens to the connection"*
  (`docs/spec/client-manual.md:282-284`; `C_CANCEL` has no handler,
  `docs/spec/protocol.md` §10), and *"Session state … All dropped on
  disconnect"* (`protocol.md:130`). After a drop the work may still be in
  flight, and the session that could have been asked is gone.
- **The engine issues primary keys** (CLAUDE.md invariant 11), so a blind
  retry of an `INSERT` that omitted its pk inserts a second row. A named
  pk already in use — a deleted row's included — *"is refused as a
  duplicate"* (`manual/sql/sql.md:198-200`): TigerBeetle's shape, with two
  gaps — the retry gets an error rather than the first result, and the key
  must be a 40-bit id.
- **Durability of an acknowledged commit.** `strict` and `group` both have
  zero loss: *"a D1/D2 commit is acknowledged … only after its commit
  record is durable"* (`docs/spec/wal.md:26-27`, `:128`). Only `relaxed`
  acknowledges on entry to the WAL ring, with loss bounded by the flush
  interval (`wal.md:28`).
- **`UNKNOWN_OUTCOME` has no producer.** Every remaining occurrence is a
  declaration, decoder, mapping, test or comment; its producers were all on
  the statement-shipping path and the last went at AT-S6 (`ac4bd64`, found
  with `git log -S'UnknownOutcome('`). Text still describing it as live:
  `docs/spec/client-manual.md:273-280`, the comments at
  `include/kds/base/status.hpp:104-108` and
  `src/server/command_dispatcher.cpp:196-201`, and CLAUDE.md's cross-core
  row. Recorded here, not fixed by this note.

## 3. The mechanism

`[design]`

- **The token.** A client-chosen 16-byte value whose leading 48 bits are a
  millisecond timestamp (the UUIDv7 layout), sent **before** the work: on
  `C_TXN_BEGIN` for an explicit transaction, on `C_EXECUTE` for
  autocommit. Scoped to the authenticated user (O2).
- **The claim.** On arrival the token is claimed atomically in one
  instance-wide map, `(user, token) → state`. Only one arrival can take
  an unclaimed token; every other arrival with it — a resend racing an
  original still queued on a dead socket, or the late original racing its
  resend — finds it claimed and is answered from its state, never run.
  **The claim is what makes a resend with the same token safe**; nothing
  else in this section is needed for that. A claim is not logged: if the
  instance crashes, recovery rolls the work back and the token is
  correctly unclaimed.
- **The commit record.** `TXN_COMMIT` carries the token and a **result
  summary** — rows affected per statement and, for an `INSERT` that
  omitted its pk, the issued ids as ranges (O4) — plus a request
  fingerprint: the statement's `pattern_id`, its `arg_hash` and its bound
  values, since a `pattern_id` alone replaces every inline literal with a
  marker (`include/kds/parser/fingerprint.hpp:18-19`, `[source-read]`).
  The token then commits with the work in one record, so neither can be
  durable without the other.
- **The lookup and the burn.** `OUTCOME(token)` on an unclaimed token
  **claims it as burned** and logs the burn before answering
  `NOT_COMMITTED`. A burned token is dead: any later arrival with it, the
  late original included, is refused. So `NOT_COMMITTED` is final, and a
  client that wants to resend after it uses a **fresh token**. This is
  Oracle's per-attempt fence with the attempt number replaced by a new
  token. The burn is a logged write, so lookups are a way to force log
  appends — a cost O7 names.
- **The answers**:

| state of the token | `OUTCOME(token)` | an arrival carrying the token |
|---|---|---|
| unclaimed, within retention | burns it; `NOT_COMMITTED` | claims it and runs |
| claimed, work in flight | waits for the decide (O10), then as below | waits likewise |
| committed | `COMMITTED` + summary | not run; the summary, or a refusal if the fingerprint differs |
| aborted | released to unclaimed by the abort — then as the first row | as the first row |
| burned | `NOT_COMMITTED` | refused, non-retryable, naming the burn |
| older than the retention horizon | `UNKNOWN` | refused, naming retention |

- **Retention**, and why the timestamp is in the token. Tokens past the
  horizon are dropped from the map; without a time in the token a late
  resend of a dropped committed token would look unclaimed and run twice.
  With it, anything older than the horizon is refused on sight, so
  "expired" and "never seen" never meet. A count cap refuses **new**
  claims when full (a resource refusal), and never evicts early — an
  early eviction would reopen the same double run under load.
- **Recovery.** The map is rebuilt at mount from a checkpoint snapshot of
  committed and burned tokens, then from `TXN_COMMIT` and burn records
  after it; the token's own timestamp drives retention, so no clock field
  is added to the record.

## 4. Placement against existing structures

`[design]`

- **The lock family** is where the in-flight wait would live, and it does
  not fit as it stands: its units are relation, range, slice and tuple of
  a relation, and its holders and waiters are transactions
  (`include/kds/txn/lock_table.hpp:273-351`, `[source-read]`). A token is
  none of these, and a lookup has no transaction. So the wait is either a
  new unit kind outside any relation with a transaction-less waiter, or a
  slot of the token map's own reusing the family's wake mechanism (O10).
  Either way it is bounded by the fault net, 1 s since AT-S6
  (`lock_table.hpp:438`, `[source-read]`).
- **The read view** is untouched. The token map is a second, narrower
  memory of commits, kept for clients, and it feeds no visibility.
- **Named pks** keep their duplicate refusal; a client using a pk as its
  key keeps working, and the token is the general form.
- **CN-4** (Transition) is orthogonal; a saga step using both gets
  at-most-once from this note and legal states from that one.

## 5. Durability

`[design]` Under `strict` and `group` a token's `COMMITTED` is as durable
as the commit, which is durable before it is acknowledged (§2). Under
`relaxed` a crash inside the flush interval can turn a `COMMITTED` the
client already saw into an unclaimed token after mount — with the work
genuinely gone, so the next resend runs it once, correctly, but the
earlier answer was withdrawn. O5 decides whether a token is allowed there.
A burn must be durable before `NOT_COMMITTED` is answered, whatever the
session's class, or a crash reopens the late-commit window.

## 6. What this note does not license

- Any frame, capability bit, status code, WAL record field or catalog
  relation.
- Distributed transactions, XA or a coordinator role: a token names work
  on this instance only.
- Automatic retry inside the server, or a client library's retry policy.
- Cancellation. A burn stops a late commit, not the statement.
- A claim about cost: the bytes per commit record, the map's memory and
  the burn's log writes have not been measured, and the overhead
  measurement is suspended by operator decision.

## 7. Known consequences

`[design]`

- **A half-open connection can hold a token indefinitely.** There is no
  idle-session timeout (`protocol.md` §10, `[source-read]`), so an
  explicit transaction on a dead socket keeps its token claimed and every
  lookup answers after the fault net with "still in flight". The bound on
  a transaction's life, AN-R14's ceiling, is not enforced today
  (`docs/inflight/known-gaps.md`). A token makes that gap visible to
  clients; it does not create it.
- **A `kFingerprintVersion` bump inside the retention window** would make
  a genuine resend's fingerprint differ from the stored one (O9).

## 8. Operator decisions this note leaves open

Each carries CLA's proposal, none ratified.

| # | question | CLA's proposal |
|---|---|---|
| O1 | Token form | **16 bytes, client-chosen, UUIDv7 layout** — the leading timestamp is what makes retention sound (§3) |
| O2 | Token scope | **Per authenticated user.** Per instance lets tenants collide and probe each other's tokens |
| O3 | Retention | **A time horizon plus a count cap that refuses new claims**, both settings. Ten minutes (DynamoDB) is short for a batch retry; a day (Stripe) is the familiar number |
| O4 | The summary | Rows affected per statement and issued pk ids **as ranges, capped**, falling back to a count past the cap; **not** returned rows — a retried `SELECT` is re-run, not replayed |
| O5 | `relaxed` durability | **Refuse a token on a `relaxed` transaction**, since §5's withdrawn answer is exactly what a token exists to prevent. Forcing `strict` silently would accept one spelling and enforce another |
| O6 | Lookup by trx id as well | **No.** It needs an abort index §2 shows does not exist, names an attempt rather than an operation, and is unknown to an autocommit client whose reply was lost |
| O7 | Surface | Two frame fields (`C_TXN_BEGIN`, `C_EXECUTE`), one lookup frame, and a read-only `sys.commit_tokens` for audit. No SQL spelling in v1. The lookup's logged burn is its cost, and it is rate-bound by the same per-user scope |
| O8 | New codes | A fingerprint mismatch, a burned token and a token past retention are distinct non-retryable refusals, each naming the token; whether they are new registry entries or details under one is the registry owner's call |
| O9 | Fingerprint across a `kFingerprintVersion` bump | Store the version beside the fingerprint and **compare only within a version**, accepting a resend across a bump unchecked rather than refusing a genuine one |
| O10 | The in-flight wait | **A slot on the token map's own entry**, woken by the decide through the family's wake registry, bounded by the fault net, answering "still in flight" (retryable) at the bound. A token as a lock unit would put a relation-less key and a transaction-less waiter into a table built for neither |
