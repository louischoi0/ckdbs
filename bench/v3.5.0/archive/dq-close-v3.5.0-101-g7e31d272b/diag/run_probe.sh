#!/bin/sh
# DQ-S5 diagnosis part 1: compile against execute, A / A2 / B.
cd /home/louis/ckdbs/.claude/worktrees/dq-cabin-miss-build || exit 1
mkdir -p /home/louis/dqbench/diag
: > /home/louis/dqbench/diag/probe.heartbeat
python3 tools/dq_compile_probe.py --binA /home/louis/dqbench/kds_server_A_68e5a69f \
    --binB /home/louis/dqbench/kds_server_B_7e31d272 --workdir /home/louis/dqbench/diag/work \
    --out /home/louis/dqbench/diag/probe.json --rounds 8 --k 2000 \
    --heartbeat /home/louis/dqbench/diag/probe.heartbeat > /home/louis/dqbench/diag/probe.log 2>&1
echo "driver exit=$?" >> /home/louis/dqbench/diag/probe.heartbeat
