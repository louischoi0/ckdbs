import sys, shutil
sys.path.insert(0, "/home/cdkbs/bench-runs/be-close/driver")
from pool_lib import *
wd = f"{R}/pg"
shutil.rmtree(wd, ignore_errors=True)
s = Server("B", wd, 15660, 65536).start()
s.q("CREATE TABLE be_s (id int64, v int64, w int64) BTREE")
for lo in range(1, 100001, 500):
    s.q("INSERT INTO be_s VALUES " + ",".join(f"({i}, {i}, {i})" for i in range(lo, lo + 500)))
    if lo + 499 in (10000, 100000):
        pass
for n in (10000, 100000):
    r = s.q(f"ANALYZE SELECT COUNT(*), MIN(id), MAX(v) FROM be_s WHERE id < {n + 1} AND v >= 0")
    print(n, r[:200].replace("\\n", " | "))
r = s.q("ANALYZE SELECT COUNT(*), MIN(id), MAX(v) FROM be_s")
print(r[:200].replace("\\n", " | "))
s.stop()
shutil.rmtree(wd, ignore_errors=True)
