#!/usr/bin/env bash
# launch.sh <name> [smoke] - runs the S2 schedule into scratchpad/cq-s2/<name>
S=/tmp/claude-1000/-home-louis-ckdbs/3b3f74ff-c58d-49b7-accd-f52f3f568ee0/scratchpad/cq-s2
W=/home/louis/ckdbs/.claude/worktrees/cq-lock-free-reservation
NAME=$1; MODE=${2:-full}
rm -rf "/var/tmp/cq-s2-run/$NAME" "$S/$NAME"
mkdir -p "$S/$NAME"
if [ "$MODE" = full ]; then "$S/wait_quiet.sh" 96 > "$S/$NAME/wait_quiet.txt" 2>&1; fi
ROOT=$W OUT=$S/$NAME RUN=/var/tmp/cq-s2-run/$NAME BIN=$W/build-release/kds_server \
  BASE=/var/tmp/cq-s2-base/build/kds_server DESCRIBE=$(cat "$S/describe.txt") "$S/run_s2.sh" "$MODE"
