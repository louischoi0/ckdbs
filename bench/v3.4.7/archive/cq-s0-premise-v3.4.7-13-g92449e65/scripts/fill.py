import re
import sys

tables, doc = sys.argv[1], sys.argv[2]
sec, cur = {}, None
for line in open(tables).read().split("\n"):
    if line.startswith("### "):
        cur = line[4:]
        sec[cur] = []
    elif cur:
        sec[cur].append(line)
text = open(doc).read()
text = re.sub(r"@@(.+?)@@", lambda m: "\n".join(sec[m.group(1)]).strip(), text)
assert "@@" not in text
open(doc, "w").write(text)
