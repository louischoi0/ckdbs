#!/bin/sh
# DI close: driver 1 again. The first pass (raw1-contaminated.jsonl) ran while another session's
# measurement driver loaded the host (loadavg 15-22 against the gate's 2.7 at its start), so it is set
# aside and this pass, after driver 2 and a fresh gate, is the record.
R=/home/louis/bench-runs/di-close
W=/home/louis/ckdbs/.claude/worktrees/di-shape-identity
HERE=$W/bench/v3.4.15/archive/di-close-v3.4.15-12-g871845e4
st() { echo "$(date -u +%FT%TZ) $*" >> "$R/pipeline.status"; }
until grep -q "PIPELINE DONE" "$R/pipeline.status" 2>/dev/null; do sleep 5; done
rm -f "$R/quiet.done"
python3 "$HERE/quietwait.py"
st "gate3 $(cat $R/quiet.done)"
cp "$R/quiet.done" "$R/gate3.txt"
mkdir -p "$R/run1b"
python3 "$HERE/di_arms.py" --bins A=$R/bin/kds_A,B=$R/bin/kds_B --tools $W/tools --wd $R/run1b \
    --out $R/raw1b.jsonl --status $R/run1b.status --rounds 6 --start-round ${START1:-1} \
    --cells select1,select8,insert1,insert8,fk1 --secs 6 --warm 2 >> $R/driver1b.log 2>&1
st "driver1b exit $?"
st "PIPELINE2 DONE"
