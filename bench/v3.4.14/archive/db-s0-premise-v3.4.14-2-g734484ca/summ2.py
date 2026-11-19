import json,statistics as st
files=['out-c1.jsonl','out-c4.jsonl','out-c1-pressure-a.jsonl','out-c1-pressure-b.jsonl']
for f in files:
    for l in open('/home/louis/bench-runs/db-s0/'+f):
        r=json.loads(l); d=r['delta']; w=r['window_s']; n=r['statements']
        ck=[x['pages_flushed'] for x in r['checkpoints']]
        exc=max(0,ck[0]-st.median(ck[1:])) if len(ck)>1 else 0
        raw=d['contention_checkpoint_pages_written']; steady=raw-exc
        dr=d['pool_dirty_drained']
        lb=d['wal_append_lsn']/w
        pw=(steady+dr)/w; img=pw*8192
        runs=d['contention_checkpoint_runs']
        print(f"{r['cell']:30s}|{n/w:8.0f}|{lb:9.0f}|{d['wal_append_lsn']/n:7.1f}|{d['stmt_log_records']/max(1,d['stmt_calls']):.2f}|runs={runs}|raw={raw}|exc={exc:.0f}|steady={steady:.0f}|stepsum={sum(ck)}|drain={dr}|ckpg/s={steady/w:.1f}|wb/s={dr/w:.1f}|img/s={img:.0f}|fac={(lb+img)/lb if lb>1000 else float('nan'):.2f}|imgB/st={img/(n/w):.1f}|pg/run={steady/runs:.1f}|bad={r['bad']}|syncs/s={d['wal_syncs']/w:.0f}|batch={d['wal_group_commits']/max(1,d['wal_group_batches']):.2f}|ld={r['loadavg_before']}/{r['loadavg_after']}|dur_ms={st.median([x['duration_us'] for x in r['checkpoints']])/1000:.1f}")
