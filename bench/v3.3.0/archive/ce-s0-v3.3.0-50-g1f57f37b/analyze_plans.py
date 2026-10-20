"""ANALYZE of the row-cost sweep's statements (pages and rows examined, to turn slices into counts).
usage: analyze_plans.py PORT OUT"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools"))
import kwp
sql = {
    "range-1": "SELECT * FROM daily_stats WHERE id BETWEEN 1 AND 1",
    "range-100": "SELECT * FROM daily_stats WHERE id BETWEEN 1 AND 100",
    "range-1000": "SELECT * FROM daily_stats WHERE id BETWEEN 1 AND 1000",
    "range-10000": "SELECT * FROM daily_stats WHERE id BETWEEN 1 AND 10000",
    "range-60480": "SELECT * FROM daily_stats WHERE id BETWEEN 1 AND 60480",
    "filter-sym3": "SELECT * FROM daily_stats WHERE symbol_id = 3",
}
c = kwp.Connection("127.0.0.1", int(sys.argv[1]), timeout=120)
with open(sys.argv[2], "w") as f:
    for label, s in sql.items():
        c.execute(s)
        c.execute(s)
        fields, rows, tag, aff = c.execute("ANALYZE " + s)
        f.write(f"=== {label}: {s}\n")
        for r in rows:
            f.write("  " + " | ".join(x.decode("utf-8", "replace") if x is not None else "NULL" for x in r) + "\n")
