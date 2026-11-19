#!/usr/bin/env python3
"""Build kds_server Release (tests, sim off) from each knob export in turn; prints sha256. usage: buildk.py K1 K2 K3 KA"""
import hashlib, subprocess as sp, sys, time
ROOT = "/home/louis/dfclose"
for n in sys.argv[1:]:
    t0 = time.time()
    r = sp.run(f"cmake -S {ROOT}/src{n} -B {ROOT}/build{n} -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > {ROOT}/cmake{n}.log 2>&1"
               f" && cmake --build {ROOT}/build{n} --target kds_server -j8 > {ROOT}/build{n}.log 2>&1", shell=True)
    if r.returncode != 0:
        print(n, "BUILD FAILED", flush=True)
        continue
    sp.run(f"cp {ROOT}/build{n}/kds_server {ROOT}/bin/kds_server_{n}", shell=True)
    h = hashlib.sha256(open(f"{ROOT}/bin/kds_server_{n}", "rb").read()).hexdigest()
    print(f"{n} ok {time.time() - t0:.0f}s sha256 {h} {time.strftime('%FT%TZ', time.gmtime())}", flush=True)
print("ALLDONE", flush=True)
