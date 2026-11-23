#!/bin/bash
# follow-up: unique distribution only, 8 rounds, 8 statements, A B A B
W=/home/louis/ckdbs/.claude/worktrees/dq-cabin-miss-build
O=/home/louis/dqbench/close
D=$W/tools/dq_s0_premise.py
HB=$O/m2u.heartbeat
A=/home/louis/dqbench/kds_server_A_68e5a69f
B=/home/louis/dqbench/kds_server_B_7e31d272
for pass in 1 2; do
  for X in A B; do
    if [ $X = A ]; then BIN=$A; else BIN=$B; fi
    python3 $D --bin $BIN --workdir $O/w2 --out $O/m2u-$X-$pass.json --heartbeat $HB --sizes 10000 --dists unique --rounds 8 --stmts 8 --port 15991 > $O/m2u-$X-$pass.log 2>&1
  done
done
echo finished > $O/m2u.finished
