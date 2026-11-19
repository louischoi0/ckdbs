import json,sys
for c in (1,4):
    for l in open(f'/home/louis/bench-runs/db-s0/out-c{c}.jsonl'):
        r=json.loads(l); d=r['delta']; w=r['window_s']; n=r['statements']
        lb=d['wal_append_lsn']/w; cp=d['contention_checkpoint_pages_written']/w; dr=d['pool_dirty_drained']/w
        img=(cp+dr)*8192
        ck=r['checkpoints']
        print(f"{r['cell']:28s} stmt/s={n/w:8.0f} logB/s={lb:10.0f} logB/stmt={d['wal_append_lsn']/n:6.1f} rec/stmt={d['stmt_log_records']/max(1,d['stmt_calls']):.2f} runs={d['contention_checkpoint_runs']} ckpg={d['contention_checkpoint_pages_written']} (flushed {[x['pages_flushed'] for x in ck]}) drain={d['pool_dirty_drained']} cp/s={cp:6.1f} img/s={img:10.0f} factor={(lb+img)/lb if lb else 0:6.2f} imgB/stmt={img/(n/w):8.1f} bad={r['bad']} dur_us={[x['duration_us'] for x in ck]} load={r['loadavg_before']}/{r['loadavg_after']}")
