#!/bin/bash
cd /home/louis/bench-runs/dh-s1
echo $$ > run.pid
while [ ! -f build.done ]; do sleep 2; done
mkdir -p bin; [ -f bin/kds_server_dh1 ] || cp src/build/kds_server bin/kds_server_dh1
sha256sum bin/kds_server_dh1 > bin/sha256
python3 /home/louis/ckdbs/.claude/worktrees/dh-page-image-volume/tools/dh_s0.py --bin bin/kds_server_dh1 --workdir /home/louis/bench-runs/dh-s1/wd --port 17432 --out /home/louis/ckdbs/.claude/worktrees/dh-page-image-volume/bench/v3.4.15/archive/dh-s1-packed-image-spike-v3.4.15-1-gf4138860/runs.jsonl --plan plan.txt --arch /home/louis/ckdbs/.claude/worktrees/dh-page-image-volume/bench/v3.4.15/archive/dh-s1-packed-image-spike-v3.4.15-1-gf4138860 --keep-stderr /home/louis/ckdbs/.claude/worktrees/dh-page-image-volume/bench/v3.4.15/archive/dh-s1-packed-image-spike-v3.4.15-1-gf4138860/stderr --resume >> run.out 2>&1
echo finished >> run.out
