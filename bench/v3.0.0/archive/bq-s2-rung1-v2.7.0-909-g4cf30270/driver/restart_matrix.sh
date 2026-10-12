#!/bin/sh
# Stops BQ-S2's matrix and its current cell (driver, server, clients) by
# PID, so the watchdog relaunches the chain with the edited matrix; the
# interrupted cell has no result and is re-run.
for p in $(pgrep -f 's2/s2_matrix.py'); do kill $p 2>/dev/null; done
sleep 1
for p in $(pgrep -f 'tools/bl_mixed.py'); do kill $p 2>/dev/null; done
for p in $(pgrep -f 'kds_server.blmixed'); do kill $p 2>/dev/null; done
sleep 2
echo "left: $(pgrep -fc 's2/s2_matrix.py') matrix, $(pgrep -fc 'tools/bl_mixed.py') driver, $(pgrep -fc 'kds_server.blmixed') server"
