#!/bin/bash
R=/home/cdkbs/ckdbs/.claude/worktrees/online-index-build
cd $R
for i in 0 1 2; do
 for v in base closing; do
  b=build-release/ci-ab/kds_server-dd437a76; [ $v = closing ] && b=build-release/ci-ab/kds_server-614864c4
  build-release/ci-close/gate.sh
  echo "$(date -u +%H:%M:%S) start $v pair $i" >> build-release/ci-close/armb.prog
  python3 tools/ci_s0_premise.py --bin $b --workdir build-release/ci-close/$v --rows 10000000 --window 200 --build-at 5 --tail 20 --runs 1 --run-offset $i --arms B --reuse-template --keep-template --progress build-release/ci-close/armb-$v.prog --out build-release/ci-close/b-$v-$i.json
  echo "$(date -u +%H:%M:%S) done $v pair $i" >> build-release/ci-close/armb.prog
 done
done
echo ok > build-release/ci-close/armb.done
