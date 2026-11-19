#!/bin/bash
# Engine A/B on this host: the v3.3.0-38 binary (a40882af, Release) against the v3.4.14 binary, same driver (35086008's tools/),
# interleaved old, new, old, new, old, new. Waits for run_all.sh to finish. Idempotent per cell.
set -uo pipefail
source /home/louis/bench-runs/scenarios-v3.4.14/lib.sh
echo $$ > "$RUN/ab.pid"
export PYTHONPATH=$TOOLS
while [ ! -e "$RUN/all.done" ]; do sleep 5; done
S2_ARGS="--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync"
eval "$(sed -n '/^cell2() {/,/^S1DRV=/p' "$RUN/run_all.sh" | sed '$d')"
if [ ! -x "$RUN/kds_server_old" ]; then
  status "ab: building a40882af"
  date -u +%FT%TZ > "$RUN/oldbuild-start.txt"
  cmake -S "$RUN/src-old" -B "$RUN/build-old" -DCMAKE_BUILD_TYPE=Release > "$RUN/oldcmake.log" 2>&1
  cmake --build "$RUN/build-old" --target kds_server -j8 > "$RUN/oldbuild.log" 2>&1
  date -u +%FT%TZ > "$RUN/oldbuild-end.txt"
  cp "$RUN/build-old/kds_server" "$RUN/kds_server_old"
  sha256sum "$RUN/kds_server_old" > "$RUN/binary-old.sha256"
  stat -c %y "$RUN/build-old/kds_server" > "$RUN/source-binary-old-mtime.txt"
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
