#!/bin/bash
R=/home/cdkbs/ckdbs/.claude/worktrees/online-index-build
cd $R
D=build-release/ci-close
while [ ! -f $D/load.done ]; do sleep 5; done
$D/armb.sh
for i in 0 1 2; do
 for n in 200 10000 200000; do
  for v in base closing; do
   b=build-release/ci-ab/kds_server-dd437a76; [ $v = closing ] && b=build-release/ci-ab/kds_server-614864c4
   $D/gate.sh
   echo "$(date -u +%H:%M:%S) start A $v n=$n pair $i" >> $D/arma.prog
   python3 tools/ci_close_arm_a.py --bin $b --workdir $D/a-$v --rows $n --ops 8000 --out $D/a-$v-$n-$i.json
   echo "$(date -u +%H:%M:%S) done A $v n=$n pair $i" >> $D/arma.prog
  done
 done
done
echo ok > $D/arma.done
