#!/usr/bin/env python3
"""Simple CLI client for the ckdbs server's plain-text TCP protocol.

The server (src/server, ./build/kds_server) speaks one command per line,
newline-terminated, and always replies with exactly one line back. This
is a thin client for that protocol - it does no parsing/validation of its
own for most commands, it just ships whatever you type and prints back
whatever the server says (see format_reply()). SELECT is the one
exception: its reply is rendered dataframe-style (a real pandas
DataFrame when pandas is installed, a column-aligned text table
otherwise) instead of the raw "header line + \\n-escaped comma rows" text
- see render_select_reply(). The wire protocol itself is unchanged either
way; this is purely a client-side display choice.

Usage:
    ckdbs_cli.py                        interactive REPL
    ckdbs_cli.py PING                    one-shot: send "PING", print reply, exit
    ckdbs_cli.py SHOW META
    ckdbs_cli.py SHOW TABLES
    ckdbs_cli.py SHOW PAGE 128
    ckdbs_cli.py SHOW PAGE 128 VALUES
    ckdbs_cli.py DESCRIBE accounts
    ckdbs_cli.py --host 127.0.0.1 --port 15432 SHOW TABLES

    # Full SQL grammar (src/parser) - quote as one shell argument:
    ckdbs_cli.py "CREATE TABLE accounts (id int64, name varchar, balance int64)"
    ckdbs_cli.py "INSERT INTO accounts VALUES ('alice', 100)"
    ckdbs_cli.py "SELECT * FROM accounts WHERE id = 1"
    ckdbs_cli.py "UPDATE accounts SET balance = 150 WHERE id = 1"

    # Run a local .sql file, results on stdout (so it pipes and redirects):
    ckdbs_cli.py -f schema.sql
    ckdbs_cli.py -f schema.sql -f load.sql -f report.sql > out.txt
    ckdbs_cli.py -f queries.sql --echo         also print each statement
    cat report.sql | ckdbs_cli.py -f -         read the script from stdin

Script files. Statements are separated by `;`, and one may span several
lines - it is flattened to a single line before it goes out, because the
wire protocol is one line in / one line out. A file containing no `;` at
all is read as **one statement per line** instead, which is the convention
adhoc/*.sql already follows. `--` starts a comment to end of line, except
inside a quoted string. Only replies are printed; the exit status is 1 if
any statement came back `ERR`.

For a script with inline *expectations* - "this query must return 3 rows" -
use tools/run_sql.py instead. This runs a script; that one checks one.

REPL-only local commands (never sent to the server):
    help / ?     list known server commands
    exit / quit  close the connection and exit (does NOT stop the server -
                 use the server's own STOP command for that)

Line editing in the REPL, via the stdlib readline module:
    up / down    walk back and forward through the command history
    left/right   move the cursor within the line; ctrl-a and ctrl-e jump to
                 its start and end, alt-b and alt-f move by word
    ctrl-r       search backwards through history incrementally
    ctrl-c       abandon the line being edited without leaving the REPL
    ctrl-d       leave the REPL (on an empty line)

History is kept in ~/.ckdbs_history, capped at 1000 entries and shared
across sessions, so a long SELECT typed yesterday is one ctrl-r away.
Consecutive duplicates are not recorded twice. On a platform without
readline the REPL still works, just without any of the above.
"""

import argparse
import atexit
import os
import socket
import sys

# Importing readline is the whole line-editing feature: it hooks input(),
# which is what gives the REPL up/down through history, left/right and
# ctrl-a/e to move within a line, and ctrl-r to search. Nothing below calls
# into it for that - the import is the mechanism, which is why it is not
# behind a "if interactive" guard.
#
# Guarded anyway because it is a POSIX module: on Windows it is absent
# unless pyreadline3 is installed, and a CLI that cannot start there is
# worse than one that starts without arrow keys.
try:
    import readline
except ImportError:  # pragma: no cover - Windows without pyreadline3
    readline = None

# History persists across sessions: a REPL that forgets what you typed the
# moment you leave is a REPL you retype long SELECTs into. Kept in the home
# directory rather than beside the data file, because it belongs to the
# person, not to the database.
HISTORY_FILE = os.path.join(os.path.expanduser("~"), ".ckdbs_history")
HISTORY_LIMIT = 1000

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 15432

# Kept here only to print a hint in `help` - the server is the source of
# truth for what it actually accepts; this list can drift if commands are
# added there without updating this comment.
KNOWN_COMMANDS = """\
  PING                    -> PONG
  SHOW META               -> superblock stats
  SHOW TABLES             -> space-separated table names
  SHOW PAGE <page_id> [VALUES]
                          -> heap page header + slot directory, pretty-printed;
                             VALUES also hex-encodes each live tuple's payload
  DESCRIBE <name>         -> table header + one section per column (DESC works too)
  CREATE TABLE <name>     -> always ERR: a table needs a Keystone pk column;
                             bare form, no columns
  CREATE TABLE <name> (<col> <type> [, ...]) [HEAP|BTREE]
                          -> same CREATED/EXISTS reply, real columns (SQL form)
  INSERT INTO <name> VALUES (<val> [, ...])
                          -> "INSERTED oid=<n> slot=<n>" or ERR ...
  SELECT * FROM <name> [WHERE <cond> [AND <cond>]*]
                          -> header line + one row per match
  UPDATE <name> SET <col> = <val> [, ...] [WHERE <cond> [AND <cond>]*]
                          -> "UPDATED <n>" or ERR ...
  SYNC                    -> persists everything written so far
  STOP                    -> shuts the whole server down (not just this client)
"""


class ServerConnection:
    """One TCP connection to the ckdbs server, one line in / one line out."""

    def __init__(self, host, port, timeout=5.0):
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._buf = b""

    def send_command(self, line):
        self._sock.sendall(line.encode("utf-8") + b"\n")
        return self._read_line()

    def _read_line(self):
        while b"\n" not in self._buf:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode("utf-8", errors="replace")

    def close(self):
        self._sock.close()


def format_reply(reply):
    """Renders a reply for display.

    The wire protocol allows exactly one line back per command (see
    docs/spec/client-manual.md section 2) - a raw newline byte in a reply would
    desync this client's "read up to the next \\n" framing. Commands that
    want a readable multi-line dump (e.g. SHOW PAGE) instead join sections
    with the literal two-character escape "\\n", which is unescaped here
    into a real newline purely for display; nothing is sent back over the
    wire in this form.
    """
    return reply.replace("\\n", "\n")


# ---- SELECT rendering (dataframe-style) -----------------------------------
#
# Only SELECT gets client-side treatment; every other command's reply is
# printed exactly as format_reply() renders it, unchanged. The wire
# protocol itself is not touched either way.

def _is_select(command):
    words = command.strip().split()
    return bool(words) and words[0].upper() == "SELECT"


def _coerce(text):
    """Turns a cell into an int when it parses as one, else leaves it as
    text - a SELECT reply has no type tags, only comma-separated text."""
    try:
        return int(text)
    except ValueError:
        return text


def _parse_select_rows(text):
    # Naive comma-split: a varchar value containing a comma would be
    # mis-split. The wire format has no quoting/escaping for this - a
    # real fix means changing the reply format, out of scope here.
    lines = [line for line in text.split("\n") if line != ""]
    if not lines:
        return [], []
    columns = lines[0].split(",")
    rows = [[_coerce(v) for v in line.split(",")] for line in lines[1:]]
    return columns, rows


def to_dataframe(columns, rows):
    """Builds a pandas DataFrame from parsed SELECT columns/rows. pandas
    is not a hard dependency of this file (see the module docstring) -
    imported lazily here, only when a caller wants a DataFrame. Raises
    RuntimeError if pandas isn't installed.
    """
    try:
        import pandas as pd
    except ImportError as e:
        raise RuntimeError("pandas is required for to_dataframe() (pip install pandas)") from e
    return pd.DataFrame(rows, columns=columns)


def _text_table(columns, rows):
    """Column-aligned text table - the fallback when pandas isn't
    installed, so SELECT output still reads like a dataframe printout
    rather than raw comma-joined text.

    Tolerates ragged rows: the wire format has no quoting (see
    _parse_select_rows), so a varchar holding a comma splits into extra
    cells and one holding the two-character "\\n" splits into a short row.
    The true cell boundaries are unrecoverable client-side; extra cells
    render under an unnamed header and short rows pad with blanks, so the
    reply is shown as it arrived instead of crashing the REPL.
    """
    str_rows = [[str(v) for v in row] for row in rows]
    n_cols = max([len(columns)] + [len(r) for r in str_rows])
    headers = list(columns) + [""] * (n_cols - len(columns))
    widths = [len(c) for c in headers]
    for row in str_rows:
        for i, v in enumerate(row):
            widths[i] = max(widths[i], len(v))

    def fmt(values):
        padded = values + [""] * (n_cols - len(values))
        return "  ".join(v.rjust(widths[i]) for i, v in enumerate(padded))

    lines = [fmt(headers), "  ".join("-" * w for w in widths)]
    lines.extend(fmt(row) for row in str_rows)
    return "\n".join(lines)


def render_select_reply(raw_reply):
    """Renders a SELECT reply dataframe-style: a real pandas DataFrame
    when pandas is installed, a column-aligned text table otherwise. An
    "ERR ..." reply passes through format_reply() unchanged - there is no
    table to build from an error.
    """
    text = format_reply(raw_reply)
    if text.startswith("ERR"):
        return text

    columns, rows = _parse_select_rows(text)
    if any(len(row) != len(columns) for row in rows):
        # Ragged reply: some varchar held a comma (or the two-character
        # "\n") and mis-split - the wire format cannot say where the cell
        # boundaries were. pandas refuses ragged input outright, so these
        # always render through the tolerant text table.
        return _text_table(columns, rows)
    try:
        return to_dataframe(columns, rows).to_string(index=False)
    except RuntimeError:
        return _text_table(columns, rows)


def _print_response(command, raw_reply):
    if _is_select(command):
        print(render_select_reply(raw_reply))
    else:
        print(format_reply(raw_reply))


def run_one_shot(conn, command):
    _print_response(command, conn.send_command(command))


# ---- Running a local .sql file --------------------------------------------
#
# The point of this path is that its stdout is *only* replies: no banners,
# no per-statement decoration unless asked for, so `-f report.sql > out.txt`
# gives a file worth diffing and `| grep` works. Anything about the run
# itself (a missing file, how many statements failed) goes to stderr, which
# is what keeps the two streams separable.

def split_sql_statements(text):
    """Splits script text into statements ready to put on the wire.

    Statements are separated by `;` and may span lines - each is flattened
    to one line, since the protocol frames on newlines (see the module
    docstring). `--` begins a comment to end of line, and a `;` or `--`
    inside a single-quoted string is neither a separator nor a comment;
    SQL's doubled `''` escape is honoured.

    A script with no `;` anywhere falls back to one statement per line.
    That is not a guess about intent: it is the format adhoc/*.sql and
    tools/run_sql.py already use, and reading such a file as a single
    enormous statement would fail in a way that explains nothing.
    """
    out = []
    current = []
    in_string = False
    saw_separator = False
    i = 0

    while i < len(text):
        ch = text[i]

        if in_string:
            current.append(ch)
            if ch == "'":
                # A doubled quote is an escaped quote, not the end.
                if i + 1 < len(text) and text[i + 1] == "'":
                    current.append("'")
                    i += 2
                    continue
                in_string = False
            i += 1
            continue

        if ch == "'":
            in_string = True
            current.append(ch)
            i += 1
            continue

        if ch == "-" and text.startswith("--", i):
            end = text.find("\n", i)
            i = len(text) if end == -1 else end
            continue

        if ch == ";":
            saw_separator = True
            out.append("".join(current))
            current = []
            i += 1
            continue

        current.append(ch)
        i += 1

    out.append("".join(current))

    if not saw_separator:
        # One statement per line, comments already stripped above.
        lines = []
        for chunk in out:
            lines.extend(chunk.splitlines())
        return [line.strip() for line in lines if line.strip()]

    return [" ".join(chunk.split()) for chunk in out if chunk.strip()]


def _read_script(path):
    if path == "-":
        return sys.stdin.read()
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def run_script(conn, path, echo=False):
    """Runs every statement in `path`, printing replies to stdout.

    Returns the number of statements that replied `ERR`. A failing
    statement does not stop the run - a script is usually a sequence whose
    later statements are still worth seeing, and stopping would hide them.
    """
    statements = split_sql_statements(_read_script(path))
    failures = 0

    for statement in statements:
        if echo:
            print(f"-- {statement}")
        reply = conn.send_command(statement)
        if reply.startswith("ERR"):
            failures += 1
        _print_response(statement, reply)

    return failures


def _init_history():
    """Loads the saved history and arranges for it to be saved on exit.

    Registered with atexit rather than saved in the REPL's finally block so
    it also survives the paths that do not return through it - an
    unhandled exception, or the STOP command taking the server down under
    us. History you lose on the one session that ended badly is history you
    stop trusting.
    """
    if readline is None:
        return
    try:
        readline.read_history_file(HISTORY_FILE)
    except FileNotFoundError:
        pass  # first run
    except OSError:
        pass  # unreadable (permissions, a directory in its place) - not fatal

    readline.set_history_length(HISTORY_LIMIT)

    def save():
        try:
            readline.write_history_file(HISTORY_FILE)
        except OSError:
            pass  # a read-only home is not a reason to fail on the way out

    atexit.register(save)


def _drop_duplicate_history_entry():
    """Removes the line just entered when it repeats the one before it.

    readline appends every line unconditionally, so pressing enter on the
    same statement three times puts three copies between you and the thing
    you were actually looking for. Consecutive duplicates only - a command
    repeated later in the session is a real thing to scroll back to.
    """
    if readline is None:
        return
    n = readline.get_current_history_length()
    if n < 2 or readline.get_history_item(n) != readline.get_history_item(n - 1):
        return
    # The two APIs disagree about indexing on purpose-of-nobody's:
    # get_history_item is 1-based, remove_history_item is 0-based. So the
    # last entry is n for the getter and n - 1 for the remover.
    readline.remove_history_item(n - 1)


def run_repl(conn):
    _init_history()
    editing = "" if readline is not None else \
        " (no line editing: the readline module is unavailable)"
    print(f"ckdbs interactive CLI. Type 'help' for known commands, 'exit' to quit.{editing}")

    while True:
        try:
            line = input("ckdbs> ")
        except EOFError:
            print()
            break
        except KeyboardInterrupt:
            # Ctrl-C abandons the line being edited, it does not end the
            # session - which is what every other SQL shell does, and what
            # anyone who has just learned the line editor will expect the
            # first time they mistype a long statement.
            print("^C")
            continue

        _drop_duplicate_history_entry()

        stripped = line.strip()
        if not stripped:
            continue
        if stripped.lower() in ("exit", "quit"):
            break
        if stripped.lower() in ("help", "?"):
            print(KNOWN_COMMANDS, end="")
            continue

        try:
            _print_response(stripped, conn.send_command(stripped))
        except ConnectionError as e:
            print(f"connection lost: {e}")
            break

        if stripped.strip().upper() == "STOP":
            # The server just shut itself down - nothing more to send.
            break


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"default: {DEFAULT_HOST}")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"default: {DEFAULT_PORT}")
    parser.add_argument("-f", "--file", action="append", default=[], metavar="PATH",
                         help="run a local .sql file and print replies to stdout; "
                              "repeatable, files run in the order given; '-' reads stdin")
    parser.add_argument("--echo", action="store_true",
                         help="with -f, also print each statement before its reply")
    parser.add_argument("command", nargs="*",
                         help="command to send (e.g. PING, DESCRIBE accounts); "
                              "omit for an interactive REPL")
    args = parser.parse_args()

    if args.file and args.command:
        parser.error("give either -f/--file or a command, not both")
    if args.echo and not args.file:
        parser.error("--echo only applies with -f/--file")

    try:
        conn = ServerConnection(args.host, args.port)
    except OSError as e:
        print(f"could not connect to {args.host}:{args.port}: {e}", file=sys.stderr)
        sys.exit(1)

    failures = 0
    try:
        if args.file:
            for path in args.file:
                try:
                    failures += run_script(conn, path, args.echo)
                except OSError as e:
                    # stderr, not stdout: a missing file is not a result,
                    # and a redirected stdout must not collect it.
                    print(f"could not read {path}: {e}", file=sys.stderr)
                    sys.exit(1)
        elif args.command:
            run_one_shot(conn, " ".join(args.command))
        else:
            run_repl(conn)
    finally:
        conn.close()

    if failures:
        print(f"{failures} statement(s) replied ERR", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
