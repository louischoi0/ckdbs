#!/usr/bin/env python3
"""BR close, M3: restore time. For one backup directory, `kds_server --restore-from <dir> --restore-only` into a fresh target,
timed from the process start; the two stdout lines the restore prints (the layout done, the recovery done) split the wall
time into layout (copy + verify) and recovery (mount + replay + exit). Also `--verify-backup`, and one run that serves
(no --restore-only) so SHOW META's recovery block gives the records replayed and the phase times of that mount.

  br_restore.py --backup DIR --cores N [--archive DIR --target latest] --label X --out JSON [--reps 3]
"""
import argparse, json, os, shutil, signal, subprocess, sys, time

from br_common import *  # noqa

BIN_B = f"{R}/bin/kds_server-B"


def write_conf(root, port, cores, frames, extra=()):
    os.makedirs(root, exist_ok=True)
    data = f"{root}/kds.db"
    lines = [f"data_file = {data}", f"port = {port}", f"cores = {cores}", "durability = relaxed",
             f"buffer_pool_frames = {frames}", "log_file = kds.log", f"log_dir = {root}", "log_level = warn"]
    if cores > 1:
        lines.append("reactor_cpus = 4,2,6,0")
    else:
        lines.append("reactor_cpus = 4")
    lines += list(extra)
    conf = f"{root}/kds.conf"
    open(conf, "w").write("\n".join(lines) + "\n")
    return conf, data


def drop_cache(path):
    """posix_fadvise(DONTNEED) over every file under path: the clean pages of a just-written backup leave the page cache."""
    n = 0
    for dp, _, fs in os.walk(path):
        for f in fs:
            fd = os.open(os.path.join(dp, f), os.O_RDONLY)
            try:
                os.fsync(fd) if False else None
                os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
                n += 1
            finally:
                os.close(fd)
    return n


def timed_restore(args, rep, serve=False):
    cold = rep == 0 and not args.warm_only
    if cold:
        drop_cache(args.backup)
        if args.archive:
            drop_cache(args.archive)
    root = f"{R}/work/m3-{args.label}-{rep}{'-serve' if serve else ''}"
    shutil.rmtree(root, ignore_errors=True)
    conf, data = write_conf(root, args.port, args.cores, args.frames)
    cmd = ["taskset", "-c", "0,2,4,6", BIN_B, "--config", conf, "--restore-from", args.backup]
    if args.archive:
        cmd += ["--restore-archive", args.archive, "--restore-target", args.target]
    if not serve:
        cmd.append("--restore-only")
    h0 = host()
    t0 = time.perf_counter()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    stamps = []
    meta_recovery = None
    if not serve:
        for line in proc.stdout:
            stamps.append((time.perf_counter() - t0, line.strip()))
        proc.wait()
    else:
        # serve: read the two restore lines, then the listening banner, then SHOW META and stop
        for line in proc.stdout:
            stamps.append((time.perf_counter() - t0, line.strip()))
            if line.startswith("listening on"):
                break
        c = ServerConnection("127.0.0.1", args.port, timeout=60)
        m = bb.meta_all(c)
        meta_recovery = {k: v for k, v in m.items() if k.startswith(("recovery_", "pool_resident", "wal_durable_lsn", "wal_first_segment"))}
        c.close()
        proc.send_signal(signal.SIGTERM)
        proc.wait(120)
    total = time.perf_counter() - t0
    laid = next((t for t, l in stamps if "laid out" in l), None)
    rec = next((t for t, l in stamps if l.startswith("restore of backup")), None)
    d = {"rep": rep, "cold_backup_files": cold, "serve": serve, "rc": proc.returncode, "total_s": round(total, 3),
         "layout_s": round(laid, 3) if laid is not None else None,
         "recovery_s": round((rec if rec is not None else total) - laid, 3) if laid is not None else None,
         "lines": [l for _, l in stamps if l][:6], "meta": meta_recovery, "host_before": h0, "host_after": host()}
    d["data_bytes"] = os.path.getsize(data) if os.path.exists(data) else None
    wal_dir = data + ".wal"
    if os.path.isdir(wal_dir):
        d["wal_dir_bytes"] = sum(os.path.getsize(os.path.join(wal_dir, f)) for f in os.listdir(wal_dir))
    shutil.rmtree(root, ignore_errors=True)
    return d


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--backup", required=True)
    p.add_argument("--cores", type=int, required=True)
    p.add_argument("--frames", type=int, default=262144)
    p.add_argument("--archive")
    p.add_argument("--target")
    p.add_argument("--label", required=True)
    p.add_argument("--reps", type=int, default=3)
    p.add_argument("--warm-only", action="store_true")
    p.add_argument("--port", type=int, default=31841)
    p.add_argument("--out", required=True)
    args = p.parse_args()
    out = {"label": args.label, "backup": args.backup, "cores": args.cores, "archive": args.archive, "target": args.target}
    man = {}
    for line in open(f"{args.backup}/backup_manifest"):
        parts = line.split()
        if len(parts) >= 2 and parts[0] != "file":
            man[parts[0]] = parts[1]
        elif parts and parts[0] == "file":
            man.setdefault("files", []).append((parts[1], int(parts[2])))
    out["manifest"] = man
    out["backup_bytes"] = sum(os.path.getsize(os.path.join(args.backup, f)) for f in os.listdir(args.backup)
                              if os.path.isfile(os.path.join(args.backup, f)))
    # verify
    t0 = time.perf_counter()
    r = subprocess.run([BIN_B, "--verify-backup", args.backup], capture_output=True, text=True)
    out["verify"] = {"rc": r.returncode, "s": round(time.perf_counter() - t0, 3), "stdout": r.stdout.strip()[:300]}
    print("verify", out["verify"], flush=True)
    out["restores"] = []
    for rep in range(args.reps):
        d = timed_restore(args, rep)
        out["restores"].append(d)
        print(f"{args.label} rep{rep}: total {d['total_s']}s layout {d['layout_s']}s recovery {d['recovery_s']}s rc={d['rc']} "
              f"load {d['host_before']['loadavg'].split()[0]}", flush=True)
    out["serve"] = timed_restore(args, 99, serve=True)
    print("serve meta:", {k: v for k, v in (out["serve"]["meta"] or {}).items() if k.startswith("recovery_")}, flush=True)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    json.dump(out, open(args.out, "w"), indent=1)


if __name__ == "__main__":
    main()
