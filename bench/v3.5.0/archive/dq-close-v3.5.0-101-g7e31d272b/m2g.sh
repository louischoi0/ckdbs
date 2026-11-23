#!/bin/bash
W=/home/louis/ckdbs/.claude/worktrees/dq-cabin-miss-build
O=/home/louis/dqbench/close
D=$W/tools/dq_s0_premise.py
HB=$O/m2g.heartbeat
A=/home/louis/dqbench/kds_server_A_68e5a69f
B=/home/louis/dqbench/kds_server_B_7e31d272
one() { # name bin gap
  python3 $D --bin $2 --workdir $O/w2 --out $O/m2g-$1-$PASS.json --heartbeat $HB --sizes 10000 --dists unique --rounds 6 --stmts 5 --port 15991 --gap-s $3 > $O/m2g-$1-$PASS.log 2>&1
}
declare -A BIN=( [A0]=$A [B0]=$B [A15]=$A [B15]=$B )
declare -A GAP=( [A0]=0 [B0]=0 [A15]=1.5 [B15]=1.5 )
ORD=("A15 B15 A0 B0" "B0 A0 B15 A15")
PASS=1
for o in "${ORD[@]}"; do
  for n in $o; do one $n ${BIN[$n]} ${GAP[$n]}; done
  PASS=$((PASS+1))
done
echo finished > $O/m2g.finished
