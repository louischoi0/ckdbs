import sys, re
sys.path.insert(0, "/home/cdkbs/bench-run")
from edlib import *
sys.path.insert(0, "/home/cdkbs/bench-run")
from edit_b_helpers import map_rows

# ---- scenario 2: spread definition (max - min) / median, re-derived
d = Doc(2)
d.rep("spreads 0.5 % and 3.4 %)", "spreads 0.5 % and 3.3 %)")
d.rep("spread (max/min-1)", "spread ((max-min)/median)")
d.rep("**1,965.0 tps** | 0.5 % | 1,902.7 tps | 1.5 % | +3.3 % |", "**1,965.0 tps** | 0.5 % | 1,902.7 tps | 1.4 % | +3.3 % |")
d.rep("**587.6 tps** | 1.7 % | 583.1 tps | 8.0 % | +0.8 % |", "**587.6 tps** | 1.7 % | 583.1 tps | 7.6 % | +0.8 % |")
d.rep("**1,674.2 tps** | 3.4 % | 1,584.9 tps | 3.2 % | +5.6 % |", "**1,674.2 tps** | 3.3 % | 1,584.9 tps | 3.1 % | +5.6 % |")
d.rep("against spreads 0.5 % and 1.5 %", "against spreads 0.5 % and 1.4 %")
d.rep("against 3.4 % and 3.2 %)", "against 3.3 % and 3.1 %)")
d.rep("**1,674.2 tps** | 3.4 % | 1,584.9 tps | +5.6 % |", "**1,674.2 tps** | 3.3 % | 1,584.9 tps | +5.6 % |")
d.rep("| 419.5 tps | 3.1 % | 1.00x |", "| 419.5 tps | 3.0 % | 1.00x |")
d.rep("the PostgreSQL cells' is 3.1 %", "the PostgreSQL cells' is 3.0 %")
d.rep("0.5 % (`group`) to 3.4 % (`strict`)", "0.5 % (`group`) to 3.3 % (`strict`)")
print(d.s.count("3.4 %"), "remaining '3.4 %' in scenario 2 (shares tables legitimately carry some)")
# ---- units
sec2_i, sec2_j = d.section(r"^## 2\. What was run", r"^## 3\.")
seg = d.s[sec2_i:sec2_j]
seg = seg.replace("| 8 / 1 / 1 / 8 |", "| 8 / 1 / 1 / 8 cores |").replace("| 1 / 8 | group | 8 | 1 each |", "| 1 / 8 cores | group | 8 bookers | 1 run each |")
seg = seg.replace("| 8 | group and strict | 1, 2, 4, 16 | 1 each |", "| 8 cores | group and strict | 1, 2, 4, 16 bookers | 1 run each |")
seg = seg.replace("| 8 | group and strict | 16, 32 | 1 each |", "| 8 cores | group and strict | 16, 32 bookers | 1 run each |")
seg = seg.replace("| strict / strict / group / group | 8 | 3 each |", "| strict / strict / group / group | 8 bookers | 3 runs each |")
seg = seg.replace("| 1 | group / strict | 1 | 3 each |", "| 1 core | group / strict | 1 booker | 3 runs each |")
seg = seg.replace("| `synchronous_commit = on` | 1 | 3 |", "| `synchronous_commit = on` | 1 booker | 3 runs |")
d.s = d.s[:sec2_i] + seg + d.s[sec2_j:]
for hdr in ("| cell | cores | durability | run 1", ):
    map_rows(d, hdr, lambda c: [c[0], c[1] + (" core" if c[1] == "1" else " cores")] + c[2:])
d.rep("| `sat-b16-s2-c8-g` | 19 |", "| `sat-b16-s2-c8-g` | 19 samples |")
d.rep("| `sat-b16-s2-c8-s` | 19 |", "| `sat-b16-s2-c8-s` | 19 samples |")
d.rep("| `sat-b32-s2-c8-g` | 19 |", "| `sat-b32-s2-c8-g` | 19 samples |")
d.rep("| `sat-b32-s2-c8-s` | 19 |", "| `sat-b32-s2-c8-s` | 19 samples |")
d.save()

# ---- scenario 1 units
d = Doc(1)
i, j = d.section(r"^## 2\. What was run", r"^## 3\.")
seg = d.s[i:j]
seg = re.sub(r"\| (1|8) \| (group|strict) \| 3 runs", lambda m: f"| {m.group(1)} core{'s' if m.group(1) == '8' else ''} | {m.group(2)} | 3 runs", seg)
seg = re.sub(r"\| - \| (same|`synchronous)", r"| - | \1", seg)
d.s = d.s[:i] + seg + d.s[j:]
map_rows(d, "| bars | cell | runs |", lambda c: [c[0] + " bars", c[1], c[2] + " runs", c[3], c[4], c[5], c[6], c[7], re.sub(r"\((\d)\)", r"(\1 runs)", c[8]), c[9]])
d.save()

# ---- exit codes in host tables, all files
for k in (1, 2, 3, 4):
    d = Doc(k)
    d.s = re.sub(r"(samples \| )(\d+) \|\n", r"\1exit \2 |\n", d.s)
    d.save()
print("ok")
