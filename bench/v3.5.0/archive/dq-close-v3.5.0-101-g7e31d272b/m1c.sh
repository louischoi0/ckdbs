#!/bin/bash
# M1 with a fourth arm B2 (B plus noinline on BuiltWalk and MissWalk); servers on CPUs 0-3, driver on CPU 4
A=/home/louis/dqbench/kds_server_A_68e5a69f
B=/home/louis/dqbench/kds_server_B_7e31d272
B2=/home/louis/dqbench/kds_server_B2_noinline
W=/home/louis/ckdbs/.claude/worktrees/dq-cabin-miss-build
O=/home/louis/dqbench/close
python3 $W/tools/dq_close.py --binA $A --binB $B --binB2 $B2 --workdir $O/w --out $O/m1c.json --lifetimes 10 --rounds 24 --cpus 0,1,2,3 --driver-cpu 4 --port 15781 --heartbeat $O/m1c.heartbeat > $O/m1c.log 2>&1
echo finished > $O/m1c.finished
D=$W/tools/dq_s0_premise.py
HB=$O/m2e.heartbeat
one() {
  local name=$1 bin=$2; shift 2
  python3 $D --bin $bin --workdir $O/w2 --out $O/m2e-$name-$PASS.json --heartbeat $HB --sizes 10000 --dists unique --rounds 8 --stmts 5 --port 15991 "$@" > $O/m2e-$name-$PASS.log 2>&1
}
declare -A BIN=( [A]=$A [Ar]=$A [B2]=$B2 [B20]=$B2 )
declare -A EX=( [A]="" [Ar]="" [B2]="" [B20]="--build-cap 0" )
ORD=("A B2 B20 Ar" "B20 Ar A B2" "Ar A B2 B20")
PASS=1
for o in "${ORD[@]}"; do
  for n in $o; do one $n ${BIN[$n]} ${EX[$n]}; done
  PASS=$((PASS+1))
done
echo finished > $O/m2e.finished
