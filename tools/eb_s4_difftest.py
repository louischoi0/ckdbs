#!/usr/bin/env python3
"""EB-S4's differential: the upsert shapes KDS and PostgreSQL 18 both accept,
run on both and compared.

`instructions/v3.2.0/workorder-eb-key-forms-in-the-executor.md` §5, EB-S4:
"the differential against PostgreSQL 18 for the shapes both accept". A
throwaway kds_server (one core, relaxed) spoken to over KWP, and a
throwaway `postgres:18` container (podman) spoken to through `psql` in the
container. Each case is a schema per engine, then the same statements; after
each statement both engines' outcome - ok or refused - and the relation's
rows are compared. Exits 1 on any mismatch, printing it.

Usage: eb_s4_difftest.py --bin KDS_SERVER --workdir D [--image postgres:18]
"""
import argparse, os, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402

CASES = [
    {
        "name": "a unique index of two columns",
        "kds": ["CREATE TABLE u (id int64, a int64, b int64, v int64) BTREE",
                "CREATE UNIQUE INDEX u_ab ON u (a, b)"],
        "pg": ["CREATE TABLE u (id bigint PRIMARY KEY, a bigint NOT NULL, b bigint NOT NULL, "
               "v bigint NOT NULL)",
               "CREATE UNIQUE INDEX u_ab ON u (a, b)"],
        "table": "u", "select": "SELECT id, a, b, v FROM u ORDER BY id",
        "statements": [
            "INSERT INTO u VALUES (1, 1, 1, 10), (2, 1, 2, 20), (3, 2, 1, 30)",
            "INSERT INTO u VALUES (4, 1, 1, 99), (5, 3, 3, 50) ON CONFLICT (a, b) DO NOTHING",
            "INSERT INTO u VALUES (6, 1, 2, 5), (7, 4, 4, 40) ON CONFLICT (b, a) DO UPDATE "
            "SET v = u.v + EXCLUDED.v",
            "INSERT INTO u VALUES (8, 9, 9, 1), (9, 9, 9, 2) ON CONFLICT (a, b) DO UPDATE "
            "SET v = 0",
            "INSERT INTO u VALUES (10, 9, 9, 1), (11, 9, 9, 2) ON CONFLICT (a, b) DO NOTHING",
            "INSERT INTO u VALUES (12, 2, 1, 1) ON CONFLICT (a) DO NOTHING",
        ],
    },
    {
        "name": "a primary key of two narrow integers (KDS: embedded)",
        "kds": ["CREATE TABLE e (a int32, b int16, v int64, PRIMARY KEY (a, b))"],
        "pg": ["CREATE TABLE e (a integer, b smallint, v bigint NOT NULL, PRIMARY KEY (a, b))"],
        "table": "e", "select": "SELECT a, b, v FROM e ORDER BY a, b",
        "statements": [
            "INSERT INTO e VALUES (1, 2, 10), (2, 1, 20), (-5, -7, 30)",
            "INSERT INTO e VALUES (1, 2, 99), (3, 3, 30) ON CONFLICT (a, b) DO NOTHING",
            "INSERT INTO e VALUES (1, 2, 5), (4, 4, 40) ON CONFLICT (b, a) DO UPDATE "
            "SET v = e.v + EXCLUDED.v",
            "INSERT INTO e VALUES (2, 1, 7), (-5, -7, 8), (6, 6, 60) ON CONFLICT DO NOTHING",
            "INSERT INTO e VALUES (2147483647, 32766, 1), (-2147483648, -32768, 2) "
            "ON CONFLICT (a, b) DO NOTHING",
            "INSERT INTO e VALUES (7, 7, 1), (7, 7, 2) ON CONFLICT (a, b) DO UPDATE SET v = 0",
            "INSERT INTO e VALUES (1, 2, 1) ON CONFLICT (a, b) DO UPDATE SET v = EXCLUDED.v * 3",
        ],
    },
]


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Pg:
    def __init__(self, image):
        self.name = f"eb-s4-difftest-{os.getpid()}"
        subprocess.run(["podman", "run", "--rm", "-d", "--name", self.name, "-e",
                        "POSTGRES_PASSWORD=eb", "-e", "POSTGRES_HOST_AUTH_METHOD=trust", image],
                       check=True, capture_output=True)
        deadline = time.time() + 60
        while subprocess.run(["podman", "exec", self.name, "pg_isready", "-U", "postgres"],
                             capture_output=True).returncode != 0:
            if time.time() > deadline:
                raise SystemExit("postgres did not start")
            time.sleep(0.5)
        time.sleep(1.0)
        self.version = self.run("SHOW server_version")[1]

    def run(self, sql):
        r = subprocess.run(["podman", "exec", self.name, "psql", "-U", "postgres", "-At", "-F",
                            ",", "-v", "ON_ERROR_STOP=1", "-c", sql],
                           capture_output=True, text=True)
        return (r.returncode == 0, r.stdout.strip() if r.returncode == 0 else r.stderr.strip())

    def close(self):
        subprocess.run(["podman", "rm", "-f", self.name], capture_output=True)


class Kds:
    def __init__(self, binary, workdir):
        self.dir = workdir
        shutil.rmtree(workdir, ignore_errors=True)
        os.makedirs(workdir)
        self.port = free_port()
        conf = os.path.join(workdir, "kds.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {workdir}/kds.db\nbuffer_pool_frames = 8192\ncores = 1\n"
                    f"port = {self.port}\ndurability = relaxed\nlog_dir = {workdir}\n"
                    f"log_level = warn\n")
        self.proc = subprocess.Popen([binary, "--config", conf], stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        deadline = time.time() + 30
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", self.port, timeout=60)
                break
            except OSError:
                if time.time() > deadline or self.proc.poll() is not None:
                    raise SystemExit("kds_server did not listen")
                time.sleep(0.1)

    def run(self, sql):
        try:
            fields, rows, tag, _ = self.conn.execute(sql)
        except kwp.KwpError as exc:
            return (False, str(exc))
        if not fields:
            return (True, tag)
        return (True, "\n".join(",".join(kwp.render_value(f, v) for f, v in zip(fields, row))
                                for row in rows))

    def close(self):
        self.conn.close()
        self.proc.terminate()
        self.proc.wait(30)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--image", default="docker.io/library/postgres:18")
    a = ap.parse_args()
    pg = Pg(a.image)
    kds = Kds(a.bin, a.workdir)
    mismatches = 0
    try:
        print(f"postgres {pg.version}")
        for case in CASES:
            for sql in case["kds"]:
                ok, why = kds.run(sql)
                assert ok, (sql, why)
            for sql in case["pg"]:
                ok, why = pg.run(sql)
                assert ok, (sql, why)
            print(f"== {case['name']}")
            for sql in case["statements"]:
                k_ok, k_out = kds.run(sql)
                p_ok, p_out = pg.run(sql)
                k_rows = kds.run(case["select"])[1]
                p_rows = pg.run(case["select"])[1]
                same = k_ok == p_ok and k_rows == p_rows
                mismatches += 0 if same else 1
                print(f"  {'same' if same else 'MISMATCH'}: {sql}")
                print(f"    kds {'ok' if k_ok else 'refused'}: {k_out[:160]}")
                print(f"    pg  {'ok' if p_ok else 'refused'}: {p_out[:160]}")
                if not same:
                    print(f"    kds rows: {k_rows!r}\n    pg  rows: {p_rows!r}")
    finally:
        kds.close()
        pg.close()
    print(f"mismatches: {mismatches}")
    sys.exit(1 if mismatches else 0)


if __name__ == "__main__":
    main()
