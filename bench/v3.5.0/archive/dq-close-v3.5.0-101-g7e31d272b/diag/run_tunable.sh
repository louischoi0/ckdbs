#!/bin/sh
# DQ-S5 diagnosis part 3: glibc's dynamic mmap threshold. With the threshold
# fixed (its dynamic raise on freeing an mmapped chunk disabled), does B's
# statement #3 come back level with A's?
cd /home/louis/ckdbs/.claude/worktrees/dq-cabin-miss-build || exit 1
D=/home/louis/dqbench/diag
: > $D/tun.heartbeat
for pass in 1 2; do
  for cfg in A B; do
    case $cfg in
      A) BIN=/home/louis/dqbench/kds_server_A_68e5a69f ;;
      B) BIN=/home/louis/dqbench/kds_server_B_7e31d272 ;;
    esac
    GLIBC_TUNABLES=glibc.malloc.mmap_threshold=131072 \
      python3 tools/dq_s0_premise.py --bin $BIN --workdir $D/work --out $D/tun-$cfg-$pass.json \
        --sizes 10000 --dists unique --rounds 6 --stmts 5 \
        --heartbeat $D/tun.heartbeat > $D/tun-$cfg-$pass.log 2>&1
    echo "cell $cfg pass $pass exit=$?" >> $D/tun.heartbeat
  done
done
echo "driver exit=0" >> $D/tun.heartbeat
