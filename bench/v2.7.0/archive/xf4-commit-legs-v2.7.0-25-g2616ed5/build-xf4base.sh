#!/bin/sh
# XF4's control arm: HEAD (timers in) with XE1's one ternary reverted, so
# the participant acks at durability again. One variable, both arms
# instrumented - which is the A/B H-XF3 actually needs and which
# `85d2bda` (no timers) cannot give.
set -e
S=/tmp/claude-1000/-home-cdkbs-ckdbs/4e50e866-8005-43e4-b524-1d53355b0c93/scratchpad
WT=/home/cdkbs/ckdbs/.claude/worktrees/xf
SRC=$S/src-xf4base
rm -rf "$SRC"
mkdir -p "$SRC"
git -C "$WT" archive HEAD | tar -x -C "$SRC"
# The one line. `kAtAppend` -> `kWhenDurable` on the commit arm of
# StartDecision, which is exactly XE1's change and nothing else.
sed -i 's/? CommandDispatcher::CommitAck::kAtAppend/? CommandDispatcher::CommitAck::kWhenDurable/' \
    "$SRC/src/server/shipped_statement_executor.cpp"
grep -c "kWhenDurable" "$SRC/src/server/shipped_statement_executor.cpp"
cmake -S "$SRC" -B "$SRC/b" -DCMAKE_BUILD_TYPE=Release \
    -DOPENSSL_ROOT_DIR="$S/ossl" > "$S/cfg-xf4base.log" 2>&1
cmake --build "$SRC/b" --target kds_server -j8 > "$S/build-xf4base.log" 2>&1
cp "$SRC/b/kds_server" "$S/arms/kds_server-xf4base"
sha256sum "$S/arms/kds_server-xf4base" | cut -c1-24
echo XF4BASE_DONE
