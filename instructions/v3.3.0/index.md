# Work orders for v3.3.0

The version of record is `v3.3.0` (operator-named 2026-10-19, the tag's
message). Work orders opened after it live here; the operator's words that
open and mark them are in `../v3.2.0/raft-marks-<date>.md` until a v3.3.0
marks file is opened.

| document | what it is |
|---|---|
| `workorder-cd-dropped-column-cleaning.md` | **CD - cleaning dropped columns.** **Written 2026-10-19** on `worktree-cd-column-slot-cleaning` from `f3ec25ea` (`v3.3.0`), on the operator's W0 (`../v3.2.0/raft-marks-2026-10-19.md` §2): BW's D-e - a `PURGE DROPPED COLUMNS FROM t` pass cleans a dropped column's cells in place under PURGE's discipline (no new version, logged at `kNoTxnId`, a per-row `ResolvedForEveryReader` gate and a statement-epoch gate), flags the column clean, and a later `ADD COLUMN` reuses a clean slot at the same position; row size never changes, resize left for a later milestone. CD-OI-1..14 **await the operator's mark**; not opened |
