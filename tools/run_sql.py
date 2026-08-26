#!/usr/bin/env python3
"""Runs a .sql script against a ckdbs server, with optional inline
expectations - so an ad-hoc query and the answer it should give live on
adjacent lines and neither needs a code change to add.

    tools/run_sql.py adhoc/subqueries.sql
    tools/run_sql.py adhoc/*.sql --port 15432
    tools/run_sql.py adhoc/subqueries.sql --quiet     only failures

Script format. One statement per line - the wire protocol is one line in,
one line out (docs/spec/client-manual.md), so a statement cannot span lines
here either. Blank lines are skipped. A line starting with `--` is a
comment, and four spellings of comment are directives that apply to the
**next statement**:

    -- expect: <text>         the reply must contain <text>
    -- reject: <text>         the reply must NOT contain <text>
    -- error: <text>          the reply must be an ERR containing <text>
    -- rows: <n>              the reply must hold exactly <n> data rows

Several may stack on one statement. A statement with no directive is run
for its effect and its reply is printed - which is the ad-hoc case: paste a
query, look at the answer, add an expectation once you know what it should
be.

    -- rows: is worth the extra directive because the failures that matter
    in a query engine are wrong *answers*, and the most common wrong answer
    is the right values with the wrong multiplicity - a join that pairs a
    row twice, or a subquery predicate that filters nothing. Neither shows
    up as a missing substring.

Exit status is 0 when every expectation held, 1 otherwise. A script with
no expectations at all always exits 0; it is a transcript, not a test.
"""

import argparse
import glob
import sys

from ckdbs_cli import DEFAULT_HOST, DEFAULT_PORT, ServerConnection, format_reply


class Directive:
    """The expectations attached to one statement."""

    def __init__(self):
        self.expect = []
        self.reject = []
        self.error = []
        self.rows = None

    def empty(self):
        return not (self.expect or self.reject or self.error or self.rows is not None)

    def check(self, reply):
        """Returns a list of failure reasons, empty when everything held."""
        problems = []
        is_err = reply.startswith("ERR")

        if self.error:
            if not is_err:
                problems.append(f"expected an error, got a result: {reply!r}")
            for text in self.error:
                if text not in reply:
                    problems.append(f"error does not mention {text!r}")
        elif (self.expect or self.reject or self.rows is not None) and is_err:
            problems.append(f"expected a result, got: {reply}")

        for text in self.expect:
            if text not in reply:
                problems.append(f"missing {text!r}")
        for text in self.reject:
            if text in reply:
                problems.append(f"contains {text!r}, which must not be there")

        if self.rows is not None and not is_err:
            # A SELECT reply is a header line then one "\n"-escaped
            # section per row, so the row count is sections minus the
            # header. A non-SELECT reply has no header and no rows.
            sections = format_reply(reply).split("\n")
            actual = max(0, len([s for s in sections if s != ""]) - 1)
            if actual != self.rows:
                problems.append(f"expected {self.rows} row(s), got {actual}")
        return problems


def parse_script(text):
    """Yields (heading, statement, directive) in file order.

    `heading` carries any plain comment lines seen since the last
    statement, so a script reads as prose with queries in it.
    """
    heading = []
    directive = Directive()

    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("--"):
            body = line[2:].strip()
            for name, target in (("expect:", directive.expect), ("reject:", directive.reject),
                                 ("error:", directive.error)):
                if body.lower().startswith(name):
                    target.append(body[len(name):].strip())
                    break
            else:
                if body.lower().startswith("rows:"):
                    directive.rows = int(body[len("rows:"):].strip())
                else:
                    heading.append(body)
            continue

        yield ("\n".join(heading), line, directive)
        heading = []
        directive = Directive()


def run_file(conn, path, quiet):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()

    print(f"\n{'=' * 72}\n{path}\n{'=' * 72}")
    checked = failed = 0

    for heading, statement, directive in parse_script(text):
        reply = format_reply(conn.send_command(statement))
        problems = directive.check(reply)
        if problems:
            failed += 1
        if not directive.empty():
            checked += 1

        # A passing expectation prints one line in quiet mode; anything
        # unexpected, or anything with no expectation, prints in full -
        # the ad-hoc case is exactly "I do not know the answer yet".
        if quiet and not problems and not directive.empty():
            continue

        if heading:
            print()
            for line in heading.splitlines():
                print(f"# {line}")
        print(f"  {statement}")
        for line in reply.split("\n"):
            print(f"  -> {line}")
        for problem in problems:
            print(f"  !! {problem}")

    return checked, failed


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scripts", nargs="+", help=".sql files, run in the order given")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"default: {DEFAULT_HOST}")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"default: {DEFAULT_PORT}")
    parser.add_argument("--quiet", action="store_true",
                        help="print only statements that failed or carry no expectation")
    args = parser.parse_args()

    paths = []
    for pattern in args.scripts:
        matched = sorted(glob.glob(pattern))
        paths.extend(matched if matched else [pattern])

    try:
        conn = ServerConnection(args.host, args.port)
    except OSError as e:
        print(f"could not connect to {args.host}:{args.port}: {e}", file=sys.stderr)
        print("or run: adhoc/run.sh, which starts a throwaway server", file=sys.stderr)
        sys.exit(1)

    total_checked = total_failed = 0
    try:
        for path in paths:
            try:
                checked, failed = run_file(conn, path, args.quiet)
            except FileNotFoundError:
                print(f"no such script: {path}", file=sys.stderr)
                sys.exit(1)
            total_checked += checked
            total_failed += failed
    finally:
        conn.close()

    print(f"\n{'=' * 72}")
    if total_failed:
        print(f"{total_failed} of {total_checked} checked statement(s) did not match.")
        sys.exit(1)
    if total_checked == 0:
        print("no expectations in these scripts - ran as a transcript.")
    else:
        print(f"all {total_checked} checked statement(s) matched.")
    sys.exit(0)


if __name__ == "__main__":
    main()
