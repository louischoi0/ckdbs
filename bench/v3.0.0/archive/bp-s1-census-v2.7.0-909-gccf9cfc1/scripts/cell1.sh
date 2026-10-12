source /tmp/claude-1000/-home-cdkbs-ckdbs/701b3864-f591-47e7-bbe7-0972e02af1f5/scratchpad/bp-s1/common.sh
cd $WT
port=31951
for dur in group relaxed; do
  for k in 1 2 4; do
    s=$((2*k))
    case $k in
      1) sc=0; cc=4,5,6,7;;
      2) sc=0,2; cc=4,5,6,7;;
      4) sc=0,2,4,6; cc=1,3,5,7;;
    esac
    hoststamp "cell1 $dur k$k s$s"
    python3 tools/ba_census.py --engine kds --bin $BIN --port $port --shape insertN --durability $dur --cores $k --sessions $s \
      --server-cpus $sc --client-cpus $cc --placement balanced --seconds 20 --warmup 0 --runs 3 \
      --workdir $RUN/work --out $RUN/out/cell1-$dur-k$k --max-load 2.5 --max-wait 90 >> $RUN/out/cell1.log 2>&1
  done
done
touch $RUN/out/cell1.done
