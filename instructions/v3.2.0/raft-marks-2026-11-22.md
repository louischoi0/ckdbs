# Raft marks - 2026-11-22

The operator's words of 2026-11-22, recorded in English.

## 1. DO-S3: the operator's mark on E2's proof

| | |
|---|---|
| **Word** | *"go ahead"*, given after CLA's account of E2 - what it walks, why it is `[quiet-wrong]`, the proof's cases in the DO order's §5a, what it buys and costs, and that it waited for the operator's own mark - which ended "if you mark it, I build E2 with its crash cells, measure it like E1, and close DO" |
| **Mark** | **The operator's own mark on the DO order's §5a** (DO-Q3 (a)'s gate): E2, the checkpoint's dirty table walked a slot chunk per structure-latch hold, is built, with §5a's crash cells. Recorded as said: "go ahead", read as the mark the account asked for |
| **CLA's additions under it** | a handover between chunks (the latch's waiters counted on its contended path; the walker yields up to 20 us while one waits), because a measured fetch still waited out the whole walk without it; and §5a's merge corrected in the build to keep a page's lowest **non-zero** recLSN, so a 0 never hides a real one. Both recorded in the order's §7 as CLA's, not the operator's |
| **Does not settle** | a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; the DO order's header, §5a and §7; its row and this file's row in `instructions/v3.2.0/index.md` |

## 2. DO's E2 landed: push no verify

| | |
|---|---|
| **Word** | *"push no verify"*, given after CLA's account of where DO stood: closed on the work branch, E2 at `4b7b4855` waiting for a push |
| **Mark** | **E2 and DO's close land on `main`**: the work branch `worktree-do-frame-table-long-holds` (worktree `dg-wal-segment-body`; `4b7b4855`, its sync with `origin/main` `8f463edf`, and this record), merged `--no-ff` and pushed with `--no-verify`. The full suite ran 0 failed of 5107 on E2's build inputs, and nothing after it changed a build input (the sync and this record are documents only), so the hook was not run again. E2's measurement: `bench/v3.5.0/results-do-s3-e2-v3.5.0-62-g1cfe6c0f.md` |
| **Skipped on that word** | the pre-push hook |
| **Does not settle** | a tag or a version; the walking core's stall and the latch's unfairness between fetches, left in `known-gaps.md` |
| **Recorded at** | this file; this file's row in `instructions/v3.2.0/index.md` |

## 3. DP written: VIEW

| | |
|---|---|
| **Word** | *"Bring the kdbs repo up to date and write the view milestone work order again to fit the recent state of the work"* (2026-11-22), after the operator's words of 2026-11-21 in conversation: *"I want to apply the view feature to ckdbs in the way that is optimized for it"*, and, correcting CLA's first draft, *"The contract that the query is the plan has been retired. In its place, a little relaxed: the database cannot change a plan freely, and the plan itself is a managed object."* |
| **Mark** | **DP is written**, not opened: `workorder-dp-view.md`, at `71013396`. The 2026-11-21 conversation draft (VW1-VW19, never committed) is superseded, three of its premises corrected against the tree (DP's header) |
| **CLA's, not the operator's** | the letter DP; absorption as a class (a) compile-time item; every DP-R and DP-Q; the stages |
| **Does not settle** | any DP-Q item; an opening; a push beyond this record; a tag or a version |
| **Recorded at** | this file; `workorder-dp-view.md`; its row in `instructions/v3.2.0/index.md` and in `instructions/milestones.md` |

## 4. DP-Q0..Q13 marked as proposed

| | |
|---|---|
| **Word** | *"For now, mark them as proposed, revise the work order and push"*, given after CLA's account of each DP-Q item's proposal and its reason (DP-Q6 and DP-Q1 first, as the two that the standing go-ahead could not settle) |
| **Mark** | **DP-Q0..Q13 marked (a), the operator's own mark**: a build order (Q0); §2's absorbable form, a subquery predicate admitted (Q1 `[quiet-wrong]`); a non-absorbable body answered by the query layer with nothing pushed in (Q2); `kTypeView` in `sys.objects` plus `sys.views` (Q3); a view's DDL transactional (Q4); the written text's `fetch_id` (Q5); no `CREATE OR REPLACE`, a new body a new oid, an enabled administrator's plan RESTRICTing `DROP VIEW` (Q6 `[plan-identity]`); `<view>.<inner>` binding names (Q7); depth 8 `[provisional]` (Q8); no write through a view (Q9); `FOR UPDATE` where the absorbed block is one relation (Q10); a body's `*` fixed at `CREATE VIEW` (Q11); `SELECT *` from a view over a join admitted (Q12); `admin` creates a view (Q13) |
| **Does not settle** | DP's opening; a tag or a version |
| **Recorded at** | this file; the DP order's header, §4 and §7; its row and this file's row in `instructions/v3.2.0/index.md` |

## 5. DP opened on the standing go-ahead; DP-Q14..Q22 adopted

| | |
|---|---|
| **Word** | `/go-ahead-achieving-milestone DP`: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session"* |
| **Mark** | **DP is opened** at `30c47abf` (worktree `sync-stale-records`, branch `worktree-dp-view`). **DP-Q14..Q22, raised by DP-S0's census, are adopted (a) on that standing go-ahead** - CLA's proposals, not the operator's per-item mark: fixed-width `sys.views` rows under the DDL's transaction (Q14), a view's column list naming its columns (Q15), column dependencies by name with `ADD COLUMN` RESTRICTed (Q16 `[quiet-wrong]`), a view list beside the plan key's relations (Q17 `[plan-identity]`), a "view exists" fast-path word (Q18), no `ANALYZE` conjunct mark (Q19), a non-absorbable body compiled at its `CREATE` (Q20), a body's names in the view's namespace (Q21), `DROP VIEW` taking no lock (Q22). **DP-Q1's wording revised** under the same word: the absorbable form's aliases are the view's column list, the native select list carrying none |
| **Does not settle** | a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; the DP order's header, §4 and §7; its row in `instructions/v3.2.0/index.md` and in `instructions/milestones.md` |

## 6. DP closed on the standing go-ahead, not landed

| | |
|---|---|
| **Word** | the same `/go-ahead-achieving-milestone DP` (§5) |
| **Mark** | **DP is closed**: DP-S0..S3 on `worktree-dp-view` (worktree `sync-stale-records`) at `03242261`, `184b1c2c`, `5e2491fe`, `78a32c4f`, each reviewed and suite-run; the close measured at `v3.5.0-84-g78a32c4f` against `v3.5.0-79-g30c47abf` (`bench/v3.5.0/results-dp-close-v3.5.0-84-g78a32c4f.md`). **Adopted on the standing go-ahead, CLA's proposals**: DP-Q23 (a body's names are its own, raised by DP-S0's review), DP-Q22 revised to (b) (`CREATE VIEW` holds `IS` on what it reads, `DROP VIEW` takes the view's `X`), and DP-R11's `[provisional]` lifted from depth 8 on the measured 3 us a level. **DP-R10 read as met within what the method resolves**, with `pkmix` stated at the edge of its floor (-0.79 %, 6 of 6 paired runs lower), not as level |
| **Does not settle** | a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; the DP order's header and §7; its row in `instructions/v3.2.0/index.md` and in `instructions/milestones.md` |

## 7. DP landed: push no verify

| | |
|---|---|
| **Word** | *"push no verify"*, given after CLA's report of DP's close |
| **Mark** | **DP lands on `main`**: `worktree-dp-view` (worktree `sync-stale-records`, at `62001c4c`) merged `--no-ff` over `origin/main` `30c47abf` as `8fef04fa` and pushed with `--no-verify`. The merged tree is `62001c4c`'s, on which the full suite ran 0 failed of 5146 (Debug), so the hook was not run again; this record is documents only |
| **Skipped on that word** | the pre-push hook |
| **Does not settle** | a tag or a version |
| **Recorded at** | this file; the DP order's header; its row in `instructions/v3.2.0/index.md` and in `instructions/milestones.md` |
