#!/bin/bash
# The close A/B for BP and BQ (CLAUDE.md step 3; BP-S8 §6.2, BQ-S5), release
# builds from exports. A = cf889b63 (BP's opening; byte-identical to BQ's
# opening 59c4139f), Q = 4cf30270 (BQ's instruments, BQ's only engine
# change), B = 3e77e7d1 (the close of both), S6A = ccf9cfc1 (BP before
# BP-S6). Every cell waits for a quiet host, writes one result, and is
# skipped when it exists (resume).
set -u
C=/home/cdkbs/bench-runs/close
WT=/home/cdkbs/ckdbs/.claude/worktrees/bp-per-core-residue
OUT=$C/out
A=$C/bin/kds_server.cf889b63
Q=/home/cdkbs/bench-runs/bq/bin/kds_server.4cf30270
B=$C/bin/kds_server.3e77e7d1
S6A=/home/cdkbs/bench-runs/bp-s1/v2/bin/kds_server.b
S=/home/cdkbs/bench-runs/bp-s1/v2/scripts
mkdir -p $OUT/bn $OUT/ba $OUT/lock $C/work
STAMPS=$OUT/hoststamps.log

busy() {
  pgrep -af 'cc1plus|cmake --build|ctest|kds_tests|tools/bl_mixed.py --bin|s2_matrix|bp-s1/v2/chain' \
    | grep -v -e pgrep -e shell-snapshots -e 'bench-runs/close' > /dev/null
}
gate() {
  while busy || ! awk '{ exit !($1 <= 1.6) }' /proc/loadavg; do date +%s > $OUT/gate.beat; sleep 10; done
  echo "$(date -u '+%F %T') $1 load=$(cut -d' ' -f1-3 /proc/loadavg) competing=$(pgrep -af 'kds_server|python3' | grep -v -e pgrep -e bench-runs/close -e autotrade -e kds.conf | wc -l)" >> $STAMPS
  date +%s > $OUT/cell.start
}
done_cell() { echo "$(date -u '+%F %T') after $1 rc=$2" >> $STAMPS; rm -f $OUT/cell.start; }

# ---- cores = 1, interleaved blocks: BP's whole change and BQ's ----
for r in 1 2 3; do
  for pair in "AB $A $B" "AQ $A $Q"; do
    set -- $pair
    f=$OUT/bn/$1-r$r.json
    [ -s $f ] && continue
    gate "bn $1 r$r"
    rm -rf $C/work/bn
    taskset -c 4 python3 $WT/tools/bn_census.py --bin $2 --bin-b $3 --workdir $C/work/bn --port 32111 \
      --pin-server 2 --pin-driver 4 --rows 10000 --ops 3000 --range-ops 60 --blocks 12 --runs 1 \
      --only select-pk,update-lit,update-range --out $f.tmp > $OUT/bn/$1-r$r.log 2>&1
    rc=$?; [ $rc = 0 ] && mv $f.tmp $f; done_cell "bn $1 r$r" $rc
  done
done

# ---- cores = 4: point reads and the monotonic insert, three binaries rotated ----
for r in 1 2 3; do
  for shape in point insert1; do
    case $r in 1) order="A Q B";; 2) order="Q B A";; 3) order="B A Q";; esac
    for side in $order; do
      eval bin=\$$side
      d=$OUT/ba/$shape-$side-r$r
      ls $d/*.json > /dev/null 2>&1 && continue
      gate "ba $shape $side r$r"
      rm -rf $d
      python3 $WT/tools/ba_census.py --shape $shape --durability relaxed --cores 4 --sessions 8 \
        --seconds 20 --warmup 2 --runs 1 --bin $bin --port 32121 --server-cpus 0,2,4,6 \
        --client-cpus 1,3,5,7 --workdir $C/work/ba --out $d --max-wait 0 > $d.log 2>&1
      rc=$?; [ $rc = 0 ] || rm -rf $d; done_cell "ba $shape $side r$r" $rc
    done
  done
done

# ---- BP-S6's A/B: the lock cells at cores 8, plain and one held fence ----
for r in 0 1 2; do
  for mode in plain fence; do
    for side in S6A B; do
      eval bin=\$$side
      f=$OUT/lock/k8-$mode-$side-r$r.done
      [ -f $f ] && continue
      fl=""; [ $mode = fence ] && fl="--fence"
      gate "lock k8 $mode $side r$r"
      taskset -c 0-7 python3 $S/lockcell.py --bin $bin --workdir $C/work --port 32131 --cores 8 \
        --server-cpus 0,1,2,3,4,5,6,7 --durability relaxed --run $r $fl --out $OUT/lock/$side \
        > $OUT/lock/k8-$mode-$side-r$r.log 2>&1
      rc=$?; [ $rc = 0 ] && touch $f; done_cell "lock k8 $mode $side r$r" $rc
    done
  done
done
touch $OUT/all.done
