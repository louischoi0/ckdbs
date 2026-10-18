#!/bin/bash
S=/tmp/claude-1000/-home-cdkbs-ckdbs/00ec4c4c-8445-418e-92d3-c235d2b282e7/scratchpad/cc-s0
R=/home/cdkbs/bench-runs/cc-s0
mkdir -p $R/run
rm -f $S/run.done
python3 $S/cc_s0_run.py --bin $R/kds_server --pristine $R/load/pristine.db --wd $R/run --out $S/out/results.json \
  --status $S/run.status --rounds ${ROUNDS:-3} --target-s 1.2 --cpu 2 \
  --s2 '{"n200":[20,"n200"],"n1k":[50,"n1k"],"ca":[300,"ca10k"],"df":[2000,"df200k"]}' > $S/run.log 2>&1
echo "rc=$?" >> $S/run.log
echo done > $S/run.done
