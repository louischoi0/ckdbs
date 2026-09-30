q = lambda x: round(1e6 / x)
rows = [("200", "insert-assert-existing", 52.0, 52.9), ("200", "insert-assert-newgroup", 52.8, 53.5), ("200", "insert-none-existing", 49.7, 50.0), ("200", "insert-none-newgroup", 49.8, 49.8), ("200", "insert-none-again", 49.7, 49.9), ("200", "select", 64.7, 64.4), ("200", "ping", 38.0, 38.0),
        ("1000", "insert-assert-existing", 52.2, 52.8), ("1000", "insert-assert-newgroup", 52.5, 53.1), ("1000", "insert-none-existing", 49.7, 49.9), ("1000", "insert-none-newgroup", 49.6, 50.0), ("1000", "insert-none-again", 49.8, 50.1), ("1000", "select", 64.4, 65.3), ("1000", "ping", 38.0, 38.1),
        ("10000", "insert-assert-existing", 53.0, 53.0), ("10000", "insert-assert-newgroup", 52.9, 53.0), ("10000", "insert-none-existing", 49.9, 49.9), ("10000", "insert-none-newgroup", 49.8, 49.9), ("10000", "insert-none-again", 49.8, 49.9), ("10000", "select", 63.2, 63.6), ("10000", "ping", 38.3, 38.2),
        ("c2", "fk", 51.0, 51.3), ("c2", "fk-txn", 110.8, 110.8), ("c2", "plain", 49.7, 49.7), ("c2", "pupd", 50.7, 50.4), ("c2", "cupd", 51.0, 50.6), ("c2", "again", 50.6, 50.9), ("c2", "select", 64.2, 64.4), ("c2", "ping", 38.3, 38.2),
        ("c3", "K64 fk", 54.1, 54.2), ("c3", "K1024 fk", 53.0, 53.1), ("c3", "K4096 fk", 54.1, 54.0), ("c3", "K16384 fk", 60.5, 61.2)]
for n, a, b, x in rows:
    print(n, a, f"{q(b)} / {q(x)}")
