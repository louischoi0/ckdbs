# CN-10 — Concept Note: Unbounded Sessions, Bounded Transactions — thousands of logical connections on a thread-per-core engine

Status: CONCEPT, not a work order, not a CIP. Nothing here opens a stage.
**No work starts on this note until the operator rules on §9.** Ordering
relative to CN-1 to CN-9 is not fixed and none of them depends on it.
Author: CLA, 2026-09-29, against `b54a769`
Origin: the operator's conversation of 2026-09-29. The operator asked how a
one-thread-per-core engine manages a pool of more than 1,000 connections;
CLA's first answer located the limits in the tree; the operator then set the
target (§0) and asked for this note. The operator's statements are the
`[operator]` claims in §0. The mechanism, its order and every value are
CLA's proposals (§9), none ratified.
Claim tags: `[operator]` for the operator's statements; `[source-read]`
with `path:line` at `b54a769`; everything else `[design]`, including every
statement about Oracle, PostgreSQL, OpenSSL and Linux, which comes from
CLA's training knowledge and is to be re-checked before a work order cites
it. Nothing is `[measured]`.
Relation to `docs/inflight/`: §2 names properties of the connection path as
built that bound the target. One is a defect against `protocol.md` §10 - a
served session never expires an idle portal - and is recorded as
`docs/inflight/bugs/a-served-kwp-session-never-expires-an-idle-portal.md`;
the rest are the tree as built, stated in its source, and none is recorded
there. *Marked 2026-09-29: that defect is fixed on `fix-portal-idle-clock`
and its bug file removed; §2's portal-sweep bullet describes `b54a769`.*

---

## 0. The concept

`[operator]` First question: *"커넥션 풀 기능에 대해 고찰. one thread per
core 모델에서도 1000개 이상의 커넥션 풀을 관리하는 방법"* - consider a
connection-pool feature: how to manage a pool of more than 1,000
connections under the one-thread-per-core model.

`[operator]` The target, after CLA's first answer: *"논리적인 동시 접속
커넥션은 최대한 상한이 없어야 해. 물론 동시에 열리는 트랜잭션은 관리가
필요하겠지. Oracle과 같은 상용 DB 와 비슷한 수준이어야해"* - logical
concurrent connections should have as close to no upper bound as possible;
concurrently open transactions of course need managing; the level should be
comparable to a commercial database such as Oracle.

`[design]` Restated: **a session is cheap and unbounded; a transaction is
scarce and admitted.** Three layers, each limited by what it actually
costs:

| layer | what it holds | limited by |
|---|---|---|
| socket | an fd, a kernel buffer pair | the OS: `RLIMIT_NOFILE`, memory |
| session | statement and portal handles, isolation, durability, role | its idle footprint (§3.2), nothing configured |
| transaction | an in-flight slot, a snapshot, borrows, a place in the wait-for graph | **admission** (§3.5), because a core executes one statement at a time |
| autocommit read | a reader lease, no transaction | the per-core lease registry (§2); not contended while a read cannot suspend |

A core runs one statement at a time, so the instance's parallelism is
`cores`. Sessions beyond that buy overlap of waits - device I/O, lock waits,
the group-commit flush - and nothing else. The concept is that the number
of *sessions* stops mattering and the number of *open transactions per
core* is what the engine manages.

## 1. What other systems ship

`[design]`, from training knowledge.

| system | sessions | transactions / active work |
|---|---|---|
| Oracle, dedicated server | one OS process per session; `PROCESSES` and `SESSIONS` are configured ceilings | `TRANSACTIONS`; undo segments |
| Oracle, shared server (MTS) | dispatchers multiplex sockets; session memory (UGA) lives in the SGA's large pool, so any shared server runs any session's next call ("virtual circuit") | as above |
| Oracle DRCP | a pooled server attaches to a session per request, released at the request's end | as above |
| Oracle Resource Manager | - | `ACTIVE_SESS_POOL_P1` caps active sessions per consumer group and **queues** the rest, `QUEUEING_P1` bounding the wait; `MAX_IDLE_TIME`; `MAX_IDLE_BLOCKER_TIME` ends an idle session that blocks others |
| Oracle Net | `SQLNET.EXPIRE_TIME` (dead connection detection) reaps sessions whose client vanished | - |
| PostgreSQL | a backend process per connection, `max_connections`; pooling is external (PgBouncer, transaction mode) | `idle_in_transaction_session_timeout` |

The shape to copy is Oracle's: a network layer that multiplexes, session
state any executor can pick up, dead-peer detection instead of an idle
cap, and a queue in front of the scarce resource instead of a refusal.

## 2. What this tree has

`[source-read]` at `b54a769`, unless marked.

- **Sockets are already multiplexed.** Each core's `TcpServer` is a reactor
  participant: non-blocking fds on a level-triggered epoll backend, every
  client in `clients_` (`include/kds/server/tcp_server.hpp:370`), a statement
  run as a coroutine that may suspend, one per connection at a time
  (`tcp_server.hpp:232-247`). No connection has a thread. `[design]` This is
  Oracle's dispatcher layer, built.
- **The accept path is sized for a handful.** `listen(fd, 16)`
  (`src/server/tcp_server.cpp:74`) and one `accept` per readable event
  (`tcp_server.cpp:239`, by design: "bounded work per handler"). Under
  `SO_REUSEPORT` the 16 is per listener, so 16 x `cores` for the instance.
  The bulk-load listener has the same backlog and the same loopback bind
  (`src/server/kwp_load_server.cpp:57`, `:66`). `[design]` A pool opening
  1,000 connections at once overflows those queues; the kernel drops the
  excess SYNs and the clients retransmit after 1 s, then 3 s.
- **The port is loopback-only.** `INADDR_LOOPBACK` (`tcp_server.cpp:62`);
  `include/kds/server/expeditor.hpp:147` makes TLS the precondition for that
  changing. A pool on another host cannot connect today.
- **Nothing bounds a session, and nothing reaps one.** `protocol.md` §10:
  "There is no idle-*session* timeout" (`docs/spec/protocol.md:132`). No
  connection cap exists; no `SO_KEEPALIVE` and no `RLIMIT_NOFILE` handling
  appear anywhere under `src/`. Nothing puts a deadline on the handshake or
  the authentication exchange either, so a connection that never finishes
  them is held as long as its peer is alive.
- **A session's handles are data; the session object is not.** A named
  statement is its SQL text, a pattern id and a parameter count
  (`include/kds/server/kwp_session.hpp:274-278`); a portal is its
  substituted text, delivery state and a result sink
  (`kwp_session.hpp:280-300`). Both are capped at 64 per session
  (`kwp_session.hpp:131-132`). The engine session, idle, is a state, an
  isolation level, two durability options, a role, a null transaction
  pointer, a result-sink pointer cleared after every statement
  (`src/server/tcp_server.cpp:680`) and an empty parked write
  (`include/kds/server/session.hpp:317-326`). None names a core. **But
  `KwpSession` holds `Session& session_`** (`kwp_session.hpp:339`), bound to
  its `Connection`'s member, and connections live by value in `clients_` -
  so a `Connection` moved into another core's map leaves that reference
  naming the source's destroyed node. The fd's epoll registration is also
  the source reactor's.
- **A session never leaves the core that accepted it.** With `SO_REUSEPORT`
  the kernel's 4-tuple hash picks the core; under D19's fallback core 0
  round-robins (`include/kds/server/connection_handoff.hpp`). Either way the
  placement is made once, at the accept, and never by load.
- **The transfer mechanism exists.** `ConnectionHandoff` moves an fd to
  another core's inbox under a per-inbox latch, then kicks
  (`connection_handoff.hpp:11-38`); today it carries only freshly accepted
  sockets.
- **The idle-portal sweep walks every connection, and does nothing.**
  `SubmitEvery(kPortalIdleTimeoutNs / 4, ...)` iterates all of `clients_`
  (`tcp_server.cpp:193-197`), portal or none; but every served session is
  built with no clock (`tcp_server.cpp:308`), and `ExpireIdlePortals`
  returns at once on a null clock (`src/server/kwp_session.cpp:994`). The
  bug file named in the header records it.
- **Autocommit reads take no transaction, but take a lease.** They read under
  `AutocommitSnapshot` (`src/server/command_dispatcher.cpp:8011`), whose
  reader lease comes from a 256-entry registry per core that refuses
  `OutOfSpace` when full (`include/kds/txn/manager.hpp:206`,
  `src/txn/manager.cpp:786`); a read that cannot suspend never holds more
  than one at a time on its core. A slot is taken by exactly three sites:
  `BeginWrite` (`command_dispatcher.cpp:8031`) - every autocommit write, and
  every autocommit DDL through `InDdlStatement` (`:5717-5718`) - `BEGIN`
  (`:7793`, which the bulk-load server also reaches) and `CREATE
  ASSERTION`'s build transaction (`:2832`).
- **A write takes its slot before any borrow.** `BeginWrite` precedes the
  statement's read borrow on every write path - `INSERT` (`:4526` before
  `:4579`), `UPDATE` (`:7000` before `:7090`), `DELETE` (`:8711` before
  `:8774`) - and a DDL's body runs inside `InDdlStatement`, after it. Inside
  an explicit transaction `BeginWrite` never calls `Begin`
  (`:8018-8027`), and `BEGIN` refuses a session that has one (`:7739`). A
  read borrow never waits (`include/kds/server/read_borrow.hpp:27-35`).
- **Transactions are capped per core and refused past it.**
  `kInFlightSlotsPerCore = 1024` (`include/kds/txn/instance_visibility.hpp:246`);
  `TransactionManager::Begin` answers `ResourceExhausted` before spending an
  id (`src/txn/manager.cpp:143-154`). The cap exists because an unpublished
  transaction would read as not in flight on every other core
  (`instance_visibility.hpp:240-245`).
- **`CREATE ASSERTION` can hold one slot while asking for a second.** Its
  build begins its own transaction; the session's own open transaction is
  refused only where it already holds the relation
  (`command_dispatcher.cpp:2824-2832`).
- **Cancel is not wired.** `KwpSession::RequestCancel`
  (`kwp_session.hpp:266`) has no caller in `src/`.

## 3. The mechanism

`[design]` throughout.

### 3.1 Socket layer: accept without a ceiling

- Backlog `SOMAXCONN` (the kernel then caps it at `net.core.somaxconn`).
- `accept4(SOCK_NONBLOCK | SOCK_CLOEXEC)` drained up to a fixed count per
  event, not to `EAGAIN`, which keeps the handler bounded and still clears a
  burst of 1,000 in a few reactor turns.
- Raise `RLIMIT_NOFILE` to its hard limit at startup and report both in
  `SHOW META`; the fd limit is the ceiling the operator sees.
- The loopback bind is lifted only with TLS on (`expeditor.hpp:147`'s
  precondition, unchanged).

No connection cap is configured. The ceilings that remain are the OS's.

### 3.2 An idle session costs kilobytes

With no count limit, the per-session idle footprint *is* the limit, so it is
the thing to shrink:

- On going idle (no statement in flight, outbox drained), release the
  capacity of `inbox`, `outbox`, `frames_out`, `current_line`, `pending` and
  the frame decoder's buffer.
- On a TLS connection, `SSL_MODE_RELEASE_BUFFERS`: OpenSSL then frees its
  record buffers (on the order of 16 KiB each way) while idle.
- The portal sweep is first made to work (the bug file of the header),
  then walks only sessions holding a portal, from a list the portal's
  creation and close maintain, so an idle session costs no timer work.

The target is a single-digit-KiB idle session; at that size 100,000 sessions
are hundreds of MiB. The figure is an estimate and the first measurement a
work order would owe (§8).

### 3.3 Dead peers, not idle sessions

With no idle-session timeout, a client that vanished without a FIN would
hold its session forever. `SO_KEEPALIVE` with `TCP_KEEPIDLE`,
`TCP_KEEPINTVL`, `TCP_KEEPCNT` and `TCP_USER_TIMEOUT` reaps it - Oracle's
dead connection detection. An idle session with a live peer is never ended.

A connection that has not finished its handshake and authentication is not
a session yet, and keepalive does not reap it: its peer is alive. It needs a
deadline of its own (U9) - without one, U1 and U2 together let a client hold
unbounded unauthenticated connections.

### 3.4 Sessions move between cores at a transaction boundary

This is Oracle's shared server, scoped to what this engine pins to a core.
A statement runs where its session is, and since AT-S9 no relation has an
owner core, so any core may run a session's next transaction. What is
pinned is the *transaction*: its in-flight slot is in its core's table, and
its waits and commit run on that core's reactor. So:

- **The unit of placement is the transaction, not the call.** A session is
  movable when it has no open transaction, no statement in flight and no
  open portal. Its handles and its engine session are then data (§2).
- **The `Connection` object is not movable as it stands** (§2): the
  `KwpSession`'s reference to its `Session` is into the source's map node,
  and the fd is registered with the source's reactor. A move therefore
  needs the `Connection` heap-pinned (`clients_` holding a
  `unique_ptr<Connection>`, so the object never moves) or the reference
  rebound at the take; the source unregisters the fd before the offer, and
  the destination registers it at the take. Any clock the session holds
  (the portal bug's fix gives it one) is re-pointed to the destination's.
- **The carrier is `ConnectionHandoff`**, extended from an fd to an fd plus
  that `Connection`. Offer and take stay as they are: the shared inbox
  under its latch, then the kick.
- **The trigger** is the §3.5 admission: a session about to begin on a core
  whose slots are full, or whose run queue is long, is offered to a core
  with room first, and queues only where no core has room.
- Moving a session re-homes its cancel target; §2 notes cancel is not
  wired, so its registry must be the instance's from the start.

### 3.5 Transaction admission: a queue, not a refusal

Oracle's active session pool, placed at the three `Begin` sites of §2.

- When the core's in-flight table is full, `Begin` parks the statement on a
  FIFO of waiters instead of answering `ResourceExhausted`. A slot freed at
  commit or rollback (`RetireInFlight` runs in both, `src/txn/manager.cpp:370`,
  `:587`) wakes the head by the same slot-and-kick mechanism AX-S2b uses for
  row waits. A wait here, like every wait today, re-runs the statement; that
  costs nothing because nothing runs before `Begin`.
- A bounded wait: past a queue timeout the statement is refused
  `ResourceExhausted` under a detail of its own, since the client's fix
  differs from every other one - `kLockCap`'s argument
  (`include/kds/wire/error_registry.hpp:140-143`).
- **It cannot deadlock inside the engine at the two session sites**: a
  session parked in `BeginWrite` or `BEGIN` holds no slot and no borrow, so
  it is not in the wait-for graph. That rests on the ordering §2 cites - a
  write takes its slot before any borrow - and **the ordering becomes an
  invariant the queue depends on**: were `BeginWrite` moved after name
  resolution, a queued session would hold an `IS` that a DDL's `X` waits
  behind while the DDL holds the slot the session wants, a cycle outside
  the wait-for graph that only the queue timeout ends. **`CREATE
  ASSERTION`'s build is the exception** (§2): its session may hold a slot
  while asking for one, so that site keeps the refusal, or waits only while
  its session holds none.
- **Outside the engine it can**: a client holding a transaction on one
  connection while its `BEGIN` on another waits for a slot is ended only by
  the queue timeout. The timeout is therefore not optional.
- The cap stays at 1,024. Raising it is a separate question, decided by
  what a longer in-flight table costs `IsInFlight`, measured.

### 3.6 Idle transactions are ended, sessions are not

An open transaction with no statement in flight holds a slot, its borrows,
and the instance's read horizon (`[source-read]` `ReadHorizon()` answers
the instance's oldest live snapshot, `include/kds/txn/instance_visibility.hpp:485`), so a transaction leaked by a pooled client stalls purge
instance-wide. Oracle's `MAX_IDLE_BLOCKER_TIME` and PostgreSQL's
`idle_in_transaction_session_timeout` both answer this. Here: past an idle
limit the transaction is rolled back and the session told so at its next
frame; the session stays connected.

## 4. Placement against existing structures

| structure | used as | change |
|---|---|---|
| per-core reactor, `TcpServer` | the dispatcher layer | accept path (§3.1), buffer release (§3.2) |
| `ConnectionHandoff` | the session carrier | carries a `Connection` with the fd |
| `InstanceVisibility` in-flight tables | the admitted resource | none; the cap and its reason stand |
| AX-S2b's wait slot and release kick | the admission queue's wake | reused, not copied |
| `kPortalIdleTimeoutNs` | the portal timeout | none; §3.6's limit is a different quantity (a transaction's idleness, not a portal's), so it is not a second name for it |

## 5. What this note does not license

- No connection cap, idle-session timeout, admission queue, migration or
  keepalive setting is added on this note.
- No change to `kInFlightSlotsPerCore` or to what it protects.
- No change to KWP/1's frames. The queue is invisible to the client except
  as latency. The pinned error registry would gain two `ResourceDetail`
  entries - the queue timeout (§3.5) and the idle-transaction rollback
  (§3.6) - and neither is added on this note.
- No protocol multiplexing (many sessions over one socket) and no external
  pooler. §7 says why neither is needed for the target.
- No statement about performance at any session count. Nothing is measured.

## 6. Known consequences

- **Latency replaces refusal.** A client that retried on `ResourceExhausted`
  now waits. Tail latency at saturation grows by the queue's depth, and the
  queue timeout is what bounds it.
- **The lifted loopback bind is the first time KDS listens off-host.** Every
  unauthenticated connection then costs the §3.2 footprint plus a TLS
  handshake before anything refuses it. The OS ceiling and U9's deadline
  are the only guards against a connection flood.
- **Migration adds a reactor turn** to a moved session's first statement,
  plus a skipped kick's idle block in the worst case (`connection_handoff.hpp`
  states that price).
- **An idle-transaction rollback is a new way for a client to lose work.**
  It is the price of a bounded read horizon, and it has to be visible:
  a counter in `SHOW META` and an error naming the limit at the next frame.
- **Unbounded sessions make `SHOW` surfaces that list sessions unbounded
  too.** Any future session listing needs paging.

## 7. Why not multiplexing or an external pooler

- **Many sessions per socket** would cut fds, not the costs that matter:
  the session state stays, and it changes the protocol. The fd ceiling of
  §3.1 is already in the hundreds of thousands on a tuned host.
- **An external transaction-mode pooler** (PgBouncer-style) must track the
  server-side statement handles and reset session state between clients,
  and it adds a hop to every statement. §3.4 and §3.5 are the same function
  inside the engine, where the session state already is.

## 8. Measurements owed before a stage opens

- The idle-session footprint, with and without TLS, before and after §3.2.
- Accept of a 1,000- and a 10,000-connection burst, at one core and at
  several.
- The admission queue's wake latency against the refusal it replaces.

## 9. Operator decisions this note leaves open

| # | decision | CLA's proposal |
|---|---|---|
| U1 | Connection ceiling | **None configured.** The OS limits (`RLIMIT_NOFILE`, memory) are reported, not duplicated |
| U2 | Off-host listening | Lifted only with TLS on, as `expeditor.hpp:147` already states |
| U3 | `Begin` at a full core | **Queue** (§3.5), FIFO per core, with a retryable refusal past a timeout; `CREATE ASSERTION`'s build keeps the refusal |
| U4 | The queue timeout's default and name | Undecided; must not re-name an existing quantity |
| U5 | Idle transactions | **Roll back past a limit, keep the session** (§3.6). Whether the limit applies to every idle transaction or only to one that blocks a waiter or holds the horizon is the operator's |
| U6 | Session migration | **At a transaction boundary** (§3.4), triggered by admission, never mid-transaction |
| U7 | Dead-peer detection | Keepalive on by default, with Linux's per-socket options set from one setting |
| U8 | Order | the portal bug's fix and §3.1-§3.3 first (they need U1, U2, U7, U9 only), then §3.5-§3.6, then §3.4 |
| U9 | Handshake and authentication deadline | **A deadline of its own**, distinct from any idle-session timeout, which U1 leaves absent; its value and name undecided |
