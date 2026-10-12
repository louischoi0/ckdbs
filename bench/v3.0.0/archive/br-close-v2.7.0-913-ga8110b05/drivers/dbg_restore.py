import subprocess, sys, shutil, time
sys.path.insert(0, "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/drivers")
from br_restore import write_conf, BIN_B, R
backup = sys.argv[1]
cores = int(sys.argv[2])
root = f"{R}/work/m3-dbg"
shutil.rmtree(root, ignore_errors=True)
conf, data = write_conf(root, 31841, cores, 262144)
t0 = time.time()
r = subprocess.run(["taskset", "-c", "0,2,4,6", BIN_B, "--config", conf, "--restore-from", backup, "--restore-only"],
                   capture_output=True, text=True)
print("rc", r.returncode, "after", round(time.time() - t0, 1), "s")
print("stdout:", r.stdout[-1500:])
print("stderr:", r.stderr[-1500:])
shutil.rmtree(root, ignore_errors=True)
