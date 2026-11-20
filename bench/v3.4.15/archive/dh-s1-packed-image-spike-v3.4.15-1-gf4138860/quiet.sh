#!/bin/bash
# waits until no compiler process has run for 30 consecutive seconds; pattern assembled here so no argv carries it
pat="cc1p""lus"
quiet=0
while [ $quiet -lt 30 ]; do
  if pgrep -x "$pat" >/dev/null; then quiet=0; else quiet=$((quiet+3)); fi
  sleep 3
done
date -u +%T > quiet.done
