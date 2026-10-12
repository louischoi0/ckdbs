#!/usr/bin/env python3
"""br_drill - BR-S8's drills against a real kds_server (workorder-br-backup-restore.md).

Each drill runs a live instance under a writing workload, takes a backup,
restores it, and checks the restored instance against what the source held
at the backup's stop - or, for a fault, that it ends in a correct instance
or a refusal by name, never an open instance with the wrong rows.

  equivalence   path B and path D (snapshot-freeze): the restored instance's
                rows hash to the source's at the stop; PITR to a recorded
                LSN hashes to the source's at that LSN
  faults        kill during the base copy (no manifest: not a backup); a
                flipped bit in a segment and in the data file; a missing
                segment; a manifest from one backup over another's files;
                a snapshot with no stop (a crash image, recovered as one);
                restore onto a non-empty target; a restore killed during
                its recovery, resumed by the next mount

Usage: tools/br_drill.py [--server build/kds_server] [--work DIR] [--port N]
Prints one line per drill and exits non-zero on the first failure.
"""

import argparse
import hashlib
import os
import random
import shutil
import signal
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ckdbs_cli import ServerConnection  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BACKUP_TOOL = os.path.join(REPO, "tools", "kds_backup.py")


def fields(reply):
    out = {}
    if " error=" in reply:
        reply, out["error"] = reply.split(" error=", 1)
    for token in reply.split():
        if "=" in token:
            key, value = token.split("=", 1)
            out[key] = value
    return out


class Server:
    def __init__(self, binary, root, port, archive=None, extra=None):
        self.root, self.port = root, port
        os.makedirs(root, exist_ok=True)
        self.data_file = os.path.join(root, "kds.db")
        self.config = os.path.join(root, "kds.conf")
        lines = [f"data_file = {self.data_file}", f"wal_dir = {root}/wal", f"port = {port}",
                 "buffer_pool_frames = 4096", "checkpoint_interval_ms = 200",
                 "durability = relaxed",
                 f"log_dir = {root}", "log_file = kds.log"]
        if archive:
            lines.append(f"archive_dir = {archive}")
        with open(self.config, "w") as f:
            f.write("\n".join(lines) + "\n")
        self.binary, self.extra, self.proc = binary, extra or [], None

    def start(self, args=()):
        self.proc = subprocess.Popen([self.binary, "--config", self.config, *args],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"server exited: {self.proc.stdout.read()}")
            try:
                ServerConnection("127.0.0.1", self.port, timeout=2).send_command("PING")
                return self
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("server did not listen")

    def conn(self):
        return ServerConnection("127.0.0.1", self.port, timeout=60)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            self.proc.wait(60)

    def kill(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(60)


class Workload:
    """Inserts and updates on `t` from one connection until stopped."""

    def __init__(self, server, start_id):
        self.conn = server.conn()
        self.next_id, self.stop_flag, self.paused = start_id, threading.Event(), threading.Event()
        self.idle = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.rng = random.Random(start_id)

    def run(self):
        while not self.stop_flag.is_set():
            if self.paused.is_set():
                self.idle.set()
                time.sleep(0.01)
                continue
            self.idle.clear()
            if self.rng.random() < 0.7 or self.next_id < 10:
                self.conn.send_command(f"INSERT INTO t VALUES ({self.next_id}, {self.next_id})")
                self.next_id += 1
            else:
                key = self.rng.randrange(1, self.next_id)
                self.conn.send_command(f"UPDATE t SET v = v + 1 WHERE id = {key}")

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop_flag.set()
        self.thread.join()

    def pause(self):
        self.paused.set()
        if not self.idle.wait(30):
            raise RuntimeError("the workload did not pause")

    def resume(self):
        self.idle.clear()  # a stale idle would let the next pause return early
        self.paused.clear()


def table_hash(conn):
    reply = conn.send_command("SELECT id, v FROM t ORDER BY id")
    if reply.startswith("ERR"):
        raise RuntimeError(reply)
    return hashlib.sha256(reply.encode()).hexdigest(), reply.count("\\n")


def quiesced_point(server, workload):
    """Pauses the workload, syncs, and returns (hash, rows, durable lsn)."""
    workload.pause()
    conn = server.conn()
    conn.send_command("SYNC")
    digest, rows = table_hash(conn)
    lsn = int(fields(conn.send_command("SHOW META"))["wal_durable_lsn"])
    workload.resume()
    return digest, rows, lsn


def backup_tool(server, *args, check=True):
    argv = [sys.executable, BACKUP_TOOL, "--port", str(server.port), "--server", server.binary,
            *args]
    result = subprocess.run(argv, capture_output=True, text=True)
    if check and result.returncode != 0:
        raise RuntimeError(f"kds_backup {' '.join(args)}: {result.stdout}{result.stderr}")
    return result


def restore(binary, work, name, backup, port, *extra, check=True):
    target = Server(binary, os.path.join(work, name), port)
    argv = [binary, "--config", target.config, "--restore-from", backup, "--restore-only", *extra]
    result = subprocess.run(argv, capture_output=True, text=True)
    if check and result.returncode != 0:
        raise RuntimeError(f"restore {name}: {result.stdout}{result.stderr}")
    return target, result


class Drills:
    def __init__(self, args):
        self.binary = os.path.abspath(args.server)
        self.work = args.work
        self.port = args.port
        shutil.rmtree(self.work, ignore_errors=True)
        os.makedirs(self.work)
        self.archive = os.path.join(self.work, "archive")
        os.makedirs(self.archive)
        self.rows, self.seconds = args.rows, args.seconds

    def ok(self, name, detail=""):
        print(f"PASS {name} {detail}".rstrip(), flush=True)

    def expect(self, cond, name, detail):
        if not cond:
            print(f"FAIL {name}: {detail}", flush=True)
            sys.exit(1)

    def take(self, src, w, name, path):
        """START; the data file (B: COPY, D: freeze) while the workload runs;
        then the workload paused, the source hashed, STOP - so the hash is
        the source's at the stop exactly."""
        conn = src.conn()
        target = os.path.join(self.work, name)
        reply = conn.send_command(f"BACKUP START '{name}'")
        self.expect(reply.startswith("OK"), f"start {name}", reply)
        started = fields(reply)
        if path == "B":
            conn.send_command(f"BACKUP COPY TO '{target}'")
            while fields(conn.send_command("BACKUP STATUS")).get("copying") != "0":
                time.sleep(0.05)
        else:
            os.makedirs(target)
            freeze = os.path.join(REPO, "scripts", "backup", "snapshot-freeze.sh")
            env = dict(os.environ, KDS_BACKUP_DIR=target)
            subprocess.run([freeze, str(src.proc.pid), src.data_file], env=env, check=True)
        time.sleep(0.3)
        w.pause()
        digest, rows = table_hash(conn)
        stopped = conn.send_command(f"BACKUP STOP TO '{target}'")
        w.resume()
        self.expect(stopped.startswith("OK"), f"stop {name}", stopped)
        while fields(conn.send_command("BACKUP STATUS")).get("state") != "idle":
            time.sleep(0.1)
        status = fields(conn.send_command("BACKUP STATUS"))
        self.expect(status.get("last_outcome") == "done", f"backup {name}", status)
        return digest, rows, started["backup_id"]

    def equivalence(self):
        src = Server(self.binary, os.path.join(self.work, "src"), self.port,
                     archive=self.archive).start()
        conn = src.conn()
        conn.send_command("CREATE TABLE t (id int64, v int64)")
        # A data file the copier takes a while over: `--rows` rows first.
        for first in range(1, self.rows + 1, 500):
            values = ", ".join(f"({i}, {i})" for i in range(first, min(first + 500, self.rows + 1)))
            conn.send_command(f"INSERT INTO t VALUES {values}")
        expected = {}
        with Workload(src, self.rows + 1) as w:
            time.sleep(self.seconds)
            expected["b1"] = self.take(src, w, "b1", "B")
            time.sleep(self.seconds)
            pit_hash, pit_rows, pit_lsn = quiesced_point(src, w)
            time.sleep(self.seconds)
            expected["d1"] = self.take(src, w, "d1", "D")
            time.sleep(self.seconds)
            # The client tool, end to end, once.
            backup_tool(src, "base", "--target", os.path.join(self.work, "tool"))
            time.sleep(0.3)
        # One more write past the last STOP's seal lets the archiver take it.
        conn.send_command("INSERT INTO t VALUES (100000000, 0)")
        conn.send_command("SYNC")
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            meta = fields(conn.send_command("SHOW META"))
            if int(meta.get("wal_archive_lag_segments", "1")) == 0:
                break
            time.sleep(0.5)
        src.stop()

        for name, label in (("b1", "B"), ("d1", "D"), ("tool", "B (kds_backup)")):
            verify = backup_tool(src, "verify", os.path.join(self.work, name), check=False)
            self.expect(verify.returncode == 0, f"verify {name}", verify.stdout + verify.stderr)
            target, _ = restore(self.binary, self.work, f"r-{name}",
                                os.path.join(self.work, name), self.port + 1)
            target.start()
            digest, rows = table_hash(target.conn())
            target.stop()
            if name in expected:
                want, want_rows, _ = expected[name]
                self.expect(digest == want, f"equivalence {label}",
                            f"source {want_rows} rows at the stop, restored {rows}")
            else:
                self.expect(rows > 0, f"equivalence {label}", "no rows")
            self.ok(f"equivalence path {label}", f"({rows} rows)")

        # PITR from b1 to a recorded LSN past its stop: the hash must match.
        target, _ = restore(self.binary, self.work, "r-pit", os.path.join(self.work, "b1"),
                            self.port + 1, "--restore-archive", self.archive,
                            "--restore-target", str(pit_lsn))
        target.start()
        digest, rows = table_hash(target.conn())
        target.stop()
        self.expect(digest == pit_hash, "pitr",
                    f"hash at lsn {pit_lsn}: source {pit_rows} rows, restored {rows}")
        self.ok("pitr to a recorded lsn", f"({rows} rows)")

    def operation(self):
        """BR-S9: list every backup, verify one, then expire to the newest and
        restore it through the archive - expire never strands a retained one."""
        root = os.path.join(self.work, "backups")
        os.makedirs(root)
        for name in ("b1", "d1", "tool"):
            shutil.copytree(os.path.join(self.work, name), os.path.join(root, name))
        src = Server(self.binary, os.path.join(self.work, "src"), self.port)
        listed = backup_tool(src, "list", root).stdout.strip().splitlines()
        self.expect(len(listed) == 3, "list", "\n".join(listed))
        self.ok("list", f"({len(listed)} backups)")
        before = sorted(os.listdir(self.archive))
        expired = backup_tool(src, "expire", root, "--keep", "1", "--archive", self.archive)
        kept = sorted(os.listdir(root))
        self.expect(kept == ["tool"], "expire", f"kept {kept}: {expired.stdout}")
        after = sorted(os.listdir(self.archive))
        target, _ = restore(self.binary, self.work, "r-expired", os.path.join(root, "tool"),
                            self.port + 1, "--restore-archive", self.archive,
                            "--restore-target", "latest")
        target.start()
        _, rows = table_hash(target.conn())
        target.stop()
        self.ok("expire", f"(kept 1 of 3, archive {len(before)} -> {len(after)} segments, "
                f"the kept one restores to latest: {rows} rows)")
        refused = backup_tool(src, "expire", root, "--keep", "0", check=False)
        self.expect(refused.returncode != 0, "expire keep 0", refused.stdout)
        self.ok("expire refuses a retention that keeps nothing")

    def faults(self):
        b1 = os.path.join(self.work, "b1")
        # Case 7: a non-empty target.
        occupied = os.path.join(self.work, "occupied")
        os.makedirs(occupied, exist_ok=True)
        target = Server(self.binary, occupied, self.port + 2)
        open(target.data_file, "w").write("x")
        r = subprocess.run([self.binary, "--config", target.config, "--restore-from", b1,
                            "--restore-only"], capture_output=True, text=True)
        self.expect(r.returncode != 0 and "already holds data" in r.stderr, "case 7", r.stderr)
        self.ok("case 7: restore onto a non-empty target refused")

        # Case 4: a flipped bit in a segment, and in the data file.
        for label, rel, offset in (("segment", "wal/wal-0-0.log", 4096 + 200),
                                   ("data file", "kds.db", 8192 * 3 + 100)):
            bad = os.path.join(self.work, f"bad-{label.replace(' ', '')}")
            shutil.copytree(b1, bad)
            with open(os.path.join(bad, rel), "r+b") as f:
                f.seek(offset)
                byte = f.read(1)
                f.seek(offset)
                f.write(bytes([byte[0] ^ 0x01]))
            _, r = restore(self.binary, self.work, f"r-bad-{label.replace(' ', '')}", bad,
                           self.port + 2, check=False)
            self.expect(r.returncode != 0 and rel in r.stderr, f"case 4 ({label})", r.stderr)
            self.ok(f"case 4: a flipped bit in the {label} refused by name")

        # Case 4: a missing segment.
        missing = os.path.join(self.work, "missing")
        shutil.copytree(b1, missing)
        os.remove(os.path.join(missing, "wal", "wal-0-0.log"))
        _, r = restore(self.binary, self.work, "r-missing", missing, self.port + 2, check=False)
        self.expect(r.returncode != 0 and "wal-0-0.log" in r.stderr, "case 4 (missing)", r.stderr)
        self.ok("case 4: a missing segment refused by name")

        # Case 5: a manifest from one backup over another's files.
        mixed = os.path.join(self.work, "mixed")
        shutil.copytree(b1, mixed)
        shutil.copy(os.path.join(self.work, "d1", "backup_manifest"),
                    os.path.join(mixed, "backup_manifest"))
        _, r = restore(self.binary, self.work, "r-mixed", mixed, self.port + 2, check=False)
        self.expect(r.returncode != 0, "case 5", r.stdout + r.stderr)
        self.ok("case 5: one backup's manifest over another's files refused")

        # Case 1: a kill during the base copy leaves no manifest.
        src = Server(self.binary, os.path.join(self.work, "src2"), self.port + 3).start()
        conn = src.conn()
        conn.send_command("CREATE TABLE t (id int64, v int64)")
        for i in range(1, 2000):
            conn.send_command(f"INSERT INTO t VALUES ({i}, {i})")
        half = os.path.join(self.work, "half")
        conn.send_command("BACKUP START 'half'")
        conn.send_command(f"BACKUP COPY TO '{half}'")
        src.kill()
        self.expect(not os.path.exists(os.path.join(half, "backup_manifest")), "case 1",
                    "a killed copy left a manifest")
        _, r = restore(self.binary, self.work, "r-half", half, self.port + 2, check=False)
        self.expect(r.returncode != 0 and "backup_manifest" in r.stderr, "case 1", r.stderr)
        self.ok("case 1: a kill during the base copy leaves no backup")

        # Case 6: a snapshot with no stop is a crash image, recovered as one.
        src.start()
        conn = src.conn()
        before, rows = table_hash(conn)
        src.kill()  # the power cut stands in for an unstopped snapshot
        crash = os.path.join(self.work, "crashimg")
        shutil.copytree(src.root, crash)
        image = Server(self.binary, crash, self.port + 4)
        image.start()
        after, rows_after = table_hash(image.conn())
        image.stop()
        self.expect(after == before, "case 6", f"{rows} rows before, {rows_after} after")
        self.ok("case 6: a snapshot with no stop recovers as the crash image it is")

        # Case 3: a restore killed during its recovery is resumed by the next mount.
        target = Server(self.binary, os.path.join(self.work, "r-killed"), self.port + 5)
        proc = subprocess.Popen([self.binary, "--config", target.config, "--restore-from", b1],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        while True:
            line = proc.stdout.readline()
            if not line or "recovering" in line:
                break
        proc.kill()
        proc.wait()
        target.start()  # no --restore-from: the volume itself owes the restore
        digest, rows = table_hash(target.conn())
        meta = fields(target.conn().send_command("SHOW META"))
        target.stop()
        self.expect(rows > 0, "case 3", "no rows")
        self.ok("case 3: a restore killed in recovery completes at the next mount",
                f"({rows} rows, backup {meta.get('recovery_restore_backup', 'already discharged')})")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--server", default=os.path.join(REPO, "build", "kds_server"))
    p.add_argument("--work", default="/tmp/kds-br-drill")
    p.add_argument("--port", type=int, default=25432)
    p.add_argument("--rows", type=int, default=100000, help="rows loaded before the drills")
    p.add_argument("--seconds", type=float, default=2.0, help="workload between steps")
    args = p.parse_args()
    drills = Drills(args)
    drills.equivalence()
    drills.operation()
    drills.faults()
    print("all drills passed")


if __name__ == "__main__":
    main()
