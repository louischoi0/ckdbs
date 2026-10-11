#!/bin/bash
# usage: callees.sh <binary> <symbol regex>   -- the call targets inside a function's body, counted
BIN=$1
SYM=$2
line=$(nm -S -C "$BIN" | grep -E " T $SYM" | head -n 1)
addr=$(echo "$line" | awk '{print $1}')
size=$(echo "$line" | awk '{print $2}')
start=$((16#$addr))
stop=$((start + 16#$size))
echo "symbol: $(echo "$line" | cut -c1-90)"
echo "size: $((16#$size)) bytes"
objdump -d -C --no-show-raw-insn --start-address=$start --stop-address=$stop "$BIN" \
  | grep -E "\bcall\b" | sed -E 's/^ +[0-9a-f]+:[[:space:]]+//' | sed -E 's/^call +[0-9a-f]+ //' | cut -c1-110 | sort | uniq -c | sort -rn | head -${3:-14}
