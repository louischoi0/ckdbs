"""`sys` view replies read back as Python values; `sys.meta` as {field: value}.

Since BO-S2 every `SHOW` is a `SELECT` over one `sys` view
(`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md` SV1), and
`SHOW META` is `SELECT * FROM sys.meta`: one row per field,
`(section, field, value)`, every value text. Over the newline protocol a reply
is a header line of column names and one line per row, comma-joined, the
lines joined by the two-character "\\n" escape (or real newlines, once a
caller has passed it through `ckdbs_cli.format_reply`); over KWP it is a
typed result set.

The text surface cannot say where a value holding a comma ends. A `sys.meta`
value may hold one (`ddl_transactional`'s list) while a section and a field
never do, so a meta row is split at its first two; any other view keeps a
row's surplus in its last column, so a caller projects such a column last.
"""


HEADER = "section,field,value"


def _lines(reply):
    if reply.startswith("ERR"):
        raise RuntimeError("a view read was refused: %s" % reply)
    return reply.split("\\n") if "\\n" in reply else reply.splitlines()


def _tokens(text):
    """The `key=value` tokens of an engine before BO-S2, whose `SHOW META`
    was one line of them - which an A/B run's baseline binary still is."""
    out = {}
    for tok in text.replace("\\n", " ").split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def from_text(reply):
    """A newline-protocol `SHOW META` reply as {field: value}, from either
    engine."""
    lines = _lines(reply)
    if lines[0] != HEADER:
        return _tokens(reply)
    out = {}
    for line in lines[1:]:
        parts = line.split(",", 2)
        if len(parts) == 3:
            out[parts[1]] = parts[2]
    return out


def from_rows(rows, tag=""):
    """A KWP `SHOW META` answer as {field: value}, from either engine: the
    three-column (section, field, value) result set, or a pre-BO-S2 engine's
    one-line tag (or one-column text rows) of `key=value` tokens."""
    if rows and len(rows[0]) == 3:
        return {r[1].decode(): (r[2] or b"").decode() for r in rows}
    return _tokens(tag + " " + " ".join(b" ".join(c or b"" for c in r).decode() for r in rows))


def view_rows(reply):
    """A newline-protocol reply of any `sys` view as a list of
    {column: value} dicts, NULL as the text `NULL`."""
    lines = _lines(reply)
    columns = lines[0].split(",")
    return [dict(zip(columns, line.split(",", len(columns) - 1)))
            for line in lines[1:] if line]


def cabin_optimizer_state(call):
    """The Cabin controller through `call` (a callable sending one statement
    and returning its text reply): (header, entries). `header` is its
    `sys.meta` fields with the `cabin_optimizer_` prefix dropped (`managed`,
    `pages_committed`, `page_budget`, `ticks`, `creates`, `drops`, ...);
    `entries` is `sys.cabin_optimizer`'s rows, each also keyed `rel`."""
    reply = call("SHOW CABIN_OPTIMIZER")
    entries = [dict(row, rel=row["rel_name"]) for row in view_rows(reply)]
    prefix = "cabin_optimizer_"
    header = {k[len(prefix):]: v for k, v in from_text(call("SHOW META")).items()
              if k.startswith(prefix)}
    return header, entries


def numeric(fields, kind=int):
    """The fields whose value parses as `kind`, converted; the rest dropped."""
    out = {}
    for k, v in fields.items():
        try:
            out[k] = kind(v)
        except ValueError:
            pass
    return out
