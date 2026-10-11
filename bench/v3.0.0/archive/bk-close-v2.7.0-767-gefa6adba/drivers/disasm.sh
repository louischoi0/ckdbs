#!/bin/bash
# usage: disasm.sh <binary> <symbol regex>   -- the instructions of one function
BIN=$1
SYM=$2
line=$(nm -S -C "$BIN" | grep -E " T $SYM" | head -n 1)
addr=$(echo "$line" | awk '{print $1}')
size=$(echo "$line" | awk '{print $2}')
start=$((16#$addr))
stop=$((start + 16#$size))
echo "symbol: $(echo "$line" | cut -c1-100)  size $((16#$size)) bytes"
objdump -d -C --no-show-raw-insn --start-address=$start --stop-address=$stop "$BIN" | grep -E "^ +[0-9a-f]+:" | sed -E 's/^ +//' | cut -c1-110
