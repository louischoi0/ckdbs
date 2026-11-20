#!/bin/sh
# DI close: driver 2 extended from 6 to 12 reps (the driver skips the finished ones, so reps 7-12 run),
# because the first six held two single-run spikes (B-opt-r1, B-r2) that a mean over six cannot absorb.
R=/home/louis/bench-runs/di-close
W=/home/louis/ckdbs/.claude/worktrees/di-shape-identity
HERE=$W/bench/v3.4.15/archive/di-close-v3.4.15-12-g871845e4
st() { echo "$(date -u +%FT%TZ) $*" >> "$R/pipeline.status"; }
until grep -q "PIPELINE2 DONE" "$R/pipeline.status" 2>/dev/null; do sleep 5; done
rm -f "$R/quiet.done"
python3 "$HERE/quietwait.py"
st "gate4 $(cat $R/quiet.done)"
cp "$R/quiet.done" "$R/gate4.txt"
KDS_TOOLS=$W/tools python3 "$HERE/cf/cf_di.py" --bins A=$R/bin/kds_A,B=$R/bin/kds_B --out $R/run2 \
    --data $R/run2data --port 15790 --reps 12 >> $R/driver2.log 2>&1
st "driver2b exit $?"
st "PIPELINE3 DONE"
