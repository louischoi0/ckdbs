#!/bin/bash
R=/home/cdkbs/bench-runs/bw-s6
W=/home/cdkbs/ckdbs/.claude/worktrees/bw-instant-add-drop-column
for X in A:045dcc68 B:33519b28; do
  n=${X%%:*}; c=${X##*:}
  ( mkdir -p $R/src-$n && git -C $W archive $c | tar -x -C $R/src-$n \
    && cmake -S $R/src-$n -B $R/build-$n -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > $R/build-$n.log 2>&1 \
    && cmake --build $R/build-$n --target kds_server -j4 >> $R/build-$n.log 2>&1
    echo "$(date -u +%T) build $n rc=$?" >> $R/progress.txt ) &
done
wait
echo "$(date -u +%T) builds done" >> $R/progress.txt
