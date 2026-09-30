#!/bin/bash
# Scenario 4 (cabin optimizer, business days) at 9a0525d. No v3 predecessor.
# Three servers (off / on / declared), cores 1, group, decay_half_life = 5 s
# (the driver docstring's 120x compression), snapshot interval 500 ms.
# Driver defaults otherwise (3 days, 45 s sessions, 45 s nights).
set -uo pipefail
P=${P:-}
CONF3=${CONF3:-}
source /home/cdkbs/bench-runs/rebaseline-9a0525d/lib.sh
echo "== ${P}s4 start $(date -u +%FT%TZ) ==" >> "$RUN/timeline"
for A in off on declared; do
    precheck "${P}s4-$A"
done
CONF1="decay_half_life = 5"
CONF2="cabin_optimizer_snapshot_interval_ms = 500"
start_server ${P}s4-off 15651 1 group "$CONF1" "$CONF2" "$CONF3" >> "$RUN/logs/${P}s4.startup" 2>&1
start_server ${P}s4-on 15652 1 group "$CONF1" "$CONF2" "$CONF3" >> "$RUN/logs/${P}s4.startup" 2>&1
start_server ${P}s4-declared 15653 1 group "$CONF1" "$CONF2" "$CONF3" >> "$RUN/logs/${P}s4.startup" 2>&1
PO=$(cat "$RUN/data/${P}s4-off/pid"); PN=$(cat "$RUN/data/${P}s4-on/pid"); PD=$(cat "$RUN/data/${P}s4-declared/pid")
python3 "$TOOLS/scenario4_cabinopt_days.py" --port-off 15651 --port-on 15652 --port-declared 15653 \
    --pid-off "$PO" --pid-on "$PN" --pid-declared "$PD" --suffix a \
    --json "$RUN/json/${P}s4.json" > "$RUN/logs/${P}s4.stdout" 2>&1
echo "$?" > "$RUN/logs/${P}s4.rc"
for A in off on declared; do stop_server ${P}s4-$A >> "$RUN/logs/${P}s4.startup" 2>&1; done
echo "== ${P}s4 end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/${P}s4.rc) ==" >> "$RUN/timeline"
touch "$RUN/${P}s4.done"
