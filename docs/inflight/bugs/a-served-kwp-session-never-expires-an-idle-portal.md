# A served KWP session never expires an idle portal

**Found by reading, not reproduced.** Verified at `b54a769` on
`cn10-unbounded-sessions`, by CN-10's review.

## What is wrong

`protocol.md` §10 promises a 60 s portal-idle timeout
(`kPortalIdleTimeoutNs`, `include/kds/server/kwp_session.hpp:122`). On a
served instance it never fires:

- `TcpServer::AdoptConnection` builds every KWP session with no clock -
  `conn.kwp.emplace(conn.session, config, durability_)`
  (`src/server/tcp_server.cpp:308`) - so the constructor's `clock`
  defaults to null (`kwp_session.hpp:224-226`).
- `KwpSession::ExpireIdlePortals` returns at once on a null clock
  (`src/server/kwp_session.cpp:994`).
- The sweep timer (`tcp_server.cpp:193-197`) therefore walks every client
  every 15 s and changes nothing.

The suite does not see it: `tests/kwp_session_test.cpp:62` hands the
session a clock directly, which the server never does.

## Smallest reproduction

Open a KWP connection, `C_PARSE`/`C_BIND`/`C_EXECUTE` a statement with a
row cap below its result so the portal suspends, then wait past 60 s and
send `C_CONTINUE`. §10 says the answer is `RESOURCE_EXHAUSTED` with
`kPortalIdleTimeout`; the server delivers the next batch.

## Cost

No wrong answer and no refusal: a suspended portal's result sink stays in
memory until the client closes it or disconnects, where §10 bounds it at
60 s. A client that abandons suspended portals holds up to 64 of them per
session (`kMaxSessionPortals`) for the life of the connection - memory the
spec says is reclaimed.

The fix is known and unscheduled: pass the reactor's clock at the emplace.
CN-10 §3.2 depends on it.
