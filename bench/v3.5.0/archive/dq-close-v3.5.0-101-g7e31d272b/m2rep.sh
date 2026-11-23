#!/bin/bash
# the in-run floor for M2's converged statements: A and B's 10k and exists cells run a second time (fresh processes)
W=/home/louis/ckdbs/.claude/worktrees/dq-cabin-miss-build
O=/home/louis/dqbench/close
D=$W/tools/dq_s0_premise.py
HB=$O/m2rep.heartbeat
A=/home/louis/dqbench/kds_server_A_68e5a69f
B=/home/louis/dqbench/kds_server_B_7e31d272
run() {
  local name=$1 bin=$2; shift 2
  python3 $D --bin $bin --workdir $O/w2 --out $O/m2rep-$name.json --heartbeat $HB "$@" > $O/m2rep-$name.log 2>&1
}
run A-10k $A --sizes 10000 --port 15991
run B-10k $B --sizes 10000 --port 15991
run A-exists10k $A --sizes 10000 --shape exists --port 15997
run B-exists10k $B --sizes 10000 --shape exists --port 15997
echo finished > $O/m2rep.finished
