#!/bin/bash
# BP's second host window: BP-S3's own A/B (BP-R6) and the BP-S1 census
# re-run of H7, H9 and H10 on the fixed counters. Every cell waits for a quiet
# host, writes one result file, and is skipped when that file exists, so a
# relaunch (the watchdog's) resumes where the chain stopped.
#   BIN   the build at BP-S3's head      BINA  the census build at 1592007e
set -u
R=/home/cdkbs/bench-runs/bp-s1/v2
S=$R/scripts
WT=/home/cdkbs/ckdbs/.claude/worktrees/bp-per-core-residue
BIN=$R/bin/kds_server.b
BINA=$R/bin/kds_server.a
OUT=$R/out
mkdir -p $OUT/ab $OUT/h7 $OUT/h9 $OUT/h10 $R/work
STAMPS=$OUT/hoststamps.log

busy() {
  pgrep -af 'cc1plus|cmake --build|ctest|kds_tests|tools/bl_mixed.py --bin|bq_s1_matrix' \
    | grep -v -e pgrep -e shell-snapshots -e 'bp-s1/v2' > /dev/null
}
gate() {  # label
  while busy || ! awk '{ exit !($1 <= 1.6) }' /proc/loadavg; do
    date +%s > $OUT/gate.beat
    sleep 10
  done
  echo "$(date -u '+%F %T') $1 load=$(cut -d' ' -f1-3 /proc/loadavg) df=$(df -T $R/work | tail -1 | awk '{print $2}')" >> $STAMPS
  date +%s > $OUT/cell.start
}
done_cell() { echo "$(date -u '+%F %T') after $1 rc=$2" >> $STAMPS; rm -f $OUT/cell.start; }

# ---- BP-S3's A/B (cores = 1, relaxed, interleaved blocks; BP-G1 cells) ----
for r in 1 2 3; do
  f=$OUT/ab/bn-r$r.json
  [ -s $f ] && continue
  gate "ab bn r$r"
  taskset -c 4 python3 $WT/tools/bn_census.py --bin $BINA --bin-b $BIN --workdir $R/work/bn \
    --port 32011 --pin-server 2 --pin-driver 4 --rows 10000 --ops 3000 --range-ops 60 \
    --blocks 12 --runs 1 --only select-pk,update-lit,update-range --out $f.tmp > $OUT/ab/bn-r$r.log 2>&1
  rc=$?; [ $rc = 0 ] && mv $f.tmp $f; done_cell "ab bn r$r" $rc
done
for r in 1 2 3; do
  for dur in group strict; do
    for side in a b; do
      d=$OUT/ab/commit-$dur-$side-r$r   # ba_census writes one JSON per run into a directory
      ls $d/*.json > /dev/null 2>&1 && continue
      b=$BINA; [ $side = b ] && b=$BIN
      gate "ab commit $dur $side r$r"
      rm -rf $d
      python3 $WT/tools/ba_census.py --shape insert1 --durability $dur --cores 1 --sessions 1 \
        --seconds 20 --warmup 2 --runs 1 --bin $b --port 32021 --server-cpus 2 --client-cpus 4 \
        --workdir $R/work/ba --out $d --max-wait 0 > $d.log 2>&1
      rc=$?; [ $rc = 0 ] || rm -rf $d; done_cell "ab commit $dur $side r$r" $rc
    done
  done
done

# ---- H7: checkpoint runs, pages written and data syncs per period ----
for rep in a b; do
  for k in 1 2 4; do
    tag=bp2-k$k-cp5000-$rep
    f=$OUT/h7/$tag.json
    [ -s $f ] && continue
    gate "h7 $tag"
    rm -rf $R/work/bi/$tag.db* $R/work/bi/$tag.log
    taskset -c 4-7 python3 $WT/tools/bi_checkpoint_stall.py --server $BIN --workdir $R/work/bi \
      --cores $k --durability group --seconds 240 --port 32031 --tag $tag --checkpoint-ms 5000 \
      --out $f.tmp > $OUT/h7/$tag.log 2>&1
    rc=$?; [ $rc = 0 ] && mv $f.tmp $f; done_cell "h7 $tag" $rc
    rm -rf $R/work/bi/$tag.db*
  done
done

# ---- H9: PURGE of 100,000 rows and the named-key loop ----
for r in 0 1 2; do
  for k in 1 4; do
    sc=0; [ $k = 4 ] && sc=0,2,4,6
    f=$OUT/h9/inflight-k$k-r$r.json
    [ -s $f ] && continue
    gate "h9 k$k r$r"
    taskset -c 5 python3 $S/inflight_cell.py --bin $BIN --workdir $R/work --port 32041 --cores $k \
      --server-cpus $sc --durability relaxed --run $r --out $f.tmp > $OUT/h9/k$k-r$r.log 2>&1
    rc=$?; [ $rc = 0 ] && mv $f.tmp $f; done_cell "h9 k$k r$r" $rc
  done
done

# ---- H10: the lock cells, plain against one held fence ----
for r in 0 1 2; do
  for dur in relaxed group; do
    for k in 1 4 8; do
      case $k in 1) sc=0; cl=1;; 4) sc=0,2,4,6; cl=1;; 8) sc=0,1,2,3,4,5,6,7; cl=0-7;; esac
      for mode in plain fence; do
        f=$OUT/h10/$dur-k$k-$mode-r$r.done
        [ -f $f ] && continue
        fl=""; [ $mode = fence ] && fl="--fence"
        gate "h10 $dur k$k $mode r$r"
        taskset -c $cl python3 $S/lockcell.py --bin $BIN --workdir $R/work --port 32051 --cores $k \
          --server-cpus $sc --durability $dur --run $r $fl --out $OUT/h10/x > $OUT/h10/$dur-k$k-$mode-r$r.log 2>&1
        rc=$?; [ $rc = 0 ] && touch $f; done_cell "h10 $dur k$k $mode r$r" $rc
      done
    done
  done
done
touch $OUT/all.done
