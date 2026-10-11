#!/bin/bash
# Re-run main pair n10000-run1 until its 1 s pre-start sample and its during-run sample show every CPU
# other than 6/7 at or below 0.5 busy; each rejected attempt is kept under res-excluded/.
cd /home/cdkbs/bench-runs/ew
rm -f res/n10000-run1.*
for a in 1 2 3 4 5 6 7 8 9 10 11 12; do
  python3 ew_run.py 1 10000 /home/cdkbs/bench-runs/ew/res > run-main-replacement-$a.log 2>&1
  ok=$(python3 - <<'EOF'
import ast, re
t = open("/home/cdkbs/bench-runs/ew/res/n10000-run1.host.txt").read()
b = ast.literal_eval(re.search(r"cpu_busy_1s_before=(\{.*\})", t).group(1))
d = ast.literal_eval(re.search(r"cpu_busy_during=(\{.*\})", t).group(1))
m = max(max(v for k, v in x.items() if k not in (6, 7)) for x in (b, d))
print("yes" if m <= 0.5 else "no")
EOF
)
  if [ "$ok" = "yes" ]; then echo "attempt $a clean" > retry1.done; exit 0; fi
  for f in res/n10000-run1.*; do mv "$f" "res-excluded/attempt$a-$(basename $f)"; done
  sleep 20
done
echo "no clean attempt" > retry1.done
