#!/bin/bash
# quiet gate: no build/ctest/cc1plus, no foreign kds_server (sandbox excepted), CPU idle >= 90% for 30 consecutive s; then launch driver
D=/home/louis/bench-runs/dl-close
S=/tmp/claude-1000/-home-louis-ckdbs/e799d8bb-d13e-4b45-a458-1ebc557349e3/scratchpad
t0=$(date +%s); ok=0
idle() {
  read -r _ a b c d e f g h _ < /proc/stat; t1=$((a+b+c+d+e+f+g+h)); i1=$((d+e))
  sleep 5
  read -r _ a b c d e f g h _ < /proc/stat; t2=$((a+b+c+d+e+f+g+h)); i2=$((d+e))
  echo $(( 100*(i2-i1)/(t2-t1) ))
}
while true; do
  busy=$(ps -eo args | grep -E "cc1plus|cmake --build|ctest|kds_server|dl_close|di_arms|dl_s0|\.py" | grep -v -E "grep|gate_run|build-sandbox|sandbox_server|networkd|unattended" | wc -l)
  id=$(idle)
  if [ "$busy" = 0 ] && [ "$id" -ge 90 ]; then ok=$((ok+5)); else ok=0; fi
  echo "gate held=$(( $(date +%s)-t0 ))s busy=$busy idle=$id ok=$ok $(date -u +%T)" > $D/gate.status
  [ $ok -ge 30 ] && break
done
echo "released $(date -u +%T) held=$(( $(date +%s)-t0 ))" >> $D/gate.status
echo "$(date -u +%FT%T) hold=$(( $(date +%s)-t0 ))" > $D/driver.start
cd $S
python3 dl_close.py --bins a=$D/kds_a,b=$D/kds_b,x=$D/kds_x --tools /home/louis/ckdbs/tools --wd $D/run --out $D/raw.jsonl --status $D/run.status --port 28300 > $D/driver.log 2>&1
echo "$(date -u +%FT%T)" > $D/driver.end
