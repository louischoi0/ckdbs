#!/usr/bin/env python3
"""Re-take M2's idle backup the same way: binary B, cores = 4, archive_dir set, the 540,000-row ballast loaded alone, 5 s,
then BACKUP START / COPY / STOP. usage: br_retake.py <port> <out json>"""
import json, shutil, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/drivers")
from br_common import *  # noqa

port, out = int(sys.argv[1]), sys.argv[2]
root = f"{R}/work/retake"
shutil.rmtree(root, ignore_errors=True)
srv = Server("B", f"{R}/bin/kds_server-B", root, port, cores=4, durability="relaxed", frames=262144, archive_dir=f"{root}/archive",
             reactor_cpus="4,2,6,0", taskset="0,2,4,6")
srv.start()
h0 = host()
c = srv.conn_on_core(0)
ctl = srv.conn(timeout=120)
must(c, f"CREATE TABLE ballast (id int64, v int64, payload varchar({WIDTH})) BTREE", "CREATED")
t0 = time.time()
load_wide(c, "ballast", 1, 540000)
load_s = time.time() - t0
time.sleep(float(sys.argv[3]) if len(sys.argv) > 3 else 5)
m = bb.meta_all(c)
bdir = f"{R}/work/bk-retake" + (sys.argv[4] if len(sys.argv) > 4 else "")
started = fields(must(ctl, "BACKUP START 'retake'"))
t1 = time.time()
must(ctl, f"BACKUP COPY TO '{bdir}'")
while fields(ctl.send_command("BACKUP STATUS")).get("copying") != "0":
    time.sleep(0.1)
copy_s = time.time() - t1
stop = fields(must(ctl, f"BACKUP STOP TO '{bdir}'"))
t2 = time.time()
while True:
    st = fields(ctl.send_command("BACKUP STATUS"))
    if st.get("state") == "idle" and st.get("last_id") == started["backup_id"]:
        break
    time.sleep(0.1)
res = {"backup_id": started["backup_id"], "start_lsn": int(started["start_lsn"]), "stop_lsn": int(stop["stop_lsn"]), "data_pages": int(started["data_pages"]),
       "window_mib": (int(stop["stop_lsn"]) - int(started["start_lsn"])) / (1 << 20), "durable_lsn_at_start": m.get("wal_durable_lsn"),
       "ballast_load_s": round(load_s, 1), "copy_s": round(copy_s, 2), "stop_finish_s": round(time.time() - t2, 1), "outcome": st.get("last_outcome"),
       "host_before": h0, "host_after": host()}
print(res, flush=True)
json.dump(res, open(out, "w"), indent=1)
c.close(); ctl.close()
srv.stop()
shutil.rmtree(f"{root}/archive", ignore_errors=True)
