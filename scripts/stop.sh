#!/bin/sh
# Stops a running kds_server by sending its own STOP command (see
# docs/spec/client-manual.md) - the server's normal graceful shutdown path,
# not a signal-based kill. Run from the repo root, same as run.sh.
python3 tools/ckdbs_cli.py STOP || echo "kds_server does not appear to be running (nothing to stop)" >&2
