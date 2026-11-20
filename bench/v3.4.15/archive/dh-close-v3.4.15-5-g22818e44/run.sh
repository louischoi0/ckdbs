#!/bin/bash
cd /home/louis/bench-runs/dh-close
echo $$ > run.pid
while [ ! -f build.done ]; do sleep 2; done
mkdir -p bin
for v in A B X; do [ -f bin/kds_server_$v ] || cp $v/build/kds_server bin/kds_server_$v; done
sha256sum bin/kds_server_* > bin/sha256
python3 /home/louis/ckdbs/.claude/worktrees/dh-page-image-volume/tools/dh_close.py --binA bin/kds_server_A --binB bin/kds_server_B --binX bin/kds_server_X --workdir /home/louis/bench-runs/dh-close/wd --port 17433 --out /home/louis/ckdbs/.claude/worktrees/dh-page-image-volume/bench/v3.4.15/archive/dh-close-v3.4.15-5-g22818e44/runs.jsonl --plan plan3.txt --arch /home/louis/ckdbs/.claude/worktrees/dh-page-image-volume/bench/v3.4.15/archive/dh-close-v3.4.15-5-g22818e44 --seed-snaps /home/louis/bench-runs/dh-s0/wd/snap --resume --until 1795160607 >> run.out 2>&1
echo finished >> run.out
