#!/usr/bin/env python3
"""kds_backup - backup and restore for a ckdbs instance (BR, manual/server/backup-restore.md).

Two backup paths over one restore:

  base      path B: START, the server copies its data file under a write
            fence (COPY), STOP, and the server copies the window's log and
            writes the manifest. Needs nothing outside the server.
  snapshot  path D: START, an operator command takes an atomic snapshot
            (LVM, ZFS, btrfs, scripts/backup/snapshot-freeze.sh), STOP,
            and the server copies the window's log and writes the manifest.

  verify    check a backup's manifest and every file it names.
  restore   lay a backup out as a new volume and recover it past its stop
            (optionally past it through the WAL archive, to an LSN).
  list      every backup under a root directory.
  expire    remove backups beyond a retention, and the archived segments no
            retained backup needs - never one a retained backup needs.

The server runs START/COPY/STOP; the backup directory is a path on the
server's host. The client speaks KWP/1 through tools/kwp.py, which has no
SCRAM or TLS: it reaches an `auth = off` server's admin role only.
"""

import argparse
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ckdbs_cli import ServerConnection  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = "backup_manifest"


def fields(reply):
    """`OK a=1 b=2 error=free text` -> dict; `error` takes the rest of the line."""
    out = {}
    if " error=" in reply:
        reply, out["error"] = reply.split(" error=", 1)
    for token in reply.split():
        if "=" in token:
            key, value = token.split("=", 1)
            out[key] = value
    return out


def run(conn, sql):
    reply = conn.send_command(sql)
    if not reply.startswith("OK"):
        raise SystemExit(f"{sql.split(' TO ')[0]}: {reply}")
    return fields(reply)


def wait(conn, done, what, timeout):
    deadline = time.monotonic() + timeout
    while True:
        status = fields(conn.send_command("BACKUP STATUS"))
        if done(status):
            return status
        if time.monotonic() > deadline:
            raise SystemExit(f"timed out waiting for {what}: {status}")
        time.sleep(0.5)


def quote(text):
    return "'" + text.replace("'", "''") + "'"


def finish(conn, backup_id, target, timeout):
    stop = run(conn, f"BACKUP STOP TO {quote(target)}")
    status = wait(conn, lambda s: s.get("state") == "idle" and s.get("last_id") == backup_id,
                  "the log copy and the manifest", timeout)
    if status.get("last_outcome") != "done":
        raise SystemExit(f"backup {backup_id} failed: {status.get('error', status)}")
    print(f"backup {backup_id} whole in {target}: path {stop['path']}, "
          f"stop lsn {stop['stop_lsn']}")


def cmd_base(args):
    conn = ServerConnection(args.host, args.port, timeout=args.timeout)
    started = run(conn, f"BACKUP START {quote(args.label)}")
    backup_id = started["backup_id"]
    print(f"backup {backup_id} started at lsn {started['start_lsn']}, "
          f"{started['data_pages']} data pages")
    try:
        run(conn, f"BACKUP COPY TO {quote(args.target)}")
        status = wait(conn, lambda s: s.get("copying") == "0", "the data copy", args.wait)
        if "error" in status:
            raise SystemExit(f"data copy failed: {status['error']}")
        finish(conn, backup_id, args.target, args.wait)
    except SystemExit:
        conn.send_command("BACKUP ABORT")
        raise


def cmd_snapshot(args):
    conn = ServerConnection(args.host, args.port, timeout=args.timeout)
    started = run(conn, f"BACKUP START {quote(args.label)}")
    backup_id = started["backup_id"]
    print(f"backup {backup_id} started at lsn {started['start_lsn']}; taking the snapshot")
    env = dict(os.environ, KDS_BACKUP_ID=backup_id, KDS_BACKUP_DIR=args.target)
    if subprocess.run(args.command, shell=True, env=env).returncode != 0:
        conn.send_command("BACKUP ABORT")
        raise SystemExit("the snapshot command failed; backup aborted")
    finish(conn, backup_id, args.target, args.wait)


def server_binary(args):
    return args.server or os.path.join(REPO, "build", "kds_server")


def cmd_verify(args):
    result = subprocess.run([server_binary(args), "--verify-backup", args.backup])
    if result.returncode == 0:
        with open(os.path.join(args.backup, "last_verified"), "w") as f:
            f.write(f"{int(time.time())}\n")
    return result.returncode


def cmd_restore(args):
    argv = [server_binary(args), "--restore-from", args.backup]
    if args.config:
        argv += ["--config", args.config]
    if args.data_file:
        argv.append(args.data_file)
    if args.snapshot_data:
        argv += ["--restore-data", args.snapshot_data]
    if args.archive:
        argv += ["--restore-archive", args.archive]
    if args.target:
        argv += ["--restore-target", args.target]
    if args.only:
        argv.append("--restore-only")
    return subprocess.run(argv).returncode


def read_manifest(path):
    values, files = {}, []
    with open(os.path.join(path, MANIFEST)) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "file":
                files.append((parts[1], int(parts[2])))
            elif len(parts) >= 2:
                values[parts[0]] = parts[1]
    values["files"] = files
    return values


def backups_under(root):
    found = []
    for name in sorted(os.listdir(root)):
        path = os.path.join(root, name)
        if os.path.isfile(os.path.join(path, MANIFEST)):
            found.append((path, read_manifest(path)))
    return sorted(found, key=lambda b: int(b[1]["created_unix"]))


def cmd_list(args):
    for path, m in backups_under(args.root):
        size = sum(n for _, n in m["files"])
        verified = "never"
        stamp = os.path.join(path, "last_verified")
        if os.path.exists(stamp):
            verified = time.strftime("%Y-%m-%d %H:%M", time.gmtime(int(open(stamp).read())))
        print(f"{m['backup_id']}  path {m['path']}  lsn {m['start_lsn']}..{m['stop_lsn']}  "
              f"segments {m['start_segment']}..{m['stop_segment']}  lineage {m['lineage']}  "
              f"{size} bytes  verified {verified}  {path}")


def cmd_expire(args):
    backups = backups_under(args.root)
    if args.keep is not None:
        keep = backups[-args.keep:] if args.keep > 0 else []
    else:
        keep = [b for b in backups if int(b[1]["created_unix"]) >= args.keep_since]
    if not keep:
        raise SystemExit("expire: the retention keeps no backup; refusing to remove them all")
    kept = {path for path, _ in keep}
    for path, m in backups:
        if path not in kept:
            print(f"removing backup {m['backup_id']} ({path})")
            shutil.rmtree(path)
    if args.archive:
        # A retained backup needs every archived segment of its lineage from
        # its own start on (a PITR from it may read any of them); a lineage
        # no retained backup names is left alone - it may be a live history.
        floor = {}
        for _, m in keep:
            lineage = int(m["lineage"])
            floor[lineage] = min(floor.get(lineage, 1 << 64), int(m["start_segment"]))
        for name in sorted(os.listdir(args.archive)):
            parts = name[:-len(".log")].split("-") if name.endswith(".log") else []
            if len(parts) != 3 or parts[0] != "kds":
                continue
            lineage, segment = int(parts[1], 16), int(parts[2])
            if lineage in floor and segment < floor[lineage]:
                print(f"removing archived segment {name}")
                os.remove(os.path.join(args.archive, name))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=15432)
    p.add_argument("--timeout", type=float, default=30.0, help="per statement, seconds")
    p.add_argument("--wait", type=float, default=3600.0, help="for a copy, seconds")
    p.add_argument("--server", help="the kds_server binary (default build/kds_server)")
    sub = p.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("base", help="path B: the server copies its data file")
    b.add_argument("--target", required=True, help="a new directory on the server's host")
    b.add_argument("--label", default="base")
    b.set_defaults(func=cmd_base)

    s = sub.add_parser("snapshot", help="path D: an atomic snapshot between START and STOP")
    s.add_argument("--target", required=True)
    s.add_argument("--command", required=True,
                   help="takes the snapshot; KDS_BACKUP_DIR and KDS_BACKUP_ID are set")
    s.add_argument("--label", default="snapshot")
    s.set_defaults(func=cmd_snapshot)

    v = sub.add_parser("verify")
    v.add_argument("backup")
    v.set_defaults(func=cmd_verify)

    r = sub.add_parser("restore")
    r.add_argument("backup")
    r.add_argument("--config", help="the restored instance's kds.conf")
    r.add_argument("--data-file", help="the restored instance's data file")
    r.add_argument("--snapshot-data", help="path D: the snapshot's data file")
    r.add_argument("--archive", help="the WAL archive, for a target past the stop")
    r.add_argument("--target", help="an lsn, or 'latest'")
    r.add_argument("--only", action="store_true", help="recover, then exit without serving")
    r.set_defaults(func=cmd_restore)

    l = sub.add_parser("list")
    l.add_argument("root")
    l.set_defaults(func=cmd_list)

    e = sub.add_parser("expire")
    e.add_argument("root")
    group = e.add_mutually_exclusive_group(required=True)
    group.add_argument("--keep", type=int)
    group.add_argument("--keep-since", type=int, help="unix seconds")
    e.add_argument("--archive")
    e.set_defaults(func=cmd_expire)

    args = p.parse_args()
    sys.exit(args.func(args) or 0)


if __name__ == "__main__":
    main()
