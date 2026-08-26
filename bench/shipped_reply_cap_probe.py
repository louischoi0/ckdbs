#!/usr/bin/env python3
"""Where the 992-byte shipped-reply cap actually bites, in rows.

`statement_ship_service.hpp` rule 1 states the bound - a reply fills one
1,024-byte ring slot less a 32-byte header, so 992 bytes of answer - and
refuses rather than truncating past it. What a reader of a results file
needs is not the byte count but **how many rows of a real relation that
is**, because that is the number that decides whether a shipped read is
usable for a workload.

So: a peer-owned relation, a session on a core that does not own it, and
`SELECT * FROM t LIMIT k` for rising k until the answer stops arriving. The
same k is asked from a session seated on the owner, where no cap exists, so
the two columns say plainly that the cap is shipping's and not the engine's.

The refusal past the cap is `UnknownOutcome`, **not** a retryable error and
not a truncated answer: the statement ran on the owner and its answer could
not be carried (`statement_ship_service.cpp`'s `OverLongReply`). For a read
that is harmless - nothing was mutated - and the probe records the exact
spelling so a results file can quote it.

Usage:
    bench/shipped_reply_cap_probe.py --server build-release/kds_server \\
        --workdir ~/ssb/cap --cores 4 --rows 200
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
from multicore_benchmark import (  # noqa: E402
    Conn, check_host, collect_connections, field, is_retryable, wait_for_port,
)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--rows", type=int, default=200)
    ap.add_argument("--port", type=int, default=20600)
    ap.add_argument("--max-connects", type=int, default=512)
    ap.add_argument("--json", default="")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)
    conf = os.path.join(args.workdir, "cap.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {os.path.join(args.workdir, 'cap.db')}\n"
                f"port = {args.port}\ncores = {args.cores}\n"
                f"placement = rotate\npeer_listeners = on\n"
                f"log_file = cap.log\nlog_dir = {args.workdir}\nlog_level = warn\n")
    stderr_path = os.path.join(args.workdir, "cap.stderr")
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([os.path.abspath(args.server), "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    out = dict(cores=args.cores, rows=args.rows)
    try:
        wait_for_port(args.port, stderr_path)
        got, _ = collect_connections(args.port, {0: 1}, args.max_connects)
        setup = got[0][0]
        r = setup.cmd("CREATE TABLE t (id int64, owner varchar, balance int64) BTREE")
        if r.startswith("ERR"):
            raise RuntimeError(r)
        owner = int(field(setup.cmd("DESCRIBE t"), "owner_core"))
        out["owner_core"] = owner
        seats = {owner: 1}
        foreign = next(c for c in range(args.cores) if c != owner)
        seats[foreign] = 1
        per, _ = collect_connections(args.port, seats, args.max_connects)
        local, remote = per[owner][0], per[foreign][0]
        out["foreign_core"] = foreign
        setup.close()

        for i in range(args.rows):
            reply = local.cmd(f"INSERT INTO t VALUES ('o{i % 7}', {i})")
            while reply.startswith("ERR") and is_retryable(reply):
                reply = local.cmd(f"INSERT INTO t VALUES ('o{i % 7}', {i})")
            if reply.startswith("ERR"):
                raise RuntimeError(f"insert {i}: {reply}")

        rows = []
        last_ok = 0
        first_bad = None
        for k in range(1, args.rows + 1):
            q = f"SELECT * FROM t LIMIT {k}"
            seated = local.cmd(q)
            shipped = remote.cmd(q)
            ok = not shipped.startswith("ERR")
            rows.append(dict(limit=k, seated_bytes=len(seated),
                             shipped_ok=ok, shipped_bytes=len(shipped)))
            if ok:
                last_ok = k
            elif first_bad is None:
                first_bad = dict(limit=k, seated_bytes=len(seated), reply=shipped)
                # The seated answer is what the shipped one would have been.
                break
        out["largest_shipped_limit"] = last_ok
        out["largest_shipped_reply_bytes"] = next(
            (r["shipped_bytes"] for r in rows if r["limit"] == last_ok), None)
        out["first_refused"] = first_bad
        out["per_limit"] = rows[-6:]
        out["seated_answers_past_the_cap"] = (
            not local.cmd(f"SELECT * FROM t LIMIT {args.rows}").startswith("ERR"))
        local.close()
        remote.close()
        stop = Conn(args.port)
        stop.cmd("STOP")
        stop.close()
    finally:
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
    print(json.dumps(out, indent=2))
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(out, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
