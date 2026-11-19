#!/usr/bin/env python3
"""Build kds_server (Release, tests and sim off) at each commit from a git archive,
outside the worktree.  usage: build.py <commit>...  (run from inside the repository)"""
import hashlib, os, subprocess as sp, sys, time
ROOT = "/home/louis/cxbisect"
def sh(cmd, **kw): return sp.run(cmd, shell=True, **kw)
for c in sys.argv[1:]:
    src, bld = f"{ROOT}/src{c}", f"{ROOT}/build{c}"
    t0 = time.time()
    if not os.path.exists(f"{ROOT}/bin/kds_server_{c}"):
        os.makedirs(src, exist_ok=True); os.makedirs(f"{ROOT}/bin", exist_ok=True)
        tar = sp.run(["git", "archive", c], capture_output=True, check=True).stdout
        sp.run(["tar", "-x", "-C", src], input=tar, check=True)
        r = sh(f"cmake -S {src} -B {bld} -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > {ROOT}/cmake{c}.log 2>&1"
               f" && cmake --build {bld} --target kds_server -j8 > {ROOT}/build{c}.log 2>&1")
        if r.returncode != 0:
            print(f"{c} BUILD FAILED", flush=True); continue
        sh(f"cp {bld}/kds_server {ROOT}/bin/kds_server_{c}")
    h = hashlib.sha256(open(f"{ROOT}/bin/kds_server_{c}", "rb").read()).hexdigest()
    print(f"{c} ok {time.time() - t0:.0f}s sha256 {h} {time.strftime('%FT%TZ', time.gmtime())}", flush=True)
print("ALLDONE", flush=True)
