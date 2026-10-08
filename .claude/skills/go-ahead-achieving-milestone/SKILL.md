---
name: go-ahead-achieving-milestone
description: Work the current milestone stage after stage, in this session, until it closes — adopting CLA's own proposal wherever a decision is needed, because the operator has said in advance to follow it. Use when the operator says "go ahead until the milestone is done", "keep going, I'll follow your proposal", "마일스톤 끝날 때까지 진행해줘", or invokes /go-ahead-achieving-milestone. It still stops before any push, tag or version.
---

# go-ahead-achieving-milestone

The operator's standing word, given when the skill is invoked: *"go ahead
until achieving milestone, I will follow CLA proposal if decision needed,
keep going in this session."* Three things follow from it.

- **Scope: one milestone.** That is every stage in its work order, through
  the milestone's close.
- **Decisions: CLA's proposal is the mark.** An open item does not stop the
  work. CLA records its proposal as adopted under this word and continues.
- **Pace: one session.** Do not stop between stages to report or ask. Do
  not write a handoff to end the turn early.

Everything else in `CLAUDE.md` still applies unchanged: the Session
Workflow's worktree, the per-step review, the suite, the sync, and the
claim and report conventions.

## 1. Which milestone

Take the first of these that applies:

1. the milestone named in the skill's argument;
2. the milestone whose work this session or its worktree is already on
   (the handoff, the branch name, the work order last edited);
3. the first open milestone in `instructions/<version>/index.md`, in the
   index's own order.

Read the milestone's work order whole: survey, specification, rulings,
stages, items and review. Name the milestone, its work order, the stages
left, and every unmarked item in one short progress note. Then start.

## 2. Decisions — adopt the proposal, record it truthfully

When a stage reaches an item its work order or owning spec does not
decide (a Q-, D- or R-item, a BH-Q0-style "open the milestone", a choice
that surfaces mid-step):

1. **If the order already carries CLA's proposal**, adopt it as written.
2. **If it carries none**, write the item into the order's items table
   first. Give its options, CLA's proposal and the reason, in the house
   format. Then adopt that proposal.
3. **Record each adoption** where the order's marks are recorded: the
   day's `raft-marks-<date>.md`, the order's header, the mark column of its
   items table, its review section and the index row. Record it as
   *"adopted on the operator's standing go-ahead of `<date>`
   (go-ahead-achieving-milestone)"*, never as the operator's own
   per-item mark. The record must say who decided, and how.
4. **A `[quiet-wrong]` item is adopted too**, because the operator said to
   follow the proposal. Before adopting it, re-read the premise its
   proposal rests on against the tree. If the premise no longer holds,
   revise the proposal, record the revision, and adopt the revised one.
   Every adopted `[quiet-wrong]` item is listed first in the final report.

Adopt a proposal; never invent a mark the order cannot justify. Never
re-open an item the operator already marked.

## 3. Work each stage — the Session Workflow, unmodified

For each stage, in the order's sequence:

1. **Worktree.** Stay in the milestone's worktree, or `EnterWorktree` with
   a name taken from the milestone if none exists.
2. **Per step.** Implement the step, then run a `critics-developer` review
   of it. Apply the findings, and name each rejected finding with the
   reason.
3. **Suite.** The full suite gates every step. A stage lands carrying the
   statement "overhead not measured; measured at the milestone's close".
4. **Commit** on the work branch when the step is reviewed and green. A
   red-cell census stage commits its red cells red, if its order says so.

A failing gate is work, not a stop:

- **A review finding:** fix it.
- **A red suite:** fix the cause. A flake named in memory is repeated
  before it is blamed.
- **A regression measured at the close:** propose a remedy within the
  milestone, apply it, and measure again.

Bound each attempt. After three tries at the same failure without
progress, it becomes a §5 stop.

Progress notes stay one or two lines, without a UTC time line. Findings go
into the work order and the docs, not into chat.

## 4. Close the milestone

The milestone is achieved when all of these hold:

- Every stage in the order is landed on the work branch.
- The milestone-close measurement has run: `ck-tester`'s interleaved A/B
  in `build-release`, over the commit the milestone opened from against
  its closing commit, written to `bench/<version>/` with `git describe`.
- The durable content has moved to its owning documents. The spec states
  what is built, the index row is flipped, `CLAUDE.md`'s milestone row is
  updated (one line, linking the spec), `known-gaps.md` and `bugs/` are
  updated, and the manual is updated where the SQL surface changed.
- The branch is synced: `git fetch origin && git merge origin/main` on
  the work branch, conflicts resolved there, and the suite run again after
  a merge that brought code.

## 5. Stops — the only reasons to end the turn before §4

The standing word does not reach these. Stop, commit what is reviewed and
green on the branch, and report:

- **Push, merge to `main`, a tag, or a version number.** These wait for
  the operator's word (Version Management, Session Workflow step 4). They
  are always a stop, after §4.
- **Another session's state:** a stash, a worktree or a branch this
  milestone did not create. Leave it untouched.
- **A decision CLA cannot propose from the tree**: it needs a fact only the
  operator holds, such as a business requirement or an external system. A
  decision outside this milestone's scope is also a stop.
- **An environment that cannot build or run** the suite or the
  measurement. Report it as not executed, never as a pass.
- **A failure bounded out under §3.**
- **A proposal that would break a hard invariant** that the order does not
  itself amend under a recorded mark.

If the context nears its limit, let compaction carry the session. Run
`organize-session` only if the session cannot continue, and say so.

## 6. Report

One reply, at the push gate or at a stop. It opens with
`date -u '+%Y-%m-%d %H:%M UTC'`. Every claim carries its worktree and short
commit id inside the sentence (`CLAUDE.md`). The reply contains:

- **Adopted decisions**, with `[quiet-wrong]` items first: each item, the
  option taken, and any revision made against the tree.
- **Stages landed**, each with its commit.
- **Per stage**, the review: what it found, what was applied, and what was
  rejected and why.
- **The suite** at the closing commit, or that it was not executed.
- **The measurement**, with its `git describe` and results file, or that it
  was not taken.
- **What the merge of `origin/main` resolved.**
- **The stop**, if the reply ends at one: what holds the work, and what the
  operator's word would unblock.

Then wait. On "push", land per the worktree push pattern.

## What this skill does not do

- It does not push, merge to `main`, tag, or name a version.
- It does not touch another session's tree or stash.
- It does not cross into a second milestone.
- It never reports an unrun suite or measurement as a pass.
- It never records an adopted proposal as the operator's own mark.
