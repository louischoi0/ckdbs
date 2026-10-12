source /tmp/claude-1000/-home-cdkbs-ckdbs/701b3864-f591-47e7-bbe7-0972e02af1f5/scratchpad/bp-s1/common.sh
cd $WT
mkdir -p $RUN/out/cell4
for r in 0 1 2; do
  for k in 1 4; do
    case $k in
      1) sc=0;;
      4) sc=0,2,4,6;;
    esac
    hoststamp "cell4 inflight relaxed k$k r$r"
    taskset -c 5 python3 $SP/inflight_cell.py --bin $BIN --workdir $RUN/work --port 31971 --cores $k --server-cpus $sc \
      --durability relaxed --run $r --out $RUN/out/cell4/inflight-k$k-r$r.json >> $RUN/out/cell4.log 2>&1
  done
done
# the PURGE under the sampler
for k in 1 4; do
  case $k in
    1) sc=0;;
    4) sc=0,2,4,6;;
  esac
  export KDS_TAG=purge-k$k
  hoststamp "cell4 purge under sampler k$k"
  taskset -c 5 python3 $SP/inflight_cell.py --bin $RUN/bin/kds_server_prof --workdir $RUN/work --port 31971 --cores $k --server-cpus $sc \
    --durability relaxed --run 9 --sample --loop 1000 --waits 1 --out $RUN/out/cell4/inflight-sampled-k$k.json >> $RUN/out/cell4.log 2>&1
done
touch $RUN/out/cell4.done
