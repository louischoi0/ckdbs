import re
R = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/drivers"


def sub(path, pairs):
    s = open(path).read()
    for a, b in pairs:
        assert a in s, (path, a[:60])
        s = s.replace(a, b)
    open(path, "w").write(s)


sub(f"{R}/analyze_m1b.py", [
    ('    return f"{st.median(v):.{d}f} [{min(v):.{d}f}-{max(v):.{d}f}]"', '    return f"{st.median(v):.{d}f} [{min(v):.{d}f} to {max(v):.{d}f}]"'),
    ('''    print(f"\\npaired copy - preceding baseline, {reps} backups: p50 {rng(dp50)} µs; p99 {rng(dp99)} µs; excl-stall rate {rng(drate, 0)} /s ({rng(drel)} %)")''',
     '''    print(f"\\n{reps} backups, each copy window minus the baseline window just before it: p50 {rng(dp50)} µs; p99 {rng(dp99)} µs; "
          f"service rate excluding statements over 1 ms {rng(drate, 0)} /s, {rng(drel)} %.")'''),
    ('''    print(f"\\ncopy: {rng([w['copy_s'] for w in cw], 2)} s, {rng([w['copy_mib_s'] for w in cw], 1)} MiB/s over {rng([w['data_pages'] for w in cw], 0)} pages; "
          f"STOP finish {rng([w['stop_finish_s'] for w in sw], 2)} s; fence writes waited (total at the end) {d['backups'][-1]['fence_writes_waited_total']}; "
          f"log bytes copied per backup {[int(b['log_bytes_copied']) // (1 << 20) for b in d['backups']]} MiB; backup size {[b['du_bytes'] // (1 << 20) for b in d['backups']]} MiB; "
          f"window log span (stop - start LSN) {[(int(b['stop_lsn']) - int(b['start_lsn'])) // (1 << 20) for b in d['backups']]} MiB")''',
     '''    print(f"\\nThe copy: {rng([w['copy_s'] for w in cw], 2)} s at {rng([w['copy_mib_s'] for w in cw], 1)} MiB/s over {rng([w['data_pages'] for w in cw], 0)} pages. "
          f"STOP's finish: {rng([w['stop_finish_s'] for w in sw], 2)} s. Writes that waited for the fence, in total at the end: {d['backups'][-1]['fence_writes_waited_total']:.0f} writes. "
          f"Log copied per backup: {', '.join(str(int(b['log_bytes_copied']) // (1 << 20)) for b in d['backups'])} MiB. "
          f"Backup size: {', '.join(str(b['du_bytes'] // (1 << 20)) for b in d['backups'])} MiB. "
          f"Log span of the window (stop LSN - start LSN): {', '.join(str((int(b['stop_lsn']) - int(b['start_lsn'])) // (1 << 20)) for b in d['backups'])} MiB.")'''),
    ('| statement kind | baseline p50 | copy p50 | baseline p99 | copy p99 |', '| statement kind | baseline p50 | copy p50 | baseline p99 | copy p99 |'),
    ('{len(ws)} windows', '{len(ws)} windows'),
])
sub(f"{R}/analyze_m1a.py", [
    ('    return f"{st.median(v):.{d}f} [{min(v):.{d}f}-{max(v):.{d}f}]"', '    return f"{st.median(v):.{d}f} [{min(v):.{d}f} to {max(v):.{d}f}]"'),
    ('print(f"| {cores} | {name} | {lab} | {len(v)} | ', 'print(f"| {cores} | {name} | {lab} | {len(v)} rounds | '),
    ('print(f"| {cores} | {name} | on vs off, paired | {len(d)} | {rng(d)} % | | | | |")', 'print(f"| {cores} | {name} | on vs off, paired | {len(d)} rounds | {rng(d)} % | | | | |")'),
])
print("ok")
