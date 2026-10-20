#!/bin/bash
# Re-run of s1-c1-g (3 runs) to test whether bar-lookup warm -14 % against v3.1.0 reproduces.
set -uo pipefail
source /home/cdkbs/bench-runs/v3.3.0-33-g9682b725/lib.sh
echo $$ > "$RUN/master.pid"
export PYTHONPATH=$TOOLS
for I in 1 2 3; do
    TAG=rerun-s1-c1-g-r$I
    [ -e "$RUN/done/$TAG" ] && continue
    status "$TAG start"; tl "== $TAG start $(date -u +%FT%TZ) =="
    precheck "$TAG"
    start_server "$TAG" 15610 1 group >> "$RUN/logs/$TAG.startup" 2>&1 || { tl "== $TAG SERVER FAILED =="; continue; }
    python3 "$TOOLS/scenario1_backtest.py" --port 15610 --suffix ${TAG//-/_} --seed 1 \
        --bars-clustered btree --verify --sync --analyze --json "$RUN/json/$TAG.json" > "$RUN/logs/$TAG.stdout" 2>&1
    echo "$?" > "$RUN/logs/$TAG.rc"
    stop_server "$TAG" >> "$RUN/logs/$TAG.startup" 2>&1
    tl "== $TAG end $(date -u +%FT%TZ) rc=$(cat $RUN/logs/$TAG.rc) =="; status "$TAG end"; touch "$RUN/done/$TAG"
    sleep 4
    python3 "$RUN/analysis/cellvalid.py" "$TAG" "$TAG" >> "$RUN/logs/validity.log" 2>&1 || tl "== $TAG CONTAMINATED =="
done
status "RERUN DONE"
touch "$RUN/rerun.done"
