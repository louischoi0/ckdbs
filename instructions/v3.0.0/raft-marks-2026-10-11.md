# Ratification marks — 2026-10-11

**The operator's words of 2026-10-11**, recorded by CLA in the main
checkout on `main` at `ab2c46d0` (`v2.7.0-771-gab2c46d0`; the v3.0.0 tag is
not cut). All of it is the operator's words, translated into English, or
the option the operator chose where CLA asked with options.

## 1. This session's documents go to `main` directly

| | |
|---|---|
| **Word** | *"In this session it will be almost only documents, so push directly."*, answering CLA's question of whether the session's documents land by a direct push to `main` or by a branch and a pull request |
| **Mark** | A direction for this session: a document CLA writes in it is committed in the main checkout and pushed to `main`, with no worktree and no work branch. A push of documents only skips the pre-push hook under the hook's own exemption, and the reply says so |
| **Does not settle** | Code: a change that touches code still runs the Session Workflow and its gates. Any other session |
| **Recorded at** | This file; `workorder-bk-sys-statistics-show-sugar.md`'s header |

## 2. Statistics tables wanted; five proposed, one excluded

| | |
|---|---|
| **Word** | *"Right now ckdbs has only the tables the sys catalog strictly needs, so the meta-information tables for analysis, operations and profiling are limited. Pick about five tables worth adding."* CLA proposed `sys.statement_stats`, `sys.relation_stats`, `sys.lock_waits`, `sys.checkpoints` and `sys.sessions`; asked which to leave out, the operator chose *"Exclude `sys.sessions`."* (the option CLA had marked as its recommendation) |
| **Mark** | BK-R1: four relations, `sys.sessions` not among them |
| **Does not settle** | BK-Q0..Q13 |
| **Recorded at** | `workorder-bk-sys-statistics-show-sugar.md` §2 |

## 3. `SHOW` is sugar; BK written and pushed

| | |
|---|---|
| **Word** | *"Write a workplan that adds four tables, and it must contain this: keywords such as `SHOW META` and `BUDGET` are only syntactic sugar, and the same result must be referenceable through `SELECT` as well. And push it."* |
| **Mark** | BK-R2 (SV1-SV4): every `SHOW` target is one `SELECT` over one `sys` view, the same result through either spelling, nothing readable only through `SHOW`. BK-S0: the order written and pushed to `main`. **Not opened**: BK-Q0 is unmarked |
| **Does not settle** | BK-Q0..Q13, among them the rendering of a `SHOW` reply (BK-Q1), each view's filter class (BK-Q2, `[quiet-wrong]`) and whether three of the relations are persisted at the cost of a superblock bump (BK-Q5) |
| **Recorded at** | `workorder-bk-sys-statistics-show-sugar.md` §2, §7; `index.md`'s BK row |
