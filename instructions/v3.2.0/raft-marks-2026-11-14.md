# Raft marks, 2026-11-14

The operator's words of 2026-11-14 that open a new matter, and the decisions
CLA adopted under them.

## 1. The audit's fifteen verified findings, taken up

| | |
|---|---|
| **Word** | W1, on 2026-11-14: *"handle the 15 findings that passed adversarial verification in the audit"*, with the skill `go-ahead-achieving-milestone` invoked (*"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session"*) (given in Korean; recorded in English) |
| **Mark** | G1-G15 of `docs/pending/whole-engine-audit-second-run-twelve-slices.md` §4 are taken up as one milestone, on worktree `audit-g1-g15-fixes` from `2789a457`, each as its verifiers refined it. No work order is written: the pending entry carries every finding's claim, scenario and refined fix, and §8 of it records what became of each, as the first run's entry did for F1-F32 |
| **Does not settle** | G16-G64 (unjudged), the 15 unread slices, a merge, a push, a version |
| **Recorded at** | this file; the pending entry's status line and §8; this file's row in `instructions/v3.2.0/index.md` |

## 2. Decisions adopted on the standing go-ahead

Each is **adopted on the operator's standing go-ahead of 2026-11-14
(go-ahead-achieving-milestone)**: CLA's proposal, not the operator's own
per-item mark.

| Item | Options | Adopted, and why |
|---|---|---|
| **G4-D** `[quiet-wrong]` - does the engine owe recovery from a scattered log cut, and may a mount write the log tail before recovery has succeeded (the intent verifier's "operator decision needed first") | (a) yes to both: the open zeroes an unsealed tail from one record header past the scan's end, and syncs; (b) no: leave the hole unmodelled and unanswered, as `faults.hpp` argued | **(a).** Its premise was re-read against the tree at `2789a457` before adoption. `file_log_device.hpp:72-76`'s "a refused mount leaves its log whole" is about leftover segments below the anchor's segment, which the scrub does not touch. The scrub erases only bytes past the first record no scan accepts and past that record's header, so every scan reads the same log before and after it, and the torn-tail meter is unchanged. Restore's `CutLogAt` already zeroes for this hazard. Without (a) the hazard is a silent wrong replay, where (a) costs one read of the tail at a mount |
| **G3-D** - which of the two refined fixes | (a) log each spill at its append (`VarHeapSink::on_append`, the codepath verifier); (b) log the collected spills on a shared failure exit (the intent verifier) | **(a).** (b)'s own failure exit can be refused under the same pool pressure that caused the failure, while (a) is AT-S21's rule for every user-row path. The cost is the order of the success path's records: a spill's records now precede the heap chain's `PAGE_INIT` and tail image. They name different pages and all still precede `HEAP_INSERT`, so redo is unaffected |
| **G2-D** - the bound | (a) `kMaxPageCount` only; (b) Brent's cycle check bounded by `kMaxPageCount` (the intent verifier's recommendation) | **(b).** A real cycle is refused within about three times its length rather than after 2^31 hops with the caller's leaf held. Revised at the step's review: the `kMaxPageCount` backstop was dropped, because Brent's check finds every cycle and an acyclic chain cannot exceed the id space |
| **G10-D** - how much to delete | (a) the finder's list; (b) the verifier's narrowed list, keeping BZ-R5's `ChainTail`/`ChainLength`, `PageLiveSlots`, `BumpPageEpoch` and `varheap::ChainLength`; the optional test-only items left to choose | **(b)**, with the optional `ChainAppendResult::grew()` and `PageView::HasSpaceFor` taken. `PageView::SlotCapacity` is kept, because two contract comments in `heap_page.hpp` are written in its terms |
| **G6-D** - `AcquireResult::already_held` (the verifier's optional step 6) | (a) delete; (b) keep | **(a)**: with `Acquire` gone, nothing reads it |
| **G13-D** - the optional hardening of `undo_log_test.cpp` | (a) take; (b) leave | **(b)**: the dedup is byte-identical on its own, and the hardening is a separate test change |
| **G1-R** - a review finding next to G1: a writable `FileLogDevice` open adopted segment names without a directory fsync | (a) fsync the directory once at a writable open; (b) record it in `known-gaps.md` | **(a)**: G1's guarantee - `durable_lsn()` never claims more than the device has synced - covers the segment's name as well as its bytes, and the change is one call at open |
| **G4-M** - the close measured mounts 25-33 ms slower at `713a84d0` (`bench/v3.4.8/results-audit-g-close-v3.4.8-16-g713a84d0.md`); remedied at `7c2dec3b`/`4cf04d4c` (the scrub reads the scan's own buffer, a 4 KiB `memcmp` test), measured again 4-6 ms over `2789a457` (`results-audit-g-close-v3.4.8-20-g4cf04d4c.md`), a shift inside the baseline's own 9-11 ms spread | (a) accept the residual as G1's and G4's stated cost - one memory pass over the last segment and two syncs a mount; (b) skip the pass for a segment the scan saw zero to its end | **(a)**: a scan that stops at a zero header has not seen the bytes after it, which is exactly where G4's hole lies, so (b) would give G4 back. The cost is per mount, not per statement, and stated in `wal.md` §4.2 |
