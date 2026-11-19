#!/usr/bin/env python3
"""CY close: the 'other admin session'. Opens one connection, SET PRIORITY background,
then loops GATHER STATISTICS ON <relation> while told 'on' on stdin; 'off' prints
a JSON line {n, mean_ms, max_ms}; 'quit' ends. Idle (connected, level set) while off."""
import json, os, select, sys, time
PACE = float(os.environ.get('GPACE', '0'))
sys.path.insert(0, sys.argv[3])
import kwp
port, rel = int(sys.argv[1]), sys.argv[2]
c = kwp.Connection("127.0.0.1", port, timeout=300, client_name="cy_gatherer")
tag, rows, _b, err = c.query("SET PRIORITY background")
if err is not None: sys.exit(f"set priority: {err}")
# optional argv[4]: a suffix for the GATHER statement, or "SQL:<statement>" for a control statement
arg = sys.argv[4] if len(sys.argv) > 4 else ""
STMT = arg[4:] if arg.startswith("SQL:") else f"GATHER STATISTICS ON {rel}{arg}"
on, n, durs = False, 0, []
print("ready", flush=True)
while True:
    r, _, _ = select.select([sys.stdin], [], [], 0 if on else 0.01)
    if r:
        line = sys.stdin.readline().strip()
        if line == "on": on, n, durs = True, 0, []
        elif line == "off":
            on = False
            print(json.dumps({"n": n, "mean_ms": (sum(durs) / n * 1e3) if n else 0,
                              "max_ms": max(durs) * 1e3 if durs else 0}), flush=True)
        elif line == "quit" or line == "": break
    if on:
        t0 = time.perf_counter()
        _t, _n, _b, err = c.query(STMT)
        if err is not None: sys.exit(f"gather: {err}")
        durs.append(time.perf_counter() - t0); n += 1
        if PACE: time.sleep(PACE)  # CY close diagnostic: GPACE seconds between gathers (default 0 = a tight loop)
