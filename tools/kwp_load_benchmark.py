#!/usr/bin/env python3
"""Prices the T2 KWP load stream (docs/spec/bulkinsert.md §3, KL02-KL06).

Drives the KWP v0 binary load endpoint (`kwp_port`) with pre-encoded D5
chunks and measures rows/s and per-chunk latency, against the same
int-only `write_probe` schema Part III's T1 cells used - so the T2-vs-T1
delta is the parse + text-decode + round-trip removal, measured.

Wire shapes implemented here (authoritative sources in the tree):
  frame   = length u32 LE (counts type..payload) + type u8 + flags u8 +
            reserved u16 + payload                     (include/kds/wire/kwp.hpp)
  C_HELLO = magic 'KWP1' u32 + max_ver u16 + min_ver u16 + caps u64 +
            auth u8 + name str(u16-len)                (kwp_types.hpp)
  C_LOAD_BEGIN = relation str + flags u16 + declared_rows u64
  S_LOAD_READY = load_id u64 + window u16 + max_chunk u32 + field_count u16
                 + row description (skipped here)
  C_LOAD_CHUNK = load_id u64 + chunk_seq u32 + row_count u16 + row batch
  row batch    = row_count u16 + rows; each field {len i32 LE, bytes},
                 int64 = 8-byte LE                     (wire/row_codec)
  S_LOAD_ACK   = load_id u64 + chunk_seq u32 + rows_accepted u64
  S_COMPLETE   = tag str + rows u64
Capability bit 16 (BULK_LOAD) must be set in C_HELLO.

Two modes:
  --mode pipelined  keep up to the server-announced window (4) chunks
                    unacknowledged - the protocol's point (BI7). Per-chunk
                    latency = send-to-own-ACK, which deliberately includes
                    queueing behind earlier chunks; the throughput is the
                    number to read.
  --mode serial     stop-and-wait, one chunk in flight - the clean
                    per-chunk latency, and the window's price by delta.

A text-protocol connection on the ordinary port does CREATE TABLE and the
COUNT verification; the load itself never touches it.

Usage:
    ./build-release/kds_server ~/f.db --port 15432 --config kwp.conf
        # kwp.conf: kwp_port = 15499 (+ durability = ...)
    python3 tools/kwp_load_benchmark.py --kwp-port 15499 --port 15432 \
        --rows 100000 --chunk-rows 1000 --mode pipelined --json out.json
"""

import argparse
import re
import socket
import struct
import sys
import time

from bench_common import Phase, report, write_json
from ckdbs_cli import DEFAULT_HOST, ServerConnection

CAP_BULK_LOAD = 1 << 16
KWP_MAGIC = 0x3150574B

C_HELLO, C_PING, C_TERMINATE = 1, 2, 3
C_LOAD_BEGIN, C_LOAD_CHUNK, C_LOAD_END, C_LOAD_ABORT = 16, 17, 18, 19
S_HELLO, S_ERROR, S_COMPLETE, S_PONG = 1, 2, 3, 4
S_LOAD_READY, S_LOAD_ACK = 16, 17

COLUMNS = "(id int64, a int64, b int64, c int64, d int64) HEAP"


def frame(ftype, payload=b""):
    return struct.pack("<IBBH", 4 + len(payload), ftype, 0, 0) + payload


def pstr(s):
    b = s.encode()
    return struct.pack("<H", len(b)) + b


class KwpConnection:
    """One KWP socket. TCP_NODELAY on our side; TCP_QUICKACK re-armed
    around every recv by default, because the server's KWP endpoint does
    NOT set TCP_NODELAY (tcp_server.cpp does, kwp_load_server.cpp does
    not - found by this driver's first run), so its small S_LOAD_ACK
    frames are Nagle-held against our delayed ACKs for the classic ~40 ms
    whenever the pipeline drains. QUICKACK defeats the interaction from
    the client side so the benchmark measures the engine, not the
    artifact; --no-quickack shows the artifact instead."""

    def __init__(self, host, port, timeout=120.0, quickack=True):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.quickack = quickack and hasattr(socket, "TCP_QUICKACK")
        self.buf = b""

    def send(self, data):
        self.sock.sendall(data)

    def read_frame(self):
        while len(self.buf) < 4:
            self._fill()
        (length,) = struct.unpack_from("<I", self.buf)
        total = 4 + length
        while len(self.buf) < total:
            self._fill()
        ftype = self.buf[4]
        payload = self.buf[8:total]
        self.buf = self.buf[total:]
        if ftype == S_ERROR:
            sys.exit(f"FATAL: S_ERROR from server: {payload[:200]!r}")
        return ftype, payload

    def _fill(self):
        if self.quickack:
            # One-shot on Linux: re-armed before every read.
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_QUICKACK, 1)
        chunk = self.sock.recv(1 << 16)
        if not chunk:
            raise ConnectionError("KWP server closed the connection")
        self.buf += chunk

    def close(self):
        self.sock.close()


def encode_chunk_rows(start, count):
    """The D5 batch for rows [start, start+count): 4 int64 fields per row,
    values (i, 2i, 3i, 5i) - Part III's exact rows."""
    out = bytearray(struct.pack("<H", count))
    pack = struct.Struct("<IqIqIqIq").pack
    for i in range(start, start + count):
        out += pack(8, i, 8, 2 * i, 8, 3 * i, 8, 5 * i)
    return bytes(out)


def run_load(kwp, table, total_rows, chunk_rows, window, phase_name):
    """One LOAD session; returns (Phase, rows_affected)."""
    kwp.send(frame(C_LOAD_BEGIN, pstr(table) + struct.pack("<HQ", 0, total_rows)))
    ftype, payload = kwp.read_frame()
    if ftype != S_LOAD_READY:
        sys.exit(f"FATAL: expected S_LOAD_READY, got frame type {ftype}")
    load_id, srv_window, max_chunk, field_count = struct.unpack_from("<QHIH", payload)
    if field_count != 4:
        sys.exit(f"FATAL: server announced {field_count} fields, expected 4")
    window = min(window, srv_window)

    # Frames pre-encoded outside the clock, as every driver here does.
    frames = []
    sent_rows = 0
    seq = 0
    while sent_rows < total_rows:
        n = min(chunk_rows, total_rows - sent_rows)
        head = struct.pack("<QIH", load_id, seq, n)
        frames.append(frame(C_LOAD_CHUNK, head + encode_chunk_rows(sent_rows, n)))
        if len(frames[-1]) - 8 > max_chunk:
            sys.exit(f"FATAL: chunk exceeds announced max_chunk_bytes {max_chunk}")
        sent_rows += n
        seq += 1

    phase = Phase(phase_name, f"{chunk_rows} rows/chunk, {len(frames)} chunks, "
                              f"{total_rows} rows, window {window}")
    send_times = {}
    acked = 0
    started = time.perf_counter()
    for i, fr in enumerate(frames):
        while i - acked >= window:
            t_ack = _read_ack(kwp, load_id)
            phase.record(time.perf_counter() - send_times.pop(t_ack), "OK")
            acked += 1
        send_times[i] = time.perf_counter()
        kwp.send(fr)
    while acked < len(frames):
        t_ack = _read_ack(kwp, load_id)
        phase.record(time.perf_counter() - send_times.pop(t_ack), "OK")
        acked += 1

    kwp.send(frame(C_LOAD_END))
    ftype, payload = kwp.read_frame()
    phase.elapsed = time.perf_counter() - started
    if ftype != S_COMPLETE:
        sys.exit(f"FATAL: expected S_COMPLETE, got frame type {ftype}")
    (taglen,) = struct.unpack_from("<H", payload)
    tag = payload[2:2 + taglen].decode()
    (rows_affected,) = struct.unpack_from("<Q", payload, 2 + taglen)
    if tag != "LOAD" or rows_affected != total_rows:
        sys.exit(f"FATAL: S_COMPLETE tag={tag!r} rows={rows_affected}, "
                 f"expected LOAD {total_rows}")
    return phase


def _read_ack(kwp, load_id):
    ftype, payload = kwp.read_frame()
    if ftype != S_LOAD_ACK:
        sys.exit(f"FATAL: expected S_LOAD_ACK, got frame type {ftype}")
    got_id, seq, _accepted = struct.unpack_from("<QIQ", payload)
    if got_id != load_id:
        sys.exit(f"FATAL: ACK for load {got_id}, expected {load_id}")
    return seq


def count_rows(client, table):
    reply = client(f"SELECT COUNT(*) FROM {table}")
    m = re.search(r"\\n(\d+)", reply)
    if not m:
        sys.exit(f"FATAL: unparseable COUNT reply: {reply!r}")
    return int(m.group(1))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=15432,
                    help="text-protocol port (DDL + verification)")
    ap.add_argument("--kwp-port", type=int, required=True)
    ap.add_argument("--rows", type=int, default=100000)
    ap.add_argument("--chunk-rows", default="100,1000")
    ap.add_argument("--mode", default="pipelined",
                    choices=("pipelined", "serial"))
    ap.add_argument("--no-quickack", action="store_true",
                    help="do not arm TCP_QUICKACK: exhibits the ~40 ms "
                         "Nagle/delayed-ACK stalls of the server's missing "
                         "TCP_NODELAY instead of defeating them")
    ap.add_argument("--durability", default="unknown")
    ap.add_argument("--suffix", default=str(int(time.time())))
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    text = ServerConnection(args.host, args.port, timeout=120.0)
    client = text.send_command

    kwp = KwpConnection(args.host, args.kwp_port,
                        quickack=not args.no_quickack)
    kwp.send(frame(C_HELLO, struct.pack("<IHHQB", KWP_MAGIC, 1, 1,
                                        CAP_BULK_LOAD, 0) +
                   pstr("kwp_load_benchmark")))
    ftype, _ = kwp.read_frame()
    if ftype != S_HELLO:
        sys.exit(f"FATAL: expected S_HELLO, got frame type {ftype}")

    window = 4 if args.mode == "pipelined" else 1
    phases = []
    for chunk_rows in [int(c) for c in args.chunk_rows.split(",") if c]:
        table = f"kwp_{args.suffix}_{chunk_rows}"
        reply = client(f"CREATE TABLE {table} {COLUMNS}")
        if reply.startswith("ERR"):
            sys.exit(f"FATAL: CREATE TABLE {table}: {reply!r}")
        phase = run_load(kwp, table, args.rows, chunk_rows, window,
                         f"load-{chunk_rows}")
        counted = count_rows(client, table)
        if counted != args.rows:
            sys.exit(f"FATAL: {table} holds {counted}, sent {args.rows}")
        phase.detail += ", COUNT verified"
        phases.append(phase)
        print(f"  load-{chunk_rows:>5} [{args.mode}]: "
              f"{args.rows / phase.elapsed:>10,.0f} rows/s, COUNT ok",
              flush=True)

    meta = {
        "engine": "ckdbs-kwp",
        "driver": "kwp_load_benchmark.py",
        "durability": args.durability,
        "mode": args.mode,
        "quickack": not args.no_quickack,
        "rows": args.rows,
        "columns": 5,
        "clustered": "heap",
        "host": args.host,
        "port": args.kwp_port,
        "table": f"kwp_{args.suffix}_<chunk>",
    }
    report(phases, meta, footer=(
        "latencies are per CHUNK; in pipelined mode they include deliberate "
        "queueing behind the window - read the throughput.",
        "rows/s here = total rows / whole-session elapsed (chunks + END/COMPLETE).",
    ))
    if args.json:
        write_json(args.json, meta, phases)
    kwp.close()
    text.close()


if __name__ == "__main__":
    main()
