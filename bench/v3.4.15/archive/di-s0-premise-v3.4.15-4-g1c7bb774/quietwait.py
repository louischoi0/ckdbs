import os, time, re
pat = re.compile(r"cc1plus|cmake --build|ctest|kds_tests")
me = os.getpid()
quiet = 0
t0 = time.time()
while True:
    busy = []
    for p in os.listdir('/proc'):
        if not p.isdigit() or int(p) == me:
            continue
        try:
            c = open(f'/proc/{p}/cmdline', 'rb').read().replace(b'\0', b' ').decode()
        except Exception:
            continue
        if 'quietwait.py' in c or c.startswith('/bin/bash -c') or c.startswith('sh -c'):
            continue
        if pat.search(c):
            busy.append(p)
    la = float(open('/proc/loadavg').read().split()[0])
    quiet = quiet + 1 if (not busy and la < 4) else 0
    open('/home/louis/bench-runs/quiet.status', 'w').write(f"{time.time()-t0:.0f}s busy={len(busy)} load={la} quiet={quiet}\n")
    if quiet >= 6:
        break
    time.sleep(10)
open('/home/louis/bench-runs/quiet.done', 'w').write(time.strftime('%FT%TZ', time.gmtime()) + f" held {time.time()-t0:.0f}s load {open('/proc/loadavg').read()}")
