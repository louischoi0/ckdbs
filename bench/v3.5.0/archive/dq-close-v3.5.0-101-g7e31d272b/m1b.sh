#!/bin/bash
A=/home/louis/dqbench/kds_server_A_68e5a69f
B=/home/louis/dqbench/kds_server_B_7e31d272
W=/home/louis/ckdbs/.claude/worktrees/dq-cabin-miss-build
O=/home/louis/dqbench/close
python3 $W/tools/dq_close.py --binA $A --binB $B --workdir $O/w --out $O/m1b.json --lifetimes 10 --rounds 24 --port 15781 --heartbeat $O/m1b.heartbeat > $O/m1b.log 2>&1
