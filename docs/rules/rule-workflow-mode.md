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
how automatable a task looks. Once active, it plans before it builds —
see "One task source" below.

## One task source — the milestone. Two ways to plan into it.

**A run is scoped to exactly one milestone, and the loop never fetches
outside it.** That is the whole task-source rule: `intermediary-agent`'s
first iteration settles a milestone and registers the run's tasks under
it, and from the second iteration on every fetch carries that
`milestone_id`. A pending task under a different milestone, or under
none, is not this run's work — not picked, not claimed, not reported
against, however urgent it looks. The scope does not widen mid-run.

**Iteration 1 plans; it does not build.** Its output is the milestone
(`POST {SERVER_URL}/milestones/`, or an existing one adopted — matched
on `directory` plus `version`, never duplicated) and one cws task per
planned unit of work (`POST {SERVER_URL}/tasks/`), each carrying
`milestone_id`, a `priority`, and `derived_from` where the plan makes one
task a subtask of another. Where the milestone already exists **with
tasks under it**, the plan was made by an earlier run: it is adopted, not
re-made.

Two things can supply that plan:

- **A named instruction file** — open the session in a git worktree
  (`CLAUDE.md` §1), activate workflow mode, and name a file under
  `instructions/`, ckdbs's own convention for a work order
  (`instructions/v2.2.0-stmtshipping.md`, `instructions/v2.4.0/2pc.md`).
  `intermediary-agent` reads it once, sets the run's **goal** from what
  the file states it delivers, and registers the file's own task table —
  its gates/build rows, **ids kept verbatim** (`G1`, `SS1`) as the first
  token of each task's `title` — as the milestone's tasks. Those ids
  still feed the issue-alias rule under "Documentation" below,
  unchanged. Because the rows are now real cws tasks, they report through
  `POST /tasks/{id}/results/` like any other task; the old exception that
  said they had no `/tasks/{id}` to report against is gone.
- **No instruction file** — the goal is whatever the invocation states,
  and the plan is `intermediary-agent`'s own breakdown of it into tasks
  under the new milestone. Nothing already sitting in the queue is
  adopted by this route: an unrelated pending task is not part of this
  run's plan and is never fetched by it.

Either way the milestone's criteria are the goal's completion condition,
checked the same way `reporter-agent` already checks milestone progress —
not a task count kept locally. **`priority` is an integer the server
fixes no direction for; this loop reads it lower-runs-first** (`1` before
`10`), a convention chosen without operator input and reversible by
saying so. The mechanics live in `intermediary-agent.md`'s own two-phase
loop — this file states the policy, that one states the procedure.

## What it is

Workflow mode is the process that drives ckdbs development from `cws`'s
external task/result queue instead of from an interactive session, using
two agents:

- **`intermediary-agent`** plans the run's milestone and tasks on its
  first iteration, then pulls one pending task **from that milestone**,
  works inside ckdbs on it, and reports a result back. It has no working
  style of its
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
exists*, `reporter-agent` is the one that *tells cws about it*. **Such a
subtask is created under this run's own `milestone_id`, always** — the
loop fetches nothing else, so a subtask filed anywhere else is a subtask
the loop that raised it can never pick up.

## What the loop optimizes for

Five standing emphases. They are not extra steps bolted onto the
iteration above — they are what makes an unattended loop worth running
instead of a queue of scripted edits, and each one is checkable in the
task's report.

**Self-enhancement — the loop improves the machinery it runs on.** An
iteration that hits friction fixes it where it lives rather than working
around it silently: an ambiguous rule in `docs/rules/`, a missing step in
`.claude/agents/intermediary-agent.md` or `reporter-agent.md`, a doc that
sent the task to the wrong file, a stale claim in `CLAUDE.md`'s milestone
table. That fix is part of the iteration's output and is reported as
such. **Bounded, and the bound is the point**: self-enhancement never
edits a Hard Invariant, never relaxes a gate in the Session Workflow,
never widens its own authority — in particular it cannot grant itself the
go-ahead the push gate withholds. It sharpens the process; it does not
rewrite what the process is allowed to do. A change to this file or to an
agent file is a change like any other: reviewed, and named in the report.

**Reasoning — done before the edit, and written down.** The loop's value
is that thinking happens where the work does, not that edits arrive
quickly. A task begins by reading the owning spec, `docs/inflight/`, the
tests and the git history, and the reasoning is recorded before the first
edit rather than reconstructed after it. `CLAUDE.md`'s standing rule
applies unchanged and hardest here — **re-measure a premise before
building the fix**, because no reviewer is watching the guess. A task
whose report cannot state *why* this approach and *what was ruled out* is
not finished, however green its suite; and the long form of that
reasoning lands in the owning doc under "Documentation" above, not in the
cws result field.

**Hypothesis, verification, and the plan that follows from it.** Most
non-trivial tasks start from a guess — where the cost is, why the test
fails, which layer owns the bug. Workflow mode requires that guess be
**stated as a hypothesis with the check that would falsify it**, and the
check run before anything is built on it. Verification is evidence, not
assent: a measurement in `build-release` carrying its `git describe
--tags`, a test that fails before and passes after, a spec section that
says it. **A refuted hypothesis is a result, not a wasted iteration** —
it is written down with the same care as a confirmed one, because the
next iteration has no memory and would otherwise pay for the same guess
twice.

Then the plan is re-formed on what the check returned, in the same
iteration, before the loop moves on. Confirmed: the dependent tasks stand
and the next one proceeds. Refuted: every task that rested on that
premise is rewritten, re-prioritized, or replaced with new tasks under
the same `milestone_id` — **a plan is never left standing on a premise
this iteration just disproved**, and "the queue still says so" is not a
reason to build the next task. Hypothesis, check, verdict and the plan
change it forced all land in the owning doc under "Documentation" below;
the cws report names them and points there.

**Achieving the milestone — the milestone is the unit of success, not the
task count.** Tasks are one decomposition of the goal, made at planning
time with the least information the run will ever have. When working a
task shows the decomposition wrong, `intermediary-agent` re-plans inside
the milestone — re-prioritizing, splitting a task, filing new ones under
the same `milestone_id` — rather than following a plan it has already
disproved. Two things it may not do: widen into a second milestone (the
scope rule under "One task source" is absolute), or call a milestone met
because its task list emptied. Completion is the milestone's criteria
checked against the tree. **Partial is reported as partial**, with what
is missing named and pointed at its doc.

**Review in various aspects — one pass is not a review.** Every step
takes `critics-developer` (Session Workflow §2) and every feature takes
`ck-tester` (§3); beyond that floor, the loop reviews the change from the
aspects it actually touches, because a single reviewer reading for
correctness will not notice a broken durability claim or a doc left
stale. The aspects, applied where they apply: correctness against the
Hard Invariants; conformance to the spec the change claims to implement;
crash, recovery and restart behaviour; concurrency, cross-core and
ownership; performance and per-statement overhead; and the doc trail the
task owes. **An aspect that does not apply is named as not applying** —
"no on-disk format touched, so no recovery aspect" — never dropped in
silence, since silence and unexamined read identically in a report. Where
an aspect wants a different reader, use the agent that owns it —
`kdbs-architect` for structure, `ck-tester` for measurement — rather than
folding every aspect into one pass.

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

**Iteration 1 is the planning iteration** and runs steps 0 and 6 only:
`intermediary-agent` settles the milestone, registers the run's tasks
under it with their priorities, builds nothing, and hands the plan to
`reporter-agent`. Every iteration after it runs the steps below.

0. **(iteration 1 only)** `intermediary-agent` derives the goal, finds or
   creates the milestone, and creates one task per planned unit of work
   under it — `milestone_id` on every one, `priority` in dependency order
   (lower first), `derived_from` where a task is another's subtask, and a
   task-row id kept verbatim in the title where the plan came from an
   instruction file.
1. `intermediary-agent` asks cws for the next pending task **under this
   run's milestone** — `GET {SERVER_URL}/tasks/?milestone_id={id}&pending=true`
   — and takes the lowest `priority`, oldest `raised_at` breaking ties.
   The unfiltered pending query is not used, and no other milestone's
   tasks are fetched.
2. It works the task inside `CLAUDE.md`'s ordinary Session Workflow,
   **unmodified**: a worktree named for the work (§1) — carrying the cws
   task id is recommended, so `reporter-agent` can trace state back to
   it — `critics-developer` review per step (§2), `ck-tester` per feature
   (§3), sync-and-report at land time (§4). Inside that, the emphases of
   "What the loop optimizes for" apply: the task's hypothesis and its
   falsifier are stated before the first edit, the falsifier is run
   before anything is built on the guess, and the review is read from
   every aspect the change touches, not one.
3. **The go-ahead gate does not move.** `CLAUDE.md` §4 requires an
   explicit go-ahead before anything pushes to `origin main`; workflow
   mode grants none of its own authority, and cannot — that push stays
   the user's decision, not the loop's, regardless of how autonomously
   the rest of the task was decided. A task that reaches this gate
   reports `awaiting go-ahead` as its outcome — it does not push itself,
   and the loop does not block waiting for one: it moves on to the next
   pending task **under the same milestone**.
4. `intermediary-agent` reports the task's outcome — done, blocked,
   awaiting go-ahead, or no task found — back to the loop driver.
5. **If the task turned up unscoped work** — a fix that needs its own
   review cycle, a measurement `ck-tester` should run separately, a
   question that names an `Open Decision` — `intermediary-agent` does
   not absorb it into the current task. It writes what it found and why
   into the owning doc under `docs/` (per "Documentation" above), states
   the follow-up as one or more subtasks pointing at that doc, and hands
   them to `reporter-agent` along with the iteration's other output.
5b. **If verification refuted the premise the plan rested on**,
   `intermediary-agent` states the re-plan in the same iteration — which
   tasks are now wrong, and what replaces them — and hands it to
   `reporter-agent` with the rest of the iteration's output. It does not
   leave a disproved task standing for a later iteration to build.
6. `reporter-agent` syncs the task outcome, checks that any doc update
   the task claimed is actually there, enqueues the handed-off subtasks
   into cws **under the same `milestone_id`**, and applies any handed-off
   re-plan with `PATCH {SERVER_URL}/tasks/{id}/`, then the loop returns to
   step 1. A subtask waits its turn like any other pending
   task — its `priority` places it, and it does not jump the queue or run
   inline in the iteration that raised it.
7. **The run ends when the milestone-scoped pending list is empty**, not
   when the global queue is. `intermediary-agent` then checks the
   milestone's criteria and reports what landed and what didn't; it does
   not fall back to the unfiltered queue looking for more work.

## What never changes

Identical to interactive use: Hard Invariants, `docs/rules/rules.md`,
the Session Workflow's four steps and their gates, the go-ahead
requirement before any push to `origin main`, Version Management, and the
worktree-and-commit-in-text convention. A task worked under workflow
mode that cannot pass `critics-developer` or `ck-tester` stops at
exactly the point an interactive session would stop, and says so in its
report to cws — it is never waved through because no human was
watching.

## Closed by `/help`, 2026-08-27

`GET {SERVER_URL}/help` was run against the server and answered the three
endpoint gaps this section used to carry. All three are now written into
the two agent files rather than left as intent:

- **Creating tasks exists.** `POST {SERVER_URL}/tasks/` takes `version`,
  `title`, `content`, `type`, and optionally `derived_from` (a parent
  task id, checked app-side — KDS has no self-referencing FK),
  `milestone_id` (an engine-enforced FK — 400 if it doesn't exist) and
  `priority` (an integer with **no server-fixed direction**, hence this
  file's lower-runs-first convention). That is what both the planning
  iteration and `reporter-agent`'s subtask enqueue call.
- **Updating a milestone exists.** `PATCH {SERVER_URL}/milestones/{id}/`
  writes only the fields present, so advancing `state` alone restates
  nothing else. `reporter-agent.md`'s old "there is currently no API to
  update a milestone's `state`" was true when written and is not now.
- **The instruction-file tasks have real `/tasks/{id}` rows**, because
  iteration 1 registers them, so recording their outcome is the ordinary
  `POST /tasks/{id}/results/` — not an issue-shaped workaround. Issues
  (`POST /issue/{project}/`) go back to meaning what `reporter-agent`
  always used them for: problems the work surfaced.

- **Editing a task in place exists** — `PATCH {SERVER_URL}/tasks/{id}/`
  writes only the keys present (an explicit `null` clears a nullable
  column, and `raised_at`/`last_shipped_at`/`claimed_by`/`claimed_at` are
  refused by name). That is what makes the re-plan under "What the loop
  optimizes for" an operation and not an intention: a task whose premise
  this iteration disproved gets its `content` and `priority` rewritten
  rather than left to be built as planned. `reporter-agent` makes the
  call, per the split — `intermediary-agent` decides the re-plan, the
  reporter tells cws.

Also on the server and **not yet used by either agent**: `POST
/tasks/{id}/claim/` and `/release/`, an exclusive 30-minute lease so two
sessions can't work one task at once, with `GET /tasks/?claimable=true`
as its advisory filter. One loop against one milestone doesn't need it;
two loops sharing a milestone would. Unadopted, not rejected.

## Open

- **The doc-trail check's depth.** `reporter-agent.md` now names the
  check ("confirm the doc this task pointed at actually exists"), but
  what counts as confirmation — the file existing, the section existing,
  or the section actually saying what the task claimed — is not settled.
  Today it reads as file-and-section.
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
