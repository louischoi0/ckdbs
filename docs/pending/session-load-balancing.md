# Pending: session load balancing after the accept

Status: **PENDING** - deferred by the operator for later discussion. Decides
nothing, opens no stage.
Raised: 2026-10-08, in conversation with the operator.
Read at: `ef03771` (`[source-read]` line numbers are at this commit).
Related: `instructions/v3.0.0/workorder-ba-parallelism.md` (P11, BA-R13,
BA-Q12, BA-S16); `docs/conceptnotes/cn10-unbounded-sessions-bounded-transactions.md`
(§3.4, decision U6).

## 1. What prompted it

`[operator]` The operator started a server at `cores = 4` and observed only
core 0 doing work, then asked whether a session's requests all go to the
core it first landed on, and said: *"I think this should not be fixed per
connection; there should be load balancing in between"*.

## 2. What the tree does

- `[source-read]` Above one core every core listens on the default port
  with `SO_REUSEPORT` (`src/server/core_runtime.cpp:595`); the kernel's hash
  picks the accepting core. `force_listener_handoff` instead makes core 0
  accept and hand each connection round-robin
  (`src/server/expeditor.cpp:1485`, `include/kds/server/connection_handoff.hpp`).
- `[source-read]` The accepting core registers the client fd with its own
  reactor (`src/server/tcp_server.cpp:368`). Every later frame of that
  connection is read and executed there; no path moves a session to another
  core.
- `[source-read]` `debug_text_port` is bound by core 0 alone, without
  `SO_REUSEPORT` (`src/server/expeditor.cpp:1547`): every newline-protocol
  session runs on core 0 whatever `cores` is.
- `[source-read]` Core 0 owns the log and its commit drain syncs inline,
  where a peer only asks for a sync (`src/wal/manager.cpp:478`; BA P7), so a
  write load shows on core 0 even when its sessions run elsewhere.
- `[design]` A statement runs on one core; the remote-step path that split
  one was deleted at AT-S10. **One connection therefore uses one core at a
  time wherever it is placed** - load balancing, at the accept or later,
  spreads *several* sessions, never one.

## 3. Where the question already stands

- **BA-R13** (`workorder-ba-parallelism.md:984`) proposes placement by load
  *at the accept only*: the handoff arm picks the core with the fewest live
  sessions and becomes the default above one core. It states
  *"Not proposed: migration"* (`:992`). Its decision BA-Q12 (`:1038`) is
  unmarked, and BA-R13 is gated on the census finding P11 material.
- **CN-10 §3.4** proposes migration *at a transaction boundary*, triggered
  by its §3.5 admission queue, and names what blocks it in the tree as read
  at `b54a769` (CN-10 §2, §3.4; summarised in option (b) below). Its
  decision U6 is unmarked, and U8 orders §3.4 last.

## 4. Options for the discussion

| option | what | cost / risk |
|---|---|---|
| (a) | BA-R13 as written: the least-loaded core at the accept, no migration | one hop per connection; a long-lived pool can still skew as sessions end unevenly |
| (b) | (a) plus migration at a transaction boundary (no open transaction, no statement in flight, no open portal), carried by `ConnectionHandoff` | `Connection` heap-pinned or its `Session&` rebound; the fd re-registered on the destination (both CN-10); `[design]`, not in CN-10: a prepared statement compiled against the source core's catalog state shown sound on the destination; one reactor turn added to a moved session's next statement |
| (c) | Nothing until BA's census measures P11 material | the observed skew stays; consistent with BA-Q1's "measure first" |

`[design]` CLA's proposal, unratified: (a) first, and (b) as its own stage
only if the census shows long-lived sessions skewed enough to matter. A
ruling for (b) also names its trigger - load (run-queue length, live
sessions) or CN-10's admission - and where it is written: an amendment to
BA-R13, CN-10 U6's mark, or a work order of its own.
