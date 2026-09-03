#!/usr/bin/env python3
"""AL-S8 probe: open sessions on a peer_listeners=on server until every core
0..cores-1 has at least one, send SHOW META on each, and dump the replies as
JSON keyed by core. Not a tools/ driver - a read-only client probe built on
the unmodified tools/ckdbs_cli.ServerConnection, the same class
tools/multicore_benchmark.py uses for exactly this (collect_connections).

Usage: probe_meta.py <port> <cores> [max_attempts]
"""
import json
import sys

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/v3.0.0-arch-revision/tools")
from ckdbs_cli import ServerConnection  # noqa: E402


def field(reply, key):
    for tok in reply.split():
        if tok.startswith(key + "="):
            return int(tok[len(key) + 1:])
    raise RuntimeError(f"reply carries no {key}= field: {reply}")


def main():
    port = int(sys.argv[1])
    cores = int(sys.argv[2])
    max_attempts = int(sys.argv[3]) if len(sys.argv) > 3 else 500

    got = {}
    attempts = 0
    conns_to_close = []
    while len(got) < cores and attempts < max_attempts:
        conn = ServerConnection("127.0.0.1", port, timeout=30)
        attempts += 1
        reply = conn.send_command("SHOW META")
        core = field(reply, "core")
        if core not in got:
            got[core] = reply
            conns_to_close.append(conn)
        else:
            conn.close()
    for c in conns_to_close:
        c.close()

    out = {"attempts": attempts, "cores_reached": sorted(got.keys()), "replies": got}
    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()
