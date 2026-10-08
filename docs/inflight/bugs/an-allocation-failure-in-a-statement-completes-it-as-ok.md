# An allocation failure inside a statement completes it as OK

**Found by reading, not reproduced.** Verified at `dc6baf3e` on
`worktree-bug-aggregate-bad-alloc`, from the question of what an aggregated
statement does past its memory: AG11's caps (`docs/spec/aggregate.md` §6)
count groups and DISTINCT entries, not bytes, and nothing handles an
allocation that fails before a cap is reached.

## What is wrong

Exceptions are enabled in the build (`CMakeLists.txt` adds `-Wall -Wextra`
and no `-fno-exceptions`), and there is no `catch` anywhere in `src/` or
`include/`. `docs/rules/rules.md` §1 forbids exceptions in engine logic, but
the standard library still throws `std::bad_alloc` from every container the
engine grows: the aggregator's `groups_`, `index_` and each DISTINCT set
(`src/exec/aggregate.cpp`, the founding of a group under the
`aggregate_max_groups` test and the `distinct->insert` under the
`aggregate_max_distinct` test) among them.

A statement on the default port runs inside `CommandDispatcher::DispatchAsync`
(`src/server/command_dispatcher.cpp:674`), a `sched::Coro`, which calls the
non-coroutine `DispatchAndStage` and assigns its outcome with
`*out = DispatchAndStage(line, session)`. A `bad_alloc` thrown below that
call unwinds to the coroutine body and reaches
`Coro::promise_type::unhandled_exception()` (`include/kds/sched/coro.hpp:131`),
which **does nothing**. Its comment expects a build with exceptions enabled
to "fail at the throw site"; the throw does not fail there, it unwinds and is
swallowed. The coroutine then reaches `final_suspend` as done, with its
`result` still the default `Status::OK()`, and `*out` was never assigned. A re-run after a lock or write-block wait
(`command_dispatcher.cpp:420`, `:670`) calls `DispatchAndStage` from a
child `Coro`, which swallows the same way; `*out` then keeps the first
run's refusal, so that path reports the wait's refusal, not success.

`TcpServer` (`src/server/tcp_server.cpp:586`, `:712`) passes `&conn.pending`
as `out`, and `conn.pending` is reset to `DispatchOutcome{}` after every
statement (`:737`, `:769`) - so the outcome handed to
`KwpSession::OnStatementComplete` is the default one: `status` OK and an
empty `response`, which `StatusFromErrorReply` also reads as OK
(`src/server/command_dispatcher.cpp:232`). The `debug_text_port` path
(`:770`) writes that empty `response` as a blank line, no `ERR`.

## Smallest reproduction (not run)

Start a server under `ulimit -v` low enough that a fold of fewer than
`aggregate_max_groups` groups cannot be held - wide `varchar(4096)` group
keys make that reachable at the default cap - and run
`SELECT k, COUNT(*) FROM t GROUP BY k` over at least that many distinct
`k`. Expected: an error. Predicted from the source: a successful completion
with no rows.

## What it costs

A **quiet wrong answer**. For an aggregated `SELECT`, `RunAggregated`
(`src/server/command_dispatcher.cpp:6457`) describes the sink before the
fold and emits rows only after it finishes, and the wire sink buffers rows
until `KwpSession::OnStatementComplete` (`src/server/kwp_session.cpp:743`)
sees it described and delivers. A throw during the fold therefore completes
as `SELECT` with zero rows - an empty result where groups exist; a throw
while emitting delivers the groups emitted so far as the whole answer.

The same path covers every statement, not only aggregation: any `bad_alloc`
below the first `DispatchAndStage` is reported to the client as success - a
truncated unaggregated `SELECT`, or `S_TXN_OK` (`kwp_session.cpp:681`) for
a `COMMIT` that threw. What a write
leaves behind when it is interrupted mid-mutation - a page written and its
record not appended, a hold released by unwinding mid-protocol - was not
traced, and is the larger risk; this entry claims only the reporting.

Reachability is bounded by the host: under Linux's default overcommit an
oversized fold is more likely to be ended by the OOM killer than to see
`bad_alloc`. With `vm.overcommit_memory=2`, a cgroup memory limit, or an
`RLIMIT_AS`, `bad_alloc` is what arrives.

## The fix is not decided

Either `unhandled_exception()` records a failure (`std::terminate`, or a
non-OK `result` the caller turns into a refusal - `TcpServer`'s `on_done`
ignores the `Status` today, `tcp_server.cpp:587`, `:713`), or allocation failure is
made a `Status` at the sites that grow per-statement state. The first fixes
the reporting for every statement; whether a statement interrupted
mid-mutation may continue at all is the question the second has to answer.
No work order carries it.
