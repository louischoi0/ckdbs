#!/bin/bash
# The PW7 re-measurement's second half, run back to back after the matrix:
# the probes, the row-set sweep (A/B at 200 and 10,000, C at 200 and, new
# at this tree, 10,000), then the PostgreSQL twin (2 and 4 tables, 3 runs
# each, interleaved), with the cluster started only for its own cells.
set -u
W=/home/cdkbs/ckdbs/.claude/worktrees/agent-a88b32b3e80c45166
B=/home/cdkbs/mcbench-pw7/bin/kds_server
R=/home/cdkbs/mcbench-pw7/run
PGBIN=/home/cdkbs/pg16/usr/lib/postgresql/16/bin

echo "== probes start $(date -u +%FT%TZ)"
echo "probes already ran; skipped"
echo "== probes exit=$? $(date -u +%FT%TZ)"

port=15540
for rows in 200 10000; do
    tag=$rows; [ "$rows" = 10000 ] && tag=10k
    for cell in A-creating-t2 B-rotate-t2 C-rotate-t4; do
        echo "== sweep $cell rows=$rows port=$port start $(date -u +%FT%TZ)"
        python3 $W/bench/run_pw6.py --binary $B --workdir $R --cell $cell --rows $rows \
            --port $port --tag=-rows$tag >> $R/sweep.log 2>&1
        echo "== sweep $cell rows=$rows exit=$? $(date -u +%FT%TZ)"
        port=$((port + 2))
    done
done

echo "== pg start $(date -u +%FT%TZ)"
$PGBIN/pg_ctl -D /home/cdkbs/pg-bench/data -l /home/cdkbs/pg-bench/pg.log -w start >> $R/pg.log 2>&1
sleep 2
for r in 1 2 3; do
    for t in 2 4; do
        echo "== pg t$t r$r start $(date -u +%FT%TZ)"
        python3 $W/bench/run_pw6.py --workdir $R --pg --tables $t --rows 2000 --tag=-r$r >> $R/pg.log 2>&1
        echo "== pg t$t r$r exit=$? $(date -u +%FT%TZ)"
    done
done
$PGBIN/pg_ctl -D /home/cdkbs/pg-bench/data -w stop >> $R/pg.log 2>&1
echo "== pg stopped $(date -u +%FT%TZ)"
echo "== all done $(date -u +%FT%TZ)"
