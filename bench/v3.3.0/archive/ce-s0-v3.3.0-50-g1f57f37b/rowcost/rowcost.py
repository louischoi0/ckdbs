#!/usr/bin/env python3
"""Row-cost attribution: KDS (KWP/1) against PostgreSQL 18.6 (simple query, text format).

  rowcost.py load  --kds-port P --pg-port Q [--rows 60480]
  rowcost.py run   --kds-port P --kds-pid PID --pg-port Q --out FILE.jsonl --progress FILE [--reps 3]
  rowcost.py prof  --kds-port P --pg-port Q --size N [--n 20]      (cProfile of the client decode)
  rowcost.py idle  --kds-pid PID --pg-pid PID

Cells are (rep, size, mode, engine). Engines alternate within a cell group. Resumable: a cell already
in the output file is skipped. Server CPU = utime+stime of the server process (KDS) or the backend (PG)
from /proc/<pid>/stat over the cell; client CPU = time.process_time() of this process.
Modes: drain (framing only, no cell decoded), split (kwp.execute / pg_wire.fetch: cells become bytes
objects), values (split + every cell converted to a Python int).
Run with the repo's tools/ on PYTHONPATH.
"""
import argparse, json, os, struct, subprocess, sys, time, random
import kwp, pg_wire

SCHEMA_COLS = ("id int64, bar_id int64, symbol_id int64, session_no int32, ret_bp int64, mom5_bp int64, "
               "mom20_bp int64, mom60_bp int64, mom120_bp int64, vol10_bp int64, vol20_bp int64, vol60_bp int64")
PG_COLS = SCHEMA_COLS.replace("int64", "bigint").replace("int32", "integer")
WIDTHS = (8, 8, 8, 4, 8, 8, 8, 8, 8, 8, 8, 8)
SIZES = (1, 10, 100, 1000, 10000, 60480)
MODES = ("drain", "split", "values")
TICK = os.sysconf("SC_CLK_TCK")


def cpu_ticks(pid):
    with open(f"/proc/{pid}/stat") as f:
        s = f.read()
    p = s[s.rindex(")") + 2:].split()
    return int(p[11]) + int(p[12])      # utime, stime


def gen_rows(n):
    r = random.Random(1)
    for i in range(1, n + 1):
        yield (i, i, 1 + i % 8, i % 252, r.randint(-900, 900), r.randint(-3000, 3000), r.randint(-6000, 6000),
               r.randint(-9000, 9000), r.randint(-12000, 12000), r.randint(10, 400), r.randint(10, 500),
               r.randint(10, 600))


def stmts(n):
    rows = list(gen_rows(n))
    for lo in range(0, n, 1000):
        yield ("INSERT INTO daily_stats VALUES " +
               ",".join("(" + ",".join(map(str, r)) + ")" for r in rows[lo:lo + 1000]))


def sql_for(size):
    return f"SELECT * FROM daily_stats WHERE id BETWEEN 1 AND {size}"


class Drain:
    """Reads one reply to its end knowing only the frame headers (KWP: u32 length + type byte, ends at
    S_READY; PG: type byte + u32 length, ends at 'Z'). No cell is touched. Returns bytes read."""
    def __init__(self, sock, kind):
        self.sock = sock
        self.kwp = kind == "kwp"
        self.buf = bytearray(1 << 20)
        self.mv = memoryview(self.buf)

    def reply(self):
        sock, buf, mv = self.sock, self.buf, self.mv
        have = p = total = 0
        unpack = struct.unpack_from
        while True:
            while have - p < 5:
                if p:
                    buf[0:have - p] = buf[p:have]; have -= p; p = 0
                n = sock.recv_into(mv[have:])
                if not n:
                    raise ConnectionError("closed")
                have += n; total += n
            if self.kwp:
                (length,) = unpack("<I", buf, p); size = 4 + length; last = buf[p + 4] == kwp.S_READY
            else:
                (length,) = unpack(">I", buf, p + 1); size = 1 + length; last = buf[p] == 0x5A
            avail = have - p
            if avail >= size:
                p += size
            else:
                skip = size - avail
                p = have = 0
                while skip:
                    n = sock.recv_into(mv[:min(skip, len(buf))])
                    if not n:
                        raise ConnectionError("closed")
                    skip -= n; total += n
            if last:
                return total
            if p == have:
                p = have = 0


class Kds:
    name = "kds"

    def __init__(self, port, pid):
        self.c = kwp.Connection("127.0.0.1", port, timeout=120)
        self.pid = pid
        self.d = Drain(self.c.sock, "kwp")

    _I32 = struct.Struct("<i").unpack_from
    _U16 = struct.Struct("<H").unpack_from

    def inline(self, sql):
        """CONTROL: the same bytes as split mode, with kwp._row_batch's per-cell _Reader calls inlined into
        one loop (the shape pg_wire._decode_data_row has). Same output: a list of rows of bytes cells."""
        c = self.c
        c.sock.sendall(kwp.frame(kwp.C_PARSE, kwp._Writer().s("").text(sql).take()) + kwp._QUERY_TAIL)
        rows, fields, i32, u16 = [], 0, self._I32, self._U16
        while True:
            ftype, _, payload = c._recv_frame()
            if ftype == kwp.S_ROW_BATCH:
                (count,) = u16(payload, 0)
                at = 2
                for _ in range(count):
                    row = []
                    for _ in range(fields):
                        (n,) = i32(payload, at)
                        at += 4
                        if n < 0:
                            row.append(None)
                        else:
                            row.append(payload[at:at + n]); at += n
                    rows.append(row)
            elif ftype == kwp.S_ROW_DESC:
                (fields,) = u16(payload, 0)
            elif ftype == kwp.S_ERROR:
                raise c._error(payload)
            elif ftype == kwp.S_READY:
                return len(rows)

    def run(self, mode, sql):
        if mode == "drain":
            self.c.sock.sendall(kwp.frame(kwp.C_PARSE, kwp._Writer().s("").text(sql).take()) + kwp._QUERY_TAIL)
            return self.d.reply()
        if mode == "inline":
            return self.inline(sql)
        fields, rows, tag, aff = self.c.execute(sql)
        if mode == "values":
            fb = int.from_bytes
            for row in rows:
                for cell in row:
                    fb(cell, "little", signed=True)
        return len(rows)


class Pg:
    name = "pg"

    def __init__(self, port, pid=None):
        self.c = pg_wire.PgConnection("127.0.0.1", port, user="cdkbs", database="rowcost", timeout=120)
        self.pid = self.c.backend_pid or int(self.c.scalar("select pg_backend_pid()"))
        self.d = Drain(self.c._sock, "pg")
        os.sched_setaffinity(self.pid, {4})      # the backend, as the KDS reactor is, on its own CPU

    def run(self, mode, sql):
        if mode == "drain":
            self.c._send(b"Q", sql.encode() + b"\0")
            return self.d.reply()
        rows, err = self.c.fetch(sql)
        if err:
            raise RuntimeError(err)
        if mode == "values":
            for row in rows:
                for cell in row:
                    int(cell)
        return len(rows)


def cell(eng, mode, size, n):
    sql = sql_for(size)
    lat = []
    pc, pw = time.process_time, time.perf_counter
    first = eng.run(mode, sql)         # warm-up; for drain, the wire bytes of one reply
    s0 = cpu_ticks(eng.pid); c0 = pc(); w0 = pw(); last = w0
    for _ in range(n):
        eng.run(mode, sql)
        now = pw(); lat.append(now - last); last = now
    w1 = pw(); c1 = pc(); s1 = cpu_ticks(eng.pid)
    return {"engine": eng.name, "mode": mode, "size": size, "n": n,
            "wall_us_per_stmt": (w1 - w0) / n * 1e6,
            "server_cpu_us_per_stmt": (s1 - s0) / TICK / n * 1e6,
            "client_cpu_us_per_stmt": (c1 - c0) / n * 1e6,
            "server_ticks": s1 - s0, "first_result": first,
            "lat_us": [round(x * 1e6, 1) for x in lat]}


TARGET_S = 0.0


def nstmts(size):
    if TARGET_S:        # fixed-window mode: about TARGET_S seconds per cell, so a 10 ms tick is <= 0.5 %
        return max(20, int(TARGET_S * 1e6 / (45 + 0.5 * size)))
    return min(3000, max(20, 200000 // size))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd")
    ap.add_argument("--kds-port", type=int); ap.add_argument("--kds-pid", type=int)
    ap.add_argument("--pg-port", type=int); ap.add_argument("--pg-pid", type=int)
    ap.add_argument("--rows", type=int, default=60480)
    ap.add_argument("--out"); ap.add_argument("--progress"); ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--target-s", type=float, default=0.0); ap.add_argument("--modes", default="drain,split,values")
    ap.add_argument("--size", type=int, default=1000); ap.add_argument("--n", type=int, default=20)
    a = ap.parse_args()
    if a.cmd == "load":
        k = kwp.Connection("127.0.0.1", a.kds_port, timeout=120)
        k.execute(f"CREATE TABLE daily_stats ({SCHEMA_COLS}) BTREE")
        for s in stmts(a.rows):
            k.execute(s)
        print("kds rows", k.execute("SELECT COUNT(*) FROM daily_stats")[1])
        p = pg_wire.PgConnection("127.0.0.1", a.pg_port, user="cdkbs", database="rowcost")
        p.fetch(f"CREATE TABLE daily_stats ({PG_COLS}, PRIMARY KEY (id))")
        for s in stmts(a.rows):
            _, e = p.fetch(s)
            assert not e, e
        p.fetch("ANALYZE daily_stats")
        print("pg rows", p.scalar("select count(*) from daily_stats"))
        return
    if a.cmd == "idle":
        for name, pid in (("kds", a.kds_pid), ("pg", a.pg_pid)):
            s0 = cpu_ticks(pid); time.sleep(10); s1 = cpu_ticks(pid)
            print(name, "idle cpu ticks over 10 s:", s1 - s0)
        return
    if a.cmd == "prof":
        import cProfile, pstats
        for eng in (Kds(a.kds_port, a.kds_pid or os.getpid()), Pg(a.pg_port)):
            for mode in ("split", "values"):
                pr = cProfile.Profile()
                eng.run(mode, sql_for(a.size))
                pr.enable()
                for _ in range(a.n):
                    eng.run(mode, sql_for(a.size))
                pr.disable()
                print(f"=== {eng.name} {mode} size={a.size} n={a.n}")
                pstats.Stats(pr).sort_stats("tottime").print_stats(8)
        return
    global TARGET_S, MODES
    TARGET_S = a.target_s
    MODES = tuple(a.modes.split(","))
    k = Kds(a.kds_port, a.kds_pid)
    p = Pg(a.pg_port)
    for e in (k, p):
        print(e.name, "server/backend pid", e.pid, flush=True)
    done = set()
    if os.path.exists(a.out):
        for line in open(a.out):
            d = json.loads(line); done.add((d["rep"], d["size"], d["mode"], d["engine"]))
    with open(a.out, "a") as out:
        for rep in range(1, a.reps + 1):
            for size in SIZES:
                for mode in MODES:
                    for eng in ((k, p) if (rep + size) % 2 else (p, k)):
                        if (rep, size, mode, eng.name) in done:
                            continue
                        d = cell(eng, mode, size, nstmts(size))
                        d["rep"] = rep
                        out.write(json.dumps(d) + "\n"); out.flush()
                        with open(a.progress, "w") as pf:
                            pf.write(f"{time.time()} rep={rep} size={size} mode={mode} eng={eng.name}\n")
    print("FINISHED", flush=True)


if __name__ == "__main__":
    main()
