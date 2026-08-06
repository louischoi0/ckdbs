#!/usr/bin/env python3
"""Minimal PostgreSQL v3 frontend/backend protocol client, dependency-free.

Why this exists instead of psycopg: the repo's benchmarks are deliberately
dependency-free (see the note at the top of bench/bench_main.cpp), the
target hosts have no psycopg and no pip, and the benchmark needs exactly one
thing from a driver - send one statement, read one reply, time it. That is a
few hundred lines of protocol, and it keeps the ckdbs and PostgreSQL sides of
the comparison paying comparable client-side costs: a raw socket, a buffered
read, no result-object machinery.

Scope, on purpose:

  * simple query only (`Q`), one statement per round trip. This mirrors what
    tools/benchmark.py sends ckdbs - an inline-literal statement that the
    server parses every time - so neither side is silently getting the
    benefit of prepared statements. Extended-protocol (PARSE/BIND/EXECUTE)
    support is the obvious next step once KWP/1's own PARSE path lands.
  * text result format, rows counted and their payload bytes measured but not
    converted to Python objects (the benchmark never reads a value). Column
    lengths still have to be walked, so this is the floor, not a cheat.
  * auth: trust, cleartext password, md5, and SCRAM-SHA-256 (no channel
    binding). SCRAM is PostgreSQL's default since 14, so it is not optional.
  * no COPY, no notifications, no cursors, no transaction helpers. A
    statement outside an explicit BEGIN commits on its own, exactly as it
    would through libpq.

Replies are flattened to the same one-line convention tools/benchmark.py sees
from ckdbs, so bench_common.Phase can score both backends unchanged:

    OK <command tag>        e.g. "OK INSERT 0 1", "OK UPDATE 1"
    ROWS <n> <bytes>B       a row-returning statement
    ERR <code> <message>    an ErrorResponse

Usage:
    conn = PgConnection(host="127.0.0.1", port=15433, user="ec2-user",
                        database="bench")
    print(conn.send_command("SELECT 1"))
    conn.close()
"""

import base64
import hashlib
import hmac
import os
import socket
import struct

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 5432

# Protocol version 3.0 as it appears on the wire: major<<16 | minor.
PROTOCOL_VERSION = 3 << 16

_INT32 = struct.Struct("!i")
_INT16 = struct.Struct("!h")


class PgError(Exception):
    """A backend ErrorResponse, or a protocol violation by either side."""


class PgConnection:
    """One connection, one statement at a time.

    Not thread-safe and not meant to be: the benchmark is a single-connection
    driver because the ckdbs server serves one connection at a time, and
    adding client threads would measure the accept queue rather than either
    engine.
    """

    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, user=None, database=None,
                 password=None, timeout=60.0, application_name="pg_benchmark.py"):
        self.user = user or os.environ.get("PGUSER") or os.environ.get("USER") or "postgres"
        self.database = database or os.environ.get("PGDATABASE") or self.user
        self._password = password if password is not None else os.environ.get("PGPASSWORD")
        self.parameters = {}
        self.backend_pid = None
        self.last_error = None

        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        # Buffered reads: every message is an exact-length read, which recv()
        # alone does not give.
        self._in = self._sock.makefile("rb")

        self._startup(application_name)

    # ---- framing ---------------------------------------------------------

    def _send(self, kind, body=b""):
        """Sends one frontend message. `kind` is None for the startup packet,
        which is the one message with no type byte."""
        length = _INT32.pack(len(body) + 4)
        self._sock.sendall(length + body if kind is None else kind + length + body)

    def _read_message(self):
        """Reads one backend message as (type_byte, body). The length field
        counts itself but not the type byte."""
        head = self._in.read(5)
        if len(head) < 5:
            raise PgError("connection closed by server")
        kind = head[:1]
        (length,) = _INT32.unpack(head[1:])
        if length < 4:
            raise PgError(f"bad message length {length} for type {kind!r}")
        body = self._in.read(length - 4)
        if len(body) < length - 4:
            raise PgError("connection closed mid-message")
        return kind, body

    # ---- startup and authentication --------------------------------------

    def _startup(self, application_name):
        params = {
            "user": self.user,
            "database": self.database,
            "application_name": application_name,
            "client_encoding": "UTF8",
        }
        body = _INT32.pack(PROTOCOL_VERSION)
        for key, value in params.items():
            body += key.encode() + b"\0" + value.encode() + b"\0"
        body += b"\0"
        self._send(None, body)

        while True:
            kind, payload = self._read_message()
            if kind == b"R":
                self._authenticate(payload)
            elif kind == b"S":
                key, value = _split_cstrings(payload, 2)
                self.parameters[key] = value
            elif kind == b"K":
                self.backend_pid = _INT32.unpack(payload[:4])[0]
            elif kind == b"Z":
                return
            elif kind == b"E":
                raise PgError(_format_error(payload))
            elif kind in (b"N", b"A"):
                continue
            else:
                raise PgError(f"unexpected message {kind!r} during startup")

    def _authenticate(self, payload):
        (code,) = _INT32.unpack(payload[:4])
        rest = payload[4:]
        if code == 0:            # AuthenticationOk
            return
        if code == 3:            # cleartext
            self._send(b"p", self._require_password().encode() + b"\0")
            return
        if code == 5:            # md5
            salt = rest[:4]
            digest = self._md5_password(salt)
            self._send(b"p", digest.encode() + b"\0")
            return
        if code == 10:           # SASL, i.e. SCRAM
            mechanisms = [m for m in rest.split(b"\0") if m]
            if b"SCRAM-SHA-256" not in mechanisms:
                raise PgError(f"no supported SASL mechanism in {mechanisms}")
            self._scram()
            return
        raise PgError(f"unsupported authentication request {code}")

    def _require_password(self):
        if self._password is None:
            raise PgError("server requires a password; pass --password or set PGPASSWORD")
        return self._password

    def _md5_password(self, salt):
        inner = hashlib.md5(self._require_password().encode() + self.user.encode()).hexdigest()
        return "md5" + hashlib.md5(inner.encode() + salt).hexdigest()

    def _scram(self):
        """SCRAM-SHA-256 without channel binding (RFC 5802/7677).

        Kept explicit rather than pulled from a library so the whole exchange
        is auditable here; the server signature is verified, so a
        man-in-the-middle cannot complete the handshake by echoing success.
        """
        password = self._require_password().encode()
        nonce = base64.b64encode(os.urandom(18)).decode()
        client_first_bare = f"n=,r={nonce}"
        self._send(b"p", b"SCRAM-SHA-256\0"
                   + _INT32.pack(len(client_first_bare) + 3)
                   + b"n,," + client_first_bare.encode())

        kind, payload = self._read_message()
        if kind == b"E":
            raise PgError(_format_error(payload))
        if kind != b"R" or _INT32.unpack(payload[:4])[0] != 11:
            raise PgError("expected SASLContinue")
        server_first = payload[4:].decode()
        attrs = dict(part.split("=", 1) for part in server_first.split(","))
        server_nonce, salt, iterations = attrs["r"], attrs["s"], int(attrs["i"])
        if not server_nonce.startswith(nonce):
            raise PgError("server nonce does not extend the client nonce")

        salted = hashlib.pbkdf2_hmac("sha256", password, base64.b64decode(salt), iterations)
        client_key = hmac.new(salted, b"Client Key", hashlib.sha256).digest()
        stored_key = hashlib.sha256(client_key).digest()
        client_final_bare = f"c=biws,r={server_nonce}"
        auth_message = f"{client_first_bare},{server_first},{client_final_bare}"
        client_signature = hmac.new(stored_key, auth_message.encode(), hashlib.sha256).digest()
        proof = bytes(a ^ b for a, b in zip(client_key, client_signature))
        final = f"{client_final_bare},p={base64.b64encode(proof).decode()}"
        self._send(b"p", final.encode())

        server_key = hmac.new(salted, b"Server Key", hashlib.sha256).digest()
        expected = hmac.new(server_key, auth_message.encode(), hashlib.sha256).digest()
        kind, payload = self._read_message()
        if kind == b"E":
            raise PgError(_format_error(payload))
        if kind != b"R" or _INT32.unpack(payload[:4])[0] != 12:
            raise PgError("expected SASLFinal")
        verifier = dict(part.split("=", 1)
                        for part in payload[4:].decode().split(","))["v"]
        if not hmac.compare_digest(base64.b64decode(verifier), expected):
            raise PgError("server signature mismatch")

    # ---- queries ---------------------------------------------------------

    def query(self, sql):
        """Runs one simple query. Returns (tag, rows, payload_bytes, error).

        `error` is None on success; on an ErrorResponse the messages are
        drained through ReadyForQuery first, so the connection stays usable
        for the next statement - a benchmark that died on the first ERR would
        report nothing.
        """
        self._send(b"Q", sql.encode() + b"\0")
        tag, rows, payload_bytes, error = "", 0, 0, None
        columns = 0
        while True:
            kind, body = self._read_message()
            if kind == b"D":
                rows += 1
                payload_bytes += self._skip_data_row(body, columns)
            elif kind == b"T":
                (columns,) = _INT16.unpack(body[:2])
            elif kind == b"C":
                tag = body.split(b"\0", 1)[0].decode()
            elif kind == b"Z":
                return tag, rows, payload_bytes, error
            elif kind == b"E":
                error = _format_error(body)
            elif kind == b"I":          # EmptyQueryResponse
                tag = "EMPTY"
            elif kind in (b"N", b"S", b"A", b"G", b"H"):
                continue                # notice, parameter status, notify, COPY
            else:
                raise PgError(f"unexpected message {kind!r} in query reply")

    @staticmethod
    def _skip_data_row(body, columns):
        """Walks a DataRow's column lengths and returns its payload bytes.

        Values are not decoded: the benchmark counts rows, and building a
        Python object per column would charge PostgreSQL for client-side work
        the ckdbs driver does not do either.
        """
        (count,) = _INT16.unpack(body[:2])
        if columns and count != columns:
            raise PgError(f"DataRow has {count} columns, RowDescription said {columns}")
        offset, total = 2, 0
        for _ in range(count):
            (length,) = _INT32.unpack(body[offset:offset + 4])
            offset += 4
            if length > 0:
                offset += length
                total += length
        return total

    def fetch(self, sql):
        """Runs one simple query and returns (rows, error).

        Each row is a list of raw column bytes in the statement's column
        order, with None for SQL NULL; values are left undecoded because the
        caller knows the types and this module deliberately has no type map.

        Separate from query() because that one is the *benchmark* path and
        must not build a Python object per column - see _skip_data_row. This
        one is for a driver that actually needs the values (a balance read
        back, an id from RETURNING), and it pays for them, so a latency
        measured around it is not comparable to one measured around
        send_command().
        """
        self._send(b"Q", sql.encode() + b"\0")
        rows, error, columns = [], None, 0
        while True:
            kind, body = self._read_message()
            if kind == b"D":
                row = _decode_data_row(body)
                if columns and len(row) != columns:
                    raise PgError(f"DataRow has {len(row)} columns, "
                                  f"RowDescription said {columns}")
                rows.append(row)
            elif kind == b"T":
                (columns,) = _INT16.unpack(body[:2])
            elif kind == b"E":
                error = _format_error(body)
            elif kind == b"Z":
                if error is not None:
                    self.last_error = error
                return rows, error
            elif kind in (b"C", b"I", b"N", b"S", b"A", b"G", b"H"):
                continue
            else:
                raise PgError(f"unexpected message {kind!r} in query reply")

    def send_command(self, sql):
        """One statement in, one flat reply line out - the shape
        bench_common.Phase scores. See the module docstring."""
        tag, rows, payload_bytes, error = self.query(sql)
        if error is not None:
            self.last_error = error
            return f"ERR {error}"
        if rows or tag.startswith("SELECT"):
            return f"ROWS {rows} {payload_bytes}B"
        return f"OK {tag}"

    def scalar(self, sql):
        """Runs `sql` and returns its first column of its first row as bytes.

        Separate from send_command() because it is used for untimed setup
        (row counts, settings readback) where the value is actually wanted.
        """
        self._send(b"Q", sql.encode() + b"\0")
        value, error = None, None
        while True:
            kind, body = self._read_message()
            if kind == b"D" and value is None:
                (count,) = _INT16.unpack(body[:2])
                if count:
                    (length,) = _INT32.unpack(body[2:6])
                    value = None if length < 0 else body[6:6 + length]
            elif kind == b"E":
                error = _format_error(body)
            elif kind == b"Z":
                if error is not None:
                    raise PgError(error)
                return value

    def close(self):
        try:
            self._send(b"X")
        except OSError:
            pass
        finally:
            try:
                self._in.close()
            finally:
                self._sock.close()


def _decode_data_row(body):
    """Splits a DataRow into its column payloads. -1 is SQL NULL, which is
    not the same as an empty value and is kept distinct as None."""
    (count,) = _INT16.unpack(body[:2])
    offset, values = 2, []
    for _ in range(count):
        (length,) = _INT32.unpack(body[offset:offset + 4])
        offset += 4
        if length < 0:
            values.append(None)
        else:
            values.append(body[offset:offset + length])
            offset += length
    return values


def _split_cstrings(payload, count):
    parts = payload.split(b"\0")
    if len(parts) < count:
        raise PgError("truncated string list")
    return [p.decode(errors="replace") for p in parts[:count]]


def _format_error(payload):
    """Flattens an ErrorResponse's field list to "<SQLSTATE> <message>"."""
    fields = {}
    for chunk in payload.split(b"\0"):
        if chunk:
            fields[chunk[:1].decode()] = chunk[1:].decode(errors="replace")
    code = fields.get("C", "XX000")
    message = fields.get("M", "unknown error")
    detail = fields.get("D")
    return f"{code} {message}" + (f" ({detail})" if detail else "")


if __name__ == "__main__":
    # Smoke check: connect, run one statement, print the server version.
    import argparse

    parser = argparse.ArgumentParser(description="probe a PostgreSQL server")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--user", default=None)
    parser.add_argument("--database", default=None)
    parser.add_argument("--password", default=None)
    parser.add_argument("sql", nargs="?", default="SELECT version()")
    args = parser.parse_args()

    conn = PgConnection(args.host, args.port, args.user, args.database, args.password)
    print("server:", (conn.scalar("SELECT version()") or b"").decode())
    print("reply :", conn.send_command(args.sql))
    conn.close()
