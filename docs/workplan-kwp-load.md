# KWP v0 — the load endpoint (T2 over a minimal KWP substrate)

Decisions KW1-KW7; tasks KL01-KL06. **v0 carries exactly T2** — the bulk
load stream `spec-bulkinsert.md` §3 specifies — over the smallest honest
KWP substrate: framed connections, the version/capability handshake, and
the load session. The query surface (PARSE/BIND/EXECUTE, portals) stays
on the newline protocol and stays open in `docs/protocol-wp.md`; nothing
here forecloses it.

- **KW1, surface**: a second listener on `kwp_port` (config key, default
  0 = disabled — no exposure until asked for). Loopback only, like the
  text port. One frame codec (`wire/kwp.hpp`, already built), one row
  codec (`wire/row_codec.hpp`, already built) — v0 is their first caller,
  which is what CC2's "no second row format" was waiting for.
- **KW2, the type registry**: `include/kds/wire/kwp_types.hpp` mints the
  first concrete frame-type numbers - client and server as separate
  enums per the codec's standing comment, **append-only forever** (the
  sys.access_stats rule, on the wire). v0 assigns only what it uses:
  client `C_HELLO=1, C_PING=2, C_TERMINATE=3, C_LOAD_BEGIN=16,
  C_LOAD_CHUNK=17, C_LOAD_END=18, C_LOAD_ABORT=19`; server `S_HELLO=1,
  S_ERROR=2, S_COMPLETE=3, S_PONG=4, S_LOAD_READY=16, S_LOAD_ACK=17`.
  The 16+ block is the BULK_LOAD capability's, so the base block stays
  free for the query surface's spec-ordered assignment later.
- **KW3, handshake**: C_HELLO {magic 'KWP1', max/min version u16,
  capabilities u64, auth u8=NONE, name str} → S_HELLO {version=1,
  capabilities}. v0 accepts version 1 only and offers exactly
  `BULK_LOAD` (bit 0 of the 16+ block `[PROPOSED]`); a client without
  the bit gets a connection that can PING and TERMINATE and load
  nothing. Auth NONE only, loopback only — protocol.md's auth stages
  stay reserved.
- **KW4, the load session** — spec-bulkinsert §3.1 verbatim: modal
  between BEGIN and END/ABORT (only load frames + C_PING + C_TERMINATE
  admitted; anything else ERROR(PROTOCOL) and the §5 discard-to-sync
  contract collapses in v0 to: the connection's load is dead and the
  transaction unwound). S_LOAD_READY announces window=4 and
  max_chunk_bytes=256 KiB `[PROPOSED]`, and the post-pk field
  descriptors from the relation's schema. Chunks decode through
  `DecodeRowBatch`; `chunk_seq` strictly increasing; rows_accepted
  cumulative in S_LOAD_ACK.
- **KW5, execution is BI2's**: every decoded row enters the same write
  path a T1 statement uses - the T3 sorted fill when the relation is
  inside T3-2's gate, `InsertOneRow` otherwise - per chunk, inside one
  implicit transaction opened at BEGIN and committed at END (BI11).
  ABORT or connection loss rolls back. Errors carry `chunk N, row M`.
- **KW6, flow control (BI7)**: the window bounds unacked chunks;
  the server acks after a chunk is applied, so WAL backpressure
  propagates by not acking. v0 processes a chunk per reactor turn -
  suspension inside a chunk waits on the coroutine executor work.
- **KW7, honesty**: no resume/dedup (BI14), no TLS, no cancel, no
  compression - each answers ERROR(UNSUPPORTED) where reachable.

## KL01 — kwp_types.hpp registry + payload codecs for the v0 frames
## KL02 — KwpConnection: framed read loop over the reactor, handshake, modality
## KL03 — The load session: BEGIN/READY, CHUNK/ACK with the window, END/ABORT
## KL04 — Dispatcher seam: a LoadSink calling the T1/T3 write path per row/batch
## KL05 — Config (`kwp_port`) + expeditor wiring + client-manual note
## KL06 — Tests: handshake, modality, a loopback load E2E (rows land, COUNT
##          verified, T3 engaged on an eligible relation), abort unwinds,
##          window refusal on a too-big chunk, unknown frame ERROR(PROTOCOL)
