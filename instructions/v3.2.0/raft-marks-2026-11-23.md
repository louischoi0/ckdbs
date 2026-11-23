# Raft marks - 2026-11-23

The operator's words of 2026-11-23, recorded in English.

## 1. DQ opened on the standing go-ahead; DQ-Q0..Q8 adopted

| | |
|---|---|
| **Word** | `/go-ahead-achieving-milestone DQ`: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session"* |
| **Mark** | **DQ is opened** at `68e5a69f` (`v3.5.0-96-g68e5a69f`), on worktree `dq-cabin-miss-build`, branch `worktree-dq-cabin-miss-build`. **DQ-Q0..Q8 are adopted (a) on that standing go-ahead.** They are CLA's proposals, not the operator's per-item mark. DQ is exploratory, and E0 closes it with nothing built (Q0). A Cabin step carries `Step::build` and stays `kCabinProbe` (Q1). A key at its recording touch walks and records as today (Q2). E2 is built only if DQ-S0 shows the first miss is often a recording one, and then for top-level chains only (Q3). Top-level join steps come first, and sub-chains are DQ-S3, gated on DQ-S0's reading of the two stop rules (Q4, `[quiet-wrong]`). `BUILD <relation> OFF` takes the fallback away, and `ON` beside `ACCESS ... CABIN` stays refused (Q5). The cost model is unchanged (Q6). A value list over a Cabin is out of DQ (Q7). The bar is DQ-S0's gate-refusing cell within 1.5x of the Cabin dropped, with the 1.5 replaced by what DQ-S0 measures, no converged cell slower than today, and DQ-R6 (Q8) |
| **`[quiet-wrong]` re-read** | DQ-Q4 (a) rests on DQ-S0 (ii)'s reading of `cabin.md` §4a's completion license beside `join-inner-build.md` §6's prefix map. DQ-S0 read both at `68e5a69f` before the adoption was relied on, and the premise holds (the order's §7) |
| **Does not settle** | a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; the DQ order's header, §4 and §7; its row in `instructions/v3.2.0/index.md` and in `instructions/milestones.md` |

## 2. DQ-S0's verdict on the standing go-ahead; DQ-Q9..Q12 adopted

| | |
|---|---|
| **Word** | the same `/go-ahead-achieving-milestone DQ` (§1) |
| **Mark** | **DQ-S0's verdict is E1 without E2**, CLA's reading of its own measurement under DQ-Q0 (a) and DQ-Q3 (a), at `68e5a69f` (`bench/v3.5.0/results-dq-s0-premise-v3.5.0-96-g68e5a69fd.md`). Where the banking gate declines, a cabined join costs 116x to 249x the uncabined one in every statement. At the engine's defaults, a 60,000-row cabined join with 2,000 outer keys is refused where the uncabined one answers. That holds within `join_build_max_rows`; past it the build declines too, and E1 buys nothing (corrected after DQ-S0's review, which found the first statement of this held only with the build cap the driver raised). E2 saves 0 walks in every measured cell, so DQ-S4 does not run. E1 composes with both stop rules, so DQ-S3 runs. DQ-Q8's 1.5x stays, provisional until DQ-S5. **DQ-Q9..Q12 are adopted (a) on that standing go-ahead**, CLA's proposals, not the operator's mark: a Cabin pin keeps the fallback (Q9); a Cabin step's decline is a fallback that bucketed and published no map (Q10); the recording statement's refusal goes to `known-gaps.md`, with DQ-Q2 (b) and (c) left to the operator (Q11); a literal equality that would make the uncabined step a filter scan declines the fallback (Q12) |
| **Does not settle** | DQ-Q2 (b) or (c); a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; the DQ order's §4 and §7; its row in `instructions/v3.2.0/index.md` |

## 3. DQ stopped at DQ-S5 on a measured regression; DQ-Q13 adopted

| | |
|---|---|
| **Word** | the same `/go-ahead-achieving-milestone DQ` (§1) |
| **Mark** | **DQ is built and stopped at DQ-S5, not closed**, at `7e31d272` (`v3.5.0-101-g7e31d272b`) on its work branch, every stage reviewed, suite-green and committed. **DQ-Q13 (a) is adopted on that standing go-ahead**, CLA's proposal, not the operator's mark: DQ stays open at DQ-S5 until the operator's word either lifts the measurement budget for a diagnosis and a remedy, or accepts the residue and closes DQ. CLA first proposed closing with the residue recorded, and revised that at DQ-S5's review: it set aside the bar CLA itself set (DQ-Q8 (a)), and a spent budget is a stop, not a close. E1 is built for top-level joins and correlated sub-chains (DQ-S1..S3); E2 is not (DQ-S0). The close (`bench/v3.5.0/results-dq-close-v3.5.0-101-g7e31d272b.md`): DQ-Q8's gate cell runs at 0.79x to 1.15x of the Cabin dropped, against a bar of 1.5x; the gate-declined join is 108x to 251x cheaper than at the opening commit; at the engine's defaults the first statement is answered where it was refused, and the recording statement's refusal stays (DQ-Q11). The residue: converged cabined-join statements read 7 % to 35 % dearer in cold cells, and join statements +0.2 to +0.5 us, with the causes not isolated after one remedy (withdrawn) and two diagnostics, inside a spent measurement budget. It is recorded in `known-gaps.md`. **The merge to `main` waits for the close** (Session Workflow step 4) |
| **Does not settle** | DQ-Q2 (b) or (c); DQ-Q13 (b) or (c); the residue's diagnosis budget; a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; the DQ order's header, §4 and §7; its row in `instructions/v3.2.0/index.md` and in `instructions/milestones.md`; `docs/inflight/known-gaps.md` |


## 4. DQ landed, open at DQ-S5: push no verify

| | |
|---|---|
| **Word** | *"push no verify"*, given after CLA's report that DQ was built and stopped at DQ-S5 on a measured regression |
| **Mark** | **DQ's work branch lands on `main` as it stands**: built (DQ-S1..S3), stopped at DQ-S5, **open, not closed**. The branch `worktree-dq-cabin-miss-build` (worktree `dq-cabin-miss-build`; record `e7f733f0` and this record) is merged `--no-ff` over `origin/main` `68e5a69f` and pushed with `--no-verify`. The full suite ran 0 failed of 5,159 at `7e31d272`, and nothing after it changed a build input (the S5 record and this one are documents, bench and tools only), so the hook was not run again |
| **Skipped on that word** | the pre-push hook |
| **Does not settle** | DQ's close: the push word is not read as accepting the residue (DQ-Q13); DQ-Q2 (b) or (c); a tag or a version |
| **Recorded at** | this file; the DQ order's header; its row in `instructions/v3.2.0/index.md` and in `instructions/milestones.md` |

## 5. The measurement budget lifted; the residue diagnosed as placement

| | |
|---|---|
| **Word** | *"lift the measurement budget"*, given after CLA's report of the stop at DQ-S5, together with the "push no verify" of §4 |
| **Mark** | **The measurement budget for DQ-S5's diagnosis is lifted**, with no figure. CLA read it as one more hour, and that reading is CLA's. The diagnosis ran 03:25 to 03:35 UTC on the close's binaries, and the residue is placement, not instructions. The served path's compile is level, and its functions are the same size in both binaries, so the +0.24 us on a served Cabin join is code placement. Statement #3's excess is the sets landing in the memory the previous statement's freed map left (74 page faults against 265), plus code placement (+100 us with the fallback off). The two placement remedies measured (keeping the helpers out of line, fixing glibc's mmap threshold) each trade the cost onto another shape. **DQ stays open at DQ-S5.** CLA's proposal for the operator's word is to accept the residue and close DQ. It is not adopted on the standing word |
| **Does not settle** | accepting the residue; closing DQ; a push of the records after §4, a tag or a version |
| **Recorded at** | this file; the DQ order's header, DQ-Q13 and §7; `bench/v3.5.0/results-dq-close-v3.5.0-101-g7e31d272b.md`; `docs/inflight/known-gaps.md`; `instructions/v3.2.0/index.md` |

## 6. DQ closed: the residue accepted

| | |
|---|---|
| **Word** | *"accept it and close"* (given in Korean), after CLA's account of the gate-declined cell and of the residue diagnosed as placement (§5) |
| **Mark** | **The operator's own mark on DQ-Q13: the measured residue is accepted and DQ is closed.** DQ-Q8's gate cell is met. Its converged clause and DQ-R6 are not met, and that is accepted: converged cabined-join statements read 7 % to 35 % dearer in cold cells, and join statements +0.2 to +0.5 us, as placement of memory and code (`bench/v3.5.0/results-dq-close-v3.5.0-101-g7e31d272b.md`). The residue stays in `known-gaps.md` as an accepted measured cost |
| **Does not settle** | DQ-Q2 (b) or (c); DQ-Q13 (b) or (c); a push of this record and of `d4a59dd6`; a tag or a version |
| **Recorded at** | this file; the DQ order's header, DQ-Q13 and §7; `docs/inflight/known-gaps.md`; its row in `instructions/v3.2.0/index.md` and in `instructions/milestones.md` |


## 7. DQ's close record landed: push no verify

| | |
|---|---|
| **Word** | *"push no verify"*, given after CLA's report of DQ's close (§6) |
| **Mark** | **DQ's diagnosis (`d4a59dd6`), its close record (`f276c939`) and this record land on `main`**: the work branch `worktree-dq-cabin-miss-build` (worktree `dq-cabin-miss-build`), merged `--no-ff` over `origin/main` `38b5aa69` (§4's merge) and pushed with `--no-verify`. They change documents, bench and tools only; the last suite, 0 failed of 5,159 at `7e31d272`, ran on the same build inputs, so the hook was not run |
| **Skipped on that word** | the pre-push hook |
| **Does not settle** | a tag or a version; DQ-Q2 (b) or (c); DQ-Q13 (b) or (c) |
| **Recorded at** | this file; its row in `instructions/v3.2.0/index.md` |

## 8. BS and BG closed narrow

| | |
|---|---|
| **Word** | *"close BS and BG as closed-narrow for now"* (given in Korean), after CLA's list of the open milestones and its recommendation that BS and BG, open for over a month, be closed, closed narrow or paused |
| **Mark** | **BS and BG are closed narrow, on the operator's own word.** **BS:** S0-S8 are built and on `main` (`b5928bf2`, `v3.4.0`); `workorder-bs-test-hardening.md` §4's bar is not met, its seven consecutive nightly reports among it, and none exists on this host at the close (`crontab -l` lists no entry, and there is no `build-bs-reports` directory); BS's methods stay runnable by hand and out of the push gate. **BG:** BG-S0 and BG-S1 are built (`b27916f3`, `v3.1.0`); BG-S2..S4 - the scan queue, the slot ring's retirement and the close - are not, and the measured displacement stays in `known-gaps.md` (Eviction) |
| **Does not settle** | an order to build the scan queue (BG-R1..R5 stay as written); installing the BS nightly; a push, a tag or a version |
| **Recorded at** | this file; each order's header; their rows in `instructions/v3.0.0/index.md` and in `instructions/milestones.md`; `CLAUDE.md`'s test hardening row; `docs/inflight/known-gaps.md` (Eviction) |


## 9. BS's and BG's close records landed: push no verify

| | |
|---|---|
| **Word** | *"push no verify"*, given after CLA's report of §8 |
| **Mark** | **§8's records land on `main`**: the branch `worktree-close-bs-bg-narrow` (worktree `dq-cabin-miss-build`; `314adfed` and this record), a fast-forward of `origin/main` `5a7dfb92`, pushed with `--no-verify`. Documents only; no suite was run, and none is owed, since no build input changed |
| **Skipped on that word** | the pre-push hook |
| **Does not settle** | a tag or a version |
| **Recorded at** | this file; its row in `instructions/v3.2.0/index.md` |
