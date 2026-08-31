# KDS Wire Protocol (KWP/1) — Workplan

Work instructions, companion to `docs/spec/protocol.md` (the specification). This file holds tasks only; normative design lives in the specification, and when they disagree the specification wins — flag, don't guess.

---

## Status 2026-08-31 — the milestone is built

**P01-P16 are done; P17 was struck.** The order was `instructions/v2.7.0/kw-kwp.md`, the decisions `kw-ratification.md` (KW-D1..D6). Where a row's *shape* turned out to be wrong the spec was amended and the row is annotated below with what changed and why — nothing was quietly re-scoped.

| Row | State |
|---|---|
| P01 client manual | done — rewritten for KWP/1, newline protocol moved to Appendix A |
| P02 `CLAUDE.md` | done — milestone row and §14's opens reconciled |
| P03 `SET DURABILITY` | done, **not as an AST arm** — a routed session statement, like the two that already exist; the reason is at `HandleSetDurability` and in spec §9 |
| P04 Waystone cross-link | done — on Phase A of `waystone-workplan.md`; the T11/T18 numbers this row names no longer resolve there |
| P05 header | pre-existing; `kMaxFrame` ratified (KW-D2), the load block folded into the one registry, the duplicate hello structs removed |
| P06 frame codec | pre-existing |
| P07 handshake | done — `wire/handshake.{hpp,cpp}`, plus the `C_AUTH`/`S_AUTH` shape §3 held `[PROPOSED]` |
| P08 session | done — **`server/kwp_session.{hpp,cpp}`, not `src/wire/session.cpp`**: KW-D1 binds it to the real dispatcher, and `wire` sits below `server` |
| P09 row & type codecs | done — the batch target named, `BoundParam` and its codec added, and `EncodeValue` given the `(type_val, type_mod)` overload a fold's output needs |
| P10 portals | done — **suspension bounds delivery, not execution**; spec §7 amended rather than the build overstated |
| P11 txn & durability frames | done, less §15-5's crash-injection half, which KW-D1 assigned here and which is **owed and not run** (`known-gaps.md`) |
| P12 error registry | done — `wire/error_registry.{hpp,cpp}` and a golden list written as literals. The taxonomy needed a second half nobody had asked for: `DispatchOutcome` now carries the failing `Status`, because the only route to a category was parsing the rendered reply line, and that fold made `NotFound` and six others arrive as `INVALID_ARGUMENT` |
| P13 server integration | done — `tcp_server` gained the decoder and kept the line splitter, selected per listener; `debug_text_port` is the newline surface |
| P14 cancel | **half done**: the flag and its observation point exist and are tested; the `C_CANCEL` *connection* is not built, so the `CANCEL` **capability is not offered** — a bit advertised and then refused is worse for a client than an absent one. See the row |
| P15 reference client | done — `tools/kwp.py` and `ckdbs_cli.py` over it, with `--text` for the debug port |
| P16 conformance | first half done (golden byte sessions, `tests/testdata/kwp_golden.txt`); the socket-level half is `tests/kwp_endpoint_test.cpp` |
| P17 | **struck by KW-D1** |

### What the review changed, after the rows were done

A `critics-developer` pass over the finished change found seventeen items;
all were taken except one, and the ones that mattered are worth naming here
because three of them were **not** visible from any test that existed:

- **A shipped read answered a typed client with a text blob.** The ship wire
  carries the owner's rendered line, so on a multi-core instance one
  `SELECT` came back as a typed result set or as a diagnostic text block
  depending on which core owned the relation — invisible from the statement,
  impossible for a client to branch on. Now refused with the reason;
  `known-gaps.md` carries it against `crosscore.md`, which owns the fix.
- **Two use-after-frees on the TLS error path**, both the same shape: a
  `Connection&` used after a failed append had already closed and erased it.
- **The protocol's own settings did not survive `TcpServer`'s move**, so the
  configured *text* port came up speaking KWP and the two ports were
  indistinguishable. Pinned by a test, because the failure is invisible from
  inside either endpoint.

Rejected, with the reason: reporting the effective durability class on
`DispatchOutcome` and deleting the session's copy of §9's chain. It is a
real simplification and it is a *third* field on the outcome in a change
that already added two; the session's two-sided read is documented and
tested, and the cut is better taken on its own.

**What the milestone did not do**, stated where it will be looked for: no OUTER JOIN, no `IN (value list)`, no cursors, no sort spill, no index-served `ORDER BY`; `ALTER TABLE`, cabin, assertion and FK are no more transactional than they were; nothing is reclaimed. It does make portal suspension exist, which is the mechanism a cursor and a large result set both need.

---

Every task is startable now: where a neighboring subsystem is missing (executor, multi-core messaging), the task names the seam or fixture it builds against, and real integration is a later task. Spec `[OPEN]` items are isolated behind interfaces; no task requires deciding one.

**Note:** these tasks are numbered `P01`-`P17`, and `docs/inflight/in-progress/waystone-workplan.md` also uses `P01`-`P17`. Cite the file, not the bare number.

---

## Gate: docs & spec consistency

**P01 — Rewrite the client manual for KWP/1.**
File: `docs/spec/client-manual.md`. Replace the newline-protocol description with KWP/1: connect, handshake, extended flow, streaming, txn/durability, errors, cancel. Move the newline protocol to an appendix titled "Loopback debug surface" with its off-by-default flag documented.
Done when: no section presents the newline protocol as the client API; CLI examples use KWP.

**P02 — Refresh `CLAUDE.md`.**
Add a KWP summary line to the architecture section (custom LE binary frames, extended statements, server-side forwarding, per-txn durability field); append spec §14 opens to the open-decision list.
Done when: open list reconcilable with spec §14; summary line present.

**P03 — Reserve `SET DURABILITY` in the parser spec & AST.**
Files: parser docs + `include/kds/parser/ast.hpp`. Add a session-statement class (variant arm) for `SET DURABILITY {STRICT|GROUP|RELAXED}`; parse-only for now — execution wires in at P11.
Tests: lexer/parser round-trip for the new statement; unknown SET targets rejected.
Done when: `Statement` carries it; fingerprinting (Waystone T11) treats SET as non-pattern.

**P04 — Cross-link the Waystone workplan.**
File: `docs/inflight/in-progress/waystone-workplan.md`. Amend T11 (fingerprint is computed at KWP PARSE; `S_PARSE_OK` returns `pattern_id`) and T18 (`pattern_id`/`arg_hash` arrive via the session's plan context, not a separate plumbing path).
Done when: both tasks reference `docs/spec/protocol.md` §5.

**P05 — Protocol header `kwp.hpp`.**
File: `include/kds/wire/kwp.hpp`. Frame header constants (offsets, `kMaxFrame` as a named constexpr with `[OPEN]` note), frame-type enums (client/server, values frozen — append-only), capability bits, durability enum mirroring `docs/spec/wal.md` §1, error code taxonomy skeleton (`category u16 << 16 | detail u16`, categories mirroring `Status`), handshake payload structs (mirror-struct + offset `static_assert`s), codec function declarations. No logic.
Done when: header compiles standalone; every constant carries a derivation/consequence comment; enum values documented as frozen.

## Core implementation

**P06 — Frame codec.**
Files: `src/wire/frame_codec.cpp`, `tests/kwp_frame_test.cpp`. Encode/decode the 8-byte header + accumulate-until-complete decoder over a byte-stream interface (feed arbitrary chunk boundaries). Field-wise memcpy only.
Tests: round-trips; truncation at every byte offset ⇒ incomplete, never crash; `length > kMaxFrame` ⇒ framing error; chunk-boundary fuzz (1-byte feeds).
Needs: P05. Start immediately after the gate.

**P07 — Handshake state machine.**
Files: `src/wire/handshake.cpp`, `tests/kwp_handshake_test.cpp`. C_HELLO validation (magic, version intersection), capability intersection, S_HELLO/S_READY emission, session_id + cancel_key issuance (randomness via the injected source — rules.md §4).
Tests: version matrix (client [min,max] × server [min,max]); unknown capability bits ignored not rejected; bad magic ⇒ `ERROR(PROTOCOL)` + close.
Needs: P06.

**P08 — Session state machine (statements).**
Files: `src/wire/session.cpp`, `tests/kwp_session_test.cpp`. PARSE (→ real parser + fingerprint → `S_PARSE_OK{pattern_id}`), BIND (param decode, `arg_hash`), DESCRIBE, CLOSE, SYNC. Execution goes through a `StatementExecutor` seam — a canned-results stub until the real executor (main roadmap M1) plugs in. Implements the pipelining error contract: after `S_ERROR`, discard to next `C_SYNC`, then `S_READY(txn_state)`.
Tests: named/unnamed statement lifecycle; statement/portal count caps ⇒ `ERROR(LIMIT)`; skip-to-sync exactness under pipelined error (spec §15-3).
Needs: P06–P07; parser exists today.

**P09 — Row & type wire codecs.**
Files: `src/wire/row_codec.cpp`, `tests/kwp_row_test.cpp`. v1 type table encoders/decoders (INT*, UINT64, FLOAT64, BOOL, TEXT, BYTES, TIMESTAMP), the `{i32 len | -1=NULL}` field convention shared by params and rows, `S_ROW_DESC` / `S_ROW_BATCH` builders with the ≤64 KiB batch target behind a constant.
Tests: per-type round-trips incl. NULL; batch splitting at the size target; ~~DECIMAL explicitly absent with a failing-by-design guard referencing the `[OPEN]`~~ — the `[OPEN]` was decided 2026-08-07 (§6: unscaled int64 LE, scale in `S_ROW_DESC.type_mod`) and the row codec already implements it with round-trip and layout pins (`tests/wire_row_codec_test.cpp`); the guard now covers `FLOAT64`, which remains specified-but-unstorable.
Needs: P05. Parallel with P07–P08. Note the row-value half of this task predates it: `include/kds/wire/row_codec.hpp` was built 2026-08-05 as a crosscore prerequisite, so P09's remaining scope is the ≤64 KiB batch-splitting policy and the param decode, not the codec.

**P10 — Portals & streaming.**
Files: extend session + `tests/kwp_stream_test.cpp`. Portal registry, `max_rows` accounting, `S_PORTAL_SUSPENDED` / `C_CONTINUE`, portal-idle timeout via the injected clock (default behind a constant, `[OPEN]`), pin-release note enforced: destroying/timing out a portal releases its executor cursor through the seam.
Tests: suspension across batch boundaries; CONTINUE with changed max_rows; timeout fires only on the injected clock; `max_rows=0` unlimited path (spec §15-4).
Needs: P08–P09.

**P11 — Transaction & durability frames.**
Files: extend session + `tests/kwp_txn_test.cpp`. `C_TXN_BEGIN{durability}` / COMMIT / ABORT / `S_TXN_OK`, autocommit wrapping, failed-txn gating (only ABORT/SYNC accepted), `SET DURABILITY` execution (P03), RELAXED flag on D3 acks. Ack timing goes through a `TxnController` seam — stub now records the required WAL ack class per commit; the real controller arrives with WAL/MVCC (wal.md, roadmap M2–M3) and the stub's recorded expectations become the integration assertions.
Tests: durability field plumb-through; failed-txn statement rejection; autocommit vs explicit; RELAXED flag presence (spec §15-5 becomes fully real at WAL integration — the crash-injection half is deferred to that task and noted here).
Needs: P08, P03.

**P12 — Error registry.**
Files: `src/wire/error_registry.cpp`, `tests/kwp_error_test.cpp`. Status→`(code, retryable)` mapping table, append-only with a static check (a golden list test that fails on renumbering), `S_ERROR` builder with optional detail/position.
Tests: every `Status` category maps; retryable bits match the spec's guidance table; golden-list renumber guard.
Needs: P05. Parallel-friendly.

**P13 — Server integration.**
Files: `src/server/tcp_server.cpp`, `src/server/kwp_endpoint.cpp`, tests. Replace line splitting with the P06 decoder on the KWP port; per-connection preallocated frame buffers (steady-state no allocation); the newline dispatcher moved behind `--debug-text-port` (default off); byte-stream harness so the whole endpoint runs under deterministic simulation with scripted input.
Tests: golden byte sessions end-to-end against the stub executor; malformed-stream fuzz at the socket layer; debug port off by default.
Needs: P06–P12.

**P14 — Cancel path.** *(half done 2026-08-31.)* The session-side half is built and tested: `KwpSession::RequestCancel` sets the flag, the session observes it **at its next frame** — the reactor has no preemption, so §10's "best-effort-fast, guaranteed-eventually" is exactly what a cooperative point can give — and the cancelled statement is refused with `CANCELLED` and poisons an open transaction. **The `C_CANCEL` connection is not built.** It needs a listener-wide registry keyed on `session_id`, which is a piece of the endpoint rather than of the session, and it needs the `cancel_key` to be unguessable — which on a build without OpenSSL it is not, so the `CANCEL` capability is negotiated away there rather than advertised. Both halves are named in `known-gaps.md`. Original row:


Files: extend endpoint/session + `tests/kwp_cancel_test.cpp`. `C_CANCEL{session_id, cancel_key}` on a fresh connection sets the target session's cancel flag; the flag is observed at task yield points via a `CancelToken` seam the executor stub polls; wrong key silently ignored; post-cancel state = failed-txn rules.
Tests: cancel mid-stream interrupts at the next yield; wrong-key no-op; cancel of idle session is harmless (spec §15-6).
Needs: P08, P13.

**P15 — Reference client (CLI rewrite).**
Files: `tools/ckdbs_cli.py` (KWP implementation), keep a `--text` escape hatch for the debug port. The client is also the conformance driver: it can dump/replay golden sessions.
Tests: CLI-driven smoke against the P13 endpoint in CI.
Needs: P13.

**P16 — Conformance suite.**
Files: `tests/kwp_conformance/` golden sessions (byte-exact request/response scripts) + runner wired into `test.sh`. Every later protocol change must update goldens explicitly — accidental wire drift fails CI.
Needs: P13, P15. Regression-mandatory thereafter.

**P17 — WAL/executor integration hooks.** *(seam closure; completes with M1–M3)*
Swap the `StatementExecutor` stub for the real execution path and the `TxnController` stub for the WAL-backed controller; run spec §15-5's crash-injection half (acked D1/D2 commits survive; D3 window bound). The stubs' recorded expectations from P08/P11 are the acceptance checklist.
Needs: P11, P13; main roadmap M1–M3.

## Standing instructions

- Frame-type and error-code values are frozen append-only from P05 onward; the P12/P16 golden guards enforce it.
- No allocation on the per-frame steady-state path; all randomness, clocks, and I/O via injected seams; the endpoint must run fully under deterministic simulation.
- Conformance suite (P16) runs in every CI pass once it exists.
- Update spec + this workplan together when an `[OPEN]` lands; move it into the spec body with the date.
