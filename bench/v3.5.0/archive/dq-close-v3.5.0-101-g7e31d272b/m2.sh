#!/bin/bash
# M2: DQ-S0's cells, run with each binary in turn.
W=/home/louis/ckdbs/.claude/worktrees/dq-cabin-miss-build
O=/home/louis/dqbench/close
D=$W/tools/dq_s0_premise.py
HB=$O/m2.heartbeat
run() { # name bin args...
  local name=$1 bin=$2; shift 2
  if [ -s $O/m2-$name.done ]; then echo "skip $name"; return; fi
  python3 $D --bin $bin --workdir $O/w2 --out $O/m2-$name.json --heartbeat $HB "$@" > $O/m2-$name.log 2>&1 && echo ok > $O/m2-$name.done
}
for X in B A; do
  if [ $X = A ]; then BIN=/home/louis/dqbench/kds_server_A_68e5a69f; R100=1; else BIN=/home/louis/dqbench/kds_server_B_7e31d272; R100=3; fi
  run $X-10k $BIN --sizes 10000 --port 15991
  run $X-100k $BIN --sizes 100000 --max-rows-touched 1000000000 --rounds $R100 --port 15993
  run $X-60k $BIN --sizes 60000 --outer 2000 --engine-build-cap --rounds 1 --stmts 2 --dists unique --port 15995
  run $X-exists10k $BIN --sizes 10000 --shape exists --port 15997
done
echo finished > $O/m2.finished
