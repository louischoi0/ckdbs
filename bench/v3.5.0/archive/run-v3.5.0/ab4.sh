#!/bin/bash
# Scenario 4 on the v3.4.14 binary (one run, defaults), added after the main run showed the `on` arm's Cabins never extended at v3.5.0.
set -uo pipefail
source /home/louis/bench-runs/scenarios-v3.5.0/lib.sh
echo $$ > "$RUN/ab4.pid"
export PYTHONPATH=$TOOLS
eval "$(sed -n '/^cell2() {/,/^S1DRV=/p' "$RUN/run_all.sh" | sed '$d')"
BIN=$RUN/kds_server_old
vcell cell4 ab-old1-s4 ab-old1-s4 ""
status "AB4 DONE"
touch "$RUN/ab4.done"
