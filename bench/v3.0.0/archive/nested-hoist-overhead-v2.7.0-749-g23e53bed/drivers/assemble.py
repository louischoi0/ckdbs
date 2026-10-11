import re, shutil
S = "/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad/nh"
N = "/home/cdkbs/bench-runs/nh"
W = "/home/cdkbs/ckdbs/.claude/worktrees/fix-nested-uncorrelated-subquery"
OUT = f"{W}/bench/v3.0.0/results-nested-hoist-overhead-v2.7.0-749-g23e53bed.md"
AR = f"{W}/bench/v3.0.0/archive/nested-hoist-overhead-v2.7.0-749-g23e53bed"
shutil.copy(f"{N}/all8/analysis.json", f"{AR}/main/analysis-8runs.json")
tables = open(f"{N}/res/tables.md").read().split("\n\n")
matrix, delta, dist = [t.strip().splitlines() for t in tables[:3]]
swap = open(f"{N}/fragments/swap.md").read().strip().splitlines()
paired = open(f"{N}/fragments/paired.md").read().strip().splitlines()


def filt(lines, arms):
    head = lines[:2]
    if arms == "ALL":
        return "\n".join(lines)
    keep = [l for l in lines[2:] if l.split("|")[1].strip() in arms.split(",")]
    return "\n".join(head + keep)


def matrix_fn():
    return "\n".join(matrix)


def dist_fn():
    return "\n".join(dist)


t = open(f"{S}/template.md").read()
t = re.sub(r"\{\{DELTA:([^}]*)\}\}", lambda m: filt(delta, m.group(1)), t)
t = re.sub(r"\{\{PAIRED:([^}]*)\}\}", lambda m: filt(paired, m.group(1)), t)
t = re.sub(r"\{\{SWAP:([^}]*)\}\}", lambda m: filt(swap, m.group(1)), t)
t = t.replace("{{MATRIX}}", matrix_fn()).replace("{{DIST}}", dist_fn())
open(OUT, "w").write(t)
print(len(t.splitlines()), "lines;", "unreplaced:", re.findall(r"\{\{[^}]*\}\}", t))
