#!/bin/bash
# Engine A/B on this host: the v3.4.14 binary (35086008, Release, the copy measured on 2026-11-19, sha256 78b3aede...) against the v3.5.0 binary, same driver (07f4552a's tools/),
# interleaved old, new, old, new, old, new. Waits for run_all.sh to finish. Idempotent per cell.
set -uo pipefail
source /home/louis/bench-runs/scenarios-v3.5.0/lib.sh
echo $$ > "$RUN/ab.pid"
export PYTHONPATH=$TOOLS
while [ ! -e "$RUN/all.done" ]; do sleep 5; done
S2_ARGS="--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync"
eval "$(sed -n '/^cell2() {/,/^S1DRV=/p' "$RUN/run_all.sh" | sed '$d')"
if [ ! -x "$RUN/kds_server_old" ]; then
  cp /home/louis/bench-runs/scenarios-v3.4.14/kds_server "$RUN/kds_server_old"
  sha256sum "$RUN/kds_server_old" > "$RUN/binary-old.sha256"
fi
NEWBIN=$RUN/kds_server; OLDBIN=$RUN/kds_server_old
for I in 1 2 3; do
  for V in old new; do
    if [ $V = old ]; then BIN=$OLDBIN; else BIN=$NEWBIN; fi
    vcell cell1 ab-$V$I-s1-c1-g ab-$V$I-s1-c1-g "$TOOLS/scenario1_backtest.py" 1 group
    vcell cell2 ab-$V$I-s2-c1-g ab-$V$I-s2-c1-g abs2c1g 15604 1 group
    vcell cell2 ab-$V$I-b1-s2-c1-s ab-$V$I-b1-s2-c1-s abb1c1s 15606 1 strict --bookers 1
    vcell cell3 ab-$V$I-s3-c1-n10000-none ab-$V$I-s3-c1-n10000-none 1 group 10000 none
  done
done
status "AB DONE"
touch "$RUN/ab.done"
