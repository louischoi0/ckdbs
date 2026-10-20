def table_rows(doc, header_start):
    i = doc.s.index(header_start)
    k = doc.s.index("\n", i) + 1
    k = doc.s.index("\n", k) + 1
    j = k
    while doc.s[j:j + 1] == "|":
        j = doc.s.index("\n", j) + 1
    return k, j


def map_rows(doc, header_start, fn):
    k, j = table_rows(doc, header_start)
    out = []
    for l in doc.s[k:j].splitlines():
        cells = [c.strip() for c in l.strip().strip("|").split("|")]
        cells = fn(cells)
        out.append("| " + " | ".join(cells) + " |")
    doc.s = doc.s[:k] + "\n".join(out) + "\n" + doc.s[j:]
