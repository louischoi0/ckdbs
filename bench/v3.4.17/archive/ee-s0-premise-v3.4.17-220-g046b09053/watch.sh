#!/bin/sh
# 60 s watchdog over run.sh's progress file (the operator's rule of 2026-10-12).
P=/home/ukjin/ee-s0-run/final/progress
last=""
stale=0
while true; do
    cur=$(tail -1 "$P" 2>/dev/null)
    if [ "$cur" != "$last" ]; then
        echo "progress: $cur"
        last=$cur
        stale=0
    else
        stale=$((stale + 1))
    fi
    case "$cur" in DONE*) exit 0 ;; esac
    if grep -q FAILED "$P" 2>/dev/null; then
        echo "FAILED: $(grep FAILED "$P")"
        exit 1
    fi
    if [ "$stale" -ge 5 ]; then echo "STALLED: no progress for 5 min at '$cur'"; fi
    sleep 60
done
