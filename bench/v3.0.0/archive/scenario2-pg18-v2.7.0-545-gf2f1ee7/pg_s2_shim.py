#!/usr/bin/env python3
"""Harness shim around tools/pg_scenario2_freight.py (the driver file is not edited).

The driver's --verify path crashes after the measurement, in the shared
printer (tools/scenario2_freight.py print_bookings): pg_scenario2_freight.verify()
returns a (checks, failures, first) tuple, print_bookings reads v.checks, so
AttributeError is raised before --json is written. This shim replaces only
that print step, converting the tuple to the attribute object the printer
reads. Nothing that is measured, sent to the server or verified changes.
"""
import os
import sys
import types

TOOLS = "/home/cdkbs/ckdbs/.claude/worktrees/bench-pg-floor/tools"
sys.path.insert(0, TOOLS)
import pg_scenario2_freight as m

_orig = m.print_bookings


def _print_bookings(result, args):
    v = result.get("verify")
    if isinstance(v, tuple):
        checks, failures, first = v
        result = dict(result)
        result["verify"] = types.SimpleNamespace(
            checks=checks, failures=failures, first=first,
            unanswered=0, first_unanswered=None)
    return _orig(result, args)


m.print_bookings = _print_bookings
sys.argv[0] = os.path.join(TOOLS, "pg_scenario2_freight.py")
sys.exit(m.main())
