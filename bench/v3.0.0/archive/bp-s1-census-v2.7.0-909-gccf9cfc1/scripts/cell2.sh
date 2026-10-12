source /tmp/claude-1000/-home-cdkbs-ckdbs/701b3864-f591-47e7-bbe7-0972e02af1f5/scratchpad/bp-s1/common.sh
cd $WT
mkdir -p $RUN/out/cell2
for r in ${RUNS:-0 1 2}; do
  for k in 1 4; do
    case $k in
      1) sc=0; cl=4; rd="";;
      4) sc=0,2,4,6; cl=1,3,5,7; rd="--readers";;
    esac
    hoststamp "cell2 cabinauto group k$k r$r"
    taskset -c $cl python3 $SP/cell2.py --bin $BIN --workdir $RUN/work --cores $k --server-cpus $sc \
      --port-off 31991 --port-on 31992 --durability group --run $r $rd --out $RUN/out/cell2/cabin-k$k-r$r.json >> $RUN/out/cell2.log 2>&1
  done
done
touch $RUN/out/cell2.done
