t = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/doc2.tpl.md"
d = open(t).read()
i = d.index("\n## 11. Re-measured at 90cdf918")
sec = d[i:]
d = d[:i].rstrip("\n") + "\n"
sec = sec.replace("| Suite | not executed; the statement path was not re-measured (the change is in the mount only) |",
                  "| Host load | 1-minute load before each attempt: 2.13 (backup re-take), 2.22-2.51 (the 4,059 MiB restores), 2.11-3.73 (the 1,179 MiB comparison); `pgrep` lists held no compiler, build or test process (only this run's own shell wrappers); the kill test's load was not read (above); **no per-second sampler ran for this section**, so a foreign process under 20 s long would not show |\n| Suite | not executed; the statement path was not re-measured (the change is in the mount only) |")
j = d.index("\n## Host record")
d = d[:j].rstrip("\n") + "\n" + sec.rstrip("\n") + "\n" + d[j:]
open(t, "w").write(d)
print("ok")
