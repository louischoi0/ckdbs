#!/bin/sh
# DI close: driver 1 extended from 6 to 12 rounds (rounds 7-12 appended to raw1b.jsonl), because
# round 6 and some single lifetimes of rounds 1-5 ran while another session loaded the host.
R=/home/louis/bench-runs/di-close
W=/home/louis/ckdbs/.claude/worktrees/di-shape-identity
HERE=$W/bench/v3.4.15/archive/di-close-v3.4.15-12-g871845e4
st() { echo "$(date -u +%FT%TZ) $*" >> "$R/pipeline.status"; }
until grep -q "PIPELINE3 DONE" "$R/pipeline.status" 2>/dev/null; do sleep 5; done
rm -f "$R/quiet.done"
python3 "$HERE/quietwait.py"
st "gate5 $(cat $R/quiet.done)"
cp "$R/quiet.done" "$R/gate5.txt"
python3 "$HERE/di_arms.py" --bins A=$R/bin/kds_A,B=$R/bin/kds_B --tools $W/tools --wd $R/run1b \
    --out $R/raw1b.jsonl --status $R/run1c.status --rounds 12 --start-round 7 \
    --cells select1,select8,insert1,insert8,fk1 --secs 6 --warm 2 >> $R/driver1c.log 2>&1
st "driver1c exit $?"
st "PIPELINE4 DONE"
