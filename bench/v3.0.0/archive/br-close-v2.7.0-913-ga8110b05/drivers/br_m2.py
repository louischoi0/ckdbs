#!/usr/bin/env python3
"""BR close, M2 (archive throughput against peak WAL generation) and the base backup M3b restores.

One server (binary B, cores = 4, relaxed, archive_dir set). Steps:
  1. a ballast relation of --ballast-rows wide rows (so the base backup is >= 1 GiB);
  2. an idle base backup (BACKUP START / COPY / STOP) into work/m2/bk;  its copy rate is read from BACKUP STATUS polling;
  3. phase "steady": --writers client processes, each its own connection, INSERT 50 wide rows (~95 KB) per statement, pk omitted,
     for --steady seconds; a sampler reads SHOW META every 0.25 s on its own connection;
  4. phase "backlog": the archive directory is made unwritable (the archiver fails, backs off, holds the segments), the load
     continues --fail seconds, the directory is made writable again, the load continues --recover seconds; the sampler keeps
     reading, so the drain after the failure is the archiver's capacity against a still-running generation;
  5. the load stops and the sampler runs --tail more seconds (the archiver drains).
Output: out/m2/m2.json (the 0.25 s time series and the summary).
"""
import argparse, json, multiprocessing as mp, os, shutil, stat, time

from br_common import *  # noqa

BIN_B = f"{R}/bin/kds_server-B"


def writer(port, cpu, deadline, q, wid):
    os.sched_setaffinity(0, {cpu})
    conn = ServerConnection("127.0.0.1", port, timeout=120)
    n = errs = 0
    lat = []
    base = wid * 1_000_000_000
    while time.time() < deadline:
        k = base + n * 50
        rows = ",".join(f"({k + j}, '{payload(k + j)}')" for j in range(50))
        a = time.perf_counter()
        r = conn.send_command(f"INSERT INTO wide2 VALUES {rows}")
        lat.append(time.perf_counter() - a)
        if not r.startswith("INSERTED"):
            errs += 1
            if errs <= 3:
                q.put(("err", wid, r[:200]))
        n += 1
    q.put(("done", wid, n, errs, lat))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=31831)
    p.add_argument("--ballast-rows", type=int, required=True)
    p.add_argument("--writers", type=int, default=6)
    p.add_argument("--steady", type=float, default=45)
    p.add_argument("--fail", type=float, default=20)
    p.add_argument("--recover", type=float, default=40)
    p.add_argument("--tail", type=float, default=25)
    p.add_argument("--out", required=True)
    p.add_argument("--progress", required=True)
    args = p.parse_args()

    root = f"{R}/work/m2"
    shutil.rmtree(root, ignore_errors=True)
    arch = f"{root}/archive"
    srv = Server("B", BIN_B, root, args.port, cores=4, durability="relaxed", frames=262144, archive_dir=arch,
                 reactor_cpus="4,2,6,0", taskset="0,2,4,6")
    srv.start()
    summary = {"host_start": host()}
    series = []
    stop_sampler = [False]
    try:
        conn = srv.conn_on_core(0)
        must(conn, f"CREATE TABLE ballast (id int64, v int64, payload varchar({WIDTH})) BTREE", "CREATED")
        must(conn, f"CREATE TABLE wide2 (id int64, payload varchar({WIDTH})) BTREE", "CREATED")
        t0 = time.time()
        load_wide(conn, "ballast", 1, args.ballast_rows)
        summary["ballast_load_s"] = round(time.time() - t0, 1)
        time.sleep(5)
        # ---- idle base backup (kept: M3b restores it through the archive)
        ctl = srv.conn(timeout=120)
        bdir = f"{root}/bk"
        started = fields(must(ctl, "BACKUP START 'm2'"))
        t0 = time.time()
        must(ctl, f"BACKUP COPY TO '{bdir}'")
        while fields(ctl.send_command("BACKUP STATUS")).get("copying") != "0":
            time.sleep(0.1)
        t_copy = time.time() - t0
        stop = fields(must(ctl, f"BACKUP STOP TO '{bdir}'"))
        t1 = time.time()
        while True:
            st = fields(ctl.send_command("BACKUP STATUS"))
            if st.get("state") == "idle" and st.get("last_id") == started["backup_id"]:
                break
            time.sleep(0.1)
        summary["idle_backup"] = {"backup_id": started["backup_id"], "start_lsn": started["start_lsn"], "stop_lsn": stop.get("stop_lsn"),
                                  "data_pages": int(started["data_pages"]), "copy_s": round(t_copy, 3),
                                  "copy_mib_s": round(int(started["data_pages"]) * 8192 / (1 << 20) / t_copy, 1),
                                  "stop_finish_s": round(time.time() - t1, 3), "outcome": st.get("last_outcome")}
        print("idle backup", summary["idle_backup"], flush=True)
        m_pre = bb.meta_all(conn)
        summary["meta_pre_load"] = {k: m_pre.get(k) for k in ("wal_durable_lsn", "wal_archived", "wal_archive_next_segment", "wal_first_segment")}

        # ---- sampler
        import threading
        sconn = srv.conn(timeout=60)
        keys = ("wal_durable_lsn", "wal_archived", "wal_archive_lag_segments", "wal_archive_next_segment", "wal_archive_failures",
                "wal_archive_blocking", "wal_first_segment", "wal_segments_removed")
        t_base = time.time()

        def sample():
            while not stop_sampler[0]:
                a = time.time()
                m = bb.meta_all(sconn)
                series.append({"t": round(a - t_base, 3), **{k: m.get(k) for k in keys}})
                time.sleep(max(0, 0.25 - (time.time() - a)))
        th = threading.Thread(target=sample, daemon=True)
        th.start()

        # ---- writers
        total = args.steady + args.fail + args.recover
        deadline = time.time() + total
        q = mp.Queue()
        cpus = [1, 3, 5, 7, 1, 3, 5, 7][: args.writers]
        procs = [mp.Process(target=writer, args=(args.port, cpus[i], deadline, q, i + 1)) for i in range(args.writers)]
        t_load0 = time.time()
        for pr in procs:
            pr.start()
        marks = {"load_start": 0.0}
        os.makedirs(arch, exist_ok=True)
        time.sleep(args.steady)
        marks["fail_start"] = round(time.time() - t_base, 2)
        os.chmod(arch, stat.S_IRUSR | stat.S_IXUSR)   # archive unwritable
        with open(args.progress, "a") as pf:
            pf.write(f"{time.strftime('%H:%M', time.gmtime())} M2 steady phase done; archive made unwritable\n")
        time.sleep(args.fail)
        os.chmod(arch, stat.S_IRWXU)
        marks["recover_start"] = round(time.time() - t_base, 2)
        results = []
        errs_msgs = []
        while len(results) < len(procs):
            r = q.get()
            if r[0] == "done":
                results.append(r)
            else:
                errs_msgs.append(r)
        for pr in procs:
            pr.join()
        marks["load_end"] = round(time.time() - t_base, 2)
        marks["load_start"] = round(t_load0 - t_base, 2)
        time.sleep(args.tail)
        stop_sampler[0] = True
        th.join()
        m_end = bb.meta_all(conn)
        stmts = sum(r[2] for r in results)
        errs = sum(r[3] for r in results)
        lat = [x for r in results for x in r[4]]
        summary["writers"] = {"n": args.writers, "stmts": stmts, "errors": errs, "err_samples": errs_msgs[:5],
                              "latency": lat_summary(lat, None)}
        summary["marks"] = marks
        summary["meta_end"] = {k: m_end.get(k) for k in keys + ("wal_syncs", "wal_archive_failures")}
        summary["host_end"] = host()
        summary["series_n"] = len(series)
        conn.close()
    finally:
        stop_sampler[0] = True
        srv.stop()
        os.makedirs(os.path.dirname(args.out), exist_ok=True)
        with open(args.out, "w") as f:
            json.dump({"summary": summary, "series": series}, f)
        try:
            os.chmod(arch, stat.S_IRWXU)
        except OSError:
            pass
        with open(args.progress, "a") as pf:
            pf.write(f"{time.strftime('%H:%M', time.gmtime())} M2 done\n")


if __name__ == "__main__":
    main()
