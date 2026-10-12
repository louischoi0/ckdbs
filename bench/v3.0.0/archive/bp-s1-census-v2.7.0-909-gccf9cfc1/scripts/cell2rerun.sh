source /tmp/claude-1000/-home-cdkbs-ckdbs/701b3864-f591-47e7-bbe7-0972e02af1f5/scratchpad/bp-s1/common.sh
cd $WT
COMP='ctest|cc1plus|cmake --build|kds_tests|ninja'
quiet() {
  if pgrep -f "$COMP" > /dev/null; then return 1; fi
  awk '{ if ($1 <= 1.6) exit 0; else exit 1 }' /proc/loadavg
}
waitquiet() {
  until quiet; do sleep 5; done
  sleep 20
  quiet
}
mkdir -p $RUN/out/cell2
mv $RUN/out/cell2/cabin-k4-r2.json $RUN/out/cell2/contaminated-cabin-k4-r2.json 2>/dev/null
mv $RUN/out/cell2/cabin-k4-r2.json.driver.json $RUN/out/cell2/contaminated-cabin-k4-r2.json.driver.json 2>/dev/null
mv $RUN/out/cell2/cabin-k4-r2.json.driver.txt $RUN/out/cell2/contaminated-cabin-k4-r2.json.driver.txt 2>/dev/null
attempt=0
while [ $attempt -lt 20 ]; do
  attempt=$((attempt+1))
  until waitquiet; do sleep 5; done
  hoststamp "cell2 rerun cabinauto group k4 r2 attempt$attempt"
  taskset -c 1,3,5,7 python3 $SP/cell2.py --bin $BIN --workdir $RUN/work --cores 4 --server-cpus 0,2,4,6 \
    --port-off 31991 --port-on 31992 --durability group --run 2 --readers --out $RUN/out/cell2/cabin-k4-r2.json >> $RUN/out/cell2.log 2>&1 &
  DP=$!
  dirty=0
  while kill -0 $DP 2>/dev/null; do
    if pgrep -f "$COMP" > /dev/null; then
      dirty=1
      echo "cell2 k4 r2 attempt$attempt: competitor appeared" >> $RUN/out/cell2.log
      kill $DP 2>/dev/null
      sleep 1
      for p in $(pgrep -f "cell2-k4-r2"); do kill $p 2>/dev/null; done
      break
    fi
    sleep 3
  done
  wait $DP 2>/dev/null
  for p in $(pgrep -f "cell2-k4-r2"); do kill $p 2>/dev/null; done
  sleep 3
  if [ $dirty = 0 ]; then break; fi
  rm -f $RUN/out/cell2/cabin-k4-r2.json $RUN/out/cell2/cabin-k4-r2.json.driver.*
  echo "cell2 k4 r2 attempt$attempt discarded" >> $RUN/out/cell2.log
done
touch $RUN/out/cell2rerun.done
