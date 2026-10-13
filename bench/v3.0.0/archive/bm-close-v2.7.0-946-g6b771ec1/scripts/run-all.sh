#!/bin/sh
# BM-S6 close: every measurement phase in sequence; a phase with a .done flag is skipped (resumable).
# Detached: sh launch-run.sh   (log: run-all.log; flags: $R/flags/<phase>.done)
WT=/home/cdkbs/ckdbs/.claude/worktrees/bm-scan-kernel-open
B=/home/cdkbs/bench-runs/bm-s6/bin
R=/home/cdkbs/bench-runs/bm-s6
GATE="--cell-quiet-pct 10 --cell-quiet-wait 60 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
ARMS="--arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B --arm Boff=$B/kds_server-B,scan_kernel=off --arm Apad=$B/kds_server-Apad"
mkdir -p $R/flags $R/census $R/bn200 $R/bn1000 $R/bn10000 $R/pg $R/prof
phase() { # name command...
    name=$1; shift
    if [ -f $R/flags/$name.done ]; then echo "skip $name"; return 0; fi
    echo "=== $name start $(date -u +%H:%M:%S) load $(cut -d' ' -f1-3 /proc/loadavg)"
    "$@"
    rc=$?
    echo "=== $name end $(date -u +%H:%M:%S) rc=$rc load $(cut -d' ' -f1-3 /proc/loadavg)"
    if [ $rc -eq 0 ]; then touch $R/flags/$name.done; else touch $R/flags/$name.failed; fi
}
census() {
    python3 $WT/tools/bm_scan.py census $ARMS --workdir $R/census --port 32401 --runs 8 \
        --pin-server 2 --pin-driver 4 --quiet-load 2 --quiet-wait 900 $GATE \
        --out $R/census/census.json > $R/census/census.log 2>&1
}
pgvac() {
    python3 $WT/tools/bn_pg_scan.py --shapes bm --port 15434 --runs 8 --scan-ops 192 --blocks 12 \
        --point-ops 3000 --pin-backend 2 --pin-driver 4 $GATE --out $R/pg/pg.json > $R/pg/pg.log 2>&1
}
pgnovac() {
    python3 $WT/tools/bn_pg_scan.py --shapes bm --no-vacuum --port 15434 --runs 8 --scan-ops 192 --blocks 12 \
        --point-ops 3000 --pin-backend 2 --pin-driver 4 $GATE --out $R/pg/pg-novac.json > $R/pg/pg-novac.log 2>&1
}
bn() { # rows scan-ops range-ops port
    python3 $WT/tools/bn_census.py $ARMS --workdir $R/bn$1 --port $4 --runs 8 --pin-server 2 --pin-driver 4 \
        --rows $1 --scan-rows $1 --ops 3000 --scan-ops $2 --range-ops $3 --blocks 12 \
        --only select-pk,update-lit,update-range,scan --quiet-load 2 --quiet-wait 900 $GATE \
        --out $R/bn$1/bn.json > $R/bn$1/bn.log 2>&1
}
prof() {
    python3 $WT/tools/bm_scan.py profile --bin $B/kds_server-Bfp --sampler $B/kds_sampler.so \
        --sampler-dir $R/prof --workdir $R/prof --port 32421 --pin-server 2 --pin-driver 4 \
        --profile-cells bn,wide-h --profile-scans 20000 --point-ops 3000 --out $R/prof/profile.json \
        > $R/prof/profile.log 2>&1
}
phase census census
phase pgvac pgvac
phase pgnovac pgnovac
phase bn1000 bn 1000 800 240 32501
phase bn200 bn 200 2000 480 32601
phase bn10000 bn 10000 200 60 32701
phase prof prof
echo "=== all phases done $(date -u +%H:%M:%S)"
touch $R/flags/ALL.done
