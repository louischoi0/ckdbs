#!/bin/bash
set -uo pipefail
source /home/louis/bench-runs/scenarios-v3.5.0/lib.sh
echo $$ > "$RUN/extras.pid"
export PYTHONPATH=$TOOLS
while [ ! -e "$RUN/ab.done" ]; do sleep 5; done
S2_ARGS="--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync"
eval "$(sed -n '/^cell2() {/,/^S1DRV=/p' "$RUN/run_all.sh" | sed '$d')"
BIN=$RUN/kds_server
for P in r2- r3-; do
  for N in 200 1000 10000; do for M in none all; do
    vcell cell3 ${P}s3-c8-n$N-$M ${P}s3-c8-n$N-$M 8 group $N $M
  done; done
done
vcell cell2 rr-s2-c1-g rr-s2-c1-g s2rrc1g 15604 1 group --isolation repeatable-read
vcell cell2 rr-s2-c8-g rr-s2-c8-g s2rrc8g 15605 8 group --isolation repeatable-read
for B in 16; do
  vcell cell2s sat-b$B-s2-c8-g sat-b$B-s2-c8-g satb${B}c8g 15605 8 group --bookers $B
  vcell cell2s sat-b$B-s2-c8-s sat-b$B-s2-c8-s satb${B}c8s 15607 8 strict --bookers $B
done
status "EXTRAS DONE"
touch "$RUN/extras.done"
