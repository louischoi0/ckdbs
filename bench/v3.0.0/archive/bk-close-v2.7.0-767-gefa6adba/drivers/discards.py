import glob, os, re, sys, collections
R = "/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad/run/"
out = []
out.append("""**Rules, declared before the first run unless marked** (`archive/.../drivers/analyze_bk.py`, docstring). A finished run is
discarded if, in its own record: **(1)** a real `cc1plus`, `cmake`, `make`, `ninja` or `ld` process is in its before or after list
(*amended*: a foreign `ctest` or `kds_tests` process no longer discards on its own - the first is a sleeping parent, the second one test - and the CPU rules see their load); **(2)** the CPUs outside the server's, the driver's and 6,7 averaged more than 10 % busy
(*amended during the run*: it first counted CPUs 6,7, and at `cores = 2` counted CPU 1, which the engine pins its second
reactor to); **(2b)** a hyperthread sibling of a pinned CPU (3 and 5 at `cores = 1`; 0 and 5 at `cores = 2`, amended with
rule 2) was more than 15 % busy; **(3)** any error reply, a row count other than the expected one, or a refusal probe other
than the expected one; **(4)** the `SHOW META` control's p50 in either arm exceeds 1.04 x the cell's median (*amended*: it first
read 1.10 x, and no kept run of `cores = 1` or `cores = 2` moves between the two values). Discarded runs are moved aside with their reasons, not deleted, and the same run index is run again, up to six
times; re-running a discarded index keeps the A/B start order, which alternates by index parity. The discards below were
applied by rules that read only the host and the control, never a statement's latency. A dry run of the final rules over every kept run
of every cell discards none of them (the final 1.04 rule moved two runs, one of the `cores = 1` null pair and one of the fresh pair, which were not
re-run: those cells hold 15 runs where 16 were asked). Runs discarded under an earlier form of a rule were re-run at the same index and the re-run is the one kept; the earlier
record stays under `discarded/` with its reasons.
""")
for d in ("overhead-c1", "overhead-c2", "null-c1", "null-c2", "strict-c1", "flip-c1", "nullB-c1", "nullA-c1", "fresh-c1"):
    rs = sorted(glob.glob(f"{R}{d}/discarded/*.reasons.txt"))
    rs = [r for r in rs if not os.path.basename(r).startswith("restored-")]
    kept = len(glob.glob(f"{R}{d}/c*-n*-r*.json"))
    if not rs and not kept:
        continue
    out.append(f"**`{d}`**: {kept} runs kept, {len(rs)} discarded.")
    cnt = collections.Counter()
    for r in rs:
        for line in open(r).read().splitlines():
            key = re.sub(r"[0-9.]+%?", "#", line)[:60]
            key = line.split(":")[0] if line.startswith("host_") else re.sub(r"\d+(\.\d+)?", "#", line)[:70]
            cnt[key] += 1
    for k, v in cnt.most_common():
        out.append(f"- {v} x `{k}`")
    out.append("")
print("\n".join(out))
