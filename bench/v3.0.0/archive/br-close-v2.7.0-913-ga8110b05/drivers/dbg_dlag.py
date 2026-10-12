#!/usr/bin/env python3
"""How far behind the log end is the redo start (the backup's D) after a bulk load? usage: dbg_dlag.py <cores> <archive 0|1> <port> [rows]
Loads the ballast (wide rows), then every 2 s for 20 s reads SHOW META (durable LSN, checkpoint counters) and BACKUP START's start_lsn
(aborted at once); prints the lag durable - start_lsn."""
import os, shutil, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/drivers")
from br_common import *  # noqa

cores, arch, port = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
rows = int(sys.argv[4]) if len(sys.argv) > 4 else 540000
tag = f"c{cores}-a{arch}"
root = f"{R}/work/dbg-dlag-{tag}"
shutil.rmtree(root, ignore_errors=True)
cpus = {"c1-a1": ("0,2", "0"), "c4-a0": ("4,6,1,3", "4,6,1,3"), "c4-a1": ("5,7,1,3", "5,7,1,3")}.get(tag, ("0,2", "0"))
srv = Server(tag, f"{R}/bin/kds_server-B", root, port, cores=cores, durability="relaxed", frames=262144,
             archive_dir=f"{root}/archive" if arch else None, reactor_cpus=cpus[1], taskset=cpus[0])
srv.start()
c = srv.conn_on_core(0) if cores > 1 else srv.conn()
ctl = srv.conn()
must(c, f"CREATE TABLE ballast (id int64, v int64, payload varchar({WIDTH})) BTREE", "CREATED")
t0 = time.time()
load_wide(c, "ballast", 1, rows)
print(tag, "loaded in %.1f s" % (time.time() - t0), flush=True)
for i in range(11):
    m = bb.meta_all(c)
    st = fields(ctl.send_command("BACKUP START 'dlag'"))
    ctl.send_command("BACKUP ABORT")
    start = int(st["start_lsn"]) if "start_lsn" in st else None
    dur = m.get("wal_durable_lsn")
    print(tag, "t+%2ds durable %.0f start_lsn %s lag %.0f MiB ckpt_runs %s last_completed %s refused %s dirty_at_start %s" % (
        2 * i, dur, start, ((dur - start) / (1 << 20)) if start else -1, m.get("checkpoint_runs"), m.get("checkpoint_last_completed"),
        m.get("checkpoint_start_refused"), m.get("checkpoint_last_dirty_at_start")), flush=True)
    time.sleep(2)
c.close(); ctl.close()
srv.stop()
shutil.rmtree(root, ignore_errors=True)
