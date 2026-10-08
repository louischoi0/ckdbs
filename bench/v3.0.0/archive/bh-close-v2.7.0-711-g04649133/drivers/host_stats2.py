import glob, json, sys
A = sys.argv[1]
for f in sorted(glob.glob(A + "/**/*.json", recursive=True)):
    try:
        j = json.load(open(f))
    except Exception:
        continue
    if not isinstance(j, dict) or "host_before" not in j:
        continue
    for k in ("host_before", "host_after"):
        for c in j[k]["competing"]:
            if "ctest" in c and "shell-snapshots" not in c:
                print(f.replace(A, ""), k, j[k]["loadavg"], c[:80])
