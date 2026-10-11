#!/bin/bash
# usage: wait_file.sh <flag file> <max seconds>
t=0
until [ -f "$1" ] || [ $t -ge $2 ]; do sleep 10; t=$((t+10)); done
[ -f "$1" ] && echo "flag present" || echo "timeout after $t s"
