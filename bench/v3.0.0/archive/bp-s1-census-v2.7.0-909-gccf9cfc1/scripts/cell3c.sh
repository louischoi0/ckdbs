source /tmp/claude-1000/-home-cdkbs-ckdbs/701b3864-f591-47e7-bbe7-0972e02af1f5/scratchpad/bp-s1/common.sh
cd $WT
mkdir -p $RUN/out/cell3c
SECS=${SECS:-240}
COMP='ctest|cc1plus|cmake --build|kds_tests|ninja'
quiet() {
  local lim=${1:-1.6}
  if pgrep -f "$COMP" > /dev/null; then return 1; fi
  awk -v l=$lim '{ if ($1 <= l) exit 0; else exit 1 }' /proc/loadavg
}
waitquiet() {
  local t0=$(date +%s)
  until quiet; do sleep 5; if [ $(( $(date +%s) - t0 )) -gt 7200 ]; then return 1; fi; done
  sleep 20
  quiet
}
# entries: cores:checkpoint_ms:repeat
for ent in ${ENTRIES:-4:5000:a 4:20000:a 4:5000:b 4:20000:b}; do
  k=${ent%%:*}; rest=${ent#*:}; cp=${rest%%:*}; rep=${rest#*:}
  tag=bp-k${k}-cp${cp}-${rep}
  attempt=0
  while [ $attempt -lt ${ATTEMPTS:-40} ]; do
    attempt=$((attempt+1))
    until waitquiet; do sleep 5; done
    hoststamp "cell3c checkpoint group k$k cadence ${cp}ms rep $rep ${SECS}s attempt$attempt"
    rm -f $RUN/work/bi/$tag.db* $RUN/work/bi/$tag.log
    taskset -c 4-7 python3 tools/bi_checkpoint_stall.py --server $BIN --workdir $RUN/work/bi --cores $k \
      --durability group --seconds $SECS --port 31961 --tag $tag --checkpoint-ms $cp --out $RUN/out/cell3c/$tag.json \
      >> $RUN/out/cell3c.log 2>&1 &
    DP=$!
    dirty=0
    while kill -0 $DP 2>/dev/null; do
      if pgrep -f "$COMP" > /dev/null; then
        dirty=1
        echo "$tag attempt$attempt: competitor appeared: $(pgrep -af "$COMP" | head -2 | cut -c1-120 | tr '\n' ';')" >> $RUN/out/cell3c.log
        kill $DP 2>/dev/null
        sleep 1
        for p in $(pgrep -f "$tag.conf"); do kill $p 2>/dev/null; done
        break
      fi
      sleep 3
    done
    wait $DP 2>/dev/null
    cp $RUN/work/bi/$tag.log $RUN/out/cell3c/$tag.server.log 2>/dev/null
    for p in $(pgrep -f "$tag.conf"); do kill $p 2>/dev/null; done
    sleep 3
    rm -f $RUN/work/bi/$tag.db*
    if [ $dirty = 0 ]; then break; fi
    rm -f $RUN/out/cell3c/$tag.json
    echo "$tag attempt$attempt discarded (contaminated)" >> $RUN/out/cell3c.log
  done
done
touch $RUN/out/cell3c.done
