#!/bin/bash
# Every fsync/fdatasync with its duration, per variant: S7p2 (before S12) against S12, alternating.
D=/home/cdkbs/bench-runs/carve-diag
for v in 91a14db0 7fa2bd96; do
  cat > $D/bin/wrap-$v <<EOF2
#!/bin/bash
exec strace -f --seccomp-bpf -e trace=fsync,fdatasync,rename,renameat2 -T -tt -y -o \$STRACE_OUT $D/bin/kds_server-$v "\$@"
EOF2
  chmod +x $D/bin/wrap-$v
done
for round in 1 2 3; do
  for v in 91a14db0 7fa2bd96; do
    out=$D/out4-$v-$round
    mkdir -p $out $D/work
    STRACE_OUT=$out/strace.txt python3 $D/driver/ba_census.py --engine kds --bin $D/bin/wrap-$v --port 15651 \
      --shape insert1 --durability relaxed --cores 1 --sessions 16 --server-cpus 0 \
      --out $out --runs 1 --seconds 8 --warmup 2 --client-cpus 4,5,6,7 \
      --max-load 2 --max-wait 300 --workdir $D/work >> $D/run4.log 2>&1 || echo "FAILED $v $round" >> $D/run4.log
  done
done
