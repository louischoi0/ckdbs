source /tmp/claude-1000/-home-cdkbs-ckdbs/701b3864-f591-47e7-bbe7-0972e02af1f5/scratchpad/bp-s1/common.sh
cd $WT
mkdir -p $RUN/out/cell3 $RUN/work/bi
SECS=${SECS:-600}
for k in ${KS:-1 2 4}; do
  hoststamp "cell3 checkpoint group k$k ${SECS}s run${RUNID:-0}"
  # reactors are pinned by the driver to CPUs 0..k-1 (its own choice, SMT siblings above k=1);
  # loaders and probes are pinned to 4-7.
  taskset -c 4-7 python3 tools/bi_checkpoint_stall.py --server $BIN --workdir $RUN/work/bi --cores $k \
    --durability group --seconds $SECS --port 31961 --tag bp-c$k --out $RUN/out/cell3/bi-k$k-r${RUNID:-0}.json \
    >> $RUN/out/cell3.log 2>&1
  cp $RUN/work/bi/bp-c$k.log $RUN/out/cell3/bi-k$k-r${RUNID:-0}.server.log 2>/dev/null
  rm -f $RUN/work/bi/bp-c$k.db*
done
touch $RUN/out/cell3.done${RUNID:-}
