import sys, re, io, contextlib
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis")
from g import *
import host

SUMMARY = f"summary-scenarios-{DESC}.md"
MSUMMARY = f"summary-scenarios-{MD}.md"
BUDGET = "budget lifted by the operator for this measurement; PostgreSQL reused from the same-day run at v3.3.0-33-g9682b725"


def win(tags):
    s = [host.STARTS[t] for t in tags if t in host.STARTS]
    e = [host.ENDS[t][0] for t in tags if t in host.ENDS]
    if not s or not e:
        return "n/a", "n/a"
    return min(s)[11:19], max(e)[11:19]


def hostrows(tags, pt=None):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        host.hostrows(tags, (lambda t: pt.get(t, t)) if pt else (lambda t: t))
    return buf.getvalue()


def wtext(tags):
    a, b = win(tags)
    return f"{a} to {b} UTC"


COMMONROWS = f"""| Worktree / branch | `kwp-decoder-fast-path` / `kwp-decoder-fast-path` (the main checkout is not used) |
| HEAD | `a40882af`, `git describe --tags` = `{DESC}` ("kwp: decode a row batch in one offset loop and resolve each column's renderer once"); it changes `tools/kwp.py` and `tools/ckdbs_cli.py` and nothing else; the engine source is that of `9682b725` |
| Engine measured | the morning's binary copy, byte for byte: `/home/cdkbs/bench-runs/{DESC}/kds_server` was copied from `/home/cdkbs/bench-runs/{MD}/kds_server`, **sha256 `7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a`** (checked equal before the first cell and after the last), source binary mtime 2026-10-20 03:36:57.38 UTC; nothing was rebuilt |
| Client | this worktree's `tools/` (`kwp.py`, `ckdbs_cli.py` as of `a40882af`) drove every cell of the matrix. The A/B arm `old` ran `a40882af^`'s (`f829964c`) `tools/` from a separate directory, `bench-runs/{DESC}/oldtools/tools/`, put first on `PYTHONPATH`; the tree was not modified |
| Comparator | this engine's own morning run, `{MD}` (`bench/v3.3.0/results-scenario*-{MD}.md`): **same binary, old client**, 3 h earlier. PostgreSQL 18.6 is **reused** from that run; it was not re-run |
| Time budget | **Budget lifted by the operator for this measurement; PostgreSQL reused from the same-day run at v3.3.0-33-g9682b725.** Every KDS cell of the morning's plan ran with its repeat counts |
"""

PATHS = "Everything else (build type, host, device, server config, quiet-host gate, monitor, PostgreSQL setup) is unchanged from the morning and is stamped once in `" + SUMMARY + "`, section Stamp."
