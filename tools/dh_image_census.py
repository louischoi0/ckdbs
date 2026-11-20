#!/usr/bin/env python3
"""DH-S0's image census: what the FULL_PAGE_IMAGE records of a WAL directory hold.

    dh_image_census.py --wal DIR --from-lsn N --label NAME --out CENSUS.json
                       [--sample-dir DIR] [--sample-n 3000] [--max-stats-per-type 3000]

Walks every segment file `wal-<core>-<segno>.log` of DIR (format:
`include/kds/wal/record.hpp` - a 4,096-byte segment header, then records of
`total_len | crc32c | lsn | txn_id | type | flags | reserved | page_id`, 32
bytes, 8-byte aligned, no record spans a segment, a PAD record seals one).
A record ends the walk of its segment when its length is impossible, its type
unknown, its LSN is not `segment_base + offset` (the segment's first record
fixes the base) or it is a PAD; the walker validates no CRC (a stale tail of a
recycled segment fails the LSN test) and says so in its output.

For every FULL_PAGE_IMAGE (type 10, payload = the page, 8,192 bytes) with
lsn >= --from-lsn it groups by the page's type byte
(`include/kds/base/common.hpp` PageType) and reports, per type: count, zero
bytes as a share of the page, the longest zero run (length, start-offset
quartiles) - the type-blind hole - zlib-1, LZ4 (`LZ4_compress_default`), zstd 1
and 3 sizes and microseconds a page (ctypes, a tight loop over pages already
in memory; the ctypes call is part of the time), the size after cutting the
longest zero run (L1; 4 bytes of hole descriptor, hole only if >= 32 bytes)
and LZ4 of the page after that cut (L1+L2). The per-page statistics use up to
--max-stats-per-type images a type (an even stride over the type's images);
the counts are all of them. Up to --sample-n raw images (an even stride over
all) are kept in --sample-dir as <label>.pages (concatenated 8,192-byte pages)
and <label>.index (lsn,page_id,type per line).
"""
import argparse, ctypes, json, os, re, statistics, struct, sys, time, zlib

PAGE = 8192
SEG_HDR = 4096
HDR = 32
TYPE_FPI, TYPE_PAD = 10, 13
PAGE_TYPES = {0: "invalid", 1: "heap", 2: "btree_internal", 3: "btree_leaf", 4: "undo", 5: "catalog",
              6: "superblock", 7: "freemap", 8: "headerless_map", 9: "waystone", 10: "varheap",
              11: "index_internal", 12: "index_leaf", 13: "cabin_bound", 14: "anchor"}
ZRUN = re.compile(rb"\x00+")


def libs():
    lz4 = ctypes.CDLL("liblz4.so.1")
    zs = ctypes.CDLL("libzstd.so.1")
    lz4.LZ4_compress_default.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    lz4.LZ4_compress_default.restype = ctypes.c_int
    zs.ZSTD_compress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t,
                                 ctypes.c_int]
    zs.ZSTD_compress.restype = ctypes.c_size_t
    return lz4, zs


def list_segments(wal_dir):
    """The segment files of every directory in `wal_dir` (a live WAL directory and an archive),
    one per segment number, in segment order: [(segno, 0, path)] and the info dict the walk fills.
    The LSN filter is applied by the caller, record by record."""
    segs, seen = [], set()
    for d in wal_dir:
        for f in os.listdir(d):
            m = re.fullmatch(r"wal-(\d+)-(\d+)\.log", f) or re.fullmatch(r"kds-.*-(\d+)\.log", f)
            if m:
                segno = int(m.group(2)) if m.re.pattern.startswith("wal") else int(m.group(1))
                if segno not in seen:
                    seen.add(segno)
                    segs.append((segno, 0, os.path.join(d, f)))
    segs.sort()
    info = {"segments": len(segs), "stopped": {}}
    return segs, info


def census(a):
    lz4, zs = libs()
    segs, info = list_segments(a.wal)
    rec_bytes, rec_count = {}, {}
    kept = {}  # type -> [step, seen, [(lsn, page_id, bytes)]]: an even stride, thinned as it grows
    n_since = 0
    raw = [1, 0, []]  # the saved sample: [step, seen, [(lsn, page_id, bytes)]], one even stride over ALL images
    total_img_all = 0
    for segno, core, fn in segs:
        with open(fn, "rb") as f:
            buf = f.read()
        if len(buf) < SEG_HDR or struct.unpack_from("<Q", buf, 0)[0] != 0x314C41575344584B:
            info["stopped"][fn] = "bad segment magic"
            continue
        off, base, why = SEG_HDR, None, "end of file"
        n = len(buf)
        while off + HDR <= n:
            tl, crc, lsn, txn, typ, fl, rsv, pid = struct.unpack_from("<IIQQBBHI", buf, off)
            if tl < HDR or tl % 8 or off + tl > n:
                why = "impossible total_len %d at %d" % (tl, off)
                if tl == 0:
                    why = "zero tail"
                break
            if typ == 0 or typ > 40:
                why = "unknown type %d" % typ
                break
            if base is None:
                base = lsn - off
            elif lsn != base + off:
                why = "lsn mismatch at %d" % off
                break
            if typ == TYPE_PAD:
                why = "PAD"
                break
            rec_count[typ] = rec_count.get(typ, 0) + 1
            rec_bytes[typ] = rec_bytes.get(typ, 0) + tl
            if typ == TYPE_FPI:
                total_img_all += 1
                if tl != HDR + PAGE:
                    why = "FPI with payload %d" % (tl - HDR)
                    break
                if lsn >= a.from_lsn:
                    n_since += 1
                    pg = buf[off + HDR: off + HDR + PAGE]
                    k = kept.setdefault(pg[0], [1, 0, []])
                    if k[1] % k[0] == 0:
                        k[2].append((lsn, pid, pg))
                        if len(k[2]) > 2 * a.max_stats_per_type:
                            k[2] = k[2][::2]
                            k[0] *= 2
                    k[1] += 1
                    if raw[1] % raw[0] == 0:
                        raw[2].append((lsn, pid, pg))
                        if len(raw[2]) > 2 * a.sample_n:
                            raw[2] = raw[2][::2]
                            raw[0] *= 2
                    raw[1] += 1
            off += tl
        info["stopped"][fn] = why
    del buf
    res = {"label": a.label, "wal_dirs": a.wal, "from_lsn": a.from_lsn, "segments": info["segments"],
           "segment_ends": info["stopped"], "crc_validated": False,
           "records_by_type": {str(k): {"count": rec_count[k], "bytes": rec_bytes[k]} for k in sorted(rec_count)},
           "images_in_log_all": total_img_all, "images_since_from_lsn": n_since}
    # raw sample
    if a.sample_dir and n_since:
        os.makedirs(a.sample_dir, exist_ok=True)
        step = max(1, -(-len(raw[2]) // a.sample_n))  # ceil: the whole span, never the first N
        pick = raw[2][::step]
        with open(os.path.join(a.sample_dir, a.label + ".pages"), "wb") as f, \
                open(os.path.join(a.sample_dir, a.label + ".index"), "w") as g:
            for lsn, pid, pg in pick:
                f.write(pg)
                g.write("%d,%d,%d\n" % (lsn, pid, pg[0]))
        res["raw_sample"] = len(pick)
    groups = {t: [x[2] for x in k[2]] for t, k in kept.items()}
    seen = {t: k[1] for t, k in kept.items()}
    types = {}
    out = ctypes.create_string_buffer(PAGE + 1024)
    zout = ctypes.create_string_buffer(PAGE + 1024)
    for t, pgs in sorted(groups.items()):
        step = max(1, -(-len(pgs) // a.max_stats_per_type))  # ceil: an even stride over the whole kept list
        sample = pgs[::step]
        zero, runlen, runstart, l1sz, cut_pages = [], [], [], [], []
        for pg in sample:
            zero.append(pg.count(0) / PAGE)
            best = (0, 0)
            for m in ZRUN.finditer(pg):
                if m.end() - m.start() > best[0]:
                    best = (m.end() - m.start(), m.start())
            runlen.append(best[0])
            runstart.append(best[1])
            if best[0] >= 32:
                l1sz.append(PAGE - best[0] + 4)
                cut_pages.append(pg[:best[1]] + pg[best[1] + best[0]:])
            else:
                l1sz.append(PAGE)
                cut_pages.append(pg)

        def timed(fn):
            t0 = time.perf_counter()
            sizes = [fn(pg) for pg in sample]
            return sizes, (time.perf_counter() - t0) / len(sample) * 1e6

        def f_zlib(pg):
            return len(zlib.compress(pg, 1))

        def f_lz4(pg):
            return lz4.LZ4_compress_default(pg, out, len(pg), len(out))

        def f_zs1(pg):
            return zs.ZSTD_compress(zout, len(zout), pg, len(pg), 1)

        def f_zs3(pg):
            return zs.ZSTD_compress(zout, len(zout), pg, len(pg), 3)
        szs = {}
        for name, fn in (("zlib1", f_zlib), ("lz4", f_lz4), ("zstd1", f_zs1), ("zstd3", f_zs3)):
            s, us = timed(fn)
            szs[name] = (s, us)
        t0 = time.perf_counter()
        l1l4 = [f_lz4(pg) + 4 for pg in cut_pages]
        us_l1l4 = (time.perf_counter() - t0) / len(sample) * 1e6

        def q(xs, p):
            xs = sorted(xs)
            return xs[min(len(xs) - 1, int(p * len(xs)))]

        def stat(sz):
            return {"mean_bytes": statistics.fmean(sz), "ratio_median": statistics.median(sz) / PAGE,
                    "ratio_p90": q(sz, 0.9) / PAGE}
        ent = {"name": PAGE_TYPES.get(t, "type%d" % t), "count": seen[t], "sampled": len(sample),
               "zero_share_mean": statistics.fmean(zero),
               "hole_len_mean": statistics.fmean(runlen), "hole_len_median": statistics.median(runlen),
               "hole_len_p90": q(runlen, 0.9),
               "hole_start_p25_p50_p75": [q(runstart, p) for p in (0.25, 0.5, 0.75)],
               "hole_share_mean": statistics.fmean(runlen) / PAGE,
               "L1": stat(l1sz), "L1+lz4": stat(l1l4), "L1+lz4_us": us_l1l4}
        for name, (s, us) in szs.items():
            ent[name] = stat(s)
            ent[name + "_us"] = us
        types[str(t)] = ent
    res["by_type"] = types
    json.dump(res, open(a.out, "w"), indent=1)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wal", required=True, nargs="+", help="one or more directories (a live WAL directory and an archive_dir); a segment number found twice is read once")
    ap.add_argument("--from-lsn", type=int, default=0)
    ap.add_argument("--label", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--sample-dir")
    ap.add_argument("--sample-n", type=int, default=3000)
    ap.add_argument("--max-stats-per-type", type=int, default=3000)
    a = ap.parse_args()
    t0 = time.time()
    r = census(a)
    print(f"{a.label}: segments={r['segments']} images_in_log={r['images_in_log_all']} "
          f"since_from_lsn={r['images_since_from_lsn']} took {time.time()-t0:.1f}s")
    for t, e in r["by_type"].items():
        print(f"  type {t} {e['name']}: n={e['count']} zero={e['zero_share_mean']:.2%} hole={e['hole_share_mean']:.2%} "
              f"lz4={e['lz4']['ratio_median']:.2f} ({e['lz4_us']:.1f}us) zstd1={e['zstd1']['ratio_median']:.2f} "
              f"({e['zstd1_us']:.1f}us) L1={e['L1']['ratio_median']:.2f}")


if __name__ == "__main__":
    main()
