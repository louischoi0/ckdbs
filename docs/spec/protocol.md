# KDS Wire Protocol (KWP/1)

The protocol KDS speaks: length-prefixed binary frames, a version and capability handshake, and an extended PARSE/BIND/EXECUTE statement model over server-side statement and portal handles. Replaces the newline text protocol in `docs/spec/client-manual.md`, which survives as an off-by-default loopback debug surface (§12). Companion workplan: `docs/inflight/in-progress/protocol-wp.md`. `[PROPOSED]` marks a default to confirm or amend before the affected part is built; `[OPEN]` must not be assumed. Consistent with `docs/rules/rules.md`, `docs/spec/sched.md`, `docs/spec/wal.md`, `docs/spec/waystone-concpets.md`.

**Status: built and spoken, 2026-08-31** (milestone KW, `instructions/v2.7.0/kw-kwp.md`). The default port speaks KWP/1: handshake, `C_AUTH` exchange, PARSE/BIND/EXECUTE over server-side statement and portal handles, typed row batches, portal suspension, transaction and durability frames, the structured error registry. The newline text protocol survives as `debug_text_port`'s loopback debug surface (§12), off unless configured.

Where the build and this document disagreed, **this document was amended rather than the build overstated**; every such amendment is marked *(amended 2026-08-31)* at the point it applies, and the three that change what a client sees are §5's parameter types, §7's meaning of portal suspension, and §10's `STOP`.

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
| 0 | 4 | `length` | u32 LE; byte count of everything after this field (type..payload). Sanity ceiling `kMaxFrame` = **16 MiB, ratified as KW-D2** |
| 4 | 1 | `type` | frame type (§4) |
| 5 | 1 | `flags` | per-type; 0 unless specified |
| 6 | 2 | `reserved` | 0; receivers ignore |
| 8 | … | `payload` | type-specific, binary LE |

Codec rules follow rules.md §2/§5: field-wise memcpy helpers, `static_assert`ed offsets, fixed-width integers only, no bitfields. Malformed frames (bad length, unknown type) produce an `ERROR` frame and, on framing-level corruption where resync is impossible, connection close — never a crash, never silent skip.

**Two string shapes, and the rule that picks one** *(amended 2026-08-31: this line said `{u32 len, bytes}` for every variable-length field, which no frame has ever done — the v0 hello and the row description were `u16` from the day they were written)*:

- **`Str` = `{u16 len, bytes}` — names.** A relation, a column, a statement or portal handle, a client's telemetry string. All short by construction, and the row description carries one per field, which makes this the byte the protocol sends most.
- **`Text` = `{u32 len, bytes}` — content.** SQL statement text, an error message, an error's detail. These have no natural bound short of `kMaxFrame`, and a 64 KiB ceiling on a statement would be a limit invented by a length field. `0xFFFFFFFF` is an *absent* `Text`, the `{i32 len | -1 = NULL}` convention §6 uses for values, spelled for a payload field.

Strings are UTF-8, not NUL-terminated, in both shapes.

## 3. Handshake & Versioning

First frames on a connection, before anything else:

1. **`C_HELLO`** (client): `magic u32 = 'KWP1'`, `max_version u16`, `min_version u16`, `capabilities u64` (bitset: `STREAMING`, `CANCEL`, `TLS_REQUIRED`, `COMPRESSION[OPEN]`, …), `auth_method u8` (`NONE` or `SCRAM_SHA256`; `MTLS` reserved and refused by name), client name/version string (telemetry only).

   **A bit is offered when the frame behind it is answered, and not before.** This build offers `STREAMING` and `BULK_LOAD`; it does **not** offer `CANCEL`, because `C_CANCEL` has no handler (P14's remainder), and a server that offered it would refuse the one frame it had just advertised. `TLS_REQUIRED` is never offered by a server at all — it is a demand a *client* makes.
2. **`S_HELLO`** (server): chosen `version u16`, `capabilities u64` (intersection), `session_id u64`, `cancel_key u64` (§10), server version string.
3. **The auth sub-frames** — *`[PROPOSED shape]` proposed and built 2026-08-31 (P07)*. When `auth_method` is `SCRAM_SHA256`, **`S_HELLO` is withheld** and the exchange runs first, one SCRAM message per frame: `C_AUTH {body Text}` → `S_AUTH {body Text}`, repeated until the gate admits or refuses. The body is **the exact line `server/auth.hpp`'s gate reads and writes** (`AUTH SCRAM-SHA-256 <client-first>`, `AUTH+ <server-first>`, …), which is §14's "KWP will carry these same SCRAM message bodies in handshake frames; only this line framing is protocol-specific" built rather than restated: one gate serves both surfaces, and a KWP client's transcript is byte-comparable with a text client's. `S_HELLO` and `S_READY` follow the gate's success, and **not before** — an unauthenticated connection must not learn a `session_id` or a `cancel_key`, which are the whole of §10's cancel authorization. A `NONE` method proceeds directly.
   Two refusals, both fatal, both in the same direction — the party that asked for less security never gets it silently: a server that requires authentication refuses `NONE`, and a client that offered SCRAM is refused by a server with no credential store rather than admitted unauthenticated.
3. **`S_READY`**: server is ready for statements. Also sent after every `C_SYNC` (§5) — it is the protocol's quiescent-point marker, carrying `txn_state u8` (idle / in-txn / failed-txn).

Version negotiation failure ⇒ `ERROR(UNSUPPORTED_VERSION)` + close. Capability bits gate optional behavior so features land without version breaks; version bumps are reserved for frame-format changes.

## 4. Frame Catalog

Client → server: `C_HELLO`, `C_PARSE`, `C_BIND`, `C_EXECUTE`, `C_CONTINUE`, `C_DESCRIBE`, `C_CLOSE`, `C_SYNC`, `C_TXN_BEGIN`, `C_TXN_COMMIT`, `C_TXN_ABORT`, `C_PING`, `C_CANCEL` (cancel connections only, §10), `C_TERMINATE`, `C_AUTH` (§3).

Server → client: `S_HELLO`, `S_READY`, `S_PARSE_OK`, `S_BIND_OK`, `S_ROW_DESC`, `S_ROW_BATCH`, `S_PORTAL_SUSPENDED`, `S_COMPLETE`, `S_TXN_OK`, `S_ERROR`, `S_PONG`, `S_NOTICE` (non-fatal server messages), `S_AUTH` (§3).

**One registry per direction** *(unified 2026-08-31)*. `include/kds/wire/kwp.hpp` holds both enums and they are the only numbering: the v0 bulk-load endpoint kept a second pair that collided with this catalog on five values, and the load frames moved onto this numbering at the 16+ block they already held. A `type` byte means one thing whichever endpoint receives it.

Unknown frame types: server responds `ERROR(PROTOCOL)`; clients must treat unknown *server* frame types as fatal unless a negotiated capability declared them.

## 5. Extended Statement Model

PG-shaped phases, KDS semantics:

- **`C_PARSE`** `{stmt_name str, sql str}` → server parses to the `Statement` AST and — this is the Waystone tie-in — computes the **query-template fingerprint at parse time**. `S_PARSE_OK` returns `{pattern_id u64}` (informational; clients may log it, never interpret it). Named statements are session-scoped and survive until `C_CLOSE` or disconnect; the unnamed statement (`""`) is overwritten by the next PARSE.
- **`C_BIND`** `{portal_name Str, stmt_name Str, param_count u16, params: [{type_oid u32, type_mod u32, i32 len | -1=NULL, bytes}]}` — parameters are binary LE per the type table (§6). `S_BIND_OK`.

  **A parameter carries its own type, and this line did not say so** *(amended 2026-08-31, P09)*. The frame as originally specified was `{i32 len, bytes}` per parameter and nothing else, which cannot be decoded: eight bytes are an `int64`, a `TIMESTAMP`, an unscaled `DECIMAL` or eight bytes of text, and the server has no way to choose. Every value travelling *outward* is typed by `S_ROW_DESC`; this is the same fact travelling inward. `type_mod` rides along for the reason it does on a field description — a decimal's unscaled integer means nothing without its scale.

  **Binding is by literal substitution into the statement text**, and that is a design statement rather than an implementation note. A parameter's rendered literal replaces the corresponding `?`, and the dispatcher runs the result. The alternative — binding values into a parsed statement — needs the parser to accept `?` in value positions and the compiler to carry a parameter vector, which is `parser-v2.md`'s work.

  It is also what this engine's fingerprint design already assumes: `?` and a literal emit the **same** shape marker (`waystone-workplan.md` P01, "a bind parameter's type is unknown at parse, so distinguishing int-shaped from string-shaped holes would break convergence"), so the `pattern_id` of the `?` form and of the substituted form are equal by construction, and the `arg_hash` differs per parameter set exactly as this section wants. `KwpSessionTest` asserts that equality rather than assuming it.

  **One value cannot be bound**, and it is the grammar's limit rather than the protocol's: a `TEXT`/`CHAR`/`VARCHAR` parameter containing an ASCII single quote. This engine's lexer has no escape for a quote inside a string literal, so the value has no spelling any SQL text this server accepts can carry — through KWP or through the newline protocol. Refused at `C_BIND` with that reason; registered in `known-gaps.md` against `parser-v2.md`, which owns the string-literal grammar.
- **`C_DESCRIBE`** `{kind u8 stmt|portal, name Str}` → `S_ROW_DESC`, **for an executed portal only** *(amended 2026-08-31)*. Describing a *statement* is refused with `Unsupported` and the reason: this engine resolves a result's shape by compiling the statement against a catalog and a snapshot, which happens at execution — so the shape of an unexecuted statement is not knowable without running it, and running it to answer a describe would make a describe a side effect. A client that needs the shape executes and reads `S_ROW_DESC`.
- **`C_EXECUTE`** `{portal_name str, max_rows u32}` — results per §7; `max_rows = 0` means unlimited.
- **`C_SYNC`** — pipeline barrier. Clients may pipeline PARSE/BIND/EXECUTE without waiting; after any `S_ERROR`, the server **discards frames until the next `C_SYNC`**, then answers `S_READY(failed-txn or idle)`. This skip-to-sync rule is the whole pipelining error contract.

## 6. Data Encoding

- `S_ROW_DESC`: `{field_count u16, fields: [{name str, type_oid u32, type_len i16 (-1=varlen), flags u16, type_mod u32}]}`. Field 0 of every user relation is the Keystone-derived `id` (u64). `type_mod` is zero for every type except `DECIMAL`, where it carries the column's packed `(p, s)` — precision in the high byte of the low half, scale in the low byte, **the same word the catalog stores** (`catalog::PackDecimalLen`), so there is one packing with two readers.
- Row values: `{i32 len | -1 = NULL, bytes}` per field — one NULL convention everywhere (params and rows).
- v1 type wire formats: `INT8/16/32/64` (LE two's complement), `UINT64` (Keystone ids), `FLOAT64` (IEEE 754 LE), `BOOL` (1 byte), `TEXT` (UTF-8), `BYTES`, `DECIMAL` (**decided 2026-08-07, with the type system as the `[OPEN]` required**: the unscaled **int64 LE, 8 bytes** — exactly the integer storage holds — with the scale in `S_ROW_DESC.type_mod`, once per result set and never per value; a per-value scale could only ever agree with the column or be a defect. The `[OPEN]`'s "scaled-int128 vs string" resolves as *scaled-int at the type's width*: `p > 18` is a future **separate** type per `types.md` TY2, which will carry its own type_oid and a 16-byte width, so nothing is foreclosed — and string is rejected because per-value text on an all-binary protocol reintroduces a parse step and the two-readings drift the type system removed), `DATE` (i32 epoch days since 1970-01-01), `TIMESTAMP` (i64 micros since epoch, UTC — **confirmed with `types.md` TY4**, which fixed storage to the same encoding), `DECIMAL128` (**the reserved separate type, realized 2026-08-07** — `types.md` §2a: type_oid 13, the int128 unscaled value in 16 LE bytes, low half first, `(p, s)` in `type_mod` exactly as the 8-byte type carries it).
- No text result mode exists. Human-readable rendering is a client concern (the CLI renders) — a `DATE`'s epoch day and a `DECIMAL`'s unscaled integer included.
- **Status: the row encoding above is implemented** — `include/kds/wire/row_codec.hpp` (2026-08-05; `DECIMAL`/`DATE`/`TIMESTAMP` arms and `type_mod` 2026-08-07). It is deliberately below both consumers: `docs/spec/crosscore.md` CC2 requires cross-core `STEP_BATCH` payloads in this same encoding, so the encoder knows about neither frames nor cores. `FLOAT64` is specified above but not implemented — nothing can store one, and the encoder refuses what storage refuses. A decimal value whose scale disagrees with its column is refused at encode, never rescaled — the same rule the storage codec applies.

## 7. Result Streaming

- `C_EXECUTE` produces `S_ROW_DESC` (unless suppressed by flags after a DESCRIBE) then a sequence of **`S_ROW_BATCH`** frames: `{row_count u16, rows…}`, batch size server-chosen, target `kRowBatchTargetBytes` = **64 KiB, ratified with KW-D2**. A *target*, not a ceiling: a batch is sealed once appending the next row would carry it past this, so one row wider than the target still ships whole — the alternative is a row nothing can send.
- If `max_rows > 0` and the portal has more rows when the quota is reached, the server sends **`S_PORTAL_SUSPENDED {portal_name Str}`**; the client resumes with `C_CONTINUE {portal_name Str, max_rows u32}`. This is the flow-control mechanism — explicit, deterministic, and testable, in place of TCP-buffer guesswork. Credit/window schemes stay `[OPEN]` behind a capability bit if ever needed.
- Completion: `S_COMPLETE {tag Text, rows_affected u64}`.

**What suspension means in v1, and what it does not** *(amended 2026-08-31; the previous reactor note said "a suspended portal is a suspended foreground task holding pins", and it is not one)*. This engine has no suspension point at a row boundary: a walk holds a page pin across every row of a page, and the one place a statement parks is a page boundary under the cross-core gate. So **`max_rows` bounds delivery, not execution** — the statement runs whole into the portal's buffered batches, and `max_rows` decides how many rows leave now. Three consequences, stated rather than discovered:

- **A portal ceases to exist when its statement fails** *(amended
  2026-09-01, XG-R8)*. The server closes it on the error path, where it
  already has the portal in hand. So a `C_DESCRIBE` or `C_CONTINUE` naming
  it afterwards answers "no such portal", and a later `C_CLOSE` is the
  no-op it already is on an absent name.

  This is a lifecycle statement and not an optimisation, so it is written
  here rather than left to be inferred. **What it fixes**: §5's skip-to-sync
  discards every frame up to the next `C_SYNC`, including a `C_CLOSE` a
  client pipelined behind the statement that just failed — which is exactly
  when the close matters. The portal leaked, and at `kMaxSessionPortals`
  (64) every further `C_BIND` was refused until the 60 s portal-idle sweep
  freed one, so a retrying client ran at one statement per portal
  lifetime. Clients that worked around it by sending `C_CLOSE` as its own
  frame after `S_READY` may stop; the workaround cost a round trip on every
  successful statement.

  Reaching 64 portals now means a client genuinely holds 64, which is a
  client defect: the refusal stays `RESOURCE_EXHAUSTED` with
  `retryable = 0`, and `IsRetryable` stays one code wide.

- A suspended portal holds **memory**, not pins. That is not a regression: the newline protocol already materialised every result set into one reply string, so the bytes were always spent — a portal spends them in a better shape and releases them when it closes.
- The portal-idle timeout (§10) therefore bounds memory rather than pins, which is why its refusal is `RESOURCE_EXHAUSTED` and not a protocol error.
- A **true** cursor is reachable without redesigning this: the cross-core `RemoteStepServer` already streams a walk under credit and parks it at the page boundary, which is exactly the mechanism a row-bounded `C_EXECUTE` needs. A later task, not a different design.

**A statement with no result set answers `S_COMPLETE` alone**, its `tag` being the reply the engine rendered (`INSERTED oid=… id=…`, `CREATED oid=…`). Where that reply carries *lines* rather than a tag — `SHOW`, `DESCRIBE`, `ANALYZE`, `TRACE` — it arrives instead as a **one-column `varchar` result set, one row per line**, flagged `kFieldFlagDiagnosticLine` in `S_ROW_DESC`. §10 calls these "ordinary statements returning ordinary result sets", and one column of text is the ordinary result set an answer the engine has never typed can honestly be; the alternative was a bespoke typed shape per diagnostic, which is a second model of every one of them. The flag rather than a reserved column name, because a column *can* be called `line`.

## 8. Cross-Core Execution — Server-Side Forwarding

- A connection is owned by the core that accepted it; its session state (statements, portals, txn) lives on that core (rules.md §3).
- When a statement targets data owned by another core, the owning-core work is dispatched over the cross-core message interface and results return to the session core, which frames them to the client. **Clients never see topology**; no routing hints exist in KWP v1.
- This choice keeps clients simple at the cost of a forwarding hop; a future smart-routing extension (topology frame + session migration) is `[OPEN]` and must arrive as a capability bit, not a version break.
- While the engine runs single-core (current state), forwarding is trivially absent; the protocol is unaffected.

## 9. Transactions & Durability

- Autocommit by default: a lone EXECUTE is its own transaction.
- `C_TXN_BEGIN {durability u8}` / `C_TXN_COMMIT` / `C_TXN_ABORT` → `S_TXN_OK`. `durability` ∈ {0 = session default, 1 = D1 strict, 2 = D2 group, 3 = D3 relaxed} per `docs/spec/wal.md` §1 — **confirmed 2026-08-31**: 1/2/3 are `wal::DurabilityClass`'s own values, so the wire value *is* the engine value.
- **`SET DURABILITY {STRICT|GROUP|RELAXED}` is built** *(`[PROPOSED]` resolved 2026-08-31, P03)*, and it is a session statement the dispatcher routes exactly as `SET ISOLATION LEVEL` is — **not** a `parser::Statement` arm. Both of the engine's existing session statements are routed that way and neither reaches the parser; adding one to the AST would put a third in a second place, so the engine would hold two models of what a session statement is. P03's own acceptance survives intact: "fingerprinting treats SET as non-pattern" is satisfied by a statement the fingerprinter never sees.
- **Three rungs, one chain** (`Session::EffectiveDurability`): the server's configured class, then `SET DURABILITY` for this connection, then `BEGIN … DURABILITY <class>` — or `C_TXN_BEGIN`'s field, which is the same rung reached over a frame — for one transaction. Either `BEGIN` clause may be written in either order with `ISOLATION LEVEL`.
- `S_TXN_OK` for COMMIT is sent only after the WAL ack point of the chosen class (wal.md §8-2). For D3 the reply carries `flags.RELAXED=1` so audit logs can distinguish ack semantics — and the flag names the class the commit **used**, read through the chain above rather than off the byte `C_TXN_BEGIN` carried, which may have said "session default".
- Failed-txn state: after an in-txn error, only ABORT (and SYNC) are accepted until rollback — mirrored in `S_READY.txn_state`.

## 10. Session, Cancel & Ops

- Session state: named statements, portals, txn, durability default. All dropped on disconnect; server may cap statement/portal counts (`ERROR(LIMIT)` beyond).
- **Cancel:** out-of-band — a new connection sends `C_CANCEL {session_id, cancel_key}` and closes. The server sets a cancel flag the target task observes at its cooperative yield points (the reactor has no preemption — cancellation is best-effort-fast, guaranteed-eventually). Cancel keys are random per session; a wrong key is silently ignored.
- Keepalive `C_PING`/`S_PONG`; server idle-session timeout and portal-idle timeout `[OPEN: defaults]`.
- **Admin over the same protocol:** `SHOW META`, `LIST TABLES`, Waystone/WAL observability queries are ordinary statements returning ordinary result sets (§7's one-column form).

  **"One surface, one auth story" is not true at the end of milestone KW**, and this line is amended rather than left as a claim the build does not meet *(KW-D4, 2026-08-31)*. `STOP` as a capability-gated admin statement was **deferred**: it stays today's unauthenticated line command, and since the cut-over made the newline protocol `debug_text_port`'s, `STOP` is now reachable **only on the debug port**. A deployment that stops its server by connecting and typing `STOP` needs that port open until the deferral is taken up. `STOP` sent as an ordinary *statement* over KWP works and is admin-classed like every other unclassified command — what is deferred is the capability bit, not the statement.
- **Portal-idle timeout: 60 s** *(`[OPEN: default]` resolved as KW-D3, 2026-08-31)*, on the injected clock, behind `kPortalIdleTimeoutNs`. The derivation is a reuse rather than a new number: this tree already spends 60 s on the two waits whose event is human-or-network-scale (`kIndexBuildReplyDeadlineNs`, `kAssertionBuildReplyDeadlineNs`) against 10 s for the machine-scale ones, and an idle portal waits on a client. **Defensible, not measured** — no workload has ever held a portal open in this engine, because portals did not exist; P16's conformance run is the first place a real number could come from.
- **Session caps: 64 named statements and 64 portals**, per session. Beyond either, `RESOURCE_EXHAUSTED` with its own detail code. Per session rather than per server, so the cap bounds one connection's state; what bounds the server is the connection count.

## 11. Error Model

`S_ERROR` payload: `{code u32, retryable u8, severity u8, message Text, detail Text?, position u32?}` — the two optionals encoded as `0xFFFFFFFF`, because byte 0 is a real position and `""` is a real (if useless) detail. `severity` ∈ {1 = error, the connection stands; 2 = fatal, it is closed after this frame} — exactly the distinction the last bullet below draws, and no more: a third value meaning "warning" would be an `S_NOTICE`, which is its own frame precisely so a non-fatal remark is not an error with a softer adjective.

- `code = category u16 << 16 | detail u16`; categories mirror engine `Status` (InvalidArgument, NotFound, AlreadyExists, OutOfSpace, OutOfRange, Corruption, IoError, Internal, Protocol, Unsupported, **NotImplemented**, TxnConflict, Cancelled, CardinalityViolation, ResourceExhausted, UnknownOutcome, **FkViolation**, **AssertionViolation**). Detail codes are append-only — never renumber. `tests/kwp_error_test.cpp` is the golden list, written as literals so a renumber has to edit it, and editing it is the moment someone decides whether every deployed client is renumbered too.
- **`Unsupported` and `NotImplemented` are two categories, not one** (operator rule, 2026-08-31). `Unsupported` is a form this engine's architecture cannot admit — an invariant (a pk `UPDATE`), a fixed structure's ceiling (the fan-in's stage cap, a shipped reply's one slot), a format (two decimals of different width are different types), or a protocol's shape (a `$name` bind on a wire with no bind step). `NotImplemented` is a form the design admits and nobody has built: outer joins, CTEs, `UNIQUE` indexes, `ALTER TABLE`'s data-moving verbs, every seam whose owner-side half exists and whose peer-side half does not. **The test is whether a later release could lift the refusal without changing the architecture.** The distinction is for feature detection: a client seeing `NotImplemented` may usefully ask a newer server, and one seeing `Unsupported` must rewrite. Neither is retryable, and within one process both are equally permanent — the difference is across releases, never across retries.
- **The category is the engine's own, carried and not re-parsed** *(2026-08-31)*. `DispatchOutcome::status` holds the failing `Status`, so `NotFound`, `Unsupported`, `NotImplemented`, `Corruption`, `OutOfRange`, `OutOfSpace`, `IoError`, `ResourceExhausted` and `CardinalityViolation` all reach a client as themselves. They did not at first: the only route to a category was `StatusFromErrorReply` parsing the rendered reply line, which recovers exactly the spellings `ErrorReply`'s table gives a token to and folds the rest into `InvalidArgument` — right for the cross-core path it was written for, where a rendered line is genuinely all there is, and a floor rather than the answer at this seam.
- **A new engine code earns a category rather than being folded into `InvalidArgument`.** FK and assertion violations were appended on 2026-08-31 on that rule and on a fact the newline protocol had already established: `ErrorReply` gives each of them a token a client switches on, so folding either here would have made the binary protocol *less* discriminating than the text one it replaces.
- **`detail` narrows a category; it never replaces one.** A client that understands only the category must still act correctly, so every detail value refines an answer that is already right without it. The named ones are `Protocol`'s (malformed frame, unknown frame type, bad magic, unsupported version, unexpected frame, unknown statement, unknown portal, malformed payload) and `ResourceExhausted`'s (statement limit, portal limit, portal idle timeout) — the protocol layer is the only place where the client's own next action differs *within* one category.
- `retryable` is authoritative client guidance (e.g. TxnConflict = 1, InvalidArgument = 0); financial client libraries build retry loops on this bit, so it is part of the compatibility surface. **It is never written by hand**: it comes from `kds::IsRetryable`, the engine's own spelling of the same fact, so the two cannot drift. The wire-only categories, which have no `Status`, carry it explicitly *because* they have no engine answer to defer to.
- Errors never close the connection except framing-level corruption (§2) and handshake failures.

## 12. Server Implementation Notes

- `tcp_server` gained the frame decoder **and kept the line splitter**, selected per listener by `set_protocol` *(built 2026-08-31, P13)*. One socket layer with two framings, not two servers: the accept, the TLS channel, the outbox, the write-interest bookkeeping and the async dispatch are the same code either way. The alternative — a second listener class — would have been the third copy of that file's syscall handling.
- **The state machine is `include/kds/server/kwp_session.hpp`, not `src/wire/session.cpp`** as the workplan placed it. KW-D1 binds it to the real `CommandDispatcher`, and `kds::wire` sits *below* `kds::server` (the row codec is in `wire` precisely so the server and the cross-core path can both use it), so a dispatcher-dependent state machine in `wire` would invert that arrow. What stays in `wire` is what has no engine dependency: the frame codec, the handshake, the error registry.
- **The engine's result rows reach the protocol through one seam**, `server/result_sink.hpp`. The dispatcher had exactly one output form — the newline protocol's comma-joined text — built inline at four emission points (the local walk, the sorted drain, the aggregate fold, the cross-core fan-in), each carrying a comment warning that a second formatter would drift. KWP is that second form, so the four sites call a sink instead: the text form is one implementation and the wire form is the other, and there is no third copy to forget a `type_val`. The sink lives on the `Session` and not on the dispatcher, because a statement can *park* and another connection's statement then runs on the same dispatcher.
- `debug_text_port` is the newline protocol's loopback debug surface; 0, the default, opens no socket.
- Frame parse/serialize buffers are preallocated per connection; steady-state no allocation per the reactor rules.
- All socket I/O stays on the injected `IoBackend`/reactor path — the protocol state machine must run under deterministic simulation with scripted byte streams (that is how §14's tests exist).
- `tools/ckdbs_cli.py` is the KWP reference client *(built 2026-08-31, P15)*, over `tools/kwp.py`, with a `--text` escape hatch for the debug port. Its `ServerConnection.send_command(line) -> str` renders the answer client-side into **the newline protocol's reply shape**, which is what let thirty-odd drivers under `tools/` move in one change and what keeps `bench/`'s numbers comparable across the cut. Rendering client-side is §6 working as designed, not a shim bolted on; what the shim adds is that the rendering matches `exec::FormatValue` byte for byte.

## 13. Required Amendments & Follow-ups

1. ~~Rewrite `docs/spec/client-manual.md` for KWP/1; move the newline protocol to a "debug surface" appendix.~~ **Done 2026-08-31** (P01).
2. ~~`CLAUDE.md`: architecture summary line for KWP; add §… opens below to the open list.~~ **Done 2026-08-31** (P02).
3. ~~Reserve the `SET DURABILITY` statement in the parser spec; wire `pattern_id` return into the Waystone workplan (touches T11/T18).~~ **Done 2026-08-31** (P03, P04). `SET DURABILITY` is a routed session statement rather than an AST arm — the reason is in §9 — and the Waystone cross-link is on that workplan's Phase A, since the T-numbers it named no longer resolve.
4. Client library plan (D1 consequence): reference Python client first (CLI), then the customer-facing library — separate workplan.

## 14. Open Decisions — do not assume

- ~~TLS activation phase and mode (direct vs upgrade)~~ — **decided 2026-08-13**: direct TLS at the transport seam, active now under the `tls` config key; see §1.
- ~~SCRAM parameters~~ — **decided and built 2026-08-13**: SCRAM-SHA-256 (RFC 5802/7677), server and client state machines in `src/server/scram.cpp`, active on the text protocol under `auth = scram` (an `AUTH` line exchange, `docs/spec/client-manual.md`) and carried into KWP P07 as the same message bodies in handshake frames. Parameters: PBKDF2 iterations **fixed at 4096 in v1** (the RFC floor; a `--iterations` flag was removed on review because the unknown-user mock always answers `i=4096`, so any raised verifier was a one-round-trip enumeration oracle through `i=` — raising the count is tied to teaching the mock the deployment's own number), salt 16 uniform bytes from the one producer `scram::RandomSalt`, both ends bounding `i=` to 4096..10,000,000, verifiers stored as RFC 5803-shaped strings in a flat users file behind a `CredentialStore` interface. `auth = scram` refuses an open `kwp_port` — the load endpoint has no auth stage until P07. Deliberately deferred, each stated in `scram.hpp`: channel binding (SCRAM-PLUS), SASLprep (usernames restricted to `[A-Za-z0-9_.-]` at provisioning instead), cross-connection mock-salt consistency for unknown users, and a pre-auth deadline (the pre-auth inbox is capped at 4 KiB; a timer is not yet available to bound the clock).
- ~~`kMaxFrame`, default batch size target, session/portal timeout defaults~~ — **decided 2026-08-31**, KW-D2 and KW-D3: `kMaxFrame` = 16 MiB (§2), the batch target = 64 KiB (§7), the portal-idle timeout = 60 s (§10), the session caps = 64 statements and 64 portals (§10). The **idle-session** timeout is *not* among them and stays open: nothing bounds how long an authenticated connection may sit with no statement and no portal, which is a resource question rather than a protocol one.
- **`STOP` as a capability-gated admin statement** — deferred by KW-D4 (§10); the consequence for the debug port is recorded there.
- **`COMPRESSION` capability bit** — deferred by KW-D5. Excluded from v1 and *not* a version break to add later, which is what the capability bit is for; the bit stays reserved in the enum and the server does not offer it.
- ~~`DECIMAL` wire encoding (with the engine type system)~~ — **decided 2026-08-07** with the type system built, exactly as this line required; see §6. Additional types stay open.
- Compression capability; credit-based flow control capability; topology/smart-routing extension.
- ~~Auth→authorization model (roles/permissions)~~ — **decided and built 2026-08-13: statement-class roles.** Three ranks ordered by inclusion — `readonly` (reads, SHOW/DESCRIBE/ANALYZE, transaction control, own `SET ISOLATION`), `readwrite` (+ INSERT/UPDATE/DELETE), `admin` (everything: DDL, STOP, SYNC, server-wide SET) — held per user in the users file's new role column (`<user> <role> <verifier>`, `--add-user --role`, default readonly), stamped onto the session by the auth gate, checked once per statement at the dispatcher's routing tokens (`RequiredRole`, `src/server/command_dispatcher.cpp`). **Unclassified commands require admin** — refused by default, never admitted by omission. With `auth = off` every session is admin (an unauthenticated instance is the operator's own process). Two boundaries stated so nobody overreads them: **`readonly` bounds statement classes, not page writes** — a readonly SELECT still records Waystone trails, access statistics and pattern rows, exactly as invariants 8/9 permit; and **`SYNC` is admin's** because it forces device-wide I/O — a client's durability guarantee is the durability class's job (per-transaction in KWP), never a statement a tenant may issue. Ranks, not grant sets, on purpose: **per-relation GRANT/REVOKE remains the future refinement**, slotting under the same check once the catalog is recovered (RV3) — ACLs must not live in the one subsystem a crash silently loses. Role changes are re-provisioning (delete-then-add); no runtime GRANT surface exists.

## 15. Testing Requirements

1. **Codec:** frame/payload round-trips for every type; `static_assert` offsets; fuzzed malformed frames (bad length, truncation at every byte, unknown types) — server never crashes, always `ERROR` or clean close.
2. **Handshake:** version intersection matrix; capability gating; unsupported version path.
3. **Extended flow:** pipelined PARSE/BIND/EXECUTE with mid-pipeline error ⇒ skip-to-SYNC semantics exact; named/unnamed statement lifecycles.
4. **Streaming:** suspension/CONTINUE across batch boundaries; portal-idle timeout releases pins; `max_rows=0` path.
5. **Durability semantics:** under deterministic crash injection (wal.md §16), a `S_TXN_OK(D1/D2)` acked commit always survives; D3 window bound asserted; RELAXED flag present. **Partly done, and the gap is named** *(2026-08-31)*: the RELAXED flag and the class plumbing are asserted by `KwpSessionTest`; the **crash-injection half is not run**. It was P17's, P17 was struck by KW-D1, and KW-D1 assigns the checklist to P11 — so it is owed by this milestone and is recorded unpaid in `known-gaps.md`.
6. **Cancel:** cancel during long EXECUTE interrupts at a yield point; wrong key ignored; post-cancel session state = failed-txn rules.
7. **Conformance suite:** scripted byte-level golden sessions runnable against the server under simulation — the CLI and future client libraries replay the same suite.
