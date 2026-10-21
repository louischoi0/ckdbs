#!/usr/bin/env python3
"""BM-S1 control: does Waystone's recording distort the small-relation cells?
(`bench/v3.0.0/results-bm-s1-scan-row-*.md`, section 1)

A pattern instance (a fetch with one argument value) of an opted-in shape
(`WAYSTONE ON`, CF-S1b) that executes twice gets a Waystone page
(kds.conf.sample, "waystone_recording"); the "on" arm opts each relation's
point shape in before it runs, since with nothing opted it would record
nothing and measure nothing. The census's point
statement `SELECT * FROM <rel> WHERE id = ?` runs 3,000 times per path (6,000 in
all) over a relation of 200 or 1,000 rows, so most of its argument values
repeat and the arm allocates a trail page per value; the 200- and 1,000-row
scans repeat values the same way. This runs the rows-200, rows-1k and bn cells
(and their relations' point arms) with `waystone_recording` on and off on
otherwise identical fresh servers, alternating, and prints the net row of each.

    bm_waystone_control.py --bin BIN --workdir DIR [--port P] [--runs 6] --out OUT.json
"""
import argparse, json, os, shutil, statistics, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
import bm_scan as bm  # noqa: E402
from bn_census import sched_ns  # noqa: E402


class Srv:
    def __init__(self, a, recording, tag):
        os.makedirs(a.workdir, exist_ok=True)
        self.data = os.path.join(a.workdir, "wp-%s.db" % tag)
        conf = os.path.join(a.workdir, "wp-%s.conf" % tag)
        if os.path.exists(self.data):
            os.remove(self.data)
        shutil.rmtree(self.data + ".wal", ignore_errors=True)
        with open(conf, "w") as f:
            f.write("data_file = %s\nport = %d\ncores = 1\ndurability = relaxed\nbuffer_pool_frames = 65536\n"
                    "log_file = wp-%s.log\nlog_dir = %s\nlog_level = warn\nwaystone_recording = %s\n" % (
                        self.data, a.port, tag, a.workdir, "on" if recording else "off"))
        err = open(os.path.join(a.workdir, "wp-%s.stderr" % tag), "w")
        self.proc = subprocess.Popen(["taskset", "-c", "2", a.bin, "--config", conf], stdout=err,
                                     stderr=subprocess.STDOUT)
        self.err = err
        t0 = time.time()
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", a.port, timeout=120)
                break
            except OSError:
                if time.time() > t0 + 30 or self.proc.poll() is not None:
                    raise SystemExit("server did not listen")
                time.sleep(0.1)
        self.prepared = set()

    def stop(self):
        self.conn.close()
        self.proc.terminate()
        self.proc.wait(30)
        self.err.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, default=32141)
    ap.add_argument("--runs", type=int, default=6)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.sched_setaffinity(0, {4})
    ns = argparse.Namespace(scan_ops=192, blocks=12, point_ops=3000)
    cells = {c.name: c for c in bm.make_cells(ns)}
    want = ["rows-200", "rows-1k", "bn"]
    out = {"runs": []}
    for r in range(a.runs):
        for recording in ((True, False) if r % 2 == 0 else (False, True)):
            srv = Srv(a, recording, "r%d%s" % (r, "on" if recording else "off"))
            try:
                bm.must(srv.conn, "CREATE TABLE z (id int64, v int64) BTREE", "CREATE")
                for rel in ("s200", "s1k", "s"):
                    bm.load_rel(srv.conn, rel)
                if recording:
                    # CF-S1b: nothing records until a shape is opted in, so
                    # the "on" arm opts in each relation's point shape - the
                    # one `fetch_id` its literal and prepared paths share.
                    # The scans cannot be: no tuple key equality.
                    for rel in ("s200", "s1k", "s"):
                        bm.must(srv.conn, "WAYSTONE ON SELECT * FROM %s WHERE id = 1" % rel,
                                "OK WAYSTONE ON")
                bm.flood_commits(srv.conn, 2200)
                bm.wait_idle(srv)
                for q in range(1, 401):  # the same untimed prelude as the census
                    srv.conn.execute("SELECT * FROM s200 WHERE id = %d" % bm.TARGET_ID(q, 200))
                rec = {"run": r, "recording": recording, "arms": []}
                m0 = bm.meta_vals(srv.conn, {"pages_allocated"})
                for name in want:
                    cell = cells[name]
                    arms, _ = bm.measure_arm(srv, cell, "point", ns.point_ops, ns, r, 1)
                    for arm in arms.values():
                        arm.pop("lat_us")
                        rec["arms"].append(arm)
                    arms, _ = bm.measure_arm(srv, cell, "scan", cell.ops, ns, r, 100)
                    for arm in arms.values():
                        arm.pop("lat_us")
                        rec["arms"].append(arm)
                rec["pages_allocated"] = bm.meta_vals(srv.conn, {"pages_allocated"})["pages_allocated"] - \
                    m0["pages_allocated"]
                out["runs"].append(rec)
                print(json.dumps({"run": r, "recording": recording, "pages_allocated": rec["pages_allocated"]}),
                      flush=True)
            finally:
                srv.stop()
        json.dump(out, open(a.out, "w"), indent=1)
    # summary
    for recording in (True, False):
        for name in want:
            cell = cells[name]
            nets = []
            pts, scs = [], []
            for rec in out["runs"]:
                if rec["recording"] != recording:
                    continue
                for path in ("literal", "prepared"):
                    pt = [x for x in rec["arms"] if x["kind"] == "point" and x["rel"] == cell.rel and x["path"] == path][0]
                    sc = [x for x in rec["arms"] if x["kind"] == "scan" and x["cell"] == name and x["path"] == path][0]
                    nets.append((sc["server_cpu_us"] - pt["server_cpu_us"]) * 1e3 / cell.rows)
                    pts.append(pt["server_cpu_us"])
                    scs.append(sc["server_cpu_us"])
            print("recording %-3s %-9s point %.1f us  scan %.1f us  net row %.1f ns [%.1f, %.1f]" % (
                "on" if recording else "off", name, statistics.median(pts), statistics.median(scs),
                statistics.median(nets), min(nets), max(nets)))


if __name__ == "__main__":
    main()
