# KDS Wire Protocol — Technical Specification (KWP/1)

**Status:** **Official specification**, decisions confirmed 2026-07-28. Supersedes the newline text protocol documented in `docs/client-manual.md` (retained as a loopback debug surface only, §12). Markers: `[CONFIRMED]` — decided; `[PROPOSED]` — this document's default within a confirmed decision, adopt or amend before implementing the affected part; `[OPEN]` — do not assume. Consistent with `docs/rules.md`, `docs/sched.md`, `docs/wal.md`, `docs/waystone-concept.md`.

## 0. Decision Record `[CONFIRMED 2026-07-28]`

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
- TLS `[OPEN: activation phase]`: the handshake carries a `TLS_REQUIRED` capability bit so TLS can be introduced (direct-TLS or STARTTLS-style upgrade — pick when activated) without a protocol version bump.
- The legacy newline protocol remains available only on a loopback debug port behind a server flag (§12); it is not part of KWP.

## 2. Framing `[CONFIRMED]`

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

## 3. Handshake & Versioning `[CONFIRMED]`

First frames on a connection, before anything else:

1. **`C_HELLO`** (client): `magic u32 = 'KWP1'`, `max_version u16`, `min_version u16`, `capabilities u64` (bitset: `STREAMING`, `CANCEL`, `TLS_REQUIRED`, `COMPRESSION[OPEN]`, …), `auth_method u8` (v1: `NONE`; reserved: `SCRAM_SHA256`, `MTLS`), client name/version string (telemetry only).
2. **`S_HELLO`** (server): chosen `version u16`, `capabilities u64` (intersection), `session_id u64`, `cancel_key u64` (§10), server version string. Auth sub-frames `[PROPOSED shape]` run here when a non-NONE method is negotiated; NONE proceeds directly.
3. **`S_READY`**: server is ready for statements. Also sent after every `C_SYNC` (§5) — it is the protocol's quiescent-point marker, carrying `txn_state u8` (idle / in-txn / failed-txn).

Version negotiation failure ⇒ `ERROR(UNSUPPORTED_VERSION)` + close. Capability bits gate optional behavior so features land without version breaks; version bumps are reserved for frame-format changes.

## 4. Frame Catalog `[CONFIRMED types; payloads PROPOSED]`

Client → server: `C_HELLO`, `C_PARSE`, `C_BIND`, `C_EXECUTE`, `C_CONTINUE`, `C_DESCRIBE`, `C_CLOSE`, `C_SYNC`, `C_TXN_BEGIN`, `C_TXN_COMMIT`, `C_TXN_ABORT`, `C_PING`, `C_CANCEL` (cancel connections only, §10), `C_TERMINATE`.

Server → client: `S_HELLO`, `S_READY`, `S_PARSE_OK`, `S_BIND_OK`, `S_ROW_DESC`, `S_ROW_BATCH`, `S_PORTAL_SUSPENDED`, `S_COMPLETE`, `S_TXN_OK`, `S_ERROR`, `S_PONG`, `S_NOTICE` (non-fatal server messages).

Unknown frame types: server responds `ERROR(PROTOCOL)`; clients must treat unknown *server* frame types as fatal unless a negotiated capability declared them.

## 5. Extended Statement Model `[CONFIRMED]`

PG-shaped phases, KDS semantics:

- **`C_PARSE`** `{stmt_name str, sql str}` → server parses to the `Statement` AST and — this is the Waystone tie-in — computes the **query-template fingerprint at parse time**. `S_PARSE_OK` returns `{pattern_id u64}` (informational; clients may log it, never interpret it). Named statements are session-scoped and survive until `C_CLOSE` or disconnect; the unnamed statement (`""`) is overwritten by the next PARSE.
- **`C_BIND`** `{portal_name str, stmt_name str, param_count u16, params: [{i32 len | -1=NULL, bytes}]}` — parameters are binary LE per the type table (§6). Binding computes `arg_hash` for the Waystone event stream. `S_BIND_OK`.
- **`C_DESCRIBE`** `{kind u8 stmt|portal, name}` → `S_ROW_DESC`.
- **`C_EXECUTE`** `{portal_name str, max_rows u32}` — results per §7; `max_rows = 0` means unlimited.
- **`C_SYNC`** — pipeline barrier. Clients may pipeline PARSE/BIND/EXECUTE without waiting; after any `S_ERROR`, the server **discards frames until the next `C_SYNC`**, then answers `S_READY(failed-txn or idle)`. This skip-to-sync rule is the whole pipelining error contract.

## 6. Data Encoding `[CONFIRMED binary LE; type table PROPOSED]`

- `S_ROW_DESC`: `{field_count u16, fields: [{name str, type_oid u32, type_len i16 (-1=varlen), flags u16}]}`. Field 0 of every user relation is the Keystone-derived `id` (u64).
- Row values: `{i32 len | -1 = NULL, bytes}` per field — one NULL convention everywhere (params and rows).
- v1 type wire formats: `INT8/16/32/64` (LE two's complement), `UINT64` (Keystone ids), `FLOAT64` (IEEE 754 LE), `BOOL` (1 byte), `TEXT` (UTF-8), `BYTES`, `DECIMAL` `[OPEN: encoding — financial domain will need it; scaled-int128 vs string, decide with the type system]`, `TIMESTAMP` (i64 micros since epoch, UTC `[PROPOSED]`).
- No text result mode exists. Human-readable rendering is a client concern (the CLI renders).

## 7. Result Streaming `[CONFIRMED]`

- `C_EXECUTE` produces `S_ROW_DESC` (unless suppressed by flags after a DESCRIBE) then a sequence of **`S_ROW_BATCH`** frames: `{row_count u16, rows…}`, batch size server-chosen (default target ≤ 64 KiB per frame `[OPEN: default]`).
- If `max_rows > 0` and the portal has more rows when the quota is reached, the server sends **`S_PORTAL_SUSPENDED`**; the client resumes with `C_CONTINUE {portal_name, max_rows}`. This is the flow-control mechanism — explicit, deterministic, and testable, in place of TCP-buffer guesswork. Credit/window schemes stay `[OPEN]` behind a capability bit if ever needed.
- Completion: `S_COMPLETE {tag str, rows_affected u64}`.
- Reactor note: a suspended portal is a suspended foreground task holding pins; portal-idle timeout (§10) bounds how long a slow client can hold engine resources.

## 8. Cross-Core Execution — Server-Side Forwarding `[CONFIRMED]`

- A connection is owned by the core that accepted it; its session state (statements, portals, txn) lives on that core (rules.md §3).
- When a statement targets data owned by another core, the owning-core work is dispatched over the cross-core message interface and results return to the session core, which frames them to the client. **Clients never see topology**; no routing hints exist in KWP v1.
- This choice keeps clients simple at the cost of a forwarding hop; a future smart-routing extension (topology frame + session migration) is `[OPEN]` and must arrive as a capability bit, not a version break.
- While the engine runs single-core (current state), forwarding is trivially absent; the protocol is unaffected.

## 9. Transactions & Durability `[CONFIRMED]`

- Autocommit by default: a lone EXECUTE is its own transaction.
- `C_TXN_BEGIN {durability u8}` / `C_TXN_COMMIT` / `C_TXN_ABORT` → `S_TXN_OK`. `durability` ∈ {0 = session default, 1 = D1 strict, 2 = D2 group, 3 = D3 relaxed} per `docs/wal.md` §1. The session default is set via a session-settable statement `[PROPOSED: SET DURABILITY]`.
- `S_TXN_OK` for COMMIT is sent only after the WAL ack point of the chosen class (wal.md §8-2). For D3 the reply carries `flags.RELAXED=1` so audit logs can distinguish ack semantics.
- Failed-txn state: after an in-txn error, only ABORT (and SYNC) are accepted until rollback — mirrored in `S_READY.txn_state`.

## 10. Session, Cancel & Ops `[CONFIRMED model; details PROPOSED]`

- Session state: named statements, portals, txn, durability default. All dropped on disconnect; server may cap statement/portal counts (`ERROR(LIMIT)` beyond).
- **Cancel:** out-of-band — a new connection sends `C_CANCEL {session_id, cancel_key}` and closes. The server sets a cancel flag the target task observes at its cooperative yield points (the reactor has no preemption — cancellation is best-effort-fast, guaranteed-eventually). Cancel keys are random per session; a wrong key is silently ignored.
- Keepalive `C_PING`/`S_PONG`; server idle-session timeout and portal-idle timeout `[OPEN: defaults]`.
- **Admin over the same protocol:** `SHOW META`, `LIST TABLES`, Waystone/WAL observability queries are ordinary statements returning ordinary result sets — one surface, one auth story. `STOP` becomes an admin statement requiring a capability bit `[PROPOSED]` instead of today's unauthenticated line command.

## 11. Error Model `[CONFIRMED]`

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

1. Rewrite `docs/client-manual.md` for KWP/1; move the newline protocol to a "debug surface" appendix.
2. `CLAUDE.md`: architecture summary line for KWP; add §… opens below to the open list.
3. Reserve the `SET DURABILITY` statement in the parser spec; wire `pattern_id` return into the Waystone workplan (touches T11/T18).
4. Client library plan (D1 consequence): reference Python client first (CLI), then the customer-facing library — separate workplan.

## 14. Open Decisions — do not assume

- TLS activation phase and mode (direct vs upgrade); SCRAM parameters.
- `kMaxFrame`, default batch size target, session/portal timeout defaults.
- `DECIMAL` wire encoding (with the engine type system); additional types.
- Compression capability; credit-based flow control capability; topology/smart-routing extension.
- Auth→authorization model (roles/permissions) — protocol only reserves the stage.

## 15. Testing Requirements

1. **Codec:** frame/payload round-trips for every type; `static_assert` offsets; fuzzed malformed frames (bad length, truncation at every byte, unknown types) — server never crashes, always `ERROR` or clean close.
2. **Handshake:** version intersection matrix; capability gating; unsupported version path.
3. **Extended flow:** pipelined PARSE/BIND/EXECUTE with mid-pipeline error ⇒ skip-to-SYNC semantics exact; named/unnamed statement lifecycles.
4. **Streaming:** suspension/CONTINUE across batch boundaries; portal-idle timeout releases pins; `max_rows=0` path.
5. **Durability semantics:** under deterministic crash injection (wal.md §16), a `S_TXN_OK(D1/D2)` acked commit always survives; D3 window bound asserted; RELAXED flag present.
6. **Cancel:** cancel during long EXECUTE interrupts at a yield point; wrong key ignored; post-cancel session state = failed-txn rules.
7. **Conformance suite:** scripted byte-level golden sessions runnable against the server under simulation — the CLI and future client libraries replay the same suite.
