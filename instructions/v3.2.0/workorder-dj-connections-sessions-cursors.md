# Work order DJ - connections, sessions and cursors: a connection cap and an idle-session timeout an operator sets, a session a pool can hand back clean, a wait in place of a refusal, and a reply that is bounded

Status: **WRITTEN 2026-11-20, not opened.** Written in the main checkout on
`main` at `4d72be11` (`v3.4.15-3-g4d72be11`) on the operator's words of
2026-11-20 (`raft-marks-2026-11-20.md` §5). The letter DJ is CLA's proposal.
**Two things are the operator's word and are built whatever DJ-S0 finds**:
a setting for the connection count limit and a setting for the idle-session
timeout (DJ-S1). Every other part is CLA's direction, taken up by the
operator's "write a work order for the improvement directions"; each is
gated on DJ-S0 and on the marks of §4. No DJ-Q item is marked.

No stage is started. Every `[source-read]` claim below was read at
`4d72be11`. No suite was run: the order changes no build input.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` where a wrong choice turns a
correct answer into a wrong one with no error.

Related: `docs/spec/protocol.md` §1, §3, §7, §8, §10; `docs/spec/txn.md`
§1 (the lifetime ceiling), §5; `docs/spec/sched.md` §3, §5;
`docs/spec/crosscore.md` CC1; `docs/inflight/known-gaps.md` ("Streamed
replies (CE)", "Nothing bounds the reply", the balancer's entries);
`instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`
(the head at which a session is placed);
`instructions/v3.2.0/workorder-ce-streaming-reply.md` (CE-Q5, CE-Q6);
`instructions/v3.2.0/workorder-cs-read-only-transaction.md`;
`instructions/v3.2.0/workorder-di-shape-identity.md` (a `USE` left on a
pooled connection changes what a text binds).

## 0. What DJ is

`[operator]` *"From the viewpoint of connections, cursors and connection
pools, against the commercial databases (PostgreSQL, Oracle, Azure SQL),
find the directions to fill in, or to improve into a design that fits kdbs
better."* and, after CLA's reading, *"Write a work order for the
improvement directions. And make the following options settable: a
connection count limit, an idle session timeout."* (given in Korean;
recorded in English.)

`[design]` The premise CLA gave the operator: the three products' pooling
features exist because a session is dear there - a process, or a server
bound to a session. Here a session is a coroutine's state and its buffers,
every core accepts, and a session may move to a less busy core at any
transaction's head (`protocol.md` §8). So the engine already multiplexes
sessions over cores as a transaction-mode pooler does. DJ does not add a
pooler. It supplies what that structure does not yet promise:

1. **an operator's two settings** - how many connections, and how long an
   idle one may stay (the operator's word);
2. **a number** for what a connection costs, which the tree does not have;
3. **a session a client pool can hand back clean**;
4. **a wait where the engine today refuses** a transaction it has no slot
   for;
5. **a reply that is bounded**, and a read a client can resume without the
   server holding a view for it.

**What CLA told the operator, and where the word differs.** CLA said an
idle-session timeout was not needed, since an idle session holds memory
only, and that only an idle holder of a view needs a bound. The operator's
word asks for the setting. Both stand: the setting is built (DJ-S1), and
CLA's proposal is that it is off unless set (DJ-Q4), which is today's
behaviour.

## 1. The tree as read `[source-read]`

### 1.1 Nothing bounds the connection count

- A reactor accepts up to 64 sockets an event (`kMaxAcceptsPerEvent`,
  `include/kds/server/tcp_server.hpp:88`;
  `src/server/tcp_server.cpp:319-329`) and sets each up
  (`tcp_server.cpp:331-340`); the listen backlog is 4,096
  (`tcp_server.hpp:76`). No count is compared anywhere on the path, and no
  configuration key names one: `max_connections`, `max_sessions` and their
  like appear nowhere under `include`, `src`, `docs/spec` or `manual`
  (`grep` at `4d72be11`).
- `protocol.md` §10 says of the per-session caps that "what bounds the
  server is the connection count" (`docs/spec/protocol.md:139`). Nothing
  bounds that.
- A server's connections are one map per `TcpServer`
  (`tcp_server.hpp:267`, `:501`). **How many `TcpServer`s an instance has
  above one core, and whether any word counts their connections together,
  was not read**; DJ-S0 reads it.
- A cancel arrives as a connection of its own: "a new connection sends
  `C_CANCEL {session_id, cancel_key}` and closes" (`protocol.md:135`).

### 1.2 What ends an idle connection today

- "There is no idle-*session* timeout: nothing bounds how long an
  authenticated connection may sit with no statement and no portal. A
  vanished peer is reaped by TCP keepalive" (`protocol.md:136`;
  `tcp_keepalive_s`, 60 s).
- Two sweeps already walk a reactor's connections on a timer: idle
  portals every quarter of 60 s (`tcp_server.cpp:253-258`;
  `kPortalIdleTimeoutNs`, `include/kds/server/kwp_session.hpp:94`;
  `src/server/kwp_session.cpp:1045-1053`) and the transaction lifetime
  ceiling every quarter of `txn_lifetime_ceiling_ms`
  (`tcp_server.cpp:266-268`, `:926`). The second skips a connection with a
  statement in flight and ends a transaction by the connection-close
  path's own rollback (`tcp_server.cpp:928-946`).
- A portal records `idle_since` (`kwp_session.hpp:312`). **No field
  records when a connection last carried a frame**; a keepalive `C_PING`
  is answered in the frame switch (`kwp_session.cpp:449`).

### 1.3 A transaction the core has no slot for is refused

`TransactionManager::Begin` answers `ResourceExhausted` once its core
publishes `kInFlightSlotsPerCore` (1,024) ids
(`src/txn/manager.cpp:134-139`;
`include/kds/txn/instance_visibility.hpp:320`). The session is not parked
and not moved; the client is told and retries.

### 1.4 What a session carries, and what clears it

- The isolation level, the default namespace a `USE` set, the durability
  rung a `SET DURABILITY` set, the priority level, the role the auth gate
  stamped (`include/kds/server/session.hpp:161-172`, `:190-206`,
  `:252-253`, `:436-437`), and KWP's named statements and portals, 64 of
  each (`protocol.md:139`).
- "Session state: named statements, portals, txn, durability default. All
  dropped on disconnect" (`protocol.md:134`). No statement or frame
  returns a session to its defaults: `RESET` and a session-wide `DISCARD`
  are not routed (`src/server/command_dispatcher.cpp:1914` routes
  `DISCARD PENDING`, a statistics verb, alone).
- So a client pool that hands a connection from one borrower to the next
  hands on the first one's namespace, isolation and durability. A
  borrower who inherits `SET DURABILITY RELAXED` commits at a rung it did
  not ask for, with `flags.RELAXED` on the reply the only sign
  (`protocol.md` §9) `[quiet-wrong]`.

### 1.5 A reply's bound

- "`max_rows` bounds delivery, not execution - the statement runs whole
  into the portal's buffered batches" (`protocol.md` §7). A client that
  takes 100 rows at a time of a large result has the server hold all of
  it. "Nothing bounds the reply" of a locking read or a write's
  `RETURNING` either (`known-gaps.md`, DC's entries).
- An uncapped plain `SELECT` streams, and "a client that stops reading
  parks the walk ... parked, it holds its read view (the purge horizon),
  its read borrow ... and its core's reader-lease slot"; "with keepalive
  off a live client that does not read holds the park without bound"
  (`protocol.md` §7; `known-gaps.md`, "Streamed replies (CE)").
- There is no `DECLARE CURSOR`; a portal is the one cursor.

### 1.6 The handshake is on the reactor

A connection's TLS channel is fed where the reactor reads the socket
(`tcp_server.cpp:549`, `channel->OnWireData`), so a TLS 1.3 handshake's
private-key operation runs on the core that accepted it. SCRAM's PBKDF2
is the client's ("the cost is paid by the *client* per connection",
`include/kds/server/scram.hpp:38-42`). **What the handshake costs the
core's other sessions was not measured.**

### 1.7 What is not measured

CLA found no results file with many idle connections beside active ones,
with a connect-and-close loop, or with a handshake burst. The standing
cells use a few long-lived connections.

## 2. The parts `[design]`

None is built or measured.

**P1 - the connection cap** `[operator]`. A boot key; past it a new
connection is not served. What it owes:

- **A cancel is not a connection for the cap.** A `C_CANCEL` connection
  refused at a full server would leave the one statement an operator most
  wants stopped running `[quiet-wrong]` in effect: the cancel is "silently
  ignored" by design, so nobody is told (DJ-R2).
- **An administrator can still get in** at a full server (a reserve, as
  PostgreSQL's reserved connections are), or the cap locks out the person
  who would raise it.
- **What a refused client is told**, and when: before the handshake the
  server knows no role; after it, the handshake has been paid for.
- **Which listeners count**: the default port, the debug text port, the
  KWP load port.
- **One count for the instance** above one core, where every core
  accepts: a word shared by the reactors, written at accept and close and
  never on a statement's path.

**P2 - the idle-session timeout** `[operator]`. A boot key; a session
with no statement running, no transaction open and no portal held is
closed after that long. What it owes:

- **What "idle" starts from, and what a `C_PING` does to it.** A pool
  that pings to keep connections alive defeats a timeout a ping resets;
  one that validates by ping is cut by a timeout a ping does not.
- **It ends nothing that has its own bound**: an open transaction is the
  ceiling's (`txn.md` §1), a portal the portal timeout's. The session
  timeout sweeps only a connection with neither.
- **What the client is told**: a named refusal before the close, so a
  pool can tell a timeout from a failure.
- The place exists: a third arm of the sweeps of §1.2.

**P3 - a session handed back clean.** A frame, or a statement, that
returns a session to what a new one has - namespace, isolation,
durability, priority, every named statement and portal - refused inside a
transaction; and a bit in `S_READY` that says the session is at its
defaults, so a pool or a proxy can check instead of trust.

**P4 - a wait at the transaction's head.** Where `Begin` would refuse
for want of a slot, the session parks under a wait event and is admitted
when one frees, under the fault net; or moves to a core that has one,
since the head is where a session is placed. Whether a bound below 1,024
serves better is DJ-S0's number.

**P5 - the reply's bound.** Two separate things:

- **P5a - a parked walk is bounded** whatever `tcp_keepalive_s` is: past
  a stated time a walk parked on a client that does not read ends
  `CANCELLED`, releasing its view.
- **P5b - a capped portal does not hold the whole result.** Candidates:
  a byte bound on a portal's buffer past which the statement is refused
  by name; or a read that resumes by key - "after tuple key `k`, `n`
  rows" - with the snapshot's LSN carried by the client, so no session
  holds a view between fetches and any connection on any core may
  continue it. The second **changes a contract**: "`SnapshotTooOld` is
  never raised" (`CLAUDE.md`, the Transactions row) holds because every
  view is registered with the horizon, and a view the server does not
  hold can be passed by a purge. A resume would then be refused by name,
  or continue at a newer snapshot and return rows the first fetch's view
  never held `[quiet-wrong]`.

## 3. Rules

Binding once DJ opens.

- **DJ-R1 - a default changes nothing.** With neither key set, no
  connection is refused and none is closed that is not today.
- **DJ-R2 - a cancel is always admitted**, at any connection count.
  `[quiet-wrong]`
- **DJ-R3 - the timeout ends only an idle session**: never one with a
  statement in flight, an open transaction or a held portal; those keep
  the bounds they have.
- **DJ-R4 - a refusal and a close are named.** A client refused by the
  cap or closed by the timeout is told which, by a code of the pinned
  registry (`protocol.md` §11), with the key's name in the text.
- **DJ-R5 - one name for one quantity.** Each new key is checked against
  `tcp_keepalive_s`, `txn_lifetime_ceiling_ms` and the portal timeout; if
  one of them already expresses it, that one is re-scoped.
- **DJ-R6 - the statement path gains nothing**: no word read or written
  per statement for P1 or P2 but what the sweep needs to know a
  connection is idle, and that is priced at the close.
- **DJ-R7 - one core pays nothing outside the floor.**
- **DJ-R8 - a reset is whole or refused**: no session reads as at its
  defaults with one item left. `[quiet-wrong]`
- **DJ-R9 - no result changes**, and no view is given up that a
  statement still reads through.
- **DJ-R10 - no constant without a measurement**: a default cap, a
  default timeout, an admission bound, a buffer bound is DJ-S0's number,
  the operator's word, or marked provisional where it is set.
- **DJ-R11 - nothing is built for a part DJ-S0 prices under the floor**,
  but P1 and P2, which are the operator's word.

## 4. Items for the operator (DJ-Q), CLA's proposals

(a) is CLA's proposal in each. None is marked.

| | item | (a) | other |
|---|---|---|---|
| DJ-Q0 | The order's extent. Five parts, two of them the operator's word. | **one order; P1 and P2 built first (DJ-S1), P3 next, P4 and P5 only on DJ-S0's numbers and a mark each** | (b) DJ is P1 and P2 alone and the rest is a later order; (c) every part built |
| DJ-Q1 | The cap's key and default. | **`max_connections`, an integer for the instance, `0` (the default) no limit** - DJ-R1 | (b) a default the operator names; (c) a default DJ-S0's memory figure gives `[provisional]` |
| DJ-Q2 `[quiet-wrong]` | What the cap counts. | **authenticated sessions on the default port and connections still in their handshake; never a `C_CANCEL` connection (DJ-R2); the debug text port and the load port counted apart, uncapped** | (b) every accepted socket of every listener |
| DJ-Q3 | An administrator at a full server. | **a reserve: `reserved_admin_connections`, default 3 `[provisional]`, admitted past the cap for a session whose role is `admin`**, so the refusal comes after the auth gate | (b) no reserve: the operator stops a client or restarts; (c) the debug text port is the way in |
| DJ-Q4 | The timeout's key and default. | **`idle_session_timeout_s`, `0` (the default) off** - DJ-R1; refused below the portal timeout's 60 s unless `0` | (b) a default the operator names (Azure SQL's gateway uses 30 minutes under its proxy policy); (c) milliseconds, as the ceiling's key is |
| DJ-Q5 | A `C_PING` and the idle clock. | **a ping does not reset it**: the setting is the operator's bound on a session that does no work, and a pool that must keep one open sets the key to `0` or above its own validation period | (b) a ping resets it |
| DJ-Q6 | Who the timeout exempts. | **a KWP load session and a session parked by the engine; an `admin` session is not exempt** | (b) `admin` exempt |
| DJ-Q7 | Whether either key can be changed in a running instance. | **boot keys only**, as every other limit is | (b) an admin's `SET`, memory-resident, as `SET JOIN_ORDER` is |
| DJ-Q8 | P3's form. | **a `RESET SESSION` statement the dispatcher routes as it routes `SET` (so a text client has it too), refused inside a transaction, and a defaults bit in `S_READY` behind a capability bit** | (b) a KWP frame only; (c) the statement without the bit |
| DJ-Q9 | P4. | **a park under a new wait event, bounded by the fault net, then today's refusal**; whether the session first tries another core is DJ-S0's reading of the placer | (b) a move alone; (c) nothing: the refusal stands |
| DJ-Q10 | P5a's bound. | **the transaction lifetime ceiling's value, applied to a parked walk whether or not a transaction is open** - no new key (DJ-R5) | (b) a key of its own |
| DJ-Q11 `[quiet-wrong]` | P5b. | **a byte bound on a portal's buffer, refused by name past it; the resumable read written up as a concept note, not built in DJ**, because it gives up "`SnapshotTooOld` is never raised" | (b) the resumable read built, a passed snapshot refused by name; (c) nothing |
| DJ-Q12 | Whether DJ-S0 prices the TLS handshake. | **yes, and reports it; moving it off the reactor is not in DJ** | (b) in DJ |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DJ-S0 - census and premise** | No engine code. **Read**: every listener and what it accepts into; how an instance's connections are counted above one core, and what moves with a session that changes core; every item of session state and where it is set and cleared (§1.4 completed, the session's published wait state and its statistics included); `Begin`'s refusal path and what the placer does at a head whose core is full; a portal's buffer and a parked walk's holds. **Measure**, `build-release`, inside the stage budget: resident memory per idle authenticated session, at 1,000 and 10,000 where the host's descriptor limit admits; the standing point-read cell's p50/p95/p99 beside those idle sessions against none; a connect, one statement and close loop, plain and TLS, in connections a second and in what the never-reconnecting sessions on that core lose; `Begin`'s refusals under more open transactions than a core's slots. | the lists, the results file, each DJ-Q restated against what was read, and the items back to the operator |
| **DJ-S1 - the two settings** `[operator]` | Under DJ-Q1..Q7: the keys, the instance's count, the cancel's exemption, the reserve, the third sweep, the two named refusals. `protocol.md` §10, §11, `manual/server/server.md`, `known-gaps.md`. | review, the full suite |
| **DJ-S2 - a session handed back clean** | Under DJ-Q8. `protocol.md` §3, §10, the client manual. | review, the full suite |
| **DJ-S3 - a wait in place of a refusal** | Under DJ-Q9, if marked. `txn.md`, `wait-events.md`, `crosscore.md`. | review, the full suite, the two-core rig |
| **DJ-S4 - the reply's bound** | Under DJ-Q10 and DJ-Q11, if marked. `protocol.md` §7, `known-gaps.md`. | review, the full suite |
| **DJ-S5 - the close** | DJ-S0's cells, interleaved A/B, the commit DJ opened from against the closing code, with both keys unset (DJ-R1, DJ-R6, DJ-R7) and with both set; the documents; the milestone row. | closed |

Tests owed with DJ-S1, named here because they are the settings'
contract: a connection past the cap refused by name and one below it
served, at one core and above; a `C_CANCEL` admitted at a full server and
its target cancelled; an `admin` admitted inside the reserve and a
`readwrite` refused there; the count returning to zero after every close
path - a hangup, a keepalive reap, a refused handshake, a session that
moved cores; an idle session closed at the timeout and told why; a
session with an open transaction, a held portal or a statement in flight
not closed by it; a `C_PING` under DJ-Q5 as marked; both keys at `0`
behaving as the commit DJ opened from.

## 6. What DJ does not do

- No pooler, proxy or gateway, and no topology shown to a client
  (`protocol.md` §8 kept).
- No `DECLARE CURSOR`, no scrollable or held cursor.
- No per-user, per-role or per-address limit but the administrator's
  reserve.
- No cached parse or prepared-statement store shared across sessions
  (CX-S6's ruling stands; no driver in the tree prepares).
- No change to SCRAM, to TLS, or to where the handshake runs (DJ-Q12).
- No claim against PostgreSQL, Oracle or Azure SQL: no twin cell exists
  for a connection count.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-20

In the main checkout on `main` at `4d72be11`, documents only. The order
rests on a reading of the sites in §1; nothing was run. Named as not
read: how many `TcpServer`s an instance has above one core and what
counts their connections together (§1.1), and what the handshake costs
its core (§1.6). Open with the operator: DJ-Q0..Q12, and whether DJ
opens.
