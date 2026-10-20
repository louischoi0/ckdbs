#!/bin/bash
# run.sh <subcommand args...>: runs rowcost.py with the client pinned to CPU 6.
# `run` pins the PG backend (CPU 4) once the script has printed its pid (see pin_pg.sh).
D=/home/cdkbs/ckdbs/.claude/worktrees/row-cost-attribution
export PYTHONPATH=$D/tools
exec taskset -c 6 python3 $D/bench/v3.3.0/archive/row-cost-attribution-v3.3.0-37-gf829964c/rowcost.py "$@"
