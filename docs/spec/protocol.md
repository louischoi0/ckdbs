# KDS Wire Protocol (KWP/1)

The protocol KDS speaks: length-prefixed binary frames, a version and capability handshake, and an extended PARSE/BIND/EXECUTE statement model over server-side statement and portal handles. Replaces the newline text protocol in `docs/spec/client-manual.md`, which survives as an off-by-default loopback debug surface (§12). Companion workplan: `docs/inflight/in-progress/protocol-wp.md`. `[PROPOSED]` marks a default to confirm or amend before the affected part is built; `[OPEN]` must not be assumed. Consistent with `docs/rules/rules.md`, `docs/spec/sched.md`, `docs/spec/wal.md`, `docs/spec/waystone-concpets.md`.

**Status:** only the frame codec exists (`include/kds/wire/kwp.hpp`, `src/wire/frame_codec.cpp`) and nothing calls it. The server speaks the newline text protocol today.

## 0. Decision Record

| # | Decision | Choice |
|---|---|---|
| D1 | Protocol strategy | **Custom protocol (KWP)** — no PostgreSQL wire compatibility; KDS ships its own client libraries |
| D2 | Framing | **Length-prefixed binary frames** with version/capability handshake |
| D3 | Cross-core access | **Server-side forwarding** — clients are core-topology-unaware |
| D4 | Statement model | **Extended**: PARSE / BIND / EXECUTE with server-side statement handles |
| D5 | Data encoding | **Binary, little-endian** end to end |
| D6 | Results | **Chunked row-batch streaming** with portal suspension |
| D7 | Transactions & durability | On-wire txn control; **WAL durability class (D1/D2/D3) is a per-transaction protocol field** in v1 |
| D8 | Security | Handshake reserves auth + TLS stages now; NONE auth in v1, SCRAM and TLS phased in without a version break |
| D9 | Errors | Structured error frames with a code taxonomy aligned to engine `Status` categories + retryability flag |
| D10 | Session & ops | Session-scoped statements/txn/durability default; out-of-band cancel; admin via the same protocol |

## 1. Transport & Connection

- TCP; one KWP session per connection. Default port 15432 (unchanged).
- TLS — **decided 2026-08-13 and built: direct TLS.** A TLS-enabled port speaks TLS 1.3 from its first byte; there is no STARTTLS-style upgrade and no plaintext fallback on the same port, so a plaintext client is refused at its first record rather than served accidentally. Implemented *below* the protocol, at the transport seam (`include/kds/server/wire_channel.hpp`, the OpenSSL channel in `src/server/tls_channel.cpp`, config keys `tls` / `tls_cert_file` / `tls_key_file`), so it wraps whichever protocol the port speaks — the newline text protocol today, KWP unchanged when P13 lands. The `TLS_REQUIRED` capability bit keeps its purpose: a KWP client's way to demand the transport it is on. SCRAM parameters stay `[OPEN]` (§14). **What a refused connection sends back is OpenSSL's choice, not the channel's** (stated 2026-08-26): the channel hands the caller whatever the library queued — a fatal alert, or nothing — verbatim, and never any byte the peer itself sent. A version that queues no alert for a first record that was never TLS and one that queues a fatal alert both satisfy the contract, so nothing above the transport may key on which happened. This is written down because a test once pinned the byte count instead and failed on an OpenSSL upgrade against a channel that was correct.
- The newline text protocol remains available only on a loopback debug port behind a server flag (§12); it is not part of KWP.

## 2. Framing

Every message in both directions is one frame:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `length` | u32 LE; byte count of everything after this field (type..payload). Sanity ceiling `kMaxFrame` (default 16 MiB `[OPEN]`) |
| 4 | 1 | `type` | frame type (§4) |
| 5 | 1 | `flags` | per-type; 0 unless specified |
| 6 | 2 | `reserved` | 0; receivers ignore |
| 8 | … | `payload` | type-specific, binary LE |

Codec rules follow rules.md §2/§5: field-wise memcpy helpers, `static_assert`ed offsets, fixed-width integers only, no bitfields. Malformed frames (bad length, unknown type) produce an `ERROR` frame and, on framing-level corruption where resync is impossible, connection close — never a crash, never silent skip.

Variable-length payload fields use `{u32 len, bytes}`; strings are UTF-8, not NUL-terminated.

## 3. Handshake & Versioning

First frames on a connection, before anything else:

1. **`C_HELLO`** (client): `magic u32 = 'KWP1'`, `max_version u16`, `min_version u16`, `capabilities u64` (bitset: `STREAMING`, `CANCEL`, `TLS_REQUIRED`, `COMPRESSION[OPEN]`, …), `auth_method u8` (v1: `NONE`; reserved: `SCRAM_SHA256`, `MTLS`), client name/version string (telemetry only).
2. **`S_HELLO`** (server): chosen `version u16`, `capabilities u64` (intersection), `session_id u64`, `cancel_key u64` (§10), server version string. Auth sub-frames `[PROPOSED shape]` run here when a non-NONE method is negotiated; NONE proceeds directly.
3. **`S_READY`**: server is ready for statements. Also sent after every `C_SYNC` (§5) — it is the protocol's quiescent-point marker, carrying `txn_state u8` (idle / in-txn / failed-txn).

Version negotiation failure ⇒ `ERROR(UNSUPPORTED_VERSION)` + close. Capability bits gate optional behavior so features land without version breaks; version bumps are reserved for frame-format changes.

## 4. Frame Catalog

Client → server: `C_HELLO`, `C_PARSE`, `C_BIND`, `C_EXECUTE`, `C_CONTINUE`, `C_DESCRIBE`, `C_CLOSE`, `C_SYNC`, `C_TXN_BEGIN`, `C_TXN_COMMIT`, `C_TXN_ABORT`, `C_PING`, `C_CANCEL` (cancel connections only, §10), `C_TERMINATE`.

Server → client: `S_HELLO`, `S_READY`, `S_PARSE_OK`, `S_BIND_OK`, `S_ROW_DESC`, `S_ROW_BATCH`, `S_PORTAL_SUSPENDED`, `S_COMPLETE`, `S_TXN_OK`, `S_ERROR`, `S_PONG`, `S_NOTICE` (non-fatal server messages).

Unknown frame types: server responds `ERROR(PROTOCOL)`; clients must treat unknown *server* frame types as fatal unless a negotiated capability declared them.

## 5. Extended Statement Model

PG-shaped phases, KDS semantics:

- **`C_PARSE`** `{stmt_name str, sql str}` → server parses to the `Statement` AST and — this is the Waystone tie-in — computes the **query-template fingerprint at parse time**. `S_PARSE_OK` returns `{pattern_id u64}` (informational; clients may log it, never interpret it). Named statements are session-scoped and survive until `C_CLOSE` or disconnect; the unnamed statement (`""`) is overwritten by the next PARSE.
- **`C_BIND`** `{portal_name str, stmt_name str, param_count u16, params: [{i32 len | -1=NULL, bytes}]}` — parameters are binary LE per the type table (§6). Binding computes `arg_hash` for the Waystone event stream. `S_BIND_OK`.
- **`C_DESCRIBE`** `{kind u8 stmt|portal, name}` → `S_ROW_DESC`.
- **`C_EXECUTE`** `{portal_name str, max_rows u32}` — results per §7; `max_rows = 0` means unlimited.
- **`C_SYNC`** — pipeline barrier. Clients may pipeline PARSE/BIND/EXECUTE without waiting; after any `S_ERROR`, the server **discards frames until the next `C_SYNC`**, then answers `S_READY(failed-txn or idle)`. This skip-to-sync rule is the whole pipelining error contract.

## 6. Data Encoding

- `S_ROW_DESC`: `{field_count u16, fields: [{name str, type_oid u32, type_len i16 (-1=varlen), flags u16, type_mod u32}]}`. Field 0 of every user relation is the Keystone-derived `id` (u64). `type_mod` is zero for every type except `DECIMAL`, where it carries the column's packed `(p, s)` — precision in the high byte of the low half, scale in the low byte, **the same word the catalog stores** (`catalog::PackDecimalLen`), so there is one packing with two readers.
- Row values: `{i32 len | -1 = NULL, bytes}` per field — one NULL convention everywhere (params and rows).
- v1 type wire formats: `INT8/16/32/64` (LE two's complement), `UINT64` (Keystone ids), `FLOAT64` (IEEE 754 LE), `BOOL` (1 byte), `TEXT` (UTF-8), `BYTES`, `DECIMAL` (**decided 2026-08-07, with the type system as the `[OPEN]` required**: the unscaled **int64 LE, 8 bytes** — exactly the integer storage holds — with the scale in `S_ROW_DESC.type_mod`, once per result set and never per value; a per-value scale could only ever agree with the column or be a defect. The `[OPEN]`'s "scaled-int128 vs string" resolves as *scaled-int at the type's width*: `p > 18` is a future **separate** type per `types.md` TY2, which will carry its own type_oid and a 16-byte width, so nothing is foreclosed — and string is rejected because per-value text on an all-binary protocol reintroduces a parse step and the two-readings drift the type system removed), `DATE` (i32 epoch days since 1970-01-01), `TIMESTAMP` (i64 micros since epoch, UTC — **confirmed with `types.md` TY4**, which fixed storage to the same encoding), `DECIMAL128` (**the reserved separate type, realized 2026-08-07** — `types.md` §2a: type_oid 13, the int128 unscaled value in 16 LE bytes, low half first, `(p, s)` in `type_mod` exactly as the 8-byte type carries it).
- No text result mode exists. Human-readable rendering is a client concern (the CLI renders) — a `DATE`'s epoch day and a `DECIMAL`'s unscaled integer included.
- **Status: the row encoding above is implemented** — `include/kds/wire/row_codec.hpp` (2026-08-05; `DECIMAL`/`DATE`/`TIMESTAMP` arms and `type_mod` 2026-08-07). It is deliberately below both consumers: `docs/spec/crosscore.md` CC2 requires cross-core `STEP_BATCH` payloads in this same encoding, so the encoder knows about neither frames nor cores. `FLOAT64` is specified above but not implemented — nothing can store one, and the encoder refuses what storage refuses. A decimal value whose scale disagrees with its column is refused at encode, never rescaled — the same rule the storage codec applies.

## 7. Result Streaming

- `C_EXECUTE` produces `S_ROW_DESC` (unless suppressed by flags after a DESCRIBE) then a sequence of **`S_ROW_BATCH`** frames: `{row_count u16, rows…}`, batch size server-chosen (default target ≤ 64 KiB per frame `[OPEN: default]`).
- If `max_rows > 0` and the portal has more rows when the quota is reached, the server sends **`S_PORTAL_SUSPENDED`**; the client resumes with `C_CONTINUE {portal_name, max_rows}`. This is the flow-control mechanism — explicit, deterministic, and testable, in place of TCP-buffer guesswork. Credit/window schemes stay `[OPEN]` behind a capability bit if ever needed.
- Completion: `S_COMPLETE {tag str, rows_affected u64}`.
- Reactor note: a suspended portal is a suspended foreground task holding pins; portal-idle timeout (§10) bounds how long a slow client can hold engine resources.

## 8. Cross-Core Execution — Server-Side Forwarding

- A connection is owned by the core that accepted it; its session state (statements, portals, txn) lives on that core (rules.md §3).
- When a statement targets data owned by another core, the owning-core work is dispatched over the cross-core message interface and results return to the session core, which frames them to the client. **Clients never see topology**; no routing hints exist in KWP v1.
- This choice keeps clients simple at the cost of a forwarding hop; a future smart-routing extension (topology frame + session migration) is `[OPEN]` and must arrive as a capability bit, not a version break.
- While the engine runs single-core (current state), forwarding is trivially absent; the protocol is unaffected.

## 9. Transactions & Durability

- Autocommit by default: a lone EXECUTE is its own transaction.
- `C_TXN_BEGIN {durability u8}` / `C_TXN_COMMIT` / `C_TXN_ABORT` → `S_TXN_OK`. `durability` ∈ {0 = session default, 1 = D1 strict, 2 = D2 group, 3 = D3 relaxed} per `docs/spec/wal.md` §1. The session default is set via a session-settable statement `[PROPOSED: SET DURABILITY]`.
- `S_TXN_OK` for COMMIT is sent only after the WAL ack point of the chosen class (wal.md §8-2). For D3 the reply carries `flags.RELAXED=1` so audit logs can distinguish ack semantics.
- Failed-txn state: after an in-txn error, only ABORT (and SYNC) are accepted until rollback — mirrored in `S_READY.txn_state`.

## 10. Session, Cancel & Ops

- Session state: named statements, portals, txn, durability default. All dropped on disconnect; server may cap statement/portal counts (`ERROR(LIMIT)` beyond).
- **Cancel:** out-of-band — a new connection sends `C_CANCEL {session_id, cancel_key}` and closes. The server sets a cancel flag the target task observes at its cooperative yield points (the reactor has no preemption — cancellation is best-effort-fast, guaranteed-eventually). Cancel keys are random per session; a wrong key is silently ignored.
- Keepalive `C_PING`/`S_PONG`; server idle-session timeout and portal-idle timeout `[OPEN: defaults]`.
- **Admin over the same protocol:** `SHOW META`, `LIST TABLES`, Waystone/WAL observability queries are ordinary statements returning ordinary result sets — one surface, one auth story. `STOP` becomes an admin statement requiring a capability bit `[PROPOSED]` instead of today's unauthenticated line command.

## 11. Error Model

`S_ERROR` payload: `{code u32, retryable u8, severity u8, message str, detail str?, position u32?}`.

- `code = category u16 << 16 | detail u16`; categories mirror engine `Status` (InvalidArgument, NotFound, AlreadyExists, OutOfSpace, Internal, Protocol, Unsupported, TxnConflict, Cancelled, …). Detail codes are append-only — never renumber.
- `retryable` is authoritative client guidance (e.g. TxnConflict = 1, InvalidArgument = 0); financial client libraries build retry loops on this bit, so it is part of the compatibility surface.
- Errors never close the connection except framing-level corruption (§2) and handshake failures.

## 12. Server Implementation Notes

- `tcp_server` gains a frame decoder (length-prefixed accumulate) replacing line splitting; `command_dispatcher` splits into a KWP session state machine + the retained loopback text dispatcher (debug flag, default off in production builds).
- Frame parse/serialize buffers are preallocated per connection; steady-state no allocation per the reactor rules.
- All socket I/O stays on the injected `IoBackend`/reactor path — the protocol state machine must run under deterministic simulation with scripted byte streams (that is how §14's tests exist).
- `tools/ckdbs_cli.py` is rewritten as the KWP reference client and doubles as the conformance harness driver.

## 13. Required Amendments & Follow-ups

1. Rewrite `docs/spec/client-manual.md` for KWP/1; move the newline protocol to a "debug surface" appendix.
2. `CLAUDE.md`: architecture summary line for KWP; add §… opens below to the open list.
3. Reserve the `SET DURABILITY` statement in the parser spec; wire `pattern_id` return into the Waystone workplan (touches T11/T18).
4. Client library plan (D1 consequence): reference Python client first (CLI), then the customer-facing library — separate workplan.

## 14. Open Decisions — do not assume

- ~~TLS activation phase and mode (direct vs upgrade)~~ — **decided 2026-08-13**: direct TLS at the transport seam, active now under the `tls` config key; see §1.
- ~~SCRAM parameters~~ — **decided and built 2026-08-13**: SCRAM-SHA-256 (RFC 5802/7677), server and client state machines in `src/server/scram.cpp`, active on the text protocol under `auth = scram` (an `AUTH` line exchange, `docs/spec/client-manual.md`) and carried into KWP P07 as the same message bodies in handshake frames. Parameters: PBKDF2 iterations **fixed at 4096 in v1** (the RFC floor; a `--iterations` flag was removed on review because the unknown-user mock always answers `i=4096`, so any raised verifier was a one-round-trip enumeration oracle through `i=` — raising the count is tied to teaching the mock the deployment's own number), salt 16 uniform bytes from the one producer `scram::RandomSalt`, both ends bounding `i=` to 4096..10,000,000, verifiers stored as RFC 5803-shaped strings in a flat users file behind a `CredentialStore` interface. `auth = scram` refuses an open `kwp_port` — the load endpoint has no auth stage until P07. Deliberately deferred, each stated in `scram.hpp`: channel binding (SCRAM-PLUS), SASLprep (usernames restricted to `[A-Za-z0-9_.-]` at provisioning instead), cross-connection mock-salt consistency for unknown users, and a pre-auth deadline (the pre-auth inbox is capped at 4 KiB; a timer is not yet available to bound the clock).
- `kMaxFrame`, default batch size target, session/portal timeout defaults.
- ~~`DECIMAL` wire encoding (with the engine type system)~~ — **decided 2026-08-07** with the type system built, exactly as this line required; see §6. Additional types stay open.
- Compression capability; credit-based flow control capability; topology/smart-routing extension.
- ~~Auth→authorization model (roles/permissions)~~ — **decided and built 2026-08-13: statement-class roles.** Three ranks ordered by inclusion — `readonly` (reads, SHOW/DESCRIBE/ANALYZE, transaction control, own `SET ISOLATION`), `readwrite` (+ INSERT/UPDATE/DELETE), `admin` (everything: DDL, STOP, SYNC, server-wide SET) — held per user in the users file's new role column (`<user> <role> <verifier>`, `--add-user --role`, default readonly), stamped onto the session by the auth gate, checked once per statement at the dispatcher's routing tokens (`RequiredRole`, `src/server/command_dispatcher.cpp`). **Unclassified commands require admin** — refused by default, never admitted by omission. With `auth = off` every session is admin (an unauthenticated instance is the operator's own process). Two boundaries stated so nobody overreads them: **`readonly` bounds statement classes, not page writes** — a readonly SELECT still records Waystone trails, access statistics and pattern rows, exactly as invariants 8/9 permit; and **`SYNC` is admin's** because it forces device-wide I/O — a client's durability guarantee is the durability class's job (per-transaction in KWP), never a statement a tenant may issue. Ranks, not grant sets, on purpose: **per-relation GRANT/REVOKE remains the future refinement**, slotting under the same check once the catalog is recovered (RV3) — ACLs must not live in the one subsystem a crash silently loses. Role changes are re-provisioning (delete-then-add); no runtime GRANT surface exists.

## 15. Testing Requirements

1. **Codec:** frame/payload round-trips for every type; `static_assert` offsets; fuzzed malformed frames (bad length, truncation at every byte, unknown types) — server never crashes, always `ERROR` or clean close.
2. **Handshake:** version intersection matrix; capability gating; unsupported version path.
3. **Extended flow:** pipelined PARSE/BIND/EXECUTE with mid-pipeline error ⇒ skip-to-SYNC semantics exact; named/unnamed statement lifecycles.
4. **Streaming:** suspension/CONTINUE across batch boundaries; portal-idle timeout releases pins; `max_rows=0` path.
5. **Durability semantics:** under deterministic crash injection (wal.md §16), a `S_TXN_OK(D1/D2)` acked commit always survives; D3 window bound asserted; RELAXED flag present.
6. **Cancel:** cancel during long EXECUTE interrupts at a yield point; wrong key ignored; post-cancel session state = failed-txn rules.
7. **Conformance suite:** scripted byte-level golden sessions runnable against the server under simulation — the CLI and future client libraries replay the same suite.
