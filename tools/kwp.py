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

No cancel connection and no compression, because the server does not offer
them by default either (KW-D5 deferred compression; the `CANCEL` capability
is withheld unless the server has a real identity source).

---- Authentication and TLS (protocol.md 1, 3) --------------------------------

`Connection(..., user=, password=)` runs SCRAM-SHA-256 in `C_AUTH` frames
before `S_HELLO`; `Connection(..., tls=<ssl.SSLContext>)` wraps the socket
before its first byte, since the server's TLS is direct. Three rules, each
a way a client can look authenticated and not be:

- **The server's signature is verified.** SCRAM is mutual; a client that
  sends its proof and skips the server's final message has proved itself to
  whoever answered.
- **No channel binding** (gs2 header `n,,`): the server offers none
  (`scram.hpp`), so over an unverified TLS channel SCRAM does not notice a
  relay. `tls_context` verifies the certificate and the name unless told
  not to.
- **A password is never an argument.** `connection_options` reads
  `KDS_PASSWORD`, else prompts on a terminal, else refuses.
"""

import base64
import getpass
import hashlib
import hmac
import os
import socket
import ssl
import struct
import sys

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
CAT_TRANSITION_VIOLATION = 18
CAT_READ_ONLY_TRANSACTION = 19

# `C_HELLO.auth_method` and the capability a client inside TLS demands
# (wire/kwp_types.hpp, wire/kwp.hpp).
AUTH_NONE, AUTH_SCRAM_SHA256 = 0, 1
CAP_TLS_REQUIRED = 1 << 2

# Both ends bound the server's `i=` (scram.hpp): below the RFC's floor is a
# downgrade, above the ceiling is a server asking the client to burn CPU.
SCRAM_MIN_ITERATIONS, SCRAM_MAX_ITERATIONS = 4096, 10_000_000


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
            CAT_TRANSITION_VIOLATION: "TRANSITION_VIOLATION retryable=0 ",
            CAT_READ_ONLY_TRANSACTION: "READ_ONLY_TRANSACTION retryable=0 ",
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


_U16 = struct.Struct("<H")
_I32 = struct.Struct("<i")


def frame(ftype, payload=b"", flags=0):
    """`{length u32, type u8, flags u8, reserved u16, payload}`, where
    `length` counts everything after itself."""
    return struct.pack("<IBBH", 4 + len(payload), ftype, flags, 0) + payload


# `Connection.query`'s BIND / EXECUTE / CLOSE / SYNC, over a portal named "q".
_QUERY_TAIL = (frame(C_BIND, _Writer().s("q").s("").u16(0).take())
               + frame(C_EXECUTE, _Writer().s("q").u32(0).take())
               + frame(C_CLOSE, _Writer().u8(2).s("q").take())
               + frame(C_SYNC))


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


def renderer_for(field):
    """The function `render_value` applies to one field's non-NULL bytes,
    resolved once. A result set's columns keep their type for every row, so
    a caller rendering many rows pays the type dispatch per column, not per
    cell (bench/v3.3.0/results-row-cost-attribution-v3.3.0-37-gf829964c.md)."""
    from_bytes = int.from_bytes
    oid = field["type_oid"]
    if oid in (T_INT8, T_INT16, T_INT32, T_INT64):
        return lambda raw: str(from_bytes(raw, "little", signed=True))
    if oid in (T_UINT64, T_BOOL):
        return lambda raw: str(from_bytes(raw, "little", signed=False))
    if oid == T_DATE:
        return lambda raw: format_date(from_bytes(raw, "little", signed=True))
    if oid == T_TIMESTAMP:
        return lambda raw: format_timestamp(from_bytes(raw, "little", signed=True))
    if oid in (T_DECIMAL, T_DECIMAL_WIDE):
        scale = decimal_scale_of(field["type_mod"])
        return lambda raw: format_decimal(from_bytes(raw, "little", signed=True), scale)
    # varchar, char, and anything this build does not know: the bytes as
    # they came, which for an unknown type is the honest answer - the
    # length prefix is always present, so a client can carry a value it
    # cannot interpret.
    return lambda raw: raw.decode("utf-8", errors="replace")


def render_value(field, raw):
    """One field as `exec::FormatValue` would render it. `raw` is None for
    SQL NULL - the `-1` length the format reserves for it."""
    return "NULL" if raw is None else renderer_for(field)(raw)


# ---- Connection options, one spelling for every tool -------------------------


def tls_context(cafile=None, insecure=False):
    """The client's TLS context: 1.3 only, as the server speaks nothing
    older (protocol.md 1). The certificate and its name are verified against
    `cafile`, or the system's CAs, unless `insecure`."""
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    if insecure:
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
    elif cafile is not None:
        context.load_verify_locations(cafile=cafile)
    else:
        context.load_default_certs()
    return context


def add_connection_arguments(parser):
    """`--user`, `--tls`, `--tls-ca`, `--tls-insecure`. No `--password`: an
    argument is readable in the process table."""
    parser.add_argument("--user", help="authenticate as this user (SCRAM-SHA-256); the password "
                                       "is read from KDS_PASSWORD, else prompted for")
    parser.add_argument("--tls", action="store_true",
                        help="connect with TLS 1.3 (a `tls = on` port)")
    parser.add_argument("--tls-ca", metavar="FILE",
                        help="verify the server against this CA or self-signed certificate "
                             "(implies --tls)")
    parser.add_argument("--tls-insecure", action="store_true",
                        help="do not verify the server's certificate or name (implies --tls)")


def connection_options(args, environ=None, prompt=getpass.getpass):
    """The `user` / `password` / `tls` keywords `Connection` takes, from the
    arguments above."""
    environ = os.environ if environ is None else environ
    options = {}
    if args.tls_ca is not None and args.tls_insecure:
        raise SystemExit("--tls-ca and --tls-insecure contradict each other")
    if args.user is not None:
        password = environ.get("KDS_PASSWORD")
        if password is None:
            if not sys.stdin.isatty():
                raise SystemExit("--user needs a password: set KDS_PASSWORD, or run on a terminal")
            password = prompt("password for %s: " % args.user)
        options["user"], options["password"] = args.user, password
    if args.tls or args.tls_ca is not None or args.tls_insecure:
        if args.tls_insecure:
            print("warning: --tls-insecure: the server's certificate is not verified",
                  file=sys.stderr)
        options["tls"] = tls_context(args.tls_ca, args.tls_insecure)
    return options


# ---- The connection --------------------------------------------------------


class Connection:
    """One KWP/1 session over one TCP connection."""

    def __init__(self, host, port, timeout=5.0, client_name="ckdbs-cli",
                 user=None, password=None, tls=None):
        """`user`/`password` authenticate by SCRAM-SHA-256; `tls` is an
        `ssl.SSLContext` (see `tls_context`) for a `tls = on` port."""
        if (user is None) != (password is None):
            raise ValueError("user and password are given together or not at all")
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
        self._renderers = {}     # row description bytes -> its renderers (`fetch`)
        try:
            if tls is not None:
                # Direct TLS: the first byte on the wire is the ClientHello
                # (protocol.md 1). Wrapped after TCP_NODELAY, which is the
                # underlying socket's option either way.
                self.sock = tls.wrap_socket(self.sock, server_hostname=host)
            self._handshake(client_name, user, password, tls is not None)
        except BaseException:
            self.sock.close()
            raise

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

    def _handshake(self, client_name, user=None, password=None, inside_tls=False):
        capabilities = CAP_TLS_REQUIRED if inside_tls else 0
        method = AUTH_NONE if user is None else AUTH_SCRAM_SHA256
        w = _Writer().u32(MAGIC).u16(VERSION).u16(VERSION).u64(capabilities).u8(method)
        self._send(C_HELLO, w.s(client_name).take())
        if user is not None:
            self._scram(user, password)
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

    def _auth_round(self, line):
        """One SCRAM message out, one back: the gate's own lines, a frame
        each (protocol.md 3). Answers the server's message, its `AUTH+ `
        prefix removed."""
        self._send(C_AUTH, _Writer().text(line).take())
        ftype, _, payload = self._recv_frame()
        if ftype == S_ERROR:
            raise self._error(payload)
        if ftype != S_AUTH:
            raise ConnectionError("expected S_AUTH, got frame type %d" % ftype)
        body = _Reader(payload).text()
        if body is not None and body.startswith("ERR "):
            # The gate's one refusal for every failure (auth.cpp `Refuse`):
            # a wrong password, an unknown user and a malformed line alike.
            raise ConnectionError("the server refused authentication: " + body[len("ERR "):])
        if body is None or not body.startswith("AUTH+ "):
            raise ConnectionError("the server's authentication message is not an AUTH+ line")
        return body[len("AUTH+ "):]

    def _scram(self, user, password):
        """SCRAM-SHA-256 (RFC 5802, RFC 7677), the client's half. Returns
        only once the server has proved it holds the verifier too."""
        nonce = base64.b64encode(os.urandom(18)).decode("ascii")
        # RFC 5802 5.1: `=` and `,` in a name are written `=3D` and `=2C`.
        # The server provisions names from [A-Za-z0-9_.-] and has no
        # SASLprep, so nothing else is prepared here either.
        name = user.replace("=", "=3D").replace(",", "=2C")
        first_bare = "n=%s,r=%s" % (name, nonce)
        server_first = self._auth_round("AUTH SCRAM-SHA-256 n,," + first_bare)

        # Positional, as RFC 5802 7's grammar and scram.cpp's Client read it:
        # `r=`, `s=`, `i=` first, in that order, so a leading `m=` (a
        # mandatory extension, which must fail) or a repeated attribute is
        # malformed rather than silently resolved; anything after `i=` is
        # an extension, ignored.
        fields = server_first.split(",")
        if (len(fields) < 3 or not fields[0].startswith("r=") or not fields[1].startswith("s=")
                or not fields[2].startswith("i=")):
            raise ConnectionError("malformed SCRAM server-first message")
        combined, iterations = fields[0][2:], fields[2][2:]
        if not (iterations.isascii() and iterations.isdigit()):
            raise ConnectionError("malformed SCRAM server-first message")
        rounds = int(iterations)
        try:
            salt = base64.b64decode(fields[1][2:], validate=True)
        except ValueError as e:
            raise ConnectionError("malformed SCRAM server-first message") from e
        if not combined.startswith(nonce) or len(combined) == len(nonce):
            raise ConnectionError("the server's SCRAM nonce does not extend the client's")
        if not SCRAM_MIN_ITERATIONS <= rounds <= SCRAM_MAX_ITERATIONS:
            raise ConnectionError("the server's SCRAM iteration count %d is outside %d..%d"
                                  % (rounds, SCRAM_MIN_ITERATIONS, SCRAM_MAX_ITERATIONS))

        def mac(key, text):
            return hmac.new(key, text, hashlib.sha256).digest()

        salted = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, rounds)
        client_key = mac(salted, b"Client Key")
        # `biws` is base64("n,,"): the gs2 header again, no channel binding.
        without_proof = "c=biws,r=" + combined
        auth_message = (first_bare + "," + server_first + "," + without_proof).encode("utf-8")
        signature = mac(hashlib.sha256(client_key).digest(), auth_message)
        proof = bytes(a ^ b for a, b in zip(client_key, signature))
        server_final = self._auth_round(
            "AUTH " + without_proof + ",p=" + base64.b64encode(proof).decode("ascii"))

        if server_final.startswith("e="):
            raise ConnectionError("the server refused authentication: " + server_final[2:])
        expected = "v=" + base64.b64encode(
            mac(mac(salted, b"Server Key"), auth_message)).decode("ascii")
        if not hmac.compare_digest(server_final.encode("utf-8"), expected.encode("ascii")):
            raise ConnectionError("the server did not prove it holds this user's verifier; "
                                  "the connection is not trusted")

    # -- statements ---------------------------------------------------------

    def execute(self, sql, params=None, max_rows=0, statement=None):
        """Runs one statement. Returns `(fields, rows, tag, rows_affected)`.

        `fields` is empty for a statement with no result set, and `rows`
        holds raw field bytes (None for NULL) - rendering is `render_value`'s
        and stays a client concern (§6). With `statement`, `sql` is ignored
        and the named statement a `prepare` parsed is bound instead.
        """
        portal = "p%d" % self._next_portal
        self._next_portal += 1
        b = _Writer().s(portal).s(statement or "")
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
        # **`C_CLOSE` rides in this batch again** (2026-09-01, XG2).
        #
        # It was pulled out on 2026-08-31 because §5's skip-to-sync eats a
        # `C_CLOSE` queued behind a `PARSE`/`BIND`/`EXECUTE` that errors -
        # so a same-batch close was dropped on exactly the statements that
        # most needed it and the portal leaked. That was a real defect and
        # the workaround was the right call at the time; it cost a round
        # trip on every *successful* statement, which is a tax every driver
        # written against this server would have paid forever for a server
        # behaviour.
        #
        # **The server closes it now** (XG-R8): a statement error erases the
        # portal it was executing, so the close being eaten no longer leaks
        # anything, and `protocol.md` §7 states that lifecycle for every
        # client rather than leaving it to be discovered. A `C_CLOSE` in
        # this batch is therefore honoured on the success path and harmless
        # on the error path, where the name it addresses is already gone -
        # `portals_.erase` on an absent name is silent.
        #
        # One write, four frames, one round trip.
        batch = b"" if statement else frame(C_PARSE, _Writer().s("").text(sql).take())
        batch += frame(C_BIND, b.take())
        batch += frame(C_EXECUTE, _Writer().s(portal).u32(max_rows).take())
        batch += frame(C_CLOSE, _Writer().u8(2).s(portal).take())
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
        if error is not None:
            raise error
        return fields, rows, tag, affected

    def query(self, sql):
        """Runs one statement and returns `(tag, rows, payload_bytes, error)`,
        the shape `pg_wire.PgConnection.query` returns: rows are counted and
        their field lengths walked, but no field becomes a Python object.

        The benchmark path. `execute` builds a list per row and a bytes
        object per field, and `send_command` renders every field to text; a
        timer around either charges KDS for client work the PostgreSQL
        twin's `query` does not do, which at 9 int64 fields is about 3.8 µs
        per returned row and 25 µs per statement on this Python. The frames
        after PARSE are one constant: the portal is closed in the same batch
        (or by the server, if the statement fails, §5), so its name is free
        again by the next call.
        """
        self.sock.sendall(frame(C_PARSE, _Writer().s("").text(sql).take())
                          + _QUERY_TAIL)
        tag, rows, payload_bytes, error = "", 0, 0, None
        fields = 0
        while True:
            ftype, _, payload = self._recv_frame()
            if ftype == S_ROW_BATCH:
                (count,) = _U16.unpack_from(payload, 0)
                rows += count
                at = 2
                for _ in range(count * fields):
                    (n,) = _I32.unpack_from(payload, at)
                    at += 4
                    if n > 0:
                        at += n
                        payload_bytes += n
            elif ftype == S_ROW_DESC:
                (fields,) = _U16.unpack_from(payload, 0)
            elif ftype == S_COMPLETE:
                tag = _Reader(payload).text()
            elif ftype == S_ERROR:
                error = self._error(payload)
            elif ftype == S_READY:
                return tag, rows, payload_bytes, error
            elif ftype not in (S_PARSE_OK, S_BIND_OK, S_NOTICE):
                raise ConnectionError("unexpected server frame type %d" % ftype)

    def fetch(self, sql):
        """Runs one statement and returns `(rows, tag, error)`: each row a
        list with one **rendered** string per value (None for NULL), sent as
        `query` sends - PARSE and one constant tail.

        The counterpart of `pg_wire.PgConnection.fetch` plus a decode, for a
        driver that needs its values (EF-S1). Two things `execute` and
        `render_value` do per statement are done here once per result shape:
        a row description is decoded, and its renderers resolved, the first
        time its bytes are seen and looked up by those bytes after. A
        PostgreSQL client has no such step - its values arrive as text - so
        paying it per statement charged KDS about 10 us of client CPU a
        statement that says nothing about either server (EF-S1's passes D
        and D2: 21.3 us against 11.5 us on a lookup). A
        refusal is returned, not raised, as `query` returns it.
        """
        self.sock.sendall(frame(C_PARSE, _Writer().s("").text(sql).take())
                          + _QUERY_TAIL)
        renderers, rows, tag, error = (), [], "", None
        while True:
            ftype, _, payload = self._recv_frame()
            if ftype == S_ROW_BATCH:
                for raw_row in self._row_batch(payload, len(renderers)):
                    rows.append([None if raw is None else render(raw)
                                 for render, raw in zip(renderers, raw_row)])
            elif ftype == S_ROW_DESC:
                renderers = self._renderers.get(payload)
                if renderers is None:
                    if len(self._renderers) >= 1024:
                        self._renderers.clear()
                    renderers = tuple(renderer_for(f) for f in self._row_desc(payload))
                    self._renderers[payload] = renderers
            elif ftype == S_COMPLETE:
                tag = _Reader(payload).text()
            elif ftype == S_ERROR:
                error = self._error(payload)
            elif ftype == S_READY:
                return rows, tag, error
            elif ftype not in (S_PARSE_OK, S_BIND_OK, S_NOTICE):
                raise ConnectionError("unexpected server frame type %d" % ftype)

    def prepare(self, name, sql):
        """Parses `sql` once under the statement name `name` (§5's named
        statement, held until `C_CLOSE` or disconnect). Returns its
        `pattern_id`. A later `execute_prepared(name, params)` binds and runs
        it without sending its text again (BN-S1: the PARSE-once path)."""
        self.sock.sendall(frame(C_PARSE, _Writer().s(name).text(sql).take()) + frame(C_SYNC))
        pattern_id, error = 0, None
        while True:
            ftype, _, payload = self._recv_frame()
            if ftype == S_PARSE_OK:
                pattern_id = _Reader(payload).u64()
            elif ftype == S_ERROR:
                error = self._error(payload)
            elif ftype == S_READY:
                break
        if error is not None:
            raise error
        return pattern_id

    def execute_prepared(self, name, params=None, max_rows=0):
        """`execute` without the `C_PARSE`: one BIND of the named statement,
        then EXECUTE, CLOSE and SYNC in the same write. `params` as for
        `execute`. Returns `(fields, rows, tag, rows_affected)`."""
        return self.execute(None, params, max_rows, statement=name)

    @staticmethod
    def int64(v):
        """A `T_INT64` parameter for `params`."""
        return (T_INT64, 0, struct.pack("<q", v))

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
        # One loop over offsets rather than `_Reader` calls, which were most
        # of the client's cost per returned row
        # (bench/v3.3.0/results-row-cost-attribution-v3.3.0-37-gf829964c.md).
        # A short slice does not raise, so the bound is checked once at the
        # end; a short length prefix raises in `unpack_from`. A length below
        # -1 is refused, as the server's `DecodeRowBatch` refuses it.
        unpack = _I32.unpack_from
        try:
            (count,) = _U16.unpack_from(payload, 0)
            at = 2
            rows = []
            for _ in range(count):
                row = []
                for _ in range(field_count):
                    (n,) = unpack(payload, at)
                    at += 4
                    if n == -1:
                        row.append(None)
                    elif n < 0:
                        raise KwpError(0, False, 2, "negative field length", None, None)
                    else:
                        row.append(payload[at:at + n])
                        at += n
                rows.append(row)
        except struct.error:
            at = len(payload) + 1
        if at > len(payload):
            raise KwpError(0, False, 2, "truncated payload", None, None)
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
