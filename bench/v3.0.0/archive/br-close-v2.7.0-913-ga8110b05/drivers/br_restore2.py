#!/usr/bin/env python3
"""BR close, M3 re-run at 90cdf918: br_restore.py with a chosen binary, a failure-tolerant serve step, and the failure text kept.

  br_restore2.py --binary BIN --backup DIR --cores N --label X --out JSON [--reps 3] [--no-serve]
"""
import argparse, json, os, shutil, signal, subprocess, sys, time

sys.path.insert(0, "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/drivers")
from br_common import *  # noqa
import br_restore as r1


def timed(args, rep, serve=False):
    root = f"{R}/work/m3r-{args.label}-{rep}{'-serve' if serve else ''}"
    shutil.rmtree(root, ignore_errors=True)
    conf, data = r1.write_conf(root, args.port, args.cores, args.frames)
    cold = rep == 0
    if cold:
        r1.drop_cache(args.backup)
    os.sync()  # drain the previous attempt's dirty page cache, outside the timing
    time.sleep(3)
    cmd = ["taskset", "-c", "0,2,4,6", args.binary, "--config", conf, "--restore-from", args.backup]
    if not serve:
        cmd.append("--restore-only")
    h0 = host()
    t0 = time.perf_counter()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    stamps, meta = [], None
    if not serve:
        for line in proc.stdout:
            stamps.append((time.perf_counter() - t0, line.strip()))
        proc.wait()
    else:
        for line in proc.stdout:
            stamps.append((time.perf_counter() - t0, line.strip()))
            if line.startswith("listening on") or "failed" in line:
                break
        if proc.poll() is None:
            c = ServerConnection("127.0.0.1", args.port, timeout=60)
            m = bb.meta_all(c)
            meta = {k: v for k, v in m.items() if k.startswith(("recovery_", "pool_resident", "wal_durable_lsn"))}
            c.close()
            proc.send_signal(signal.SIGTERM)
        proc.wait(180)
    total = time.perf_counter() - t0
    laid = next((t for t, l in stamps if "laid out" in l), None)
    rec = next((t for t, l in stamps if l.startswith("restore of backup")), None)
    d = {"rep": rep, "cold_backup_files": cold, "serve": serve, "rc": proc.returncode, "total_s": round(total, 3),
         "layout_s": round(laid, 3) if laid is not None else None,
         "mount_s": round(rec - laid, 3) if (laid is not None and rec is not None) else None,
         "lines": [l for _, l in stamps if l][:8], "meta": meta, "host_before": h0, "host_after": host()}
    shutil.rmtree(root, ignore_errors=True)
    return d


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--binary", required=True)
    p.add_argument("--backup", required=True)
    p.add_argument("--cores", type=int, required=True)
    p.add_argument("--frames", type=int, default=262144)
    p.add_argument("--label", required=True)
    p.add_argument("--reps", type=int, default=3)
    p.add_argument("--port", type=int, default=31841)
    p.add_argument("--no-serve", action="store_true")
    p.add_argument("--out", required=True)
    args = p.parse_args()
    man = {}
    for line in open(f"{args.backup}/backup_manifest"):
        parts = line.split()
        if len(parts) >= 2 and parts[0] != "file":
            man[parts[0]] = parts[1]
    out = {"label": args.label, "binary": args.binary, "backup": args.backup, "cores": args.cores, "manifest": man, "restores": []}
    t0 = time.perf_counter()
    v = subprocess.run([args.binary, "--verify-backup", args.backup], capture_output=True, text=True)
    out["verify"] = {"rc": v.returncode, "s": round(time.perf_counter() - t0, 3)}
    for rep in range(args.reps):
        d = timed(args, rep)
        out["restores"].append(d)
        print(f"{args.label} rep{rep}: rc={d['rc']} total {d['total_s']}s layout {d['layout_s']}s mount {d['mount_s']}s load {d['host_before']['loadavg'].split()[0]} "
              f"| {' / '.join(d['lines'][-2:])[:260]}", flush=True)
    if not args.no_serve and all(x["rc"] == 0 for x in out["restores"]):
        out["serve"] = timed(args, 99, serve=True)
        print("serve meta:", {k: v for k, v in (out["serve"]["meta"] or {}).items() if k.startswith("recovery_")}, flush=True)
    json.dump(out, open(args.out, "w"), indent=1)


if __name__ == "__main__":
    main()
