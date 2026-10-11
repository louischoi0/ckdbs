#!/bin/bash
# Every fsync/fdatasync with its duration: BA-S4's binary against B, alternating, same hour.
D=/home/cdkbs/bench-runs/carve-diag
for v in 66bb11b2 B; do
  cat > $D/bin/wrap-$v <<EOF2
#!/bin/bash
exec strace -f --seccomp-bpf -e trace=fsync,fdatasync,rename,renameat2 -T -tt -y -o \$STRACE_OUT $D/bin/kds_server-$v "\$@"
EOF2
  chmod +x $D/bin/wrap-$v
done
for round in 1 2 3 4; do
  for v in 66bb11b2 B; do
    out=$D/out5-$v-$round
    mkdir -p $out $D/work
    STRACE_OUT=$out/strace.txt python3 $D/driver/ba_census.py --engine kds --bin $D/bin/wrap-$v --port 15651 \
      --shape insert1 --durability relaxed --cores 1 --sessions 16 --server-cpus 0 \
      --out $out --runs 1 --seconds 8 --warmup 2 --client-cpus 4,5,6,7 \
      --max-load 2 --max-wait 300 --workdir $D/work >> $D/run5.log 2>&1 || echo "FAILED $v $round" >> $D/run5.log
  done
done
