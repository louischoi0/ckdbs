#!/bin/bash
G=/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad/g21
until grep -q "END m3-s0-B-a" $G/status6.txt; do sleep 2; done
ps -eo pid,args | grep -E "master6.sh" | grep -v grep | awk '{print $1}' | xargs kill
sleep 1; echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) DONE (m1x cut for the budget)" >> $G/status6.txt; touch $G/host6.stop
