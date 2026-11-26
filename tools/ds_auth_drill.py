#!/usr/bin/env python3
"""ds_auth_drill - the reference client against a server that authenticates (DS-S1).

Starts a `kds_server` with `auth = scram` and `tls = on` on a scratch
volume and drives `tools/kwp.py`, `tools/ckdbs_cli.py` and
`tools/kds_backup.py` at it. Run by hand (DS-Q8): it needs the `openssl`
command for the server's certificate, which `ctest` does not ask of a host.

    tools/ds_auth_drill.py [--server build/kds_server] [--port 15970]

Exit 0 only if every check passed; each prints `ok` or `FAIL` with a name.
"""

import argparse
import os
import shutil
import signal
import ssl
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ADMIN, ADMIN_PASSWORD = "drill_admin", "correct horse, battery=staple"
READER, READER_PASSWORD = "drill_reader", "only-reads"


class Drill:
    def __init__(self):
        self.failed = 0

    def check(self, name, cond, detail=""):
        print(("ok    " if cond else "FAIL  ") + name + (f": {detail}" if detail and not cond else ""))
        if not cond:
            self.failed += 1

    def refused(self, name, fn, want, *kinds):
        """`fn` must raise one of `kinds` whose text holds `want`."""
        try:
            fn()
        except kinds as e:
            self.check(name, want.lower() in str(e).lower(), f"refused, but with: {e!r}")
        else:
            self.check(name, False, "was admitted")


def add_user(binary, users, name, password, role):
    # The provisioning mode reads the password twice from stdin.
    subprocess.run([binary, "--add-user", name, "--role", role, "--users-file", users],
                   input=f"{password}\n{password}\n", text=True, check=True,
                   capture_output=True)


def start(binary, root, port, tls):
    config = os.path.join(root, f"kds-{port}.conf")
    lines = [f"data_file = {root}/kds-{port}.db", f"wal_dir = {root}/wal-{port}", f"port = {port}",
             "buffer_pool_frames = 4096", "durability = relaxed",
             f"log_dir = {root}", f"log_file = kds-{port}.log",
             "auth = scram", f"users_file = {root}/users"]
    if tls:
        lines += ["tls = on", f"tls_cert_file = {root}/server.crt",
                  f"tls_key_file = {root}/server.key"]
    with open(config, "w") as f:
        f.write("\n".join(lines) + "\n")
    proc = subprocess.Popen([binary, "--config", config], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    context = kwp.tls_context(os.path.join(root, "server.crt")) if tls else None
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited: {proc.stdout.read()}")
        try:
            kwp.Connection("localhost", port, timeout=2, user=ADMIN, password=ADMIN_PASSWORD,
                           tls=context).close()
            return proc
        except ConnectionRefusedError:
            time.sleep(0.1)
    raise RuntimeError("server did not listen")


def stop(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        proc.wait(60)


def tool(script, port, *args, password=None, stdin=subprocess.DEVNULL):
    env = dict(os.environ)
    env.pop("KDS_PASSWORD", None)
    if password is not None:
        env["KDS_PASSWORD"] = password
    return subprocess.run([sys.executable, os.path.join(HERE, script), "--host", "localhost",
                           "--port", str(port), *args],
                          capture_output=True, text=True, env=env, stdin=stdin)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--server", default=os.path.join(REPO, "build", "kds_server"))
    p.add_argument("--port", type=int, default=15970)
    args = p.parse_args()

    d = Drill()
    root = tempfile.mkdtemp(prefix="ds-auth-drill-")
    procs = []
    try:
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
                        "ec_paramgen_curve:P-256", "-nodes", "-days", "1", "-subj",
                        "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                        "-keyout", f"{root}/server.key", "-out", f"{root}/server.crt"],
                       check=True, capture_output=True)
        users = os.path.join(root, "users")
        add_user(args.server, users, ADMIN, ADMIN_PASSWORD, "admin")
        add_user(args.server, users, READER, READER_PASSWORD, "readonly")

        # ---- A plaintext port that authenticates ----------------------------
        plain = args.port
        procs.append(start(args.server, root, plain, tls=False))

        c = kwp.Connection("localhost", plain, user=ADMIN, password=ADMIN_PASSWORD)
        _, _, tag, _ = c.execute("CREATE TABLE drill (id int64, v int64)")
        d.check("scram: an admin's statement runs", tag is not None, f"tag {tag!r}")
        c.execute("INSERT INTO drill VALUES (1, 10)")
        _, rows, _, _ = c.execute("SELECT v FROM drill WHERE id = 1")
        # `execute` answers raw cells: an int64 is 8 little-endian bytes.
        d.check("scram: the session answers",
                len(rows) == 1 and int.from_bytes(rows[0][0], "little") == 10, repr(rows))
        d.check("scram: the session has an id", c.session_id != 0)
        c.close()

        d.refused("scram: a wrong password is refused",
                  lambda: kwp.Connection("localhost", plain, user=ADMIN, password="wrong"),
                  "refused authentication", ConnectionError)
        d.refused("scram: an unknown user is refused",
                  lambda: kwp.Connection("localhost", plain, user="nobody", password="x"),
                  "refused authentication", ConnectionError)
        d.refused("scram: no credentials are refused",
                  lambda: kwp.Connection("localhost", plain), "requires authentication",
                  kwp.KwpError)

        r = kwp.Connection("localhost", plain, user=READER, password=READER_PASSWORD)
        d.refused("scram: a readonly role is held to its class",
                  lambda: r.execute("INSERT INTO drill VALUES (2, 20)"), "needs readwrite", kwp.KwpError)
        r.close()

        # A server-first that is not the server's: the client must not send
        # its proof to a nonce it did not start, nor accept a low `i=`.
        class Fake(kwp.Connection):
            def __init__(self, reply):  # no socket: only `_scram` is driven
                self.reply, self.sent = reply, []

            def _auth_round(self, line):
                self.sent.append(line)
                return self.reply(line)

        d.refused("scram: a nonce that does not extend the client's is refused",
                  lambda: Fake(lambda _: "r=someoneelse,s=c2FsdA==,i=4096")._scram("u", "p"),
                  "nonce", ConnectionError)
        d.refused("scram: an iteration count under the floor is refused",
                  lambda: Fake(lambda line: "r=%sX,s=c2FsdA==,i=1" %
                               line.split("r=", 1)[1])._scram("u", "p"),
                  "iteration", ConnectionError)
        d.refused("scram: a server-first led by a mandatory extension is refused",
                  lambda: Fake(lambda line: "m=x,r=%sX,s=c2FsdA==,i=4096" %
                               line.split("r=", 1)[1])._scram("u", "p"),
                  "malformed", ConnectionError)
        d.refused("scram: a server-first repeating an attribute is refused",
                  lambda: Fake(lambda line: "r=%sX,r=%sY,s=c2FsdA==,i=4096" %
                               ((line.split("r=", 1)[1],) * 2))._scram("u", "p"),
                  "malformed", ConnectionError)
        fake = Fake(lambda line: ("r=%sX,s=c2FsdA==,i=4096" % line.split("r=", 1)[1])
                    if line.startswith("AUTH SCRAM") else "v=AAAA")
        d.refused("scram: a server signature that does not verify is refused",
                  lambda: fake._scram("u", "p"), "verifier", ConnectionError)

        # ---- The tools --------------------------------------------------------
        out = tool("ckdbs_cli.py", plain, "--user", ADMIN, "SELECT", "v", "FROM", "drill",
                   password=ADMIN_PASSWORD)
        d.check("cli: --user with KDS_PASSWORD", out.returncode == 0 and "10" in out.stdout,
                out.stdout + out.stderr)
        d.check("cli: the password is in no output",
                ADMIN_PASSWORD not in out.stdout + out.stderr)
        out = tool("ckdbs_cli.py", plain, "--user", ADMIN, "SELECT", "v", "FROM", "drill")
        d.check("cli: --user with no password and no terminal is refused by name",
                out.returncode != 0 and "KDS_PASSWORD" in out.stderr, out.stdout + out.stderr)

        out = tool("ckdbs_cli.py", plain, "--text", "--user", ADMIN, "PING")
        d.check("cli: --text with --user is refused before any password is asked for",
                out.returncode != 0 and "--text" in out.stderr and "KDS_PASSWORD" not in out.stderr,
                out.stdout + out.stderr)

        target = os.path.join(root, "backup-plain")
        out = tool("kds_backup.py", plain, "--user", ADMIN, "base", "--target", target,
                   password=ADMIN_PASSWORD)
        d.check("backup: a base backup through an authenticated session",
                out.returncode == 0 and os.path.exists(os.path.join(target, "backup_manifest")),
                out.stdout + out.stderr)
        out = tool("kds_backup.py", plain, "base", "--target", os.path.join(root, "backup-anon"))
        d.check("backup: without --user it is refused",
                out.returncode != 0 and "requires authentication" in out.stderr,
                out.stdout + out.stderr)
        out = tool("kds_backup.py", plain, "--user", READER, "base", "--target",
                   os.path.join(root, "backup-reader"), password=READER_PASSWORD)
        d.check("backup: a readonly user's BACKUP START is refused",
                out.returncode != 0 and "needs admin" in out.stderr,
                out.stdout + out.stderr)
        stop(procs.pop())

        # ---- A TLS port that authenticates -----------------------------------
        secure = args.port + 1
        procs.append(start(args.server, root, secure, tls=True))
        ca = os.path.join(root, "server.crt")

        c = kwp.Connection("localhost", secure, user=ADMIN, password=ADMIN_PASSWORD,
                           tls=kwp.tls_context(ca))
        d.check("tls: the channel is TLS 1.3", c.sock.version() == "TLSv1.3", str(c.sock.version()))
        c.execute("CREATE TABLE drill (id int64, v int64)")
        c.execute("INSERT INTO drill VALUES (1, 11)")
        c.close()

        d.refused("tls: an unverified certificate is refused by default",
                  lambda: kwp.Connection("localhost", secure, user=ADMIN,
                                         password=ADMIN_PASSWORD, tls=kwp.tls_context()),
                  "certificate", ssl.SSLError)
        d.refused("tls: a name the certificate does not carry is refused",
                  lambda: kwp.Connection("127.0.0.1", secure, user=ADMIN,
                                         password=ADMIN_PASSWORD, tls=kwp.tls_context(ca)),
                  "mismatch", ssl.SSLError)
        c = kwp.Connection("127.0.0.1", secure, user=ADMIN, password=ADMIN_PASSWORD,
                           tls=kwp.tls_context(insecure=True))
        d.check("tls: --tls-insecure's context connects unverified", c.session_id != 0)
        c.close()
        d.refused("tls: a plaintext client on the TLS port gets no session",
                  lambda: kwp.Connection("localhost", secure, timeout=5, user=ADMIN,
                                         password=ADMIN_PASSWORD),
                  "", ConnectionError)  # an EOF or a reset; a timeout is not one

        out = tool("ckdbs_cli.py", secure, "--user", ADMIN, "--tls-ca", ca, "SELECT", "v", "FROM",
                   "drill", password=ADMIN_PASSWORD)
        d.check("cli: --tls-ca and --user", out.returncode == 0 and "11" in out.stdout,
                out.stdout + out.stderr)
        out = tool("ckdbs_cli.py", secure, "--user", ADMIN, "--tls-ca", ca, "--tls-insecure",
                   "PING", password=ADMIN_PASSWORD)
        d.check("cli: --tls-ca with --tls-insecure is refused",
                out.returncode != 0 and "contradict" in out.stderr,
                out.stdout + out.stderr)
        target = os.path.join(root, "backup-tls")
        out = tool("kds_backup.py", secure, "--user", ADMIN, "--tls-ca", ca, "base", "--target",
                   target, password=ADMIN_PASSWORD)
        d.check("backup: a base backup over TLS and SCRAM",
                out.returncode == 0 and os.path.exists(os.path.join(target, "backup_manifest")),
                out.stdout + out.stderr)
        out = tool("kds_backup.py", secure, "verify", target)
        d.check("backup: that backup verifies", out.returncode == 0, out.stdout + out.stderr)
    finally:
        for proc in procs:
            stop(proc)
        shutil.rmtree(root, ignore_errors=True)

    print(f"{d.failed} failed" if d.failed else "all passed")
    return 1 if d.failed else 0


if __name__ == "__main__":
    sys.exit(main())
