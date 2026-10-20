#!/bin/bash
set -u
R=/home/cdkbs/bench-runs/v3.3.0-38-ga40882af
D=v3.3.0-38-ga40882af
A=/home/cdkbs/ckdbs/.claude/worktrees/kwp-decoder-fast-path/bench/v3.3.0/archive
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
copyset 1 's1-*' 'r2-s1-*' 'r3-s1-*' 's1s-*' 'r2-s1s-*' 'r3-s1s-*' 'pl-s1-*' 'ab-*-s1-*'
copyset 2 's2-*' 'r2-s2-*' 'r3-s2-*' 'rr-s2-*' 'x-b*' 'sat-*' 'b1-s2-*' 'r2-b1-s2-*' 'r3-b1-s2-*' 'ab-*-s2-*' 'ab-*-b1-s2-*'
copyset 3 's3-*' 'r2-s3-*' 'r3-s3-*' 'pl-s3-*' 'ab-*-s3-*'
copyset 4 's4' 'cd2-s4' 's4-*' 'cd2-s4-*' 'ab-*-s4' 'ab-*-s4-*'
mkdir -p $A/scenario1-$D/plans $A/scenario3-$D/plans
cp $R/plans/pl-s1-* $A/scenario1-$D/plans/
cp $R/plans/pl-s3-* $A/scenario3-$D/plans/
X=$A/run-$D
cp $R/lib.sh $R/run_all.sh $R/run_plan.sh $R/run_post.sh $R/monitor.sh $R/monitor2.sh $R/monitor3.sh $R/monitor4.sh $R/monitor5.sh $R/watchdog.sh $R/watchdog2.sh $R/watchdog3.sh $R/cpusample.sh $R/chain.sh $R/chain2.sh $R/start.sh $R/restart_wd.sh $R/archive.sh $X/
cp $R/timeline $R/monitor.log $R/watchdog.log $R/watchdog2.log $R/watchdog3.log $R/binary.sha256 $R/binary-after-run.sha256 $R/source-binary-mtime.txt $R/start-time.txt $X/ 2>/dev/null
cp $R/logs/validity.log $R/logs/master.out $X/logs/ 2>/dev/null
cp -r $R/contaminated $X/ 2>/dev/null
cd $R/analysis
cp g.py s1.py s1b.py s2.py s3.py s4.py abx.py stamp.py rcsec.py rc_valid.py doc_s1.py doc_s2.py doc_s3.py doc_s4.py build_sum.py plan_ab2.py mdlib.py host.py cellvalid.py planrun.py showplans.py $X/analysis/
# the old client, as a recipe, not a copy of the tree
echo "old client = git show a40882af^:tools/kwp.py and git show a40882af^:tools/ckdbs_cli.py (git archive a40882af^ tools, extracted to bench-runs/$D/oldtools/)" > $X/old-client.txt
# row-cost sweep (KDS only)
Z=$X/rowcost
mkdir -p $Z/contaminated
cp $R/rowcost/kdsrow.py $R/rowcost/rowcost.py $R/rowcost/run_rowcost.sh $R/rowcost/run_send.sh $R/rowcost/watchdog_rc.sh $R/rowcost/watchdog_send.sh $R/rowcost/chain3.sh $R/rowcost/prep_rerun.py $R/rowcost/rerun_rc.sh $R/rowcost/mon_wd_rerun.sh $Z/
cp $R/rowcost/cells-*.jsonl $R/rowcost/monitor.log $R/rowcost/monitor-rerun.log $R/rowcost/watchdog*.log $R/rowcost/*.conf $Z/ 2>/dev/null
cp $R/rowcost/contaminated/* $Z/contaminated/ 2>/dev/null
du -sh $A
