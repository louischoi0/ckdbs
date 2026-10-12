#!/usr/bin/env python3
"""BR close, M1: what BR costs when it is on (binary B only).

  archive   M1a: archive_dir set vs not, nothing failing. Phases alternate off/on across --pairs (order flips per pair);
            each phase is a fresh server and data file running the same fixed work: 3000 INSERT, 3000 SELECT by pk and
            3000 UPDATE by pk on a 10,000-row hot relation, then 2400 wide INSERTs of 50 rows (1.8 KB each; ~4.5 segments
            of log). Equal work, not equal time.
  backup    M1b: a base backup running. One server: a 10,000-row hot relation and a ballast relation of >= --gib GiB.
            Windows alternate: baseline (a window as long as the previous copy took), then BACKUP START; COPY; the same
            workload until the copy ends; then STOP (the workload continues through the STOP finish). The workload is one
            serial connection cycling four statements: INSERT hot, UPDATE hot by pk, UPDATE ballast by pk (random over the
            whole ballast: every first touch of a page in the window logs a full-page image), SELECT hot by pk.
"""
import argparse, json, os, random, shutil, threading, time

from br_common import *  # noqa

BIN_B = f"{R}/bin/kds_server-B"
BIN_A = f"{R}/bin/kds_server-A"


def meta(conn):
    return bb.meta_all(conn)


def phase_archive(args, cores, mode, tag):
    root = f"{R}/work/m1a-{tag}"
    shutil.rmtree(root, ignore_errors=True)
    arch = f"{root}/archive" if mode == "on" else (f"/dev/shm/br-close-arch-{tag}" if mode == "shm" else None)
    reactor = "4" if cores == 1 else "4,2,6,0"
    srv = Server(mode, BIN_B, root, args.port, cores=cores, durability="relaxed", frames=65536, archive_dir=arch,
                 reactor_cpus=reactor)
    h0 = host()
    srv.start()
    try:
        conn = srv.conn_on_core(0) if cores > 1 else srv.conn()
        must(conn, "CREATE TABLE hot (id int64, v int64, w int64) BTREE", "CREATED")
        must(conn, f"CREATE TABLE wide (id int64, v int64, payload varchar({WIDTH})) BTREE", "CREATED")
        load_hot(conn, "hot", 10000)
        m_start = meta(conn)
        res = {}
        n = 10_000_000
        lat = []
        t0 = time.perf_counter()
        for i in range(3000):
            n += 1
            a = time.perf_counter(); r = conn.send_command(f"INSERT INTO hot VALUES ({n}, {n})"); lat.append(time.perf_counter() - a)
            if not r.startswith("INSERTED"): raise SystemExit(r)
        res["insert"] = lat_summary(lat, time.perf_counter() - t0)
        lat = []; t0 = time.perf_counter()
        for i in range(3000):
            k = 1 + (i * 7919) % 10000
            a = time.perf_counter(); r = conn.send_command(f"SELECT * FROM hot WHERE id = {k}"); lat.append(time.perf_counter() - a)
        res["select-pk"] = lat_summary(lat, time.perf_counter() - t0)
        lat = []; t0 = time.perf_counter()
        for i in range(3000):
            k = 1 + (i * 7919) % 10000
            a = time.perf_counter(); r = conn.send_command(f"UPDATE hot SET v = {i} WHERE id = {k}"); lat.append(time.perf_counter() - a)
            if not r.startswith("UPDATED"): raise SystemExit(r)
        res["update-pk"] = lat_summary(lat, time.perf_counter() - t0)
        m_small = meta(conn)
        lat = []; t0 = time.perf_counter()
        w = 0
        for i in range(2400):
            rows = ",".join(f"({w + j}, '{payload(w + j)}')" for j in range(50))
            w += 50
            a = time.perf_counter(); r = conn.send_command(f"INSERT INTO wide VALUES {rows}"); lat.append(time.perf_counter() - a)
            if not r.startswith("INSERTED"): raise SystemExit(r[:300])
        el = time.perf_counter() - t0
        res["wide-insert-50rows"] = lat_summary(lat, el)
        m_end = meta(conn)
        time.sleep(2.5 if mode in ("on", "shm") else 0)   # let the archiver finish the last sealed segment
        m_after = meta(conn)
        wal_bytes_wide = m_end["wal_durable_lsn"] - m_small["wal_durable_lsn"]
        res["wide-insert-50rows"]["wal_bytes"] = wal_bytes_wide
        res["wide-insert-50rows"]["wal_mib_s"] = round(wal_bytes_wide / (1 << 20) / el, 1)
        keys = ("wal_archived", "wal_archive_lag_segments", "wal_archive_failures", "wal_archive_next_segment",
                "wal_first_segment", "wal_segments_removed", "wal_syncs")
        out = {"mode": mode, "cores": cores, "tag": tag, "host_before": h0, "host_after": host(), "results": res,
               "meta_end": {k: m_end.get(k) for k in keys}, "meta_after": {k: m_after.get(k) for k in keys}}
        conn.close()
        return out
    finally:
        srv.stop()
        shutil.rmtree(root, ignore_errors=True)
        if mode == "shm":
            shutil.rmtree(f"/dev/shm/br-close-arch-{tag}", ignore_errors=True)


def cmd_archive(args):
    os.makedirs(args.out, exist_ok=True)
    for cores in [int(c) for c in args.cores.split(",")]:
        for pair in range(args.pairs):
            modes = args.modes.split(",")
            order = modes[pair % len(modes):] + modes[:pair % len(modes)]
            for mode in order:
                bb.wait_for_load(args.max_load, args.max_wait)
                tag = f"c{cores}-p{pair}-{mode}"
                d = phase_archive(args, cores, mode, tag)
                with open(f"{args.out}/{tag}.json", "w") as f:
                    json.dump(d, f, indent=1)
                r = d["results"]
                line = " ".join(f"{k}:{v['qps']}/s p99={v['p99_us']}" for k, v in r.items())
                print(f"{tag} load {d['host_before']['loadavg'].split()[0]} {line} archived={d['meta_after']['wal_archived']}", flush=True)
                with open(args.progress, "a") as pf:
                    pf.write(f"{time.strftime('%H:%M', time.gmtime())} M1a {tag} done load {d['host_after']['loadavg'].split()[0]}\n")


# ---------------------------------------------------------------- backup windows

class Workload:
    """One serial connection cycling four statements until `stop_flag`, tagging each window."""

    def __init__(self, conn, ballast_rows):
        self.conn, self.ballast_rows = conn, ballast_rows
        self.rng = random.Random(7)
        self.n = 20_000_000
        self.q = 0

    def run_window(self, until_fn, label):
        lat = {"ins-hot": [], "upd-hot": [], "upd-ballast": [], "sel-hot": []}
        errs = 0
        t0 = time.perf_counter()
        m0 = meta(self.conn)
        i = 0
        while not until_fn():
            kind = i % 4
            i += 1
            if kind == 0:
                self.n += 1; sql = f"INSERT INTO hot VALUES ({self.n}, {self.n})"; k = "ins-hot"
            elif kind == 1:
                self.q += 1; sql = f"UPDATE hot SET v = {self.q} WHERE id = {1 + (self.q * 7919) % 10000}"; k = "upd-hot"
            elif kind == 2:
                self.q += 1; sql = f"UPDATE ballast SET v = {self.q} WHERE id = {1 + self.rng.randrange(self.ballast_rows)}"; k = "upd-ballast"
            else:
                self.q += 1; sql = f"SELECT * FROM hot WHERE id = {1 + (self.q * 7919) % 10000}"; k = "sel-hot"
            a = time.perf_counter()
            r = self.conn.send_command(sql)
            lat[k].append(time.perf_counter() - a)
            if r.startswith("ERR"):
                errs += 1
        el = time.perf_counter() - t0
        m1 = meta(self.conn)
        allat = [x for v in lat.values() for x in v]
        d = {"label": label, "elapsed_s": round(el, 3), "stmts": len(allat), "stmt_per_s": round(len(allat) / el, 1), "errors": errs,
             "all": lat_summary(allat, el), "kinds": {k: lat_summary(v, el) for k, v in lat.items()},
             "wal_bytes": m1.get("wal_durable_lsn", 0) - m0.get("wal_durable_lsn", 0),
             "fence_waited_delta": m1.get("backup_fence_writes_waited", 0) - m0.get("backup_fence_writes_waited", 0)}
        d["wal_bytes_per_stmt"] = round(d["wal_bytes"] / max(1, len(allat)), 1)
        return d


def cmd_backup(args):
    os.makedirs(args.out, exist_ok=True)
    cores = args.cores_one
    root = f"{R}/work/m1b-c{cores}" + ("-" + args.tag if args.tag else "")
    shutil.rmtree(root, ignore_errors=True)
    reactor = "4" if cores == 1 else "4,2,6,0"
    srv = Server(args.binary, BIN_A if args.binary == "A" else BIN_B, root, args.port, cores=cores, durability="relaxed", frames=262144, reactor_cpus=reactor, taskset=args.taskset)
    srv.start()
    h0 = host()
    log = {"cores": cores, "host_start": h0, "windows": [], "backups": []}
    try:
        conn = srv.conn_on_core(0) if cores > 1 else srv.conn()
        must(conn, "CREATE TABLE hot (id int64, v int64, w int64) BTREE", "CREATED")
        must(conn, f"CREATE TABLE ballast (id int64, v int64, payload varchar({WIDTH})) BTREE", "CREATED")
        load_hot(conn, "hot", 10000)
        t0 = time.time()
        rows = args.ballast_rows
        load_wide(conn, "ballast", 1, rows)
        log["ballast_load_s"] = round(time.time() - t0, 1)
        log["ballast_rows"] = rows
        wl = Workload(conn, rows)
        ctl = srv.conn(timeout=120)
        time.sleep(6)   # a checkpoint or two behind the load
        # warm: one untimed window
        t_end = time.time() + 3
        wl.run_window(lambda: time.time() > t_end, "warm")
        window_s = args.window
        for rep in range(args.reps):
            bdir = f"{root}/bk-{rep}"
            # baseline window of `window_s`
            t_end = time.time() + window_s
            base = wl.run_window(lambda: time.time() > t_end, f"base-{rep}")
            log["windows"].append(base)
            print(f"c{cores} rep{rep} base {base['stmt_per_s']}/s p99={base['all']['p99_us']}us wal/stmt={base['wal_bytes_per_stmt']}", flush=True)
            # backup
            started = fields(must(ctl, f"BACKUP START 'm1b-{rep}'"))
            t_copy0 = time.time()
            must(ctl, f"BACKUP COPY TO '{bdir}'")
            status = {}

            def copying():
                nonlocal status
                if time.time() - copying.last > 0.1:
                    copying.last = time.time()
                    status = fields(ctl.send_command("BACKUP STATUS"))
                return status.get("copying") != "0"
            copying.last = 0
            # the workload runs until the copy ends (STATUS is polled on the control connection from this thread
            # between statements: ~1 poll per 0.1 s)
            win = wl.run_window(lambda: not copying(), f"copy-{rep}")
            t_copy1 = time.time()
            win["copy_s"] = round(t_copy1 - t_copy0, 3)
            win["data_pages"] = int(started["data_pages"])
            win["copy_mib_s"] = round(int(started["data_pages"]) * 8192 / (1 << 20) / (t_copy1 - t_copy0), 1)
            m_c = meta(wl.conn)
            win["fence_writes_waited_total"] = m_c.get("backup_fence_writes_waited")
            log["windows"].append(win)
            print(f"c{cores} rep{rep} copy {win['stmt_per_s']}/s p99={win['all']['p99_us']}us copy={win['copy_s']}s {win['copy_mib_s']}MiB/s "
                  f"wal/stmt={win['wal_bytes_per_stmt']} fence_waited={win['fence_writes_waited_total']}", flush=True)
            # stop (its finish copies the window's log and writes the manifest); workload continues through it
            t_stop0 = time.time()
            stop = fields(must(ctl, f"BACKUP STOP TO '{bdir}'"))
            stop_state = {}

            def finishing():
                nonlocal stop_state
                if time.time() - finishing.last > 0.1:
                    finishing.last = time.time()
                    stop_state = fields(ctl.send_command("BACKUP STATUS"))
                return not (stop_state.get("state") == "idle" and stop_state.get("last_id") == started["backup_id"])
            finishing.last = 0
            fin = wl.run_window(lambda: not finishing(), f"stop-{rep}")
            fin["stop_finish_s"] = round(time.time() - t_stop0, 3)
            fin["outcome"] = stop_state.get("last_outcome")
            log["windows"].append(fin)
            mfin = meta(wl.conn)
            log["backups"].append({"rep": rep, "dir": bdir, "backup_id": started["backup_id"], "start_lsn": started["start_lsn"],
                                   "stop_lsn": stop.get("stop_lsn"), "data_pages": started["data_pages"],
                                   "outcome": stop_state.get("last_outcome"), "log_bytes_copied": mfin.get("backup_log_bytes_copied"),
                                   "fence_writes_waited_total": mfin.get("backup_fence_writes_waited"),
                                   "du_bytes": sum(os.path.getsize(os.path.join(bdir, f)) for f in os.listdir(bdir)
                                                   if os.path.isfile(os.path.join(bdir, f)))})
            print(f"c{cores} rep{rep} stop {fin['stmt_per_s']}/s p99={fin['all']['p99_us']}us finish={fin['stop_finish_s']}s outcome={fin['outcome']}", flush=True)
            with open(args.progress, "a") as pf:
                pf.write(f"{time.strftime('%H:%M', time.gmtime())} M1b c{cores} rep{rep} done load {host()['loadavg'].split()[0]}\n")
            # keep only the last backup (M3 restores it)
            if rep < args.reps - 1:
                shutil.rmtree(bdir, ignore_errors=True)
            window_s = max(2.0, win["copy_s"])
        # a final baseline window after the last STOP
        t_end = time.time() + window_s
        log["windows"].append(wl.run_window(lambda: time.time() > t_end, "base-final"))
        log["meta_end"] = {k: v for k, v in meta(wl.conn).items() if k.startswith(("backup", "wal_", "checkpoint_runs", "pool_"))}
        log["host_end"] = host()
    finally:
        srv.stop()
        with open(f"{args.out}/m1b-c{cores}" + ("-" + args.tag if args.tag else "") + ".json", "w") as f:
            json.dump(log, f, indent=1)


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("archive")
    a.add_argument("--cores", default="1,4")
    a.add_argument("--pairs", type=int, default=5)
    a.add_argument("--modes", default="off,on")
    a.set_defaults(fn=cmd_archive)
    b = sub.add_parser("backup")
    b.add_argument("--cores-one", type=int, required=True)
    b.add_argument("--ballast-rows", type=int, required=True)
    b.add_argument("--reps", type=int, default=4)
    b.add_argument("--window", type=float, default=6.0)
    b.add_argument("--taskset", default="0,2,4,6")
    b.add_argument("--tag", default="")
    b.add_argument("--binary", default="B")
    b.set_defaults(fn=cmd_backup)
    for s in (a, b):
        s.add_argument("--port", type=int, default=31821)
        s.add_argument("--out", required=True)
        s.add_argument("--progress", required=True)
        s.add_argument("--max-load", type=float, default=3.5)
        s.add_argument("--max-wait", type=float, default=30)
    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
