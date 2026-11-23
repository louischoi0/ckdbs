#!/bin/bash
# DQ close diagnostic: A, B, B with join_build_max_rows = 0 (B0), A again (Ar); unique, 10k, 8 rounds x 5 stmts;
# three passes, the config order rotated per pass.
W=/home/louis/ckdbs/.claude/worktrees/dq-cabin-miss-build
O=/home/louis/dqbench/close
D=$W/tools/dq_s0_premise.py
HB=$O/m2d.heartbeat
A=/home/louis/dqbench/kds_server_A_68e5a69f
B=/home/louis/dqbench/kds_server_B_7e31d272
one() { # name bin extra...
  local name=$1 bin=$2; shift 2
  python3 $D --bin $bin --workdir $O/w2 --out $O/m2d-$name-$PASS.json --heartbeat $HB --sizes 10000 --dists unique --rounds 8 --stmts 5 --port 15991 "$@" > $O/m2d-$name-$PASS.log 2>&1
}
declare -A BIN=( [A]=$A [Ar]=$A [B]=$B [B0]=$B )
declare -A EX=( [A]="" [Ar]="" [B]="" [B0]="--build-cap 0" )
ORD=("A B B0 Ar" "B0 Ar A B" "Ar A B B0")
PASS=1
for o in "${ORD[@]}"; do
  for n in $o; do one $n ${BIN[$n]} ${EX[$n]}; done
  PASS=$((PASS+1))
done
echo finished > $O/m2d.finished
