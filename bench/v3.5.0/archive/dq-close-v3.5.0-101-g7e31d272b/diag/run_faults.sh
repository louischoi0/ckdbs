#!/bin/sh
# DQ-S5 diagnosis part 2: the converged statement #3 on A, B and B with the
# fallback off, with per-thread CPU and page faults per statement.
cd /home/louis/ckdbs/.claude/worktrees/dq-cabin-miss-build || exit 1
D=/home/louis/dqbench/diag
: > $D/faults.heartbeat
for pass in 1 2; do
  for cfg in A B B0; do
    case $cfg in
      A)  BIN=/home/louis/dqbench/kds_server_A_68e5a69f; EXTRA="" ;;
      B)  BIN=/home/louis/dqbench/kds_server_B_7e31d272; EXTRA="" ;;
      B0) BIN=/home/louis/dqbench/kds_server_B_7e31d272; EXTRA="--build-cap 0" ;;
    esac
    python3 tools/dq_s0_premise.py --bin $BIN --workdir $D/work --out $D/faults-$cfg-$pass.json \
        --sizes 10000 --dists unique --rounds 6 --stmts 5 $EXTRA \
        --heartbeat $D/faults.heartbeat > $D/faults-$cfg-$pass.log 2>&1
    echo "cell $cfg pass $pass exit=$?" >> $D/faults.heartbeat
  done
done
echo "driver exit=0" >> $D/faults.heartbeat
