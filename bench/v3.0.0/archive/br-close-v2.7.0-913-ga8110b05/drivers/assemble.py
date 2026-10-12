#!/usr/bin/env python3
"""Assemble the two results files from the templates and the generated fragments."""
import glob, json, statistics as st, subprocess, sys

R = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05"
W = "/home/cdkbs/ckdbs/.claude/worktrees/br-backup-restore/bench/v3.0.0"
DESC = "v2.7.0-913-ga8110b05"


def rd(p):
    return open(p).read().rstrip("\n")


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout.rstrip("\n")


# ---- M0b
m0b = json.load(open(f"{R}/out/m0b/m0b.json"))
by = {}
for x in m0b:
    by.setdefault(x["pair"], {})[x["label"]] = x
lines = ["| pair | order | A page-write phase | B page-write phase | B - A | A checkpoint run | B checkpoint run | pages flushed |", "|---|---|---|---|---|---|---|---|"]
dd = []
for pair in sorted(by):
    a, b = by[pair]["A"], by[pair]["B"]
    order = "A,B" if pair % 2 == 0 else "B,A"
    d = b["page_write_us_per_page"] - a["page_write_us_per_page"]
    dd.append(d)
    lines.append(f"| {pair} | {order} | {a['page_write_us_per_page']:.3f} µs/page | {b['page_write_us_per_page']:.3f} µs/page | {d:+.3f} µs/page | "
                 f"{a['checkpoint_last_wall_us'] / 1000:.0f} ms | {b['checkpoint_last_wall_us'] / 1000:.0f} ms | {int(a['checkpoint_last_pages']):,} pages |")
av = [x["page_write_us_per_page"] for x in m0b if x["label"] == "A"]
bv = [x["page_write_us_per_page"] for x in m0b if x["label"] == "B"]
aw = [x["checkpoint_last_wall_us"] / 1000 for x in m0b if x["label"] == "A"]
bw = [x["checkpoint_last_wall_us"] / 1000 for x in m0b if x["label"] == "B"]
lines.append(f"| median [min-max] | | {st.median(av):.3f} [{min(av):.3f} to {max(av):.3f}] µs/page | {st.median(bv):.3f} [{min(bv):.3f} to {max(bv):.3f}] µs/page | "
             f"{st.median(dd):+.3f} [{min(dd):+.3f} to {max(dd):+.3f}] µs/page | {st.median(aw):.0f} [{min(aw):.0f} to {max(aw):.0f}] ms | {st.median(bw):.0f} [{min(bw):.0f} to {max(bw):.0f}] ms | 8,546 pages |")
M0B = "\n".join(lines)

# ---- doc 1
d1 = rd(f"{R}/doc1.tpl.md")
d1 = d1.replace("{{M0_HOST}}", rd(f"{R}/logs/m0_host.md"))
d1 = d1.replace("{{M0_TABLES}}", rd(f"{R}/logs/m0_tables.md").replace("#### ", "#### "))
d1 = d1.replace("{{VERDICT}}", rd(f"{R}/logs/verdict_m0.txt"))
d1 = d1.replace("{{M0B_TABLE}}", M0B)
open(f"{W}/results-br-close-overhead-{DESC}.md", "w").write(d1 + "\n")

# ---- doc 2
m1a = rd(f"{R}/logs/m1a_tables.md")
m1a = m1a[: m1a.index("\nDiagnostic (cores = 1)")]
m1a = m1a.replace("\nWAL and archive counters, wide INSERT phase:\n", "\nThe log and the archive's counters in the wide phase (median [min-max] over the five rounds):\n\n")

m1b = rd(f"{R}/logs/m1b_tables.md")
m1b = m1b.replace("### m1b-c1:", "#### `cores = 1`, server on CPUs 0,2,4,6, reactor on 4:")
m1b = m1b.replace("### m1b-c1-offcpu:", "#### `cores = 1`, the server's other threads kept off the reactor's CPU (mask 0,2,6, reactor on 4):")
m1b = m1b.replace("### m1b-c4:", "#### `cores = 4`, server on CPUs 0,2,4,6, reactors on 4,2,6,0:")
import re
m1b = re.sub(r"(^#### .*\n)(\|)", r"\1\n\2", m1b, flags=re.M)

# stall
rows = []
for f in sorted(glob.glob(f"{R}/out/stall/*.json")):
    d = json.load(open(f))
    tag = f.split("stall-")[1].split(".")[0]
    w = d["windows"][-1]
    a = w["all"]
    ex = (w["stmts"] - a["slow_gt_1ms_n"]) / (w["elapsed_s"] - a["slow_gt_1ms_s"])
    rows.append((tag, w, a, ex))
st_lines = ["| run (order) | binary | window | statements | statements/s, raw | statements/s excluding statements over 1 ms | share of the window inside statements over 1 ms | p50 | p99 | p99.9 | longest statement |",
            "|---|---|---|---|---|---|---|---|---|---|---|"]
order = {"A1": 1, "B1": 2, "B2": 3, "A2": 4}
for tag, w, a, ex in sorted(rows, key=lambda r: order[r[0]]):
    st_lines.append(f"| {tag} ({order[tag]} of 4) | {'A `fb953f07`' if tag[0] == 'A' else 'B `a8110b05`'} | {w['elapsed_s']:.1f} s | {w['stmts']:,} statements | {w['stmt_per_s']:,.0f} /s | {ex:,.0f} /s | "
                    f"{100 * a['slow_gt_1ms_s'] / w['elapsed_s']:.0f} % | {a['p50_us']:.1f} µs | {a['p99_us']:.1f} µs | {a['p999_us']:.1f} µs | {a['max_us'] / 1000:,.0f} ms |")
STALL = f"""### Are the stalls BR's? No: A has the same

Roughly half of every window in this workload, backup or not, sits inside statements over 1 ms, with single statements of 2-4
s. To see whether that is BR's (it changed the log device's segment reclaim and the stream's append path), the same workload
(`cores = 1`, the 540,000-row ballast, no backup, one 40 s window after 9 s of warm-up) ran on A and on B, in the order A, B, B, A,
each on a fresh server and data file. A has no `BACKUP`, so its windows are the baseline only.

{chr(10).join(st_lines)}

The share of a window inside statements over 1 ms is 49 % and 49 % on A, 47 % and 48 % on B; the service rate excluding them is
17,920-18,135 /s on both; the longest statement is 3.0-3.1 s on A and 2.5-4.1 s on B. **The stalls are in A and in B alike and
are not BR's.** They are the engine's, on a device that writes 165 MB/s, under a workload that dirties random pages of a
1 GiB relation: a candidate for its own work order, since half the wall time of a `relaxed` instance is spent inside
multi-second statements while its checkpoint falls behind. This run did not look inside them. For this file they set the
comparison: M1b's copy windows are read against baseline windows that stall as often."""

# M2
m2_lines = [l for l in rd(f"{R}/logs/m2_tables.md").splitlines() if l.startswith("|")]
M2 = "\n".join(m2_lines)
M2 += "\n\nPeak over any 5 s window 106.7 MiB/s (from 30.8 s), median of the 5 s windows 64.7, lowest 19.1; 176 windows. Max `wal_archive_lag_segments`: 5 in the steady phase, 39 after the outage (at 76.3 s)."

# M3
m3_parts = []
for f in sorted(glob.glob(f"{R}/out/m3/*.json")):
    d = json.load(open(f))
    m = d["manifest"]
    start, stop = int(m["start_lsn"]), int(m["stop_lsn"])
    title = {"m1b-c1": "`cores = 1`, the backup taken under load (M1b, backup 5 of 5)", "m1b-c4": "`cores = 4`, the backup taken under load (M1b, backup 5 of 5)",
             "m2-pitr-latest": "`cores = 4`, the idle backup of M2 restored through the archive to `latest`"}[d["label"]]
    out = [f"#### {title}", "",
           f"Backup {m['backup_id']}: path {m['path']}, start LSN {start:,}, stop LSN {stop:,}, a window of {(stop - start) / (1 << 20):,.0f} MiB of log "
           f"(segments {m['start_segment']}..{m['stop_segment']}), {d['backup_bytes'] / (1 << 20):,.0f} MiB on disk; `--verify-backup` {d['verify']['s']} s.", "",
           "| restore | backup files | total wall time | layout (copy + verify) | mount, replay (to the second line) |", "|---|---|---|---|---|"]
    for r in d["restores"]:
        out.append(f"| {r['rep'] + 1} | {'cold' if r['cold_backup_files'] else 'warm'} | {r['total_s']:.1f} s | {r['layout_s']:.1f} s | {r['recovery_s']:.1f} s |")
    mt = d["serve"]["meta"]
    ph = {k: (mt.get(k) or 0) / 1e6 for k in ("recovery_analysis_us", "recovery_redo_us", "recovery_undo_us", "recovery_checkpoint_us")}
    out += ["", f"`SHOW META` of the serving run's mount: {int(mt['recovery_records']):,} records scanned, {int(mt['recovery_redo_applied']):,} redo applied, "
            f"{int(mt['recovery_committed']):,} committed; analysis {ph['recovery_analysis_us']:.2f} s, redo {ph['recovery_redo_us']:.2f} s, undo {ph['recovery_undo_us']:.2f} s, "
            f"completion checkpoint {ph['recovery_checkpoint_us']:.2f} s (sum {sum(ph.values()):.2f} s)."]
    m3_parts.append("\n".join(out))
M3 = "\n\n".join(m3_parts)

d2 = rd(f"{R}/doc2.tpl.md")
d2 = d2.replace("{{M1A}}", m1a).replace("{{M1B}}", m1b).replace("{{STALL_SECTION}}", STALL).replace("{{M2}}", M2).replace("{{M3}}", M3)
d2 = d2.replace("{{HOSTSUM}}", run(["python3", f"{R}/drivers/hostsum.py"]))
open(f"{W}/results-br-close-backup-restore-{DESC}.md", "w").write(d2 + "\n")
print("written", f"{W}/results-br-close-overhead-{DESC}.md", f"{W}/results-br-close-backup-restore-{DESC}.md")
