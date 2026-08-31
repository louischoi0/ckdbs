# Milestone KW — KWP/1, the wire protocol

Drafted 2026-08-31 against `main` at `c8e3d31` (`v2.2.1-153-gc8e3d31`).

Spec: `docs/spec/protocol.md`. Task rows:
`docs/inflight/in-progress/protocol-wp.md`, numbered P01-P17 — cite the
file, not the bare number, because `waystone-workplan.md` reuses the
same range.

Decisions: `kw-d-ratification.md` (KW-D1 through KW-D6, operator
2026-08-31). **Read that first** — it strikes P17, reorders the rows,
and names two constants.

This order does not re-derive the workplan. It records what has changed
since the workplan was written and states the risk the row list does not
carry.

## Why this milestone

`protocol.md` §5's status line, verbatim: *"only the frame codec exists
and **nothing calls it**. The server speaks the newline text protocol
today."*

The spec is complete. TLS landed 2026-08-13 — direct TLS 1.3 at the
transport seam, below the protocol, so it wraps whichever protocol the
port speaks. SCRAM landed the same day. What is missing is the protocol:
no PARSE/BIND/EXECUTE, no server-side statement or portal handles, no
bound parameters, no portal suspension, no chunked row batches.

Nothing in it is gated on an R-series decision, on the mover, or on
reclamation.

## What is already built, so no row rebuilds it

| Row | State |
|---|---|
| P05 header | built — `include/kds/wire/kwp.hpp`, `kwp_types.hpp` |
| P06 frame codec | built — `src/wire/frame_codec.cpp`, `tests/kwp_frame_test.cpp` |
| P09 row codec | **half** — `row_codec.hpp` built 2026-08-05 as a crosscore prerequisite; DECIMAL and DECIMAL128 decided and pinned (`tests/wire_row_codec_test.cpp`). Remaining: ≤64 KiB batch-splitting policy, param decode |

`tests/kwp_load_server_test.cpp` and
`include/kds/server/kwp_load_server.hpp` exist and reach a socket.
**Read them before scoping P13** — they may already be part of the
endpoint that row describes, or a load fixture that only resembles one.
The workplan predates them.

Unbuilt: P07 handshake, P08 session, P10 portals, P11 transaction and
durability frames, P12 error registry, P13 server integration, P14
cancel, P15 CLI, P16 conformance. P17 is struck by KW-D1.

## Sequence

Per KW-D6, which moves P16's golden-session half ahead of the cut:

| | Rows |
|---|---|
| 1 | P01-P04 — docs, `SET DURABILITY` in the AST, Waystone cross-link |
| 2 | P07, P09-remainder, P12 — independent of each other |
| 3 | P08, P10, P11 — bound to the real engine per KW-D1 |
| 4 | **P16 first half** — golden byte-sessions in-process, before any port moves |
| 5 | P13 — the cut |
| 6 | P14, P15, P16 second half — regression-mandatory thereafter |

## Three things the rows were written before

**The parser exists and is v2.** P03 (`SET DURABILITY` as a session
statement) and P08 (PARSE → real parser + fingerprint) both assumed a
parser that has since changed. Re-read `parser-v2.md`'s three `[OPEN]`
items — statement-class ratification, slot-table cap, whether
`kUnclassified` is production-legal — before P03. A session statement is
a new statement class, and the first of those items owns whether that
needs ratifying.

**Per-transaction durability is specified but unreachable.**
`known-gaps.md` records the durability class as KWP/1-only. P11 makes it
reachable; it is not new surface, it is surface built without a caller.

**`FLOAT64` is on the wire and unstorable.** P09's guard covers it
deliberately. Do not resolve that here — it is `types.md`'s.

## The risk the row list does not carry

P13's line is short: *"the newline dispatcher moved behind
`--debug-text-port` (default off)"*. What it does not say is that every
test, every benchmark harness and `tools/ckdbs_cli.py` speak the newline
protocol today. KW-D6 directs a single cut-over, so all of them move in
one change.

**The paired benchmark reading is mandatory**, not advised. Every number
in `bench/` is a newline-protocol number, and `42fca65` has just made
the baseline this repository's own last run. Take one scenario before
and after the cut, named with `git describe --tags`. If the protocol
moves it, every prior baseline is a baseline for a different client
surface and the results files must say so.

## What this milestone does not do

It does not add OUTER JOIN, `IN (value list)`, cursors, sort spill, or
index-served `ORDER BY`. It does not make `ALTER TABLE`, cabin, pattern,
assertion or FK transactional. It does not reclaim anything.

It does make portal suspension exist, which is the mechanism a cursor
and a large result set both need — so it is the row those later surfaces
are built on, not a detour around them.
