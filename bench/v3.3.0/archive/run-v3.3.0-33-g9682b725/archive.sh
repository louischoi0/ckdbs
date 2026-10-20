#!/bin/bash
set -u
R=/home/cdkbs/bench-runs/v3.3.0-33-g9682b725
D=v3.3.0-33-g9682b725
A=/home/cdkbs/ckdbs/.claude/worktrees/bench-scenarios-1-4/bench/v3.3.0/archive
mkdir -p $A/run-$D/analysis $A/run-$D/logs
copyset() {  # n pattern...
    local n=$1; shift
    local T=$A/scenario$n-$D
    mkdir -p $T/json $T/logs $T/conf
    local p
    for p in "$@"; do
        cp $R/json/$p.json $T/json/ 2>/dev/null
        cp $R/logs/$p.stdout $R/logs/$p.rc $R/logs/$p.precheck $R/logs/$p.gatewait $R/logs/$p.startup $R/logs/$p.serverlog $R/logs/$p.schema $R/logs/$p.load $R/logs/$p.cpu $T/logs/ 2>/dev/null
        cp $R/conf/$p.conf $T/conf/ 2>/dev/null
    done
}
copyset 1 's1-*' 'r2-s1-*' 'r3-s1-*' 's1s-*' 'r2-s1s-*' 'r3-s1s-*' 'pg-s1*' 'r2-pg-s1*' 'r3-pg-s1*' 'pl-s1-*'
copyset 2 's2-*' 'r2-s2-*' 'r3-s2-*' 'rr-s2-*' 'x-b*' 'sat-*' 'b1-s2-*' 'r2-b1-s2-*' 'r3-b1-s2-*' 'pg-s2-*' 'r2-pg-s2-*' 'r3-pg-s2-*'
copyset 3 's3-*' 'r2-s3-*' 'r3-s3-*' 'pg-s3-*' 'r2-pg-s3-*' 'r3-pg-s3-*' 'pl-s3-*'
copyset 4 's4' 'cd2-s4' 's4-*' 'cd2-s4-*' 'pg-s4' 'r2-pg-s4' 'r3-pg-s4'
mkdir -p $A/scenario1-$D/plans $A/scenario3-$D/plans
cp $R/plans/pl-s1-* $A/scenario1-$D/plans/
cp $R/plans/pl-s3-* $A/scenario3-$D/plans/
X=$A/run-$D
cp $R/lib.sh $R/run_all.sh $R/run_plan.sh $R/monitor.sh $R/monitor2.sh $R/watchdog.sh $R/watchdog2.sh $R/cpusample.sh $X/
cp $R/timeline $R/monitor.log $R/watchdog.log $R/watchdog2.log $R/binary.sha256 $R/source-binary-mtime.txt $X/ 2>/dev/null
cp $R/logs/validity.log $R/logs/notes.txt $R/logs/master.out $X/logs/ 2>/dev/null
cp -r $R/contaminated $X/ 2>/dev/null
cp $R/analysis/cellvalid.py $R/analysis/mdlib.py $X/analysis/
cp /home/cdkbs/bench-run/*.py $X/analysis/
cp /home/cdkbs/bench-run/build.sh /home/cdkbs/bench-run/setup.sh /home/cdkbs/bench-run/assemble.sh /home/cdkbs/bench-run/start.sh /home/cdkbs/bench-run/chain.sh /home/cdkbs/bench-run/restart_wd.sh /home/cdkbs/bench-run/archive.sh $X/
cp /home/cdkbs/bench-run/stages.sh $X/stages-source.sh
sha256sum $R/kds_server | tee $X/binary-after-run.sha256
du -sh $A
