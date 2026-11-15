#!/bin/bash
G=/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad/g21
while ! grep -q "DONE" $G/status6.txt 2>/dev/null; do
  now=$(date +%s); best=999999
  for f in $G/heartbeat5 $G/heartbeat6; do [ -f $f ] || continue; hb=$(cut -d' ' -f1 $f); a=$((now-hb)); [ $a -lt $best ] && best=$a; done
  echo "$(date -u +%T) hb_age=$best s5=$(tail -n 1 $G/status5.txt | cut -c1-60) s6=$(tail -n 1 $G/status6.txt 2>/dev/null | cut -c1-60)" >> $G/wd.log
  [ $best -gt 60 ] && echo "$(date -u +%T) STALE" >> $G/wd.alert
  sleep 60
done
