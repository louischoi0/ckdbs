import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")
import analyze as A

name = sys.argv[1] if len(sys.argv) > 1 else "overhead2"
g, s = A.table(name, ["A", "A2", "B"])
A.delta_table(g, s, "A", "A2", "B", name + ": deltas and noise floor")
