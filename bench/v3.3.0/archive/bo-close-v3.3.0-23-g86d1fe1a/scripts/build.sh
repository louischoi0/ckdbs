#!/bin/bash
R=/home/cdkbs/bench-runs/bo-close
W=/home/cdkbs/ckdbs/.claude/worktrees/bo-relation-stats-checkpoints
P=/tmp/claude-1000/-home-cdkbs-ckdbs/81c2d5d2-a66c-401c-a1ba-72a1357b9f32/scratchpad/bo_off.py
mkdir -p $R/bin $R/out $R/flags $R/src-A $R/src-B
stamp() { echo "$(date -u '+%F %T UTC') $1" >> $R/progress.txt; }
stamp "task began; exporting 86d1fe1a"
git -C $W archive 86d1fe1a | tar -x -C $R/src-A
git -C $W archive 86d1fe1a | tar -x -C $R/src-B
(cd $R/src-A && python3 $P)
sha256sum $P > $R/out/patch.sha256
diff -ru $R/src-B/src $R/src-A/src > $R/out/patch.diff
stamp "build A,B start"
for X in A B; do
  ( cmake -S $R/src-$X -B $R/build-$X -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > $R/out/cmake-$X.log 2>&1 \
    && cmake --build $R/build-$X --target kds_server -j4 > $R/out/build-$X.log 2>&1; echo "rc=$?" > $R/flags/build-$X ) &
done
wait
stamp "build A,B done $(cat $R/flags/build-A) $(cat $R/flags/build-B)"
