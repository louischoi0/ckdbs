#!/bin/sh
# DI close: after the build, a quiet gate and driver 1 (statement cells), a quiet gate and driver 2
# (CF's Waystone arms). Poll R/pipeline.status. Driver 1 is resumed with --start-round, driver 2
# skips any finished (arm, rep).
R=/home/louis/bench-runs/di-close
W=/home/louis/ckdbs/.claude/worktrees/di-shape-identity
HERE=$W/bench/v3.4.15/archive/di-close-v3.4.15-12-g871845e4
st() { echo "$(date -u +%FT%TZ) $*" >> "$R/pipeline.status"; }
until grep -q "BUILD DONE" "$R/build.out" 2>/dev/null; do sleep 5; done
st "build done"
rm -f "$R/quiet.done"
python3 "$HERE/quietwait.py"
st "gate1 $(cat $R/quiet.done)"
cp "$R/quiet.done" "$R/gate1.txt"
mkdir -p "$R/run1" "$R/run2"
START=${START1:-1}
python3 "$HERE/di_arms.py" --bins A=$R/bin/kds_A,B=$R/bin/kds_B --tools $W/tools --wd $R/run1 \
    --out $R/raw1.jsonl --status $R/run1.status --rounds 6 --start-round $START \
    --cells select1,select8,insert1,insert8,fk1 --secs 6 --warm 2 >> $R/driver1.log 2>&1
st "driver1 exit $?"
rm -f "$R/quiet.done"
python3 "$HERE/quietwait.py"
st "gate2 $(cat $R/quiet.done)"
cp "$R/quiet.done" "$R/gate2.txt"
KDS_TOOLS=$W/tools python3 "$HERE/cf/cf_di.py" --bins A=$R/bin/kds_A,B=$R/bin/kds_B --out $R/run2 \
    --data $R/run2data --port 15790 --reps 6 >> $R/driver2.log 2>&1
st "driver2 exit $?"
st "PIPELINE DONE"
