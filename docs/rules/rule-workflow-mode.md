# Workflow Mode — the automated task loop that wraps the base rules

**What this file is not**: a second rulebook, or a relaxation of anything
in `docs/rules/rules.md` or `CLAUDE.md`. Every Hard Invariant, every
coding rule, every step of the Session Workflow, Version Management, and
the worktree-and-commit-in-text convention apply exactly as written, with
no exception, to work done under workflow mode. This file describes an
**outer loop that wraps the existing rules** — where tasks come from and
how their outcome gets reported — never a set of exceptions to them.

## When it activates

**Explicit request only — never the default.** Absent that request, work
on ckdbs proceeds exactly as `CLAUDE.md` already describes: an
interactive session, no `cws` involvement, nothing about the Session
Workflow inferred as "probably fine to loop." Workflow mode starts only
when a user asks for it in those terms — "work the task queue", "run the
intermediary agent", "pick up pending tasks" (`intermediary-agent.md`'s
own trigger phrases) — or invokes it directly by name. A session that
has never been asked to run the loop is not in workflow mode, no matter
how automatable a task looks.

## What it is

Workflow mode is the process that drives ckdbs development from `cws`'s
external task/result queue instead of from an interactive session, using
two agents:

- **`intermediary-agent`** pulls one pending task from cws, works inside
  ckdbs on it, and reports a result back. It has no working style of its
  own inside ckdbs — it reads and follows `CLAUDE.md` and this project's
  own subagents (`critics-developer`, `ck-tester`) exactly as an
  interactive session would. Wrapping, not replacing: the loop decides
  *which task*, never *how ckdbs work gets done*.
- **`reporter-agent`** runs after each `intermediary-agent` iteration —
  a completed task, a failed one, or none pending — and syncs ckdbs's
  local state, **`docs/` included**, back to cws's issue/task records.
  It does no development work and makes no engineering decision.

`intermediary-agent` may also **direct `reporter-agent` to enqueue
follow-up subtasks** for a later iteration — when working a task
surfaces unexpected additional work or a measurement that the current
task did not scope for, that follow-up does not get done inline; it
becomes a subtask handed to `reporter-agent`, which is the only one of
the two that talks to cws about task state going forward in time. This
keeps the split intact: `intermediary-agent` decides *what future work
exists*, `reporter-agent` is the one that *tells cws about it*.

## Autonomy — research and decide, never ask

Workflow mode has no user to answer a clarifying question mid-loop —
`intermediary-agent` runs unattended toward a milestone, and a task
paused for input is a stalled milestone, not a paused one. So where an
interactive session would use `AskUserQuestion` or otherwise stop and
ask, workflow mode does neither, on any task, for any reason short of
the go-ahead gate below:

- **An `Open Decision` (`CLAUDE.md`'s own list) does not stop the task.**
  `CLAUDE.md` already states a non-blocking path for exactly this case —
  "stop and ask, **or** implement behind an interface that keeps every
  listed option viable" — and workflow mode always takes the second
  branch. Never the first: there is nobody to ask.
- **Genuine ambiguity gets researched, not guessed.** Read the owning
  spec, `docs/inflight/`, git history and existing tests before deciding
  anything non-obvious — the same standing rule that governs every other
  claim in this project ("measure, don't argue"). A decision reached
  this way is not a shortcut around research; it is research
  substituting for a conversation that cannot happen here.
- **A decision workflow mode had to make on its own is never silent.**
  Where neither an interface nor further research removes the fork —
  the task genuinely requires picking one road — `intermediary-agent`
  picks the one it can best defend, states the choice and the
  alternatives it passed over in the task's report to cws, and marks it
  as made without operator input so a human reviewing later can reverse
  it. Undocumented is not allowed; undecided is not an option workflow
  mode has.
- **This does not reach the go-ahead gate.** Deciding an open technical
  question autonomously is not the same act as pushing to `origin main`
  — a different kind of check, over who may take an irreversible,
  shared-repo action, and one this file cannot delegate to an agent no
  matter how the rest of the loop is scoped: **that push is the user's
  decision, always.** A task can be fully decided, built and tested, and
  still stop short of it; that is the one point in the loop still built
  to wait for a person, and it is a push gate, not a question.

## Documentation — where the reasoning has to live

A ~1.2 KB cws result field cannot hold why a decision was made or what a
measurement found — `intermediary-agent.md` already says as much ("put
anything longer in the target project's own tree... and reference it
rather than pasting it here"). Workflow mode makes that mandatory,
never optional, wherever one task's output feeds the next: **research,
a follow-up fix, or a revision that continues inside one milestone must
land its background, its reasoning and any measured metrics in the doc
that owns the subsystem** — `docs/spec/` if the thing is decided and
built, `docs/inflight/in-progress|blocked|bugs|verified/` if it is not
(`CLAUDE.md`'s own taxonomy) — never only in the cws report. The next
iteration that picks up where this one left off has no memory but what
is written there.

`reporter-agent`'s sync scope widens to match: beyond new issues and
milestone criteria, it confirms the doc trail a task claimed to leave
behind actually exists in `docs/` before treating that claim as synced,
and it points cws at the doc (file and section) rather than copying its
content into the server. A task reported `done` with no corresponding
doc update, where one was owed, is a reporting defect —
`reporter-agent` says so rather than passing the claim through
unchecked.

**Issue aliases reuse this project's own task-row ids, never a freshly
invented slug.** ckdbs already names its units of work — `SS1`, `SS5`,
`BM1`, `PW1c-6c`, `RC04`, and the like, scattered through `CLAUDE.md`'s
milestone table and `docs/inflight/` workplans. When `reporter-agent`
creates a cws issue (`POST {SERVER_URL}/issue/{project}/`) for something
a task surfaced, its `alias` is that id, so the cws-side record and the
doc-side record key off the same identifier. Where none exists yet
because the work is new, `intermediary-agent` mints one in the owning
doc first, in that doc's own lettering scheme, and the alias follows it.
**`CLAUDE.md` already warns these ids collide across documents** —
"three `P`-schemes, two `R1`s: cite the file, never the bare number" —
and an alias inherits that risk: where the bare id is ambiguous, qualify
it with the owning doc or scheme (e.g. `pw1c-6c`, not an unqualified
second `r1`) rather than let cws hold a collision it has no file to
disambiguate with.

## The loop, one iteration

1. `intermediary-agent` asks cws for the next pending task.
2. It works the task inside `CLAUDE.md`'s ordinary Session Workflow,
   **unmodified**: a worktree named for the work (§1) — carrying the cws
   task id is recommended, so `reporter-agent` can trace state back to
   it — `critics-developer` review per step (§2), `ck-tester` per feature
   (§3), sync-and-report at land time (§4).
3. **The go-ahead gate does not move.** `CLAUDE.md` §4 requires an
   explicit go-ahead before anything pushes to `origin main`; workflow
   mode grants none of its own authority, and cannot — that push stays
   the user's decision, not the loop's, regardless of how autonomously
   the rest of the task was decided. A task that reaches this gate
   reports `awaiting go-ahead` as its outcome — it does not push itself,
   and the loop does not block waiting for one: it moves on to the next
   pending task.
4. `intermediary-agent` reports the task's outcome — done, blocked,
   awaiting go-ahead, or no task found — back to the loop driver.
5. **If the task turned up unscoped work** — a fix that needs its own
   review cycle, a measurement `ck-tester` should run separately, a
   question that names an `Open Decision` — `intermediary-agent` does
   not absorb it into the current task. It writes what it found and why
   into the owning doc under `docs/` (per "Documentation" above), states
   the follow-up as one or more subtasks pointing at that doc, and hands
   them to `reporter-agent` along with the iteration's other output.
6. `reporter-agent` syncs the task outcome, checks that any doc update
   the task claimed is actually there, and enqueues the handed-off
   subtasks into cws, then the loop returns to step 1. A subtask waits
   its turn like any other pending task — it does not jump the queue or
   run inline in the iteration that raised it.

## What never changes

Identical to interactive use: Hard Invariants, `docs/rules/rules.md`,
the Session Workflow's four steps and their gates, the go-ahead
requirement before any push to `origin main`, Version Management, and the
worktree-and-commit-in-text convention. A task worked under workflow
mode that cannot pass `critics-developer` or `ck-tester` stops at
exactly the point an interactive session would stop, and says so in its
report to cws — it is never waved through because no human was
watching.

## Open

- **`reporter-agent.md`'s own instructions don't yet describe the wider
  doc-trail check** above — today it documents scanning
  `docs/inflight/known-gaps.md` for issues and a read-only milestone
  check, not a general "confirm the doc this task pointed at actually
  exists." Its tool set (`Read, Bash, Grep, Glob`) already covers what
  the check needs; the instructions naming it are what's missing.
- **The subtask-enqueue mechanism itself is not yet named.**
  `reporter-agent.md` documents `POST {SERVER_URL}/issue/{project}/` for
  issues and a read-only path for milestones — no `POST` for creating a
  new task is documented there today. Until one is, "hand off a subtask
  to `reporter-agent`" describes an intent this file states, not an API
  call either agent's current instructions can make; state that
  explicitly in a report rather than inventing an endpoint.
- **Who gives the go-ahead** for a task parked at `awaiting go-ahead` —
  an operator checking in periodically, or some other signal `cws`
  carries. Undecided; until it is, those tasks simply accumulate
  unmerged.
- **Worktree/branch naming for loop-driven work** — whether it needs a
  marker distinct from ordinary work (e.g. carrying the cws task id)
  beyond "name it for the work." Recommended above, not yet ratified as
  a rule.
- **How a task that names an `Open Decision`** in `CLAUDE.md` gets
  reported — presumably as any other blocked task, but not yet stated
  explicitly.
