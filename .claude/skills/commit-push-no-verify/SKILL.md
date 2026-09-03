---
name: commit-push-no-verify
description: Commit the current working tree and push it to remote main with the pre-push hook skipped. Use when the operator says "commit and push with no verify", "push to main without the hook", "land this, skip the tests", or invokes /commit-push-no-verify. The operator is waiving the gate; do not re-litigate it, do not run the suite unasked, and never report an unrun suite as a pass.
---

# commit-push-no-verify

Land the current tree on `origin/main` with `--no-verify`. The operator has
waived `scripts/githooks/pre-push`; `CLAUDE.md`'s Working Rules say CLA does
this without re-litigating and still owes two things in the reply: **say
plainly what was skipped**, and **never report an unrun suite as a pass**.

## Steps

**1. See what is being landed.** Never commit blind.

```
git status --short
git diff --stat
git log --oneline -1
```

If the tree is clean, say so and stop — there is nothing to land.

**2. Sync on the work branch, never on `main`.**

```
git fetch origin
git merge origin/main
```

Resolve conflicts here. If `origin/main` moved, **re-check any claim the
staged work makes about the tree** — a document or comment written against
the old tip can be made false by what just arrived. This is the step that
catches it; do not skip it because the merge was clean.

**3. Stage and commit.** Include new files (`git add -A` or explicit paths).
Write the message in this repository's idiom: a statement of what changed
and why, not a label — *"AL-S8: M0's baseline, and the eight-cell matrix
turns out to measure statement shipping rather than the WAL latch"*. End
every commit message with the attribution footer the session's system
prompt gives, verbatim.

**4. Push.**

```
git push --no-verify origin HEAD:main
```

`HEAD:main` is required rather than `git checkout main && git merge`: the
primary checkout holds `main`, so a worktree cannot check it out. This is
the worktree push pattern for this repository.

If the push is rejected as non-fast-forward, another session pushed while
you worked — go back to step 2, merge, and push again. **Never** reach for
`--force` unless the operator has explicitly said to discard that specific
work.

## What the reply must say

- **What was skipped**, by name: the pre-push hook (`--no-verify`), and the
  test suite if it was not run. "Overhead not measured" stays a stated fact
  where the change touches the engine — the interleaved A/B is suspended by
  operator decision.
- **What landed**: the commit id, the branch it went to, and one line on
  what is in it.
- Every claim carries its worktree and short commit id **inside the
  sentence**, per `CLAUDE.md`.

## What this skill does not do

It does not create or push tags — a version is named by the operator and a
tag message is a durability claim that must carry what bounds it. It does
not run the suite, and it does not decide that skipping it was fine; it
records that it was skipped.
