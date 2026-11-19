#!/bin/bash
# The close's knob spike: three Release servers beside A and B.
#   K1 = the tree as it stands (B + the walk's one visit call site)
#   K2 = K1 with no descent ever armed (`MissRefusalAdmitted` answers false)
#   K3 = K1 with no page reader built at all (no thread, no one-core wake table)
# K2 and K3 are throw-away edits of two files that are otherwise at HEAD;
# both files are restored from HEAD afterwards and the tree rebuilt as K1.
S=/tmp/claude-1000/-home-louis-ckdbs/783802aa-0ec8-4cfd-866b-862e378009ca/scratchpad
W=/home/louis/ckdbs/.claude/worktrees/de-page-read-off-the-reactor
BIN=$HOME/bench-runs/de/close
cd $W
B=cmake
one() {  # label
  $B --build build-release -j8 --target kds_server > $S/build-$1.log 2>&1
  echo "$1 exit $? $(date -u +%T)" >> $S/build-k.status
  cp build-release/kds_server $BIN/kds_server_$1
}
echo "K start $(date -u +%T)" > $S/build-k.status
one K1
python3 $S/mut.py K2 && one K2
git checkout -q -- src/server/command_dispatcher.cpp
python3 $S/mut.py K3 && one K3
git checkout -q -- src/server/expeditor.cpp
$B --build build-release -j8 --target kds_server > $S/build-K1-again.log 2>&1
echo "restored exit $? $(date -u +%T) dirty: $(git status --porcelain -- src include | tr '\n' ' ')" >> $S/build-k.status
sha256sum $BIN/kds_server_K1 $BIN/kds_server_K2 $BIN/kds_server_K3 build-release/kds_server >> $S/build-k.status
