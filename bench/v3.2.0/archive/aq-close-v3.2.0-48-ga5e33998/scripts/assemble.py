import re, sys
A = "/home/cdkbs/ckdbs/.claude/worktrees/aq-expression-cabins/bench/v3.2.0/archive/aq-close-v3.2.0-48-ga5e33998/out/"
tpl = open("/home/cdkbs/bench-runs/aq-close/template.md").read()


def block(fname, idx):
    text = open(A + fname).read()
    parts = text.split("#### ")[1:]
    lines = [l for l in parts[idx].splitlines()[1:] if l.startswith("|") or l.strip() == ""]
    out = "\n".join(lines).strip("\n")
    return out


def sub(m):
    return block(m.group(1), int(m.group(2)))


tpl = re.sub(r"\{\{raw:(\S+?)\}\}", lambda m: open(A + m.group(1)).read().strip(), tpl)
res = re.sub(r"\{\{(\S+?):(\d+)\}\}", sub, tpl)
out = "/home/cdkbs/ckdbs/.claude/worktrees/aq-expression-cabins/bench/v3.2.0/results-aq-close-v3.2.0-48-ga5e33998.md"
open(out, "w").write(res)
print(len(res))
