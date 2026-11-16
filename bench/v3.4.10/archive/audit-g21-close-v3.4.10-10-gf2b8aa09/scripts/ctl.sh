#!/bin/bash
G=/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad/g21
until grep -q "END m3-s2-A-b" $G/status5.txt; do sleep 3; done
ps -eo pid,args | grep -E "master5.sh" | grep -v grep | awk '{print $1}' | xargs kill
sleep 1; touch $G/host5.stop; sleep 3
rm -f $G/host6.stop
bash $G/master6.sh > $G/master6.console 2>&1
