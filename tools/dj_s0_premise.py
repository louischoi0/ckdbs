#!/usr/bin/env python3
"""DJ-S0's premise cells: what a connection costs, what a connect-and-close
loop costs the sessions beside it, and where `Begin` refuses.

`instructions/v3.2.0/workorder-dj-connections-sessions-cursors.md` section 5
(DJ-S0). One binary (hashed), `auth = off`, the server pinned to one CPU a
core (its other threads to CPUs 4-7) and the driver to CPU 0. The timed
arms of `beside` and `churn` hold for a quiet host - no compiler, linker or
suite of another session - and an arm that noise entered is run again; the
hold is counted (`held_for_quiet_s`) and is outside the stage's budget. Four cells, each its own server lifetime:

  memory   `cores = 1`. Resident memory (VmRSS) with no session, then with
           N idle sessions that finished their handshake and ran nothing,
           then after each ran one point read (the first statement makes the
           session's wait statistics), at each --idle count. The server's
           CPU over an idle window with every session open is the sweeps'
           cost. Sessions are then closed and the memory read again.
  beside   `cores = 1`. The standing point read (`SELECT * FROM dj_t WHERE
           id = k`, one connection, k uniform over the preloaded rows) with
           no idle session beside it, with each --idle count, and with none
           again (the floor under a delta). Latency percentiles and the
           server's CPU a statement.
  churn    `cores = 1`, once plain and once with `tls = on`. The standing
           point read alone (S), beside one client that loops connect, one
           point read, close (C), and alone again (S2). The loop runs in a
           process of its own on its own CPU. Its rate is the client's - one
           Python process - so it is a floor on what the server admits, not
           its ceiling. Also the memory of --tls-idle idle sessions under
           TLS.
  begin    `cores = 1`: --begin-sessions sessions each send `BEGIN` and hold
           the transaction; how many were refused, with what, and what a
           read, a `BEGIN READ ONLY` and an autocommit write from one more
           session answer while they are held. Then every session loops
           BEGIN, one INSERT, COMMIT for --seconds: the share of BEGINs
           refused. `cores = 2`: the hold again, with as many sessions as two
           cores have slots for less --begin-margin - the kernel chooses each
           session's core, so a refusal there is a core full beside one
           that is not.
  portal   `cores = 1`. `C_EXECUTE {max_rows = 100}` of `SELECT * FROM dj_t`
           (every row of the relation): the memory --portals such suspended
           portals hold, and the time to the first batch against the same
           statement with `LIMIT 100` - what "max_rows bounds delivery, not
           execution" costs a client that asked for a hundred rows.

Usage: dj_s0_premise.py --bin B --workdir D --out OUT.json [--cells
memory,beside,churn,begin,portal] [--idle 1000,10000] [--seconds 3.0] [--reps 2]
[--port 15671] [--heartbeat F] [--status F]
"""
import argparse, hashlib, json, multiprocessing, os, random, selectors, shutil, socket, ssl
import struct, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from bench_common import nearest_rank  # noqa: E402

TABLE = "dj_t"
ROWS = 100_000
TICK = os.sysconf("SC_CLK_TCK")
SERVER_CPU = 4          # core 0's reactor; core 1's is SERVER_CPU + 1
DRIVER_CPU = 0
CHURN_CPU = 1


# What makes the host unquiet: another session's compiler, linker or suite.
# Read from /proc, not through a process whose own argv would carry the
# names (another session's gate matches on argv).
NOISE = {"cc1plus", "ld", "ctest", "kds_tests", "kds_bs_tests", "ckdbs-sim"}
HELD_S = [0.0]


def noisy_now():
    n = 0
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            if open(f"/proc/{pid}/comm").read().strip() in NOISE:
                n += 1
        except OSError:
            pass
    return n


def host_state():
    try:
        load = open("/proc/loadavg").read().strip()
    except OSError:
        load = "unavailable"
    return {"loadavg": load, "noise_processes": noisy_now()}


def hold_quiet():
    """Waits until two looks a second apart find no noise; counts the hold."""
    t0 = time.time()
    seen = 0
    while seen < 2:
        seen = seen + 1 if noisy_now() == 0 else 0
        time.sleep(1.0)
    HELD_S[0] += time.time() - t0 - 2.0


def quiet_arm(run, tries=6):
    """Runs one arm on a quiet host: held before it, and run again when noise
    appeared while it ran. After `tries` the last run is kept, marked."""
    for attempt in range(tries):
        hold_quiet()
        c = run()
        if noisy_now() == 0:
            c["attempts"] = attempt + 1
            return c
    c["attempts"] = tries
    c["noisy"] = True
    return c


def server_cpu(pid):
    stat = open(f"/proc/{pid}/stat").read()
    f = stat[stat.rindex(")") + 2:].split()
    return (int(f[11]) + int(f[12])) / TICK


def rss_kb(pid):
    for line in open(f"/proc/{pid}/status"):
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    return 0


def pcts(xs):
    s = sorted(xs)
    us = lambda v: round(v * 1e6, 1)
    return {"n": len(s), "p0_us": us(s[0]), "p25_us": us(nearest_rank(s, 25)),
            "p50_us": us(nearest_rank(s, 50)), "p75_us": us(nearest_rank(s, 75)),
            "p95_us": us(nearest_rank(s, 95)), "p99_us": us(nearest_rank(s, 99)),
            "max_us": us(s[-1])}


class TlsConnection(kwp.Connection):
    """`kwp.Connection` over TLS 1.3: the port speaks TLS from its first byte."""

    def __init__(self, host, port, context, timeout=5.0):
        raw = socket.create_connection((host, port), timeout=timeout)
        raw.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock = context.wrap_socket(raw, server_hostname="localhost")
        self.buf = b""
        self.session_id = self.cancel_key = self.capabilities = 0
        self.server_info = ""
        self._next_portal = 0
        self._handshake("dj-s0")


def client_tls_context():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    return ctx


def connect(a, tls):
    if tls:
        return TlsConnection("127.0.0.1", a.port, client_tls_context(), timeout=60)
    return kwp.Connection("127.0.0.1", a.port, timeout=60)


class Server:
    def __init__(self, a, name, cores=1, tls=False):
        self.a, self.name, self.tls = a, name, tls
        data = os.path.join(a.workdir, name + ".db")
        shutil.rmtree(data + ".wal", ignore_errors=True)
        if os.path.exists(data):
            os.remove(data)
        conf = os.path.join(a.workdir, name + ".conf")
        cpus = ",".join(str(SERVER_CPU + k) for k in range(cores))
        with open(conf, "w") as f:
            f.write(f"data_file = {data}\nport = {a.port}\ncores = {cores}\n"
                    f"reactor_cpus = {cpus}\ndurability = group\n"
                    f"buffer_pool_frames = 65536\nlog_file = {name}.log\n"
                    f"log_dir = {a.workdir}\nlog_level = warn\n")
            if tls:
                f.write(f"tls = on\ntls_cert_file = {a.workdir}/dj.crt\n"
                        f"tls_key_file = {a.workdir}/dj.key\n")
        self.err = open(os.path.join(a.workdir, name + ".stderr"), "w")
        self.proc = subprocess.Popen(
            ["taskset", "-c", f"{SERVER_CPU}-{SERVER_CPU + 3}", a.bin, "--config", conf],
            stdout=self.err, stderr=subprocess.STDOUT)
        self.pid = self.proc.pid
        deadline = time.time() + 30
        while True:
            try:
                self.conn = connect(a, tls)
                return
            except (OSError, ssl.SSLError):
                if time.time() > deadline or self.proc.poll() is not None:
                    raise SystemExit(f"{name}: server did not listen on {a.port}")
                time.sleep(0.1)

    def load(self):
        self.conn.execute(f"CREATE TABLE {TABLE} (id int64, a int64, b int64, c varchar(32)) "
                          f"BTREE")
        for lo in range(1, ROWS + 1, 500):
            self.conn.execute(f"INSERT INTO {TABLE} VALUES " + ",".join(
                f"({i},{i % 97},{i * 7},'row{i}')" for i in range(lo, min(lo + 500, ROWS + 1))))

    def stop(self):
        try:
            self.conn.close()
        except OSError:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(60)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.err.close()


def point_arm(conn, pid, seconds, rng):
    lat = []
    cpu0 = server_cpu(pid)
    t1 = time.time() + seconds
    while time.time() < t1 or not lat:
        sql = f"SELECT * FROM {TABLE} WHERE id = {rng.randrange(1, ROWS + 1)}"
        s = time.perf_counter()
        conn.execute(sql)
        lat.append(time.perf_counter() - s)
    cpu = server_cpu(pid) - cpu0
    return {"stmts": len(lat), "sps": round(len(lat) / sum(lat), 1),
            "server_cpu_us_per_stmt": round(cpu * 1e6 / len(lat), 2), "server_cpu_s": round(cpu, 3),
            "lat": pcts(lat)}


def open_idle(a, n, tls=False):
    out = []
    for _ in range(n):
        out.append(connect(a, tls))
    return out


def close_all(conns):
    for c in conns:
        c.close()
    conns.clear()


def settle(pid, expect_gone=False):
    """Lets the server take every close or accept in before a reading."""
    time.sleep(1.0 if expect_gone else 0.5)


# ---- memory ----------------------------------------------------------------

def cell_memory(a, beat, out):
    srv = Server(a, "memory")
    try:
        srv.load()
        settle(srv.pid)
        base = rss_kb(srv.pid)
        res = {"cell": "memory", "rss_kb_base": base, "steps": [], "host": host_state()}
        idle = []
        for n in a.idle:
            beat(f"memory: opening to {n} idle sessions")
            idle.extend(open_idle(a, n - len(idle)))
            settle(srv.pid)
            after_handshake = rss_kb(srv.pid)
            cpu0, t0 = server_cpu(srv.pid), time.time()
            time.sleep(a.idle_window)
            idle_cpu = server_cpu(srv.pid) - cpu0
            window = time.time() - t0
            for c in idle:
                c.execute(f"SELECT * FROM {TABLE} WHERE id = 1")
            settle(srv.pid)
            after_statement = rss_kb(srv.pid)
            res["steps"].append({
                "sessions": n, "rss_kb_after_handshake": after_handshake,
                "rss_kb_after_one_statement": after_statement,
                "kb_per_session_handshake": round((after_handshake - base) / n, 2),
                "kb_per_session_one_statement": round((after_statement - base) / n, 2),
                "idle_window_s": round(window, 2), "idle_cpu_s": round(idle_cpu, 3),
                "idle_cpu_ppm_of_one_cpu": round(idle_cpu / window * 1e6)})
        close_all(idle)
        settle(srv.pid, expect_gone=True)
        res["rss_kb_after_close"] = rss_kb(srv.pid)
        out["cells"].append(res)
    finally:
        srv.stop()


# ---- beside ----------------------------------------------------------------

def cell_beside(a, beat, out):
    srv = Server(a, "beside")
    try:
        srv.load()
        rng = random.Random(20261120)
        point_arm(srv.conn, srv.pid, 0.5, rng)
        arms = [0] + a.idle + [0]
        for rep in range(a.reps):
            idle = []
            for k, n in enumerate(arms):
                if n < len(idle):
                    close_all(idle)
                    settle(srv.pid, expect_gone=True)
                idle.extend(open_idle(a, n - len(idle)))
                settle(srv.pid)
                name = f"idle{n}" + ("_again" if k == len(arms) - 1 else "")
                beat(f"beside rep {rep} {name}")
                c = quiet_arm(lambda: point_arm(srv.conn, srv.pid, a.seconds, rng))
                c.update(cell="beside", arm=name, rep=rep, idle_sessions=n, host=host_state())
                out["cells"].append(c)
            close_all(idle)
            settle(srv.pid, expect_gone=True)
    finally:
        srv.stop()


# ---- churn -----------------------------------------------------------------

def churn_child(port, tls, seconds, q):
    os.sched_setaffinity(0, {CHURN_CPU})

    class A:
        pass
    a = A()
    a.port = port
    n, lat = 0, []
    t1 = time.time() + seconds
    while time.time() < t1:
        s = time.perf_counter()
        c = connect(a, tls)
        c.execute(f"SELECT * FROM {TABLE} WHERE id = {1 + n % ROWS}")
        c.close()
        lat.append(time.perf_counter() - s)
        n += 1
    q.put({"connections": n, "per_s": round(n / seconds, 1), "lat": pcts(lat)})


def cell_churn(a, beat, out, tls):
    name = "churn_tls" if tls else "churn_plain"
    srv = Server(a, name, tls=tls)
    try:
        srv.load()
        rng = random.Random(20261120)
        point_arm(srv.conn, srv.pid, 0.5, rng)
        if tls and a.tls_idle:
            settle(srv.pid)
            base = rss_kb(srv.pid)
            idle = open_idle(a, a.tls_idle, tls=True)
            settle(srv.pid)
            grown = rss_kb(srv.pid)
            out["cells"].append({"cell": name, "arm": "tls_idle_memory",
                                 "sessions": a.tls_idle, "rss_kb_base": base,
                                 "rss_kb_after_handshake": grown,
                                 "kb_per_session_handshake": round((grown - base) / a.tls_idle,
                                                                   2)})
            close_all(idle)
            settle(srv.pid, expect_gone=True)
        def standing(arm):
            child = None
            if arm == "C":
                q = multiprocessing.Queue()
                child = multiprocessing.Process(target=churn_child,
                                                args=(a.port, tls, a.seconds + 0.4, q))
                child.start()
                time.sleep(0.2)
            c = point_arm(srv.conn, srv.pid, a.seconds, rng)
            if child is not None:
                c["churn"] = q.get(timeout=60)
                child.join(30)
            return c

        # What one connect-and-close costs the server: the loop alone, no
        # standing statement beside it.
        def loop_alone():
            q = multiprocessing.Queue()
            cpu0 = server_cpu(srv.pid)
            child = multiprocessing.Process(target=churn_child,
                                            args=(a.port, tls, a.seconds, q))
            child.start()
            got = q.get(timeout=60)
            child.join(30)
            cpu = server_cpu(srv.pid) - cpu0
            return {"churn": got, "server_cpu_us_per_connection":
                    round(cpu * 1e6 / max(1, got["connections"]), 1)}

        for rep in range(a.reps):
            for arm in ("S", "C", "S2"):
                beat(f"{name} rep {rep} {arm}")
                c = quiet_arm(lambda: standing(arm))
                c.update(cell=name, arm=arm, rep=rep, host=host_state())
                out["cells"].append(c)
            beat(f"{name} rep {rep} loop alone")
            c = quiet_arm(loop_alone)
            c.update(cell=name, arm="loop_alone", rep=rep, host=host_state())
            out["cells"].append(c)
    finally:
        srv.stop()


# ---- portal ----------------------------------------------------------------

def capped_first_batch(conn, sql, max_rows):
    """PARSE, BIND, EXECUTE {max_rows} and SYNC of `sql` over portal "cap":
    reads to S_READY without continuing. Returns (seconds, rows in the first
    batch, suspended)."""
    w = kwp._Writer
    batch = (kwp.frame(kwp.C_PARSE, w().s("").text(sql).take())
             + kwp.frame(kwp.C_BIND, w().s("cap").s("").u16(0).take())
             + kwp.frame(kwp.C_EXECUTE, w().s("cap").u32(max_rows).take())
             + kwp.frame(kwp.C_SYNC))
    t0 = time.perf_counter()
    conn.sock.sendall(batch)
    rows, suspended, first = 0, False, None
    while True:
        ftype, _, payload = conn._recv_frame()
        if ftype == kwp.S_ROW_BATCH:
            if first is None:
                first = time.perf_counter() - t0
            rows += struct.unpack("<H", payload[:2])[0]
        elif ftype == kwp.S_PORTAL_SUSPENDED:
            suspended = True
        elif ftype == kwp.S_ERROR:
            raise conn._error(payload)
        elif ftype == kwp.S_READY:
            return first, rows, suspended


def close_portal(conn):
    conn.sock.sendall(kwp.frame(kwp.C_CLOSE, kwp._Writer().u8(2).s("cap").take())
                      + kwp.frame(kwp.C_SYNC))
    while conn._recv_frame()[0] != kwp.S_READY:
        pass


def cell_portal(a, beat, out):
    """What a capped portal costs: `max_rows = 100` of a whole-relation
    SELECT against the same statement with `LIMIT 100`."""
    srv = Server(a, "portal")
    try:
        srv.load()
        conn = srv.conn
        whole = f"SELECT * FROM {TABLE}"
        limited = f"SELECT * FROM {TABLE} LIMIT 100"
        capped_first_batch(conn, whole, 100)
        close_portal(conn)
        settle(srv.pid)
        base = rss_kb(srv.pid)
        held = []
        for k in range(a.portals):
            c = connect(a, False)
            first, rows, suspended = capped_first_batch(c, whole, 100)
            held.append(c)
            if k == 0:
                one = {"first_batch_ms": round(first * 1e3, 2), "rows": rows,
                       "suspended": suspended}
        settle(srv.pid)
        grown = rss_kb(srv.pid)
        res = {"cell": "portal", "arm": "held_portals", "portals": a.portals,
               "relation_rows": ROWS, "max_rows": 100, "first": one, "rss_kb_base": base,
               "rss_kb_with_portals": grown,
               "kb_per_suspended_portal": round((grown - base) / a.portals, 1),
               "host": host_state()}
        out["cells"].append(res)
        close_all(held)
        for name, sql, cap in (("capped_whole", whole, 100), ("limit_100", limited, 0)):
            def arm():
                lat = []
                cpu0 = server_cpu(srv.pid)
                t1 = time.time() + a.seconds
                while time.time() < t1 or not lat:
                    first, rows, _ = capped_first_batch(conn, sql, cap)
                    lat.append(first)
                    close_portal(conn)
                cpu = server_cpu(srv.pid) - cpu0
                return {"stmts": len(lat), "rows_first_batch": rows,
                        "server_cpu_us_per_stmt": round(cpu * 1e6 / len(lat), 1),
                        "first_batch": pcts(lat)}
            beat(f"portal {name}")
            c = quiet_arm(arm)
            c.update(cell="portal", arm=name, host=host_state())
            out["cells"].append(c)
    finally:
        srv.stop()


# ---- begin -----------------------------------------------------------------

BEGIN = kwp.frame(kwp.C_TXN_BEGIN, b"\x00")
COMMIT = kwp.frame(kwp.C_TXN_COMMIT)
ABORT = kwp.frame(kwp.C_TXN_ABORT)
SYNC = kwp.frame(kwp.C_SYNC)


def error_text(payload):
    e = kwp.Connection._error(payload)
    return {"category": e.code >> 16, "detail": e.code & 0xFFFF, "retryable": e.retryable,
            "message": e.message[:160]}


def hold_transactions(a, n):
    """Opens `n` sessions, each sends BEGIN and keeps what it got."""
    conns, refused, sample = [], 0, None
    for _ in range(n):
        c = connect(a, False)
        try:
            c.begin()
        except kwp.KwpError as e:
            refused += 1
            if sample is None:
                sample = {"category": e.code >> 16, "detail": e.code & 0xFFFF,
                          "retryable": e.retryable, "message": e.message[:200]}
        conns.append(c)
    return conns, refused, sample


def answer(fn):
    try:
        fn()
        return "ok"
    except kwp.KwpError as e:
        return {"category": e.code >> 16, "detail": e.code & 0xFFFF, "retryable": e.retryable,
                "message": e.message[:200]}


def begin_loop(a, conns, seconds):
    """Every session loops BEGIN, one INSERT, COMMIT; counts refused BEGINs."""
    sel = selectors.DefaultSelector()
    state = {}
    next_id = [10_000_000]

    def insert_batch():
        next_id[0] += 1
        sql = f"INSERT INTO {TABLE} VALUES ({next_id[0]},1,2,'x')"
        return (kwp.frame(kwp.C_PARSE, kwp._Writer().s("").text(sql).take()) + kwp._QUERY_TAIL)

    for c in conns:
        c.sock.setblocking(True)
        state[c.sock.fileno()] = [c, "begin", b""]
        sel.register(c.sock, selectors.EVENT_READ, c)
        c.sock.sendall(BEGIN)
    begins = ok = refused = commits = failed_other = 0
    sample = None
    t1 = time.time() + seconds
    stop = False
    live = len(conns)
    while live > 0:
        if time.time() >= t1:
            stop = True
        for key, _ in sel.select(timeout=0.2):
            c = key.data
            st = state[c.sock.fileno()]
            chunk = c.sock.recv(65536)
            if not chunk:
                sel.unregister(c.sock)
                live -= 1
                continue
            st[2] += chunk
            while len(st[2]) >= 8:
                (length,) = struct.unpack("<I", st[2][:4])
                if len(st[2]) < 4 + length:
                    break
                ftype, payload = st[2][4], st[2][8:4 + length]
                st[2] = st[2][4 + length:]
                phase = st[1]
                if phase == "begin":
                    begins += 1
                    if ftype == kwp.S_TXN_OK:
                        ok += 1
                        st[1] = "insert"
                        c.sock.sendall(insert_batch())
                    elif ftype == kwp.S_ERROR:
                        refused += 1
                        if sample is None:
                            sample = error_text(payload)
                        st[1] = "resync"
                        c.sock.sendall(SYNC)
                elif phase == "resync":
                    if ftype == kwp.S_READY:
                        if stop:
                            st[1] = "done"
                            sel.unregister(c.sock)
                            live -= 1
                        else:
                            st[1] = "begin"
                            c.sock.sendall(BEGIN)
                elif phase == "insert":
                    if ftype == kwp.S_ERROR:
                        failed_other += 1
                    if ftype == kwp.S_READY:
                        st[1] = "commit"
                        c.sock.sendall(COMMIT)
                elif phase == "commit":
                    if ftype == kwp.S_TXN_OK:
                        commits += 1
                        if stop:
                            st[1] = "done"
                            sel.unregister(c.sock)
                            live -= 1
                        else:
                            st[1] = "begin"
                            c.sock.sendall(BEGIN)
                    elif ftype == kwp.S_ERROR:
                        failed_other += 1
                        st[1] = "resync"
                        c.sock.sendall(SYNC)
    return {"begins": begins, "begin_ok": ok, "begin_refused": refused, "commits": commits,
            "other_failures": failed_other,
            "refused_share": round(refused / max(1, begins), 4), "refusal": sample}


def cell_begin(a, beat, out):
    srv = Server(a, "begin1")
    try:
        srv.load()
        beat(f"begin: {a.begin_sessions} sessions hold a transaction, cores = 1")
        conns, refused, sample = hold_transactions(a, a.begin_sessions)
        probe = connect(a, False)
        res = {"cell": "begin", "arm": "hold_cores1", "sessions": a.begin_sessions,
               "begin_refused": refused, "refusal": sample,
               "while_held": {
                   "select": answer(lambda: probe.execute(f"SELECT * FROM {TABLE} WHERE id = 1")),
                   "begin_read_only": answer(lambda: (probe.execute("BEGIN READ ONLY"),
                                                      probe.execute("COMMIT"))),
                   "autocommit_insert": answer(lambda: probe.execute(
                       f"INSERT INTO {TABLE} VALUES (20000001,1,2,'x')")),
                   "begin": answer(lambda: (probe.begin(), probe.rollback()))},
               "host": host_state()}
        out["cells"].append(res)
        for c in conns:
            try:
                c.rollback()
            except (kwp.KwpError, OSError):
                pass
        probe.close()
        beat(f"begin: {a.begin_sessions} sessions loop BEGIN, INSERT, COMMIT")
        cpu0 = server_cpu(srv.pid)
        looped = begin_loop(a, conns, a.seconds * 2)
        looped.update(cell="begin", arm="loop_cores1", sessions=a.begin_sessions,
                      seconds=a.seconds * 2, server_cpu_s=round(server_cpu(srv.pid) - cpu0, 3),
                      host=host_state())
        out["cells"].append(looped)
        close_all(conns)
    finally:
        srv.stop()
    srv = Server(a, "begin2", cores=2)
    try:
        n = 2 * 1024 - a.begin_margin
        beat(f"begin: {n} sessions hold a transaction, cores = 2")
        conns, refused, sample = hold_transactions(a, n)
        out["cells"].append({"cell": "begin", "arm": "hold_cores2", "sessions": n,
                             "slots": 2048, "begin_refused": refused, "refusal": sample,
                             "host": host_state()})
        close_all(conns)
    finally:
        srv.stop()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bin", required=True)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--port", type=int, default=15671)
    p.add_argument("--cells", default="memory,beside,churn,begin,portal")
    p.add_argument("--idle", default="1000,10000")
    p.add_argument("--tls-idle", type=int, default=1000)
    p.add_argument("--idle-window", type=float, default=30.0)
    p.add_argument("--begin-sessions", type=int, default=1500)
    p.add_argument("--begin-margin", type=int, default=148)
    p.add_argument("--portals", type=int, default=20)
    p.add_argument("--reps", type=int, default=2)
    p.add_argument("--seconds", type=float, default=3.0)
    p.add_argument("--heartbeat")
    p.add_argument("--status")
    a = p.parse_args()
    a.idle = [int(x) for x in a.idle.split(",") if x]
    os.makedirs(a.workdir, exist_ok=True)
    os.sched_setaffinity(0, {DRIVER_CPU})

    def beat(line):
        if a.heartbeat:
            open(a.heartbeat, "w").write(str(time.time()))
        if a.status:
            open(a.status, "a").write(time.strftime("%H:%M:%S ", time.gmtime()) + line + "\n")
        print(line, flush=True)

    out = {"args": vars(a), "bin_sha256": hashlib.sha256(open(a.bin, "rb").read()).hexdigest(),
           "cells": [], "skipped": []}

    def save():
        json.dump(out, open(a.out, "w"), indent=1)

    cells = a.cells.split(",")
    if "memory" in cells:
        cell_memory(a, beat, out)
        save()
    if "beside" in cells:
        cell_beside(a, beat, out)
        save()
    if "churn" in cells:
        cell_churn(a, beat, out, tls=False)
        save()
        crt = os.path.join(a.workdir, "dj.crt")
        made = subprocess.run(
            ["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
             "-nodes", "-keyout", os.path.join(a.workdir, "dj.key"), "-out", crt, "-days", "2",
             "-subj", "/CN=localhost"], capture_output=True, text=True)
        if made.returncode != 0:
            out["skipped"].append({"cell": "churn_tls", "why": made.stderr[-300:]})
        else:
            try:
                cell_churn(a, beat, out, tls=True)
            except SystemExit as e:
                out["skipped"].append({"cell": "churn_tls", "why": str(e)})
        save()
    if "begin" in cells:
        cell_begin(a, beat, out)
        save()
    if "portal" in cells:
        cell_portal(a, beat, out)
        save()
    out["held_for_quiet_s"] = round(HELD_S[0], 1)
    save()
    beat("driver finished")


if __name__ == "__main__":
    main()
