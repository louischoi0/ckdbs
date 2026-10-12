#!/usr/bin/env python3
"""BR close, M0b: the data-file write path, where BR's fence sits (PageWriteFence::WriteScope in FilePageDevice::WriteAt:
three atomic operations per write call, disarmed). A and B, pairs alternating order; each a fresh server (cores = 1,
relaxed, checkpoint_interval_ms = 3000), a wide relation loaded with --rows rows (named pks, 100 per INSERT), then SHOW META
polled until a checkpoint run that began after the load ended has completed; its page-write phase sum over its pages is the
per-page cost of the write path. Equal work: the same rows into the same schema on both."""
import argparse, json, os, shutil, time

from br_common import *  # noqa


def one(label, binary, port, rows, tag):
    root = f"{R}/work/m0b-{tag}-{label}"
    shutil.rmtree(root, ignore_errors=True)
    srv = Server(label, binary, root, port, cores=1, durability="relaxed", frames=65536, ckpt_ms=3000, taskset="4", reactor_cpus="4")
    srv.start()
    try:
        c = srv.conn()
        must(c, f"CREATE TABLE ballast (id int64, v int64, payload varchar({WIDTH})) BTREE", "CREATED")
        t0 = time.time()
        load_wide(c, "ballast", 1, rows)
        load_s = time.time() - t0
        m0 = bb.meta_all(c)
        runs0 = m0["checkpoint_runs"]
        # a run that began after the load: wait for two more completions, take the second
        deadline = time.time() + 40
        m = m0
        while time.time() < deadline:
            m = bb.meta_all(c)
            if m["checkpoint_runs"] >= runs0 + 1 and m["checkpoint_last_completed"] == 1 and m["checkpoint_last_pages"] > 0:
                break
            time.sleep(0.2)
        keys = [k for k in m if k.startswith("checkpoint_last_")] + ["checkpoint_runs", "pool_resident"]
        out = {k: m[k] for k in keys}
        out["load_s"] = round(load_s, 2)
        pw = m["checkpoint_last_page_write_sum_us"]
        pg = m["checkpoint_last_pages"]
        out["page_write_us_per_page"] = round(pw / pg, 3) if pg else None
        c.close()
        return out
    finally:
        srv.stop()
        shutil.rmtree(root, ignore_errors=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rows", type=int, default=40000)
    p.add_argument("--pairs", type=int, default=8)
    p.add_argument("--out", required=True)
    p.add_argument("--progress", required=True)
    args = p.parse_args()
    bins = {"A": f"{R}/bin/kds_server-A", "B": f"{R}/bin/kds_server-B"}
    ports = {"A": 31851, "B": 31852}
    res = []
    for pair in range(args.pairs):
        order = ["A", "B"] if pair % 2 == 0 else ["B", "A"]
        for l in order:
            bb.wait_for_load(3.5, 30)
            h = host()
            d = one(l, bins[l], ports[l], args.rows, f"p{pair}")
            d.update({"label": l, "pair": pair, "load": h["loadavg"]})
            res.append(d)
            print(f"pair{pair} {l}: pages={d['checkpoint_last_pages']} page_write_sum={d['checkpoint_last_page_write_sum_us']}us "
                  f"-> {d['page_write_us_per_page']} us/page wall={d['checkpoint_last_wall_us']}us load {h['loadavg'].split()[0]}", flush=True)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    json.dump(res, open(args.out, "w"), indent=1)
    with open(args.progress, "a") as pf:
        pf.write(f"{time.strftime('%H:%M', time.gmtime())} M0b done\n")


if __name__ == "__main__":
    main()
