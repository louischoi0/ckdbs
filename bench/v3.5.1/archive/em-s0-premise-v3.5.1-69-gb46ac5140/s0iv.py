"""EM-S0 (iv): does a prepared and a text statement with leading whitespace name
the same refusal byte? One server; each statement sent as text and as a named
statement (EA's kept parse), with two leading spaces and without."""
import sys, os
sys.path.insert(0, sys.argv[1])
import kwp  # noqa: E402
from dk_close import Server  # noqa: E402

srv = Server(sys.argv[2], sys.argv[3], "em-s0-iv", int(sys.argv[4]), 1, [2])
try:
    c = srv.conn()
    c.execute("CREATE TABLE t (id int64, v int64, w int64) BTREE")
    cases = [
        "SELECT * FROM t WHERE nosuch = 1",
        "SELECT nosuch FROM t WHERE id = 1",
        "UPDATE t SET nosuch = 1 WHERE id = 1",
        "UPDATE t SET v = 'x' WHERE id = 1",
        "DELETE FROM nosuch WHERE id = 1",
        "INSERT INTO t VALUES (1, 'x', 3)",
        "SELECT * FROM t WHERE v = 'x'",
    ]
    n = 0
    for base in cases:
        for lead in ("", "  "):
            sql = lead + base
            out = {}
            try:
                c.execute(sql)
                out["text"] = "ok"
            except kwp.KwpError as e:
                out["text"] = str(e)
            n += 1
            try:
                c.prepare("s%d" % n, sql)
                c.execute_prepared("s%d" % n, [])
                out["named"] = "ok"
            except kwp.KwpError as e:
                out["named"] = str(e)
            same = out["text"] == out["named"]
            print(("SAME " if same else "DIFF ") + repr(sql))
            if not same:
                print("   text :", out["text"])
                print("   named:", out["named"])
    c.close()
finally:
    srv.stop()
