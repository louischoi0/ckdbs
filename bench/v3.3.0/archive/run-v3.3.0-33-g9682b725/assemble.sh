#!/bin/bash
WT=/home/cdkbs/ckdbs/.claude/worktrees/bench-scenarios-1-4
S=$WT/bench/v3.1.0/archive/run-v3.1.0-30-gcd7c7c4e
R=/home/cdkbs/bench-runs/v3.3.0-33-g9682b725
head -144 $S/run_all6.sh | sed -e 's#v3.1.0-30-gcd7c7c4e#v3.3.0-33-g9682b725#g' -e 's#Scenarios 2, 3, 1, 4 at#Scenarios 1-4 full rebaseline at#' > $R/run_all.sh
cat /home/cdkbs/bench-run/stages.sh >> $R/run_all.sh
sed -i 's#grep -v "bench-runs/v3.3.0-33-g9682b725"#grep -v "bench-runs/v3.3.0-33-g9682b725" | grep -v "postgres"#' $R/monitor.sh
grep -n postgres $R/monitor.sh
cp /home/cdkbs/ckdbs/.claude/worktrees/bench-scenarios-1-4/build-release/kds_server $R/kds_server
sha256sum $R/kds_server | tee $R/binary.sha256
stat -c '%y' /home/cdkbs/ckdbs/.claude/worktrees/bench-scenarios-1-4/build-release/kds_server | tee $R/source-binary-mtime.txt
bash -n $R/run_all.sh && echo syntax ok
