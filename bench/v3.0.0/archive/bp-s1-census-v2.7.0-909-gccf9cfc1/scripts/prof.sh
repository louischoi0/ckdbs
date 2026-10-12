source /tmp/claude-1000/-home-cdkbs-ckdbs/701b3864-f591-47e7-bbe7-0972e02af1f5/scratchpad/bp-s1/common.sh
cd $WT
mkdir -p $RUN/out/prof
gcc -O2 -shared -fPIC -o $RUN/bin/libkdssampler.so tools/kds_sampler.c -lpthread 2>> $RUN/out/prof.log
cat > $RUN/bin/kds_server_prof <<EOF
#!/bin/sh
exec env LD_PRELOAD=$RUN/bin/libkdssampler.so KDS_SAMPLER_OUT=$RUN/out/prof/dump-\$KDS_TAG KDS_SAMPLER_HZ=1000 $BIN "\$@"
EOF
chmod +x $RUN/bin/kds_server_prof
for k in 4 1; do
  case $k in
    1) sc=0; cc=4,5,6,7; s=2;;
    4) sc=0,2,4,6; cc=1,3,5,7; s=8;;
  esac
  export KDS_TAG=k$k
  hoststamp "prof relaxed insertN k$k"
  python3 $SP/watch.py "bp-s1/bin/kds_server --config" 8 14 $RUN/out/prof/window-k$k.json &
  W=$!
  python3 tools/ba_census.py --engine kds --bin $RUN/bin/kds_server_prof --port 31951 --shape insertN --durability relaxed --cores $k --sessions $s \
    --server-cpus $sc --client-cpus $cc --placement balanced --seconds 20 --warmup 0 --runs 1 \
    --workdir $RUN/work-prof --out $RUN/out/prof/census-k$k --max-load 2.5 --max-wait 90 >> $RUN/out/prof.log 2>&1
  wait $W
done
touch $RUN/out/prof.done
