# Work order DJ - connections, sessions and cursors: a connection cap and an idle-session timeout an operator sets, a session a pool can hand back clean, a wait in place of a refusal, and a reply that is bounded

Status: **OPENED 2026-11-20 on the operator's standing go-ahead**
(`/go-ahead-achieving-milestone DJ`, `raft-marks-2026-11-20.md` §22), on
worktree `dj-connections-sessions-cursors` from `054dbc5e`
(`v3.4.17-1-g054dbc5e`). **DJ-S0 and DJ-S1 are done** (§7): the census and
the premise (`bench/v3.4.17/results-dj-s0-premise-v3.4.17-1-g054dbc5e.md`),
and the two settings at `69d754ae`. **Every DJ-Q item is adopted (a) on that
standing word, DJ-Q2 and DJ-Q3 as revised at DJ-S1, and none is the
operator's own mark**; DJ-Q13..Q16 were raised at DJ-S1. Not landed: no
push, merge, tag or version.

Written in the main checkout on
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

(a) is CLA's proposal in each. **Each is adopted (a) on the operator's
standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone)**
(`raft-marks-2026-11-20.md` §22) - DJ-Q2 and DJ-Q3 as revised at DJ-S1
(§7) - and none is the operator's own mark: the options not taken stay
open to it.

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
| DJ-Q13 | The reserve where no gate stamps a role. Raised at DJ-S1: with `auth = off` every session is `admin` (`session.hpp`), so DJ-Q3's "a session whose role is `admin`" names every session. | **nothing is reserved there: the ceiling is `max_connections` itself, the default reserve is silently none, and naming `reserved_admin_connections` with `auth = off` - or with `max_connections = 0` - is refused at boot**, because a key that reserves nothing would enforce something other than what was written | (b) the reserve admits everyone, so the ceiling is the cap plus the reserve; (c) `max_connections` with `auth = off` refused at boot |
| DJ-Q14 | A connection that never finishes its handshake, under the timeout. Raised at DJ-S1: it holds no seat (DJ-Q2) and nothing else ends it while its peer is alive (`protocol.md` §3: no pre-auth deadline). | **idle from its accept: the sweep closes it at the timeout, told why as far as its transport lets it be** | (b) the timeout is an authenticated session's alone |
| DJ-Q15 | The timeout on the debug text port. Raised at DJ-S1. | **it applies**: the sweep is the listener's on both protocols, as the lifetime ceiling's is, and an idle debug session is told by an `ERR` line; **the cap does not** (DJ-Q2) | (b) the default port alone |
| DJ-Q16 | What an operator reads the count from. Raised at DJ-S1: a cap nobody can see the distance to is set by guessing. | **a `connections` section in `sys.meta`: the two limits as they apply, the seats held, the sessions refused, the default port's idle closes** - three words written where a session is seated, refused or closed, none on a statement's path (DJ-R6) | (b) the server log alone; (c) a `sys.sessions` view (excluded by the operator, `sys-views.md` §8) |

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

### DJ-S0 - the census (2026-11-20)

On worktree `dj-connections-sessions-cursors`, read at `054dbc5e`
(`v3.4.17-1-g054dbc5e`), the commit DJ opened from. `[source-read]`, each
with its `path:line` at that commit. No engine code.

**Listeners, and what each accepts into.**

- **The default port: one `TcpServer` a core.** Core 0's is
  `Expeditor::ServeRuntime::listener` (`src/server/expeditor.cpp:1555`,
  bound at `:1743`); each peer's is `CoreRuntime::listener_`
  (`include/kds/server/core_runtime.hpp:542`), bound with `SO_REUSEPORT` by
  `ListenAndAttach` (`src/server/core_runtime.cpp:505`) or socketless
  (`TcpServer::Hosting()`) under D19's fallback (`:520`). Every one is
  configured from one `ClientSetup` (`expeditor.cpp:1764`), and each holds
  its own `clients_` map (`include/kds/server/tcp_server.hpp:501`).
- **The debug text port: one `TcpServer` on core 0** (`expeditor.cpp:1803`),
  loopback, the text protocol, the default port's gate where there is one.
- **The KWP load port: a `KwpLoadServer` on core 0** (`expeditor.cpp:1821`),
  a class of its own with its own accept loop and connection map
  (`src/server/kwp_load_server.cpp:225`) and no auth stage.

**What counts connections above one core: nothing, for the instance.** Each
listener counts its own map (`open_connections()`, `tcp_server.hpp:267`).
Above one core each reactor also keeps `sched::CoreLoad::Of(core).sessions`,
moved by `CountSession` at adopt, close and move
(`src/server/tcp_server.cpp:433`, `:430`, `:1141`, `:774`, `:786`) and
shared by both listeners of core 0; at one core it is not written (`:437`).
No word sums the cores, and a connection in transit between two cores is in
neither's count.

**What moves with a session that changes core** (`tcp_server.cpp:758-800`):
the map node whole - the socket's unread and unsent bytes, the TLS channel,
the frame decoder, the `KwpSession` (statements, portals, the skip-to-sync
posture) and the `Session`. Nothing per core travels: at a head the session
holds no transaction, lock, borrow, view or lease, and the clock, the cancel
registry and its entry are the instance's.

**Session state, where it is set and what clears it.**

| item | set by | cleared by |
|---|---|---|
| isolation level (`include/kds/server/session.hpp:162`) | the server's `isolation` at accept (`tcp_server.cpp:358`), `SET ISOLATION LEVEL` (`src/server/command_dispatcher.cpp:13144`) | disconnect |
| default namespace (`session.hpp:172`) | `USE` (`command_dispatcher.cpp:13126`) | disconnect |
| durability rung (`session.hpp:191`) | `SET DURABILITY` (`command_dispatcher.cpp:13174`) | disconnect |
| priority level, and its seat in the batch count (`session.hpp:444`) | `SET PRIORITY` (`command_dispatcher.cpp:13203`) | disconnect |
| role (`session.hpp:253`) | the auth gate (`src/server/kwp_session.cpp:519`, `tcp_server.cpp:899`) | never: it is who the session is |
| named statements, portals, expired-portal names (`include/kds/server/kwp_session.hpp:373-387`) | `C_PARSE`, `C_BIND`, the portal sweep | `C_CLOSE`, a failed statement (its portal), the portal timeout, disconnect |
| the transaction, its durability rung, the failed and expired states | `BEGIN` | `COMMIT`/`ROLLBACK` (`Session::Finish`), the lifetime sweep (`Expire`) |
| acknowledged-commit bound (`session.hpp:214`) | a commit | a snapshot that covers it; it must outlive any reset (BA-R1c) |
| wait statistics and the published wait state (`session.hpp:128`; `src/sched/wait_state.cpp:33`) | the session's first statement | disconnect; statistics, not settings |
| placement words: last move, counted window (`session.hpp:438`, `:449`) | the placer, the load window | not a client's |
| cancel flag (`session.hpp:242`) | a `C_CANCEL`, the connection's own close | taken by a walk or by the next frame |

`SET CABIN_OPTIMIZER`, `SET JOIN_ORDER` and `SET ASH` set the instance's
state, not a session's. Nothing returns the first four rows to their
defaults but a disconnect: `RESET` is not routed
(`command_dispatcher.cpp:1763-1773`).

**`Begin`'s refusal and the placer.** `TransactionManager::Begin` refuses
`ResourceExhausted` before it issues an id (`src/txn/manager.cpp:134-139`).
It is reached by `BEGIN` (`command_dispatcher.cpp:12863`), by an autocommit
write's scope (`:13265`) and by a declaration's lock-holder (`:5151`); a
`BEGIN ... READ ONLY` takes no slot. The placer is asked before the
statement is submitted (`tcp_server.cpp:685-696`) and reads three words a
core - busy fraction, active sessions, the window's end
(`src/server/load_placer.cpp:50-58`) - and nothing of a core's slots: a
session whose core is full is not moved for it, and by the time `Begin`
refuses the statement is past its head.

**A portal's buffer and a parked walk's holds.** A portal's sink keeps
every sealed batch in a vector with no bound (`kwp_session.hpp:180`); a
capped portal never streams (`:176`), so `C_EXECUTE {max_rows}` of any
statement holds its whole result until the portal goes. A walk parked on a
client that does not read waits on the connection's `resume_when`
(`src/exec/step_vm.cpp:2618-2621`, `tcp_server.cpp:723-729`) under
`net.send_backpressure`, holding its view, its borrow and its core's
reader-lease slot; the predicate ends on a close, an emptied outbox or a
cancel, and on no time.

**The handshake.** TLS is fed on the reactor that reads the socket
(`tcp_server.cpp:549`); the pre-auth inbox is capped at 4 KiB (`:520`) and
no deadline ends a connection that never finishes its handshake (`:519`).

### DJ-S0 - the measurement, and the items restated (2026-11-20)

`bench/v3.4.17/results-dj-s0-premise-v3.4.17-1-g054dbc5e.md`, measured at
`v3.4.17-1-g054dbc5e` with `tools/dj_s0_premise.py`, release, one binary,
its reactor pinned. The host was shared with another session's builds and
suite: the first run's timed cells were discarded for it, and the timed
cells reported are from runs that held each arm for a quiet host (held
2,652 s and 336 s, outside the stage's budget; about 4 minutes of arms).
`[measured]`:

- **An idle session is 2.5 to 3.0 KiB of resident memory, 4.2 to 4.7 KiB
  once it has run a statement, and 59.4 KiB under TLS.** Closing 10,000
  returned nothing to the host.
- **10,000 idle sessions cost the working session nothing readable**: its
  point read's p50 (71.1 to 71.5 us), p99 (86.6 to 91.4 us) and server CPU
  (30.2 to 30.4 us) sit inside the spread of the two arms with none. The
  two sweeps that walk every connection do not show in an idle server's
  CPU at 1,000 or at 10,000.
- **A connect, one point read and close costs its core about 68 us plain
  and about 750 us under TLS.** About 6,500 plain ones a second move the
  standing session's p50 from 71.5 to 93.9 us and its p99 from 88.4 to
  146.5 us; about 545 TLS ones a second move its p50 from 127.5 to
  158.8 us and its p99 from 162.2 to 614.9 us. The rates are one Python
  client's.
- **`Begin` refuses the 1,025th open transaction of a core,
  `retryable = 0`, and an autocommit write with it**; reads and
  `BEGIN READ ONLY` are answered. 1,500 sessions looping `BEGIN`, one
  `INSERT`, `COMMIT` on one core had 2.64 % of their `BEGIN`s refused. At
  `cores = 2` 1,900 held transactions met no refusal; a skew that fills
  one core beside one with room was not produced.
- **`C_EXECUTE {max_rows = 100}` of a 100,000-row result answers its first
  hundred rows after 19.8 ms where `LIMIT 100` answers in 92 us, and the
  suspended portal holds 4.4 MiB.**

Not measured: any arm under SCRAM; a handshake burst from more than one
client process; `cores > 1` but for one `begin` arm; a parked walk's hold
(read, in the census above, not timed).

**Each DJ-Q against what was read and measured**, all adopted on the
operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone,
`raft-marks-2026-11-20.md` §22), none the operator's own mark:

- **DJ-Q0 (a)** stands. P4 and P5 are taken up on the numbers above: the
  refusal is real and reaches writers that opened no transaction (P4), and
  a capped portal's price is the whole result (P5b); P5a rests on the
  census, not a number.
- **DJ-Q1 (a)** stands: nothing measured argues for a default other than
  no limit. The figures to size a cap by are 3 to 5 KiB a plain session and
  59 KiB a TLS one.
- **DJ-Q2 (a) `[quiet-wrong]`, revised twice at DJ-S1.** The proposal
  counted "authenticated sessions ... and connections still in their
  handshake". Read against the tree: a connection is a cancel or a session
  only once its first frame is read, so counting from the accept would
  refuse a `C_CANCEL` at a full server (DJ-R2); and - the step's review -
  counting during the exchange lets a peer with no credentials hold every
  seat, the reserve's too, by saying hello and never authenticating.
  **Adopted as revised: a session is counted from the end of its
  handshake to its close.** A connection that has not finished its
  handshake is neither counted nor capped (`known-gaps.md`).
- **DJ-Q3 (a)** stands in its number (3, `[provisional]`) and is revised
  in its mechanism with DJ-Q2: the seat is taken once the role is known,
  so each session is refused at its own ceiling, and no seat is taken and
  given back.
- **DJ-Q4 (a), DJ-Q5 (a), DJ-Q6 (a), DJ-Q7 (a)** stand. DJ-Q5 is
  sharpened by the build: what resets the idle clock is a statement run,
  so a `C_SYNC`, a `C_PARSE` and a `C_CLOSE` reset nothing either; and
  "a held portal" is one that still owes rows or a run - a spent portal is
  not held, or a client that never closes its unnamed portal would be idle
  only once the portal timeout had run.
- **DJ-Q8 (a)** stands.
- **DJ-Q9 (a)** stands as a park alone: the placer reads no slot count and
  the statement is past its head when `Begin` refuses, so "first tries
  another core" is not built.
- **DJ-Q10 (a), DJ-Q11 (a) `[quiet-wrong]`** stand. DJ-Q11's premise was
  re-read: every view is registered with the horizon and `SnapshotTooOld`
  is never raised (`CLAUDE.md`, the Transactions row; `txn.md` §4.1), so a
  read resumed under a snapshot the server does not hold would have to be
  refused or answered from another snapshot; the byte bound gives nothing
  up.
- **DJ-Q12 (a)** stands, and is done: priced above.
- **DJ-Q13..Q16 (a)**, raised at DJ-S1 and written into §4 before they
  were adopted.

### DJ-S1 - the two settings (2026-11-20)

On `dj-connections-sessions-cursors` at `69d754ae`. Built under DJ-Q1..Q7
and DJ-Q13..Q16:

- **`max_connections`** (`0`: no limit) and
  **`reserved_admin_connections`** (3, `[provisional]`; in force beside a
  cap and `auth = scram`, refused at boot where named without either).
  One `ConnectionLimits` for the instance
  (`include/kds/server/connection_limits.hpp`), an `Expeditor` member
  handed to every core's listener in its `ClientSetup`. A `KwpSession`
  holds a `ConnectionSeat`, taken where its handshake ends
  (`KwpSession::TakeSeat`) and given back by its destructor - so by every
  path that destroys a connection, a move between cores carrying it
  untouched in the map node. Refused: `RESOURCE_EXHAUSTED` /
  `kConnectionLimit` (6), fatal.
- **`idle_session_timeout_s`** (`0`: off; 1 to 59 refused).
  `TcpServer::CloseIdleSessions`, a third sweep, registered only where the
  key is set, every eighth of the timeout, on both protocols. A statement's
  path carries `Connection::worked`, one byte written in `SubmitStatement`
  while the key is set. Told: `RESOURCE_EXHAUSTED` / `kIdleSessionTimeout`
  (7), fatal, or an `ERR` line.
- **`sys.meta`'s `connections` section**: the two limits as they apply,
  `connection_sessions`, `connection_refusals`, `idle_session_closes`.

Documents: `protocol.md` §3, §10, §11; `sys-views.md` §7;
`client-manual.md`; `manual/server/server.md`; `kds.conf.sample`;
`known-gaps.md` ("Connections and sessions (DJ)").

**Tests** (`tests/connection_limits_test.cpp`, 15 cells; two in
`expeditor_test.cpp`; three in `config_file_test.cpp`; the detail numbers
in `kwp_error_test.cpp`), each of §5's owed cells: a session past the cap
refused by name and one below it served, at one core and at two; a
`C_CANCEL` admitted at a full server and its target cancelled; an `admin`
admitted inside the reserve and a `readwrite` refused there; the count
back at zero after a hangup, a hangup mid-exchange, a refused exchange, a
`C_TERMINATE`, a detached listener and a session moved between cores; an
idle session closed at the timeout and told why; a session with an open
transaction, a held portal or a statement in flight not closed; a `C_PING`
not resetting the clock and a statement resetting it; both keys at `0`
closing and refusing nothing. Not covered: `known-gaps.md` lists it.

**Review** (`critics-developer`). Found and fixed by it: the sweep cleared
a session's `worked` mark while its statement was still running, so a long
statement's session could be closed up to a tick short of the timeout
after the statement ended; and a text-port cell read with no timeout and
could hang the suite. Applied from it: **the seat moved from `C_HELLO` to
the end of the handshake** (its G1: hellos with no credentials could fill
the reserve; the move also removes its G2, a session refused for another's
unfinished exchange); `connection_limits_` declared above `handoff_`, so a
connection destroyed in transit gives its seat back to a live object; a
cell that pins the spent-portal rule inside the portal's 60 s; one helper
for a fatal refusal (`wire::FatalErrorFromStatus`); two dead accessors
removed; a derivation on the keys' 1,000,000 ceiling. **Rejected**: a
shared helper for "encode an error frame and append it" between the sweep
and `DrainFrames`' malformed-frame path - four lines, and the second site
is another step's code with another flow after it. Stated, not changed:
the clock-jump cells rest on one tick running between two jumps, and
"both cores' listeners were asked" is the kernel's spread over eight
attempts.

Suite: 0 failed of 4,969 (2 disabled, 2 skipped) at `69d754ae`. Overhead
not measured; measured at the milestone's close.

### DJ-S2 - a session handed back clean (2026-11-20)

On `dj-connections-sessions-cursors` at `56855c48`. Built under DJ-Q8 (a):

- **`RESET SESSION`** (`CommandDispatcher::HandleReset`), a session
  statement routed as `SET` is, open to every role, on both protocols. It
  returns the four settings a statement can leave on a connection - the
  isolation level (to the level the session began at, the server's
  `isolation`), the namespace, the durability rung, the priority level and
  its batch seat (`Session::ResetToDefaults`) - and, through
  `DispatchOutcome::session_reset`, drops the KWP session's statements,
  portals and expired-portal names after the reset's own completion is
  written. **Whole or refused** (DJ-R8): inside a transaction it is refused
  `INVALID_ARGUMENT` and changes nothing, the transaction included. One
  spelling; `RESET ALL` and `RESET <setting>` are refused with their byte.
- **`SESSION_DEFAULTS`**, capability bit 4, and `S_READY`'s frame flag
  bit 0 behind it: set when the session has no transaction, its four
  settings at their defaults, and no statement and no portal
  (`KwpSession::AtDefaults`), **computed at every `S_READY` and never
  remembered**. A client that did not offer the bit is sent flags `0`.
- **`S_HELLO` on the authenticated path carries the negotiated
  intersection**, as `protocol.md` §3 said and the unauthenticated path
  did; it carried the server's whole set. Found in passing, since the flag
  is gated on what the client offered, and fixed here.

Not reset, because they are not settings: the role, the
acknowledged-commit bound, the wait statistics, the session id, the
placement words, the cancel flag.

Documents: `protocol.md` §3, §10; `manual/sql/sql.md` (session
statements); `client-manual.md` (the role table). `tools/kwp.py` does not
offer the bit: no driver in the tree pools connections.

**Tests** (`tests/kwp_session_test.cpp`, eight cells): the flag sent only
to a client that offered the bit; each item a client can leave - the four
settings, a named statement, a named portal, a suspended unnamed portal,
the unnamed statement - clearing the flag alone and `RESET SESSION`
restoring it and the item; the reset refused inside an open transaction,
a `READ ONLY` one and a failed one, each left as it was; the level
returned to being the session's own starting level and not READ COMMITTED;
the batch seat given back; `RESET`'s refusals at their bytes. Not covered:
the flag over a real socket and a session that moved cores (the state
machine is socket-free and travels whole); an expired transaction
(answered by the failed-transaction gate, as a failed one).

**Review** (`critics-developer`). Found and fixed by it: inside
`BEGIN READ ONLY` the read-only gate answered first, telling the client to
"write in autocommit" and counting a write refused - `RESET` is on that
gate's allowlist now and the reset's own refusal answers; and the
documents said a refused reset fails its transaction, which no code does.
Applied from it: **the flag counts the unnamed statement and portal** (its
P1: the empty name is a name - a `C_CONTINUE ""` would take the last
borrower's rows - so the first cut's "the unnamed ones do not count" could
read clean over a suspended portal, against DJ-R8); the capability renamed
`kSessionDefaults`, clear of `DurabilityLevel::kSessionDefault`; the `;`
handling collapsed to the dispatcher's idiom; the in-transaction refusal
at the statement's byte; five cells (the three kinds of transaction, the
starting level, the batch seat, the transaction still open after the
refusal, both held statements counted). **Rejected**: none.

Suite on the commit's tree (it also carries DJ-S3's registered and not
yet used `res.txn_slot` wait event): **2 failed of 4,976**, both timing
cells on a host at load 17 from another session -
`StrictParkRigTest.AnExplicitStrictCommitParksAndPublishesAtItsSync`, the
flake `known-gaps.md` records, and
`DropTableReclaimCrashTest.AReclaimCutAnywhereIsFinishedByTheNextMount` -
and each passed three runs alone on the same binary. An earlier run of the
same code failed one other cell, `ShowSugarTest.SysWaitEventNamesListsTheRegistryByName`,
which pinned the registry's count as a literal; it reads the registry's
constant now. Not a clean full run: stated, not reported as a pass. Overhead not
measured; measured at the milestone's close.

### DJ-S3 - a wait in place of a refusal (2026-11-20)

Built under DJ-Q9 (a) as restated at DJ-S0 (a park alone): where
`TransactionManager::Begin` refuses because its core publishes
`kInFlightSlotsPerCore` transactions, the statement it refused - a `BEGIN`
(`HandleBegin`) or an autocommit write opening its own scope
(`BeginWrite`), each having written nothing - is marked
(`DispatchOutcome::slot_wait`), parks in `DispatchAsync` under the new wait
event `res.txn_slot` until the core has a slot, and runs again whole;
bounded by the lock family's fault net, after which the refusal is
answered as before. `TransactionManager::InFlightFull()` is the predicate.
Not parked: a declaration's lock-holder, a KWP load chunk, a structural or
`PURGE` re-run that meets the cap, and a dispatcher with no clock.
Documents: `txn.md` §4.1, `wait-events.md` (`res.txn_slot`).

**Tests** (`expeditor_test.cpp`): 1,024 holders on one core, a late `BEGIN`
and a late autocommit `INSERT` both waiting, both admitted as two holders
end, the write run once; with no slot freed, the late `BEGIN` refused naming
the cap after waiting the fault net.

**Review** (`critics-developer`). Found and fixed by it: the slot wait ran
after BA-S14's structural re-run and the `PURGE` wait, so a statement it
admitted that then met a stale descent was answered `TxnConflict` and a
`PURGE` skipped its horizon wait - the block now runs ahead of both; a
wrong section citation in `wait-events.md`. Applied from it: the refused
cell pins the refusal's text, the admitted cell the write's single run, and
`txn.md` names what is not parked. **Left, stated**: the slot wait's own
fault net is counted apart from the statement's, so a statement can wait
up to two fault nets; a cancel during the park is taken at the next frame;
each cell opens about 1,027 connections, which a 1,024 descriptor limit
would refuse.

### DJ-S4 - the reply's bound (2026-11-20)

- **P5a, under DJ-Q10 (a)**: a walk parked on a client that has taken no
  byte for `txn_lifetime_ceiling_ms` is cancelled (`TcpServer::
  SubmitStatement`'s `resume_when`), whether or not a transaction is open;
  no new key. Progress is a byte the outbox sent (`FlushOutbox`) or a drain
  of the socket's kernel queue (`SIOCOUTQ`, read only while parked).
- **P5b, under DJ-Q11 (a) `[quiet-wrong]`**: a capped portal's statement
  that produces more than `kMaxPortalHeldBytes` (16 MiB, `kMaxFrame`,
  `[provisional]`) of rows is refused `RESOURCE_EXHAUSTED` /
  `kPortalHeldBytes` (8), not fatal (`WireResultSink::Emit`). The resumable
  read is `docs/conceptnotes/cn12-read-resumed-by-key.md`, not built.

Documents: `protocol.md` §7, §11; `known-gaps.md` ("Streamed replies
(CE)", "Locking reads (DC)"); `docs/conceptnotes/README.md`.

**Tests** (`ce_streaming_reply_test.cpp`): a walk parked on a client that
does not read cancelled at a 1 s ceiling, the relation then free for a
`DROP` and the reader getting rows then `CANCELLED`; a client reading a
receive buffer every 300 ms not cancelled at a 1 s ceiling (its mutation -
no `SIOCOUTQ` reset - was run and fails it); a capped portal of about
19 MB refused by name and the connection standing, under the bound and
uncapped answered.

**Review** (`critics-developer`). Found and fixed by it: the park's clock
was reset only when the outbox emptied, so a client that read slowly was
cancelled as one that did not read - reset now at every byte the outbox
sends; four document corrections (the bound counted from the last byte; a
capped `RETURNING`'s refusal is the write's; the ceiling sweep skips a
statement over several parks, not one; CN-12's index row). Applied from it:
the slow-reader cell - which then failed, because a slow reader on
loopback frees the server's kernel buffer long before the socket turns
writable: hence the `SIOCOUTQ` read; and the capped cell widened to 10,000
rows. **Rejected**: none.

DJ-S3 and DJ-S4 were built, reviewed and suite-run on one tree and are
committed apart. Suite: 0 failed of 4,981 (2 disabled, 2 skipped) on the two together, at
`56855c48` plus both stages' changes; the golden detail number 8 added
after it, its file's 8 cells run green. Overhead not measured; measured at the
milestone's close.
