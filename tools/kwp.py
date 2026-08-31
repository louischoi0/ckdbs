"""KWP/1 - the reference client (docs/spec/protocol.md, protocol-wp.md P15).

The protocol the server speaks on its default port since the cut-over
(KW-D6): length-prefixed binary frames, a version and capability handshake,
and PARSE / BIND / EXECUTE over server-side statement and portal handles.

---- What this module is for ------------------------------------------------

Two things, and the second is why it renders text at all.

1. It is the **reference client**: one implementation of the frame codec,
   the row codec and the session flow, which `ckdbs_cli.py` drives and which
   the conformance work replays.

2. It is the **compatibility shim for this repository's own harnesses.**
   Thirty-odd benchmark and scenario drivers import `ServerConnection` from
   `ckdbs_cli` and call `send_command(line) -> str`, reading the newline
   protocol's reply shape - a header line of column names, then one
   `"\\n"`-escaped comma row per match. `Connection.send_command` here
   returns that **same string**, rendered client-side from typed rows.

   That is not a compatibility hack bolted on: spec §6 says "No text result
   mode exists. Human-readable rendering is a client concern (the CLI
   renders) - a `DATE`'s epoch day and a `DECIMAL`'s unscaled integer
   included." Rendering here is the protocol working as designed. What the
   shim adds is that the rendering matches `exec::FormatValue` byte for
   byte, so a benchmark's numbers before and after the cut are comparable.

---- What it does not do ----------------------------------------------------

No TLS (connect to a plaintext port), no SCRAM (`auth = off`), no cancel
connection, no compression - the last two because the server does not offer
them by default either (KW-D5 deferred compression; the `CANCEL` capability
is withheld unless the server has a real identity source).
"""

import socket
import struct

MAGIC = 0x3150574B  # 'KWP1' little-endian
VERSION = 1

# Frame types - include/kds/wire/kwp.hpp, one registry per direction.
C_HELLO, C_PARSE, C_BIND, C_EXECUTE, C_CONTINUE = 1, 2, 3, 4, 5
C_DESCRIBE, C_CLOSE, C_SYNC = 6, 7, 8
C_TXN_BEGIN, C_TXN_COMMIT, C_TXN_ABORT = 9, 10, 11
C_PING, C_CANCEL, C_TERMINATE, C_AUTH = 12, 13, 14, 15

S_HELLO, S_READY, S_PARSE_OK, S_BIND_OK = 1, 2, 3, 4
S_ROW_DESC, S_ROW_BATCH, S_PORTAL_SUSPENDED, S_COMPLETE = 5, 6, 7, 8
S_TXN_OK, S_ERROR, S_PONG, S_NOTICE, S_AUTH = 9, 10, 11, 12, 13

# Column type_val, shared with the catalog (catalog/well_known.hpp): the
# wire's type_oid *is* the engine's type_val, one numbering with two
# readers.
T_INT8, T_INT16, T_INT32, T_INT64, T_UINT64 = 1, 2, 3, 4, 5
T_FLOAT, T_DECIMAL, T_BOOL, T_VARCHAR, T_CHAR = 6, 7, 8, 9, 10
T_DATE, T_TIMESTAMP, T_DECIMAL_WIDE = 11, 12, 13

# Field flags (wire/row_codec.hpp).
FIELD_KEYSTONE = 0x1
FIELD_DIAGNOSTIC_LINE = 0x2

# Error categories (kwp.hpp), for the four spellings a client switches on.
CAT_TXN_CONFLICT, CAT_UNKNOWN_OUTCOME = 10, 14
CAT_NOT_IMPLEMENTED = 15
CAT_FK_VIOLATION, CAT_ASSERTION_VIOLATION = 16, 17


class KwpError(Exception):
    """A refusal the server framed as `S_ERROR`."""

    def __init__(self, code, retryable, severity, message, detail, position):
        super().__init__(message)
        self.code = code
        self.category = code >> 16
        self.detail_code = code & 0xFFFF
        self.retryable = retryable
        self.severity = severity
        self.message = message
        self.detail = detail
        self.position = position

    def as_reply_line(self):
        """The newline protocol's spelling of the same refusal.

        `CommandDispatcher::ErrorReply` is the original, and the four codes
        it gives a token to are the ones a client library switches on. Every
        other code renders bare there, so it renders bare here - which makes
        the two surfaces answer alike for a harness that only looks at the
        prefix.
        """
        tokens = {
            CAT_TXN_CONFLICT: "TXN_CONFLICT retryable=1 ",
            CAT_FK_VIOLATION: "FK_VIOLATION retryable=0 ",
            CAT_ASSERTION_VIOLATION: "ASSERTION_VIOLATION retryable=0 ",
            CAT_UNKNOWN_OUTCOME: "UNKNOWN_OUTCOME retryable=0 ",
            CAT_NOT_IMPLEMENTED: "NOT_IMPLEMENTED retryable=0 ",
        }
        return "ERR " + tokens.get(self.category, "") + self.message


# ---- Payload readers and writers -------------------------------------------
#
# The two string shapes protocol.md §2 defines: `Str` is `{u16 len, bytes}`
# for names, `Text` is `{u32 len, bytes}` for content, with 0xFFFFFFFF as an
# absent `Text`.


class _Writer:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v):
        self.b += struct.pack("<B", v)
        return self

    def u16(self, v):
        self.b += struct.pack("<H", v)
        return self

    def u32(self, v):
        self.b += struct.pack("<I", v)
        return self

    def u64(self, v):
        self.b += struct.pack("<Q", v)
        return self

    def s(self, text):
        raw = text.encode("utf-8")
        return self.u16(len(raw)).raw(raw)

    def text(self, value):
        raw = value.encode("utf-8")
        return self.u32(len(raw)).raw(raw)

    def raw(self, data):
        self.b += data
        return self

    def take(self):
        return bytes(self.b)


class _Reader:
    def __init__(self, data):
        self.d = data
        self.at = 0

    def _take(self, n):
        if self.at + n > len(self.d):
            raise KwpError(0, False, 2, "truncated payload", None, None)
        out = self.d[self.at:self.at + n]
        self.at += n
        return out

    def u8(self):
        return struct.unpack("<B", self._take(1))[0]

    def u16(self):
        return struct.unpack("<H", self._take(2))[0]

    def u32(self):
        return struct.unpack("<I", self._take(4))[0]

    def i32(self):
        return struct.unpack("<i", self._take(4))[0]

    def u64(self):
        return struct.unpack("<Q", self._take(8))[0]

    def s(self):
        return self._take(self.u16()).decode("utf-8", errors="replace")

    def text(self):
        n = self.u32()
        if n == 0xFFFFFFFF:
            return None
        return self._take(n).decode("utf-8", errors="replace")

    def rest(self):
        out = self.d[self.at:]
        self.at = len(self.d)
        return out


def frame(ftype, payload=b"", flags=0):
    """`{length u32, type u8, flags u8, reserved u16, payload}`, where
    `length` counts everything after itself."""
    return struct.pack("<IBBH", 4 + len(payload), ftype, flags, 0) + payload


# ---- Value rendering -------------------------------------------------------
#
# **The inverse of `exec::FormatValue`, and it has to be exact.** Every
# benchmark in `bench/` was measured against the newline protocol's replies,
# so a value that renders differently here makes a before/after comparison a
# comparison of two things.


def _civil_from_days(z):
    """days-since-1970-01-01 -> (y, m, d). Howard Hinnant's algorithm, the
    same one `src/exec/type_literals.cpp` uses."""
    z += 719468
    era = (z if z >= 0 else z - 146096) // 146097
    doe = z - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    y = yoe + era * 400
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    d = doy - (153 * mp + 2) // 5 + 1
    m = mp + 3 if mp < 10 else mp - 9
    return (y + (1 if m <= 2 else 0), m, d)


def format_date(epoch_day):
    y, m, d = _civil_from_days(epoch_day)
    return "%04d-%02d-%02d" % (y, m, d)


def format_timestamp(epoch_micros):
    day, rest = divmod(epoch_micros, 86400 * 1000000)  # Python floors, as C++ does here
    micros = rest % 1000000
    seconds = rest // 1000000
    out = "%s %02d:%02d:%02d" % (format_date(day), seconds // 3600,
                                 (seconds // 60) % 60, seconds % 60)
    if micros:
        out += ".%06d" % micros
    return out


def format_decimal(unscaled, scale):
    negative = unscaled < 0
    digits = str(-unscaled if negative else unscaled)
    if scale == 0:
        return ("-" if negative else "") + digits
    if len(digits) <= scale:
        digits = "0" * (scale + 1 - len(digits)) + digits
    digits = digits[:len(digits) - scale] + "." + digits[len(digits) - scale:]
    return ("-" if negative else "") + digits


def decimal_scale_of(type_mod):
    """`catalog::DecimalScaleOf`: the scale is the low byte of the packed
    (p, s) word the catalog stores and `S_ROW_DESC.type_mod` carries."""
    return type_mod & 0xFF


def render_value(field, raw):
    """One field as `exec::FormatValue` would render it. `raw` is None for
    SQL NULL - the `-1` length the format reserves for it."""
    if raw is None:
        return "NULL"
    oid = field["type_oid"]
    if oid in (T_INT8, T_INT16, T_INT32, T_INT64):
        return str(int.from_bytes(raw, "little", signed=True))
    if oid == T_UINT64:
        return str(int.from_bytes(raw, "little", signed=False))
    if oid == T_BOOL:
        return str(int.from_bytes(raw, "little", signed=False))
    if oid == T_DATE:
        return format_date(int.from_bytes(raw, "little", signed=True))
    if oid == T_TIMESTAMP:
        return format_timestamp(int.from_bytes(raw, "little", signed=True))
    if oid in (T_DECIMAL, T_DECIMAL_WIDE):
        return format_decimal(int.from_bytes(raw, "little", signed=True),
                              decimal_scale_of(field["type_mod"]))
    # varchar, char, and anything this build does not know: the bytes as
    # they came, which for an unknown type is the honest answer - the
    # length prefix is always present, so a client can carry a value it
    # cannot interpret.
    return raw.decode("utf-8", errors="replace")


# ---- The connection --------------------------------------------------------


class Connection:
    """One KWP/1 session over one TCP connection."""

    def __init__(self, host, port, timeout=5.0, client_name="ckdbs-cli"):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        # **TCP_NODELAY, and it is not a micro-optimisation.** A statement is
        # several small frames followed by a read, and with Nagle on, the
        # second write waits for the first one's ACK - which the server
        # delays, because it has nothing to say until it has seen the whole
        # batch. Measured on this client before the option was set: 42 ms
        # per statement and 23 qps, against tens of microseconds of engine
        # time. The server sets it on every accepted socket for the same
        # reason (`tcp_server.cpp`); a client that does not set it puts the
        # stall on itself.
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.session_id = 0
        self.cancel_key = 0
        self.capabilities = 0
        self.server_info = ""
        self._next_portal = 0
        self._handshake(client_name)

    # -- framing ------------------------------------------------------------

    def _send(self, ftype, payload=b"", flags=0):
        self.sock.sendall(frame(ftype, payload, flags))

    def _recv_frame(self):
        while len(self.buf) < 8:
            self._fill()
        (length,) = struct.unpack("<I", self.buf[:4])
        while len(self.buf) < 4 + length:
            self._fill()
        ftype = self.buf[4]
        flags = self.buf[5]
        payload = self.buf[8:4 + length]
        self.buf = self.buf[4 + length:]
        return ftype, flags, payload

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise ConnectionError("server closed the connection")
        self.buf += chunk

    @staticmethod
    def _error(payload):
        r = _Reader(payload)
        code = r.u32()
        retryable = r.u8() != 0
        severity = r.u8()
        message = r.text()
        detail = r.text()
        position = r.u32()
        return KwpError(code, retryable, severity, message, detail,
                        None if position == 0xFFFFFFFF else position)

    # -- handshake ----------------------------------------------------------

    def _handshake(self, client_name):
        w = _Writer().u32(MAGIC).u16(VERSION).u16(VERSION).u64(0).u8(0).s(client_name)
        self._send(C_HELLO, w.take())
        ftype, _, payload = self._recv_frame()
        if ftype == S_ERROR:
            raise self._error(payload)
        if ftype != S_HELLO:
            raise ConnectionError("expected S_HELLO, got frame type %d" % ftype)
        r = _Reader(payload)
        version = r.u16()
        if version != VERSION:
            raise ConnectionError("server chose KWP version %d" % version)
        self.capabilities = r.u64()
        self.session_id = r.u64()
        self.cancel_key = r.u64()
        self.server_info = r.s()
        ftype, _, _ = self._recv_frame()
        if ftype != S_READY:
            raise ConnectionError("expected S_READY, got frame type %d" % ftype)

    # -- statements ---------------------------------------------------------

    def execute(self, sql, params=None, max_rows=0):
        """Runs one statement. Returns `(fields, rows, tag, rows_affected)`.

        `fields` is empty for a statement with no result set, and `rows`
        holds raw field bytes (None for NULL) - rendering is `render_value`'s
        and stays a client concern (§6).
        """
        portal = "p%d" % self._next_portal
        self._next_portal += 1
        b = _Writer().s(portal).s("")
        params = params or []
        b.u16(len(params))
        for type_oid, type_mod, value in params:
            b.u32(type_oid).u32(type_mod)
            if value is None:
                b.u32(0xFFFFFFFF)
            else:
                b.u32(len(value)).raw(value)

        # **One write, three frames.** Pipelining is what §5 offers and this
        # is the shape it is for: PARSE, BIND, EXECUTE and the `C_SYNC`
        # barrier go out together, and the client then reads to `S_READY`
        # whichever way the statement went. Sending them separately costs a
        # syscall each and, on a connection without TCP_NODELAY, a delayed
        # ACK between them.
        #
        # **`C_CLOSE` never rides in this batch** (2026-08-31, XE4 -
        # measured, not theorised: this was tried and leaked). §5's own
        # skip-to-sync is why: a `C_CLOSE` queued right after a `PARSE`,
        # `BIND` or `EXECUTE` that itself errors arrives while the server
        # is already discarding frames up to the next `C_SYNC`
        # (`kwp_session.cpp`'s `skipping_to_sync_`, "discarded, silently")
        # - so a same-batch close is dropped on exactly the statements that
        # most need it closed, and the portal leaks anyway. The trailing
        # close below, sent as its own frame *after* this batch's `S_READY`
        # has already been read, always lands on a session that has left
        # skip-to-sync - the one guarantee `C_SYNC` gives.
        batch = frame(C_PARSE, _Writer().s("").text(sql).take())
        batch += frame(C_BIND, b.take())
        batch += frame(C_EXECUTE, _Writer().s(portal).u32(max_rows).take())
        batch += frame(C_SYNC)
        self.sock.sendall(batch)

        fields, rows, tag, affected = [], [], "", 0
        error = None
        while True:
            ftype, _flags, payload = self._recv_frame()
            if ftype == S_PARSE_OK or ftype == S_BIND_OK:
                continue
            if ftype == S_ROW_DESC:
                fields = self._row_desc(payload)
                continue
            if ftype == S_ROW_BATCH:
                rows.extend(self._row_batch(payload, len(fields)))
                continue
            if ftype == S_PORTAL_SUSPENDED:
                self._send(C_CONTINUE, _Writer().s(portal).u32(max_rows).take())
                continue
            if ftype == S_NOTICE:
                continue
            if ftype == S_COMPLETE:
                r = _Reader(payload)
                tag = r.text()
                affected = r.u64()
                break
            if ftype == S_ERROR:
                error = self._error(payload)
                break
            raise ConnectionError("unexpected server frame type %d" % ftype)

        # The `C_SYNC` barrier went out with the batch above, so what is
        # left is to read to it. **Always**, not only after an error: after
        # an `S_ERROR` the server discards frames until it sees one, so a
        # client that skipped the read would meet that `S_READY` in front of
        # its next statement's answer.
        while True:
            ftype, _, _ = self._recv_frame()
            if ftype == S_READY:
                break
        # **Always closed, unconditionally, as its own frame.** Not "on
        # both arms" any more - the arm that bundled `C_CLOSE` into the
        # first batch was the leaking one (see the comment above the
        # batch), so there is now exactly one way this method closes a
        # portal: after this statement's own `S_READY`, whether it carried
        # a success or an `S_ERROR`.
        self._send(C_CLOSE, _Writer().u8(2).s(portal).take())
        self._send(C_SYNC)
        while True:
            ftype, _, _ = self._recv_frame()
            if ftype == S_READY:
                break
        if error is not None:
            raise error
        return fields, rows, tag, affected

    @staticmethod
    def _row_desc(payload):
        r = _Reader(payload)
        count = r.u16()
        fields = []
        for _ in range(count):
            name = r.s()
            type_oid = r.u32()
            type_len = struct.unpack("<h", r._take(2))[0]
            flags = r.u16()
            type_mod = r.u32()
            fields.append({"name": name, "type_oid": type_oid, "type_len": type_len,
                           "flags": flags, "type_mod": type_mod})
        return fields

    @staticmethod
    def _row_batch(payload, field_count):
        r = _Reader(payload)
        count = r.u16()
        rows = []
        for _ in range(count):
            row = []
            for _ in range(field_count):
                n = r.i32()
                row.append(None if n == -1 else r._take(n))
            rows.append(row)
        return rows

    # -- transactions -------------------------------------------------------

    def begin(self, durability=0):
        self._send(C_TXN_BEGIN, _Writer().u8(durability).take())
        return self._await_txn_ok()

    def commit(self):
        self._send(C_TXN_COMMIT)
        return self._await_txn_ok()

    def rollback(self):
        self._send(C_TXN_ABORT)
        return self._await_txn_ok()

    def _await_txn_ok(self):
        ftype, flags, payload = self._recv_frame()
        if ftype == S_ERROR:
            # **The same barrier `execute` runs.** A refused transaction
            # frame arms the server's skip-to-sync posture like any other
            # refusal (§5), so a client that raised without syncing left
            # every later statement being discarded - and then blocked on a
            # reply that was never coming.
            error = self._error(payload)
            self._send(C_SYNC)
            while True:
                ftype, _, _ = self._recv_frame()
                if ftype == S_READY:
                    break
            raise error
        if ftype != S_TXN_OK:
            raise ConnectionError("expected S_TXN_OK, got frame type %d" % ftype)
        return bool(flags & 0x1)  # RELAXED: D3's ack semantics (§9)

    def ping(self):
        self._send(C_PING)
        ftype, _, _ = self._recv_frame()
        return ftype == S_PONG

    def close(self):
        try:
            self._send(C_TERMINATE)
        except OSError:
            pass
        self.sock.close()
