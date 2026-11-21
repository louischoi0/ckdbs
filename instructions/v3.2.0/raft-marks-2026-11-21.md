# Raft marks - 2026-11-21

The operator's words of 2026-11-21, recorded in English.

## 1. DL landed: push no verify

| | |
|---|---|
| **Word** | *"go ahead"*, given at DL's push gate, after the report that ended "the work is held only by the push gate" |
| **Mark** | **DL lands on `main`**: the work branch `worktree-dl-statistics-queue-no-statement`, synced with `origin/main` at `01a7609b` (documents and bench only) and again at `2c2639df` (DD's `PURGE INDEX` and DN-S0/DN-S1, with build inputs), merged `--no-ff` and pushed. The full suite ran 0 failed of 5,052 on the merged tree, whose build inputs are the pushed tree's, so the push goes out with `--no-verify` (the pre-push hook skipped, the suite not run again by it). `tsan` ran 235 cells with no report at `43cecfa0`; `bs-nightly` not run. The close measurement: `bench/v3.4.17/results-dl-close-v3.4.17-11-g30e169df.md` |
| **Skipped on that word** | the pre-push hook |
| **Does not settle** | a tag or a version; the reader storm's tail and DL-Q4's two words, left in `known-gaps.md` |
| **Recorded at** | this file; the DL order's header and §7; its row in `instructions/v3.2.0/index.md` |
