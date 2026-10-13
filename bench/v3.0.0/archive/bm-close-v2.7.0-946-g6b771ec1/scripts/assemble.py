import re, sys
SP = "/tmp/claude-1000/-home-cdkbs-ckdbs/cf0ad77a-b48f-47e8-b714-576faa5dc6c9/scratchpad/bm-s6/"
OUT = "/home/cdkbs/ckdbs/.claude/worktrees/bm-scan-kernel-open/bench/v3.0.0/results-bm-close-v2.7.0-946-g6b771ec1.md"
s = open(SP + "doc-template.md").read()

def table(m):
    name = m.group(1)
    t = open(SP + "tables/" + name + ".md").read().strip("\n")
    return t

s = re.sub(r"@@([a-z0-9-]+)@@", table, s)
left = re.findall(r"@@[A-Za-z0-9-]+@@", s)
if left:
    print("UNRESOLVED", left)
    sys.exit(1)
open(OUT, "w").write(s)
print("wrote", OUT, len(s.splitlines()), "lines")
