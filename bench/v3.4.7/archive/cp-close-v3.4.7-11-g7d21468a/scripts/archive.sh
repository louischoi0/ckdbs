#!/bin/bash
set -euo pipefail
SP=/tmp/claude-1000/-home-louis-ckdbs/5bdaae98-e052-4814-8a2c-eb67c5cf878d/scratchpad/cp-close
STAMP=$(cat $SP/run/current_stamp)
SRC=$SP/run/$STAMP
DST=/home/louis/ckdbs/.claude/worktrees/worktree-cp-lock-model-exploration/bench/v3.4.7/archive/cp-close-v3.4.7-11-g7d21468a
rm -rf "$DST"; mkdir -p "$DST/scripts" "$DST/results-analysis"
mkdir -p "$DST/pass"; tar -C "$SRC" --exclude='*.kdb.log' --exclude='*.server.out' --exclude='wal*' --exclude='server-files' --exclude='.sampler-stop' --exclude='heartbeat' --exclude='active_servers' -cf - . | tar -C "$DST/pass" -xf -
# M5 outputs (outside the stamp dir): label each by binary
for d in $SP/m5/out-*; do
  b=$(basename "$d")
  sha=$(grep '^binary_sha256' "$d/run.txt" | cut -d= -f2 | cut -c1-8)
  case $sha in d78b1147) l=base;; 049f27e6) l=close;; *) l=unknown;; esac
  mkdir -p "$DST/pass/m5-$l-${b#out-}"
  tar -C "$d" --exclude='server.out' -cf - . | tar -C "$DST/pass/m5-$l-${b#out-}" -xf -
done
cp $SP/run_all.sh $SP/run_extra.sh $SP/run_m1swap.sh $SP/host_sampler.sh $SP/watchdog.sh $SP/archive.sh "$DST/scripts/"
cp $SP/m1m4/run_m1m4.sh "$DST/scripts/m1m4_run_m1m4.sh"; cp $SP/m1m4/run_m1m4_swapped.sh "$DST/scripts/m1m4_run_m1m4_swapped.sh"
cp $SP/m1m4/srv.py "$DST/scripts/m1m4_srv.py"; cp $SP/m1m4/read_views.py "$DST/scripts/m1m4_read_views.py"
cp $SP/m2/run_m2.sh "$DST/scripts/m2_run_m2.sh"; cp $SP/m2/m2_summary.py $SP/m2/m2_sanity.py "$DST/scripts/"
cp $SP/m3/run_m3.sh "$DST/scripts/m3_run_m3.sh"; cp $SP/m3/read_views.py "$DST/scripts/m3_read_views.py"
cp $SP/m5/run_m5.sh "$DST/scripts/m5_run_m5.sh"
cp $SP/an_m14.py $SP/an_m1agg.py $SP/an_m3.py $SP/an_m5.py "$DST/scripts/"
cp $SP/run/status.txt $SP/run/watch.log "$DST/pass/" 2>/dev/null || true
cp $SP/m3_an.txt "$DST/results-analysis/m3_analysis.txt"
gzip -f "$DST/pass/m2/census.json"
find "$DST" -name '*.views.json' -size +500k -exec gzip -f {} \;
du -sh "$DST"
