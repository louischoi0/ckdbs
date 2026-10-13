#!/bin/sh
# Re-planned to the operator's one-hour budget (measuring ends 05:50 UTC): bn_census at 1,000 rows
# (A, A', B, B-off), the PostgreSQL twin (vacuumed, 3 runs), the B profile of bn alone.
WT=/home/cdkbs/ckdbs/.claude/worktrees/bm-scan-kernel-open
B=/home/cdkbs/bench-runs/bm-s6/bin
R=/home/cdkbs/bench-runs/bm-s6
DEADLINE=$(date -u -d '2026-10-13 05:50:00' +%s)
GATE="--cell-quiet-pct 10 --cell-quiet-wait 60 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
ARMS4="--arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B --arm Boff=$B/kds_server-B,scan_kernel=off"
mkdir -p $R/flags $R/bn1000 $R/pg $R/prof
fits() { # estimated seconds
    now=$(date +%s)
    [ $((now + $1)) -le $DEADLINE ]
}
phase() { # name est-seconds command...
    name=$1; est=$2; shift; shift
    if ! fits $est; then echo "=== $name NOT STARTED $(date -u +%H:%M:%S): cannot finish by 05:50"; touch $R/flags/$name.skipped; return 0; fi
    echo "=== $name start $(date -u +%H:%M:%S) load $(cut -d' ' -f1-3 /proc/loadavg)"
    "$@"
    rc=$?
    echo "=== $name end $(date -u +%H:%M:%S) rc=$rc load $(cut -d' ' -f1-3 /proc/loadavg)"
    if [ $rc -eq 0 ]; then touch $R/flags/$name.done; else touch $R/flags/$name.failed; fi
}
bn1000() {
    python3 $WT/tools/bn_census.py $ARMS4 --workdir $R/bn1000 --port 32501 --runs 8 --pin-server 2 --pin-driver 4 \
        --rows 1000 --scan-rows 1000 --ops 3000 --scan-ops 800 --range-ops 240 --blocks 12 \
        --only select-pk,update-lit,update-range,scan --quiet-load 2 --quiet-wait 900 $GATE \
        --out $R/bn1000/bn.json > $R/bn1000/bn.log 2>&1
}
pgvac() {
    python3 $WT/tools/bn_pg_scan.py --shapes bm --port 15434 --runs 3 --scan-ops 192 --blocks 12 \
        --point-ops 3000 --pin-backend 2 --pin-driver 4 $GATE --out $R/pg/pg-b.json > $R/pg/pg-b.log 2>&1
}
prof() {
    python3 $WT/tools/bm_scan.py profile --bin $B/kds_server-Bfp --sampler $B/kds_sampler.so \
        --sampler-dir $R/prof --workdir $R/prof --port 32421 --pin-server 2 --pin-driver 4 \
        --profile-cells bn --profile-scans 20000 --point-ops 3000 --out $R/prof/profile.json \
        > $R/prof/profile.log 2>&1
}
phase bn1000 420 bn1000
phase pgvac 330 pgvac
phase prof 150 prof
echo "=== all phases done $(date -u +%H:%M:%S)"
touch $R/flags/ALL2.done
