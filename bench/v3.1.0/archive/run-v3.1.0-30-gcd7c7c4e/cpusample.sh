#!/bin/bash
# cpusample.sh <outfile> <flagfile>: one line per second, whole-host busy % and steal %, until the flag file exists.
OUT=$1; FLAG=$2
read -r _ u n s i w q sq st _ < /proc/stat
pt=$((u+n+s+i+w+q+sq+st)); pi=$((i+w)); ps=$st
while [ ! -e "$FLAG" ]; do
    sleep 1
    read -r _ u n s i w q sq st _ < /proc/stat
    t=$((u+n+s+i+w+q+sq+st)); idl=$((i+w)); stl=$st
    dt=$((t-pt))
    [ "$dt" -gt 0 ] && echo "$(date -u +%FT%TZ) busy=$((100*(dt-(idl-pi))/dt)) steal=$((100*(stl-ps)/dt))" >> "$OUT"
    pt=$t; pi=$idl; ps=$stl
done
