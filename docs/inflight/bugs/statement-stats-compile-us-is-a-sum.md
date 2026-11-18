# `sys.statement_stats.compile_us` is a sum, and the spec says it is the last run's

**The wrong behaviour.** `docs/spec/sys-views.md` §6.1 says of the cost
columns that "`rows`, the chain's counts and `compile_us` are its last
run's". The code sums `compile_us` over every call that compiled a chain:
`src/stats/statement_stats.cpp` folds `shape.compile_ns += cost.compile_ns`,
and `src/server/sys_views.cpp` prints `Us(s.compile_ns)`. So the column is
the shape's total compile time in microseconds, growing with `calls`. The
spec and the code disagree; which one states the contract DF meant is DF's
to say (`instructions/v3.2.0/workorder-df-statistics-coverage.md`, DF-S1).

**Verified at** `9712f5f0` on worktree `cx-plan-change-option`, found by
CX-S6's measurement (`bench/v3.4.12/results-cx-s6-compile-share-v3.4.12-112-g9712f5f0.md`
§2): a fresh server's point read showed `compile_us` = 4,141 after about a
thousand calls, which is the sum of a 3 to 4 µs compile, not one compile.

**The smallest reproduction.** On a fresh server, run
`SELECT * FROM t WHERE id = 1` five times and read `SELECT calls, compile_us
FROM sys.statement_stats`: `compile_us` is about five compiles' time, and a
sixth call raises it again.

**What it costs.** No refusal and no wrong answer to a statement: a reading
of the view. A DBA or an agent who takes the spec's word reads a total as
one compile and finds compiles three orders of magnitude slower than they
are. As a sum the column is the better instrument - `delta(compile_us) /
delta(calls)` is the mean - but no per-call distribution can be derived
from it. The fix is one sentence of the spec or one line of the fold, and
it belongs to DF-S2 or after, whichever DF rules.
