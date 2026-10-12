source /tmp/claude-1000/-home-cdkbs-ckdbs/701b3864-f591-47e7-bbe7-0972e02af1f5/scratchpad/bp-s1/common.sh
cd $WT
mkdir -p $RUN/out/cell3
SECS=${SECS:-600}
COMP='ctest|cc1plus|cmake --build|kds_tests|ninja'
quiet() { # no competitor process, 1-min load under limit
  local lim=${1:-1.6}
  if pgrep -f "$COMP" > /dev/null; then return 1; fi
  awk -v l=$lim '{ if ($1 <= l) exit 0; else exit 1 }' /proc/loadavg
}
waitquiet() {
  local t0=$(date +%s)
  until quiet; do sleep 5; if [ $(( $(date +%s) - t0 )) -gt 3600 ]; then echo "waitquiet timeout" >> $RUN/out/cell3.log; return 1; fi; done
  # a second reading 20 s later, so a build starting right now is not missed
  sleep 20
  quiet
}
for k in ${KS:-1 2 4}; do
  r=${RUNID:-0}
  attempt=0
  while [ $attempt -lt ${ATTEMPTS:-4} ]; do
    attempt=$((attempt+1))
    until waitquiet; do sleep 5; done
    hoststamp "cell3 checkpoint group k$k ${SECS}s run$r attempt$attempt"
    rm -f $RUN/work/bi/bp-c$k.db* $RUN/work/bi/bp-c$k.log
    taskset -c 4-7 python3 tools/bi_checkpoint_stall.py --server $BIN --workdir $RUN/work/bi --cores $k \
      --durability group --seconds $SECS --port 31961 --tag bp-c$k --out $RUN/out/cell3/bi-k$k-r$r.json \
      >> $RUN/out/cell3.log 2>&1 &
    DP=$!
    dirty=0
    while kill -0 $DP 2>/dev/null; do
      if pgrep -f "$COMP" > /dev/null; then
        dirty=1
        echo "k$k attempt$attempt: competitor appeared: $(pgrep -af "$COMP" | head -2 | cut -c1-120 | tr '\n' ';')" >> $RUN/out/cell3.log
        kill $DP 2>/dev/null
        sleep 1
        for p in $(pgrep -f "bp-c$k.conf"); do kill $p 2>/dev/null; done
        break
      fi
      sleep 3
    done
    wait $DP 2>/dev/null
    cp $RUN/work/bi/bp-c$k.log $RUN/out/cell3/bi-k$k-r$r.server.log 2>/dev/null
    for p in $(pgrep -f "bp-c$k.conf"); do kill $p 2>/dev/null; done
    sleep 3
    rm -f $RUN/work/bi/bp-c$k.db*
    if [ $dirty = 0 ]; then break; fi
    rm -f $RUN/out/cell3/bi-k$k-r$r.json
    echo "k$k attempt$attempt discarded (contaminated)" >> $RUN/out/cell3.log
  done
done
touch $RUN/out/cell3b.done${RUNID:-}
