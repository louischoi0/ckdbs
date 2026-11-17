# Pending: the WAL archiver stalls on a hole among leftover segments

Status: **PENDING** - the operator deferred it on 2026-11-17 with the other
logic defects of the audit fixed. Decides nothing, opens no stage, licenses
no code.
Raised: 2026-11-13 by the whole-engine audit's second run, as its finding
G31; verified 2026-11-17; deferred 2026-11-17.
Read at: `3c30fe67` (`v3.4.12-11-g3c30fe67`), on worktree
`cw-optimistic-page-read`, branch `worktree-audit-g27-g42-fixes`. Every
`path:line` below is at that commit.
Related: `docs/pending/whole-engine-audit-second-run-twelve-slices.md` (the
finding under `#### G31.`, its two verdicts in §11.1, and §12, which fixed
the audit's other logic defects and left this one);
`docs/spec/backup.md` B11 and B13; `manual/server/backup-restore.md` §3-§4;
`instructions/v3.0.0/workorder-br-backup-restore.md` (BR-Q2 (a), BR-R8);
`instructions/v3.2.0/raft-marks-2026-11-17.md` §9.

## 1. What prompted it

`[operator]` On 2026-11-17, after CLA reported thirteen of the audit's
fifteen surviving logic defects fixed and two waiting for a ruling: *"For
now put the two issues in pending, and push the work so far, no verify."*
(given in Korean; recorded in English). This is one of the two; the other
is `well-known-name-refusal-in-create-namespace.md`.

## 2. What the tree does

`[source-read]`

- At a mount the archiver's first segment and the recycler's hold are both
  the lowest segment file on disk: `src/server/expeditor.cpp:1228`, `:1235`,
  through `FileLogDevice::lowest_segment()`
  (`src/wal/file_log_device.cpp:649`), the minimum over the live run and
  every leftover file below it. The open queues every such leftover with no
  test that they are contiguous, and the device's contract says they need
  not be (`include/kds/wal/file_log_device.hpp:74`, "gaps among them are
  expected").
- The archiver archives in segment order and never skips: a failed
  `ArchiveOne` leaves `next_` where it was and the loop ends for that pass
  (`src/server/wal_archive.cpp:130-149`). A segment whose file is not there
  fails every attempt, in `archive_dir` mode and in `archive_command` mode
  alike.
- Its floor is the recycler's bound (`src/server/expeditor.cpp:1241`), so
  nothing at or above the hole is recycled, the log grows, and once the WAL
  filesystem has fewer than four segments free every statement that writes
  the log is refused by name (B13).

So leftovers `{0, 2}` below a live run that starts at 5 stall the archiver
at segment 1 for the whole run.

## 3. What the verification narrowed

`[source-read]`, two readers told to refute it; both let it stand, at
medium confidence.

- **A process crash alone cannot leave the hole.** Unlinks run in ascending
  order (`src/wal/file_log_device.cpp:656-702`), so a kill mid-reclaim
  leaves a contiguous suffix. A hole needs a power loss on a filesystem
  that does not order unsynced unlinks, one unlink failing while later ones
  succeed (the failed ones are requeued, `:702`), or a file removed by
  hand.
- **The stall lasts a run, not for ever.** Every recycle call still asks
  for a reclaim, which removes the leftovers below the hole; each restart
  therefore clears one hole. Nothing tells the operator that a restart
  helps: the manual's remedy for a failing archive is "fix the archive
  destination; nothing else is needed", which is false here.
- **With archiving on, a hole lies below the previous run's hold**, since
  the device never unlinks at or above it. A segment that is missing was
  therefore archived before it was removed, and the stall is a false alarm
  about the archive, not a hole in it. That holds only while archiving was
  on when the hole formed.
- The comments disagree with the code: `include/kds/server/wal_archive.hpp:39`
  and `src/server/expeditor.cpp:1221` say the archiver resumes from "the
  first live segment"; the code takes the lowest on disk, on purpose
  (`expeditor.cpp:1226-1227`).

## 4. The options

`[design]`

- **(a) Start at the lowest contiguous segment.** A
  `FileLogDevice::lowest_contiguous_segment()` - the lowest segment from
  which every segment up to the live run is present - used at both sites.
  Leftovers below a hole are then reclaimed unarchived. Sound on the
  premise of section 3's third point; **not** sound for a volume on which
  archiving was enabled between two runs, where a leftover below a hole was
  never archived and would be removed so. It needs one sentence in
  `backup.md` B11 saying a leftover below a hole is not archived, and it
  changes what `wal_archive.hpp:36`'s "never skips one" covers. The
  verifiers' fix; low to medium risk.
- **(b) Skip a missing leftover, and say so.** The archiver passes over a
  segment below the live run whose file is absent, counts it and names it
  in `SHOW META`, and archives the leftovers on both sides of the hole.
  Nothing present is removed unarchived. It makes "never skips one" false
  in words where (a) makes it false in effect, and it needs a rule for
  which absences are skippable (below the live run only).
- **(c) Leave the behaviour, tell the operator.** The stall is fail-closed
  and loud. State in `backup.md` and the manual that a missing leftover
  segment stalls the archive for the run and that a restart clears one
  hole; correct the two comments. No code but the comments.

## 5. What a ruling has to say

Whether B11's "every sealed segment is archived before it can be removed"
is a statement about segments the engine removes while archiving is on -
which (a) keeps - or about every segment file that exists when archiving
starts, which only (b) and (c) keep. The two lenses split on exactly that:
one read (a) as needing the operator's word, the other as needing only a
sentence in the spec.
