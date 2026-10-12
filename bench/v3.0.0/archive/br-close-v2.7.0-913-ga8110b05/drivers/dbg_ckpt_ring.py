#!/usr/bin/env python3
"""Is the restore's 'record is larger than the ring' failure BR's or the engine's? On binary A or B: load the M1b/M2 ballast
(540,000 wide rows, ~137,000 pages) with the pool larger than the volume, watch the checkpoint counters, kill -9, restart the
same files and say whether the mount recovers."""
import json, os, shutil, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/drivers")
from br_common import *  # noqa

label = sys.argv[1]
cores = int(sys.argv[4]) if len(sys.argv) > 4 else 1
rc = "4" if cores == 1 else "4,2,6,0"
binary = f"{R}/bin/kds_server-{label}"
root = f"{R}/work/dbg-ring-{label}"
shutil.rmtree(root, ignore_errors=True)
srv = Server(label, binary, root, 31861, cores=cores, durability="relaxed", frames=262144, reactor_cpus=rc, taskset="0,2,4,6")
srv.start()
c = srv.conn_on_core(0) if cores > 1 else srv.conn()
must(c, f"CREATE TABLE ballast (id int64, v int64, payload varchar({WIDTH})) BTREE", "CREATED")
rows = int(sys.argv[2]) if len(sys.argv) > 2 else 540000
t0 = time.time()
trace = []
n = 1
while n <= rows:
    load_wide(c, "ballast", n, 20000)
    n += 20000
    m = bb.meta_all(c)
    trace.append((round(time.time() - t0, 1), n - 1, m.get("checkpoint_runs"), m.get("checkpoint_start_refused"), m.get("checkpoint_last_completed"),
                  m.get("checkpoint_last_dirty_at_start"), m.get("pool_resident"), m.get("wal_durable_lsn")))
for t in trace:
    print("t=%s rows=%s ckpt_runs=%s start_refused=%s last_completed=%s dirty_at_start=%s pool_resident=%s durable_lsn=%s" % t)
time.sleep(float(sys.argv[3]) if len(sys.argv) > 3 else 12)
m = bb.meta_all(c)
print("after the idle: ckpt_runs", m.get("checkpoint_runs"), "start_refused", m.get("checkpoint_start_refused"), "last_completed",
      m.get("checkpoint_last_completed"), "last_pages", m.get("checkpoint_last_pages"), "dirty_at_start", m.get("checkpoint_last_dirty_at_start"))
c.close()
srv.kill()
print("killed -9; restarting the same files")
srv2 = Server(label, binary, root, 31861, cores=cores, durability="relaxed", frames=262144, reactor_cpus=rc, taskset="0,2,4,6")
t1 = time.time()
try:
    srv2.start()
    c2 = srv2.conn()
    m2 = bb.meta_all(c2)
    print("mount OK after %.1f s: recovery_records=%s redo_applied=%s analysis_us=%s redo_us=%s checkpoint_us=%s" % (
        time.time() - t1, m2.get("recovery_records"), m2.get("recovery_redo_applied"), m2.get("recovery_analysis_us"),
        m2.get("recovery_redo_us"), m2.get("recovery_checkpoint_us")))
    c2.close()
    srv2.stop()
except SystemExit as e:
    print("mount FAILED after %.1f s: %s" % (time.time() - t1, str(e)[-400:]))
shutil.rmtree(root, ignore_errors=True)
