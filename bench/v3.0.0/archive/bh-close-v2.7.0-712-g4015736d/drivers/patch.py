import os
os.chdir('/home/cdkbs/bench-runs/bh-close2')
p='driver/bh_overhead_ab.py'
s=open(p).read()
s=s.replace('for t in ("a", "b", "r", "u", "d"):','for t in ("a", "b", "r", "u", "d", "k"):')
s=s.replace('''        def dele(s, tbl, ph):''','''        def bulk(s, tbl, ph):
            c = ctr[s.label]; base = c["n"] + 1; c["n"] += 20
            sql = f"INSERT INTO {tbl} VALUES " + ",".join(f"({i}, {i}, {i})" for i in range(base, base + 20))
            t0 = time.perf_counter(); r = s.conn.send_command(sql); ph.record(time.perf_counter() - t0, r)

        def dele(s, tbl, ph):''')
s=s.replace('("delete-pk", "be_d", dele),','("delete-pk", "be_d", dele), ("bulk20", "be_k", bulk),')
open(p,'w').write(s)
for f in ('analyze_overhead.py','gen_tables.py','gen_summary.py'):
    t=open('driver/'+f).read()
    t=t.replace('"delete-pk", "range100"','"delete-pk", "bulk20", "range100"').replace('("delete-pk", "DELETE by pk"),','("delete-pk", "DELETE by pk"), ("bulk20", "bulk INSERT, 20 rows ascending"),')
    t=t.replace('bh-close/','bh-close2/')
    open('driver/'+f,'w').write(t)
