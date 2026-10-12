source /tmp/claude-1000/-home-cdkbs-ckdbs/701b3864-f591-47e7-bbe7-0972e02af1f5/scratchpad/bp-s1/common.sh
cd $WT
mkdir -p $RUN/out/cell5
for r in 0 1 2; do
  for dur in group relaxed; do
    for k in 1 4 8; do
      case $k in
        1) sc=0; cl=1;;
        4) sc=0,2,4,6; cl=1;;
        8) sc=0,1,2,3,4,5,6,7; cl=0-7;;
      esac
      for mode in plain fence; do
        hoststamp "cell5 locks $dur k$k $mode r$r"
        fl=""
        if [ $mode = fence ]; then fl="--fence"; fi
        taskset -c $cl python3 $SP/lockcell.py --bin $BIN --workdir $RUN/work --port 31981 --cores $k --server-cpus $sc \
          --durability $dur --run $r $fl --out $RUN/out/cell5/x >> $RUN/out/cell5.log 2>&1
      done
    done
  done
done
touch $RUN/out/cell5.done
