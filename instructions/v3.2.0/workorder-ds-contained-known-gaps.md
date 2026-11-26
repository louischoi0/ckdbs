# Work order DS - four contained known gaps: the backup tool behind authentication, a bound on a hung archive command, and two refusals that name no byte

Status: **CLOSED 2026-11-26 on the operator's word**
(`raft-marks-2026-11-26.md` §6), every stage landed on the work branch and
the close measured at `cc46c0d85`
(`bench/v3.5.0/results-ds-close-v3.4.17-151-gcc46c0d85.md`): no `INSERT`
delta reaches the control. **It closes without a green run of the suite at
`cc46c0d85`** - three runs, each failing one or two cells that fail at a
like rate on the commit DS opened from, read as the flaky cells they are on
the operator's choice of CLA's option (a); §7's DS-S5 has the figures.
On the tree merged with `origin/main` (`4055d666a`) the suite ran 5,298
and then 5,299 of 5,299.
**Landed on `main` at `f42341d3`**, 2026-11-26, on the operator's word,
pushed with the hook skipped (§7, "The landing"). Written and opened
2026-11-26 on the operator's standing go-ahead (§2), on worktree
`ds-known-gaps-contained` from `8274c951` (`v3.4.17-148-g8274c951e`).
**DS-Q0..Q8 are adopted on that standing go-ahead** - CLA's proposals, not
the operator's per-item marks. §7 is the stage log.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[design]` CLA's, unbuilt; `[quiet-wrong]` where a wrong
choice turns a correct answer into a wrong one with no error. **No item of
DS is `[quiet-wrong]`.**

Related:
- `docs/inflight/known-gaps.md`: "Backup and restore (BR)", "Column slots
  and `ADD`/`DROP COLUMN` (BW)", "The audit's logic defects (G27-G42)";
- `docs/spec/protocol.md` §1, §3 (TLS below the protocol, the `C_AUTH`
  exchange); `docs/spec/backup.md`; `manual/server/backup-restore.md`;
- `include/kds/server/wal_archive.hpp` (the archiver's contract).

## 0. What DS is

The operator asked, on 2026-11-26, which known gaps stand first between
this engine and commercial use, ordered by how little a fix touches, and
then for the ones that are **low in impact and need no operator decision**
to be worked as one milestone. DS is that milestone: four entries of
`known-gaps.md`, each closed inside one component, none moving a persisted
format, a statement's answer, a lock, or a rule a spec states.

What was asked about and left out, and why, is §6.

## 1. The tree as read `[source-read]`, at `8274c951`

1. **The backup tool cannot authenticate.** `tools/kwp.py:31` - "No TLS
   (connect to a plaintext port), no SCRAM (`auth = off`)". Its handshake
   (`tools/kwp.py:359`) sends `auth_method` 0 and no capability bit.
   `tools/kds_backup.py:84`, `:101` connect through
   `ckdbs_cli.ServerConnection(host, port, timeout)`. `BACKUP START` is
   `admin`-classed, so on an `auth = scram` server the tool reaches
   nothing. The server's half is built: `C_HELLO.auth_method = 1`
   (`include/kds/wire/kwp_types.hpp:147`), one SCRAM message a `C_AUTH` /
   `S_AUTH` frame, the body the gate's own line (`src/server/auth.cpp:99`,
   `AUTH SCRAM-SHA-256 <client-first>`, `AUTH+ <server-first>`, `AUTH
   <client-final>`, `AUTH+ <server-final>`), `S_HELLO` after it; TLS 1.3
   direct, below the protocol (`protocol.md` §1).
2. **A hung `archive_command` holds the archive for good.**
   `src/server/wal_archive.cpp:206` waits on the child with a blocking
   `waitpid`; only `Stop()` signals it (`:112`). The archive holds the
   log's recycling and fails closed, so one command that never exits
   stops recycling until a restart.
3. **An `INSERT` value's type refusal names no byte.**
   `exec::EncodeRow` (`src/exec/row_codec.cpp`) refuses a mistyped value
   as "column 'a' expects an integer", and
   `CommandDispatcher::InsertOneRow`
   (`src/server/command_dispatcher.cpp:8928`) passes it through. Every
   value the parser produced carries `AstValue::byte_offset`
   (`include/kds/parser/ast.hpp:81`); a value built by anything else
   leaves it 0.
4. **`RENAME TO` refuses a name too long with no byte.**
   `src/server/command_dispatcher.cpp:4812-4817`: the column arm maps its
   refusals to `stmt.new_name_byte_offset`; the relation arm returns the
   catalog's status as it is.

## 2. Rules

- **DS-R1.** No persisted format, record kind, superblock word or
  fingerprint moves.
- **DS-R2.** No statement that was answered is refused, and no refusal
  changes its code; a refusal gains its byte and nothing else.
- **DS-R3.** A new setting's default leaves the engine as it was.
- **DS-R4.** A password never appears in an argv, a log line or an error.
- **DS-R5.** The tools stay on Python's standard library.

## 3. Design `[design]`

**S1, the client.** `kwp.Connection` takes `user`, `password` and `tls`
(an `ssl.SSLContext` or none). With a user it sends `auth_method = 1` and
runs RFC 5802/7677 SCRAM-SHA-256 with `hashlib`/`hmac`: gs2 header `n,,`,
no channel binding (the server offers none), the iteration count bounded
to 4096..10,000,000 as both server ends bound it, **and the server's
signature verified before `S_HELLO` is read** - a client that skips it has
authenticated nobody. With TLS the socket is wrapped before the first
byte, TLS 1.3 the floor, and the hello carries `TLS_REQUIRED`.
`kwp.add_connection_arguments` / `kwp.connection_options` give every tool
one spelling: `--user`, `--tls`, `--tls-ca <file>`, `--tls-insecure`; the
password is read from `KDS_PASSWORD`, else prompted on a terminal, else
refused. `ckdbs_cli.py` and `kds_backup.py` take them.

**S2, the archiver.** `archive_command_timeout_s`, a boot key, default
`0` = no bound. Set, a command still running at the bound is sent
`SIGTERM`, and `SIGKILL` 5 s later if it has not exited; the attempt is a
failure naming the bound, counted and retried under the existing backoff.
The segment stays unarchived, so nothing is recycled on a killed command's
word.

**S3, the encode refusal.** `EncodeRow` appends ` (byte N)` to a value's
refusal where the value's `byte_offset` is not 0. The codec already takes
`AstValue`s, so nothing is threaded.

**S4, the rename.** The relation arm maps `InvalidArgument` and
`AlreadyExists` to `stmt.new_name_byte_offset`, as the column arm does.

## 4. Items (DS-Q), CLA's proposals

| Item | Question | CLA's proposal (a) | Other options | Mark |
|---|---|---|---|---|
| DS-Q0 | Is DS opened, and with what scope? | the four gaps of §1, one stage each | (b) fewer; (c) the wider list of §6 | (a), adopted on the operator's standing go-ahead of 2026-11-26 (go-ahead-achieving-milestone) |
| DS-Q1 | The letter. | DS | (b) another | (a), adopted as DS-Q0 |
| DS-Q2 | Where a tool's password comes from. | `KDS_PASSWORD`, else a terminal prompt, else a refusal; never an argument (DS-R4) | (b) a `--password` flag; (c) a password file | (a), adopted as DS-Q0 |
| DS-Q3 | What a TLS client verifies by default. | the server's certificate and name against the system's CAs, or `--tls-ca`'s file; `--tls-insecure` turns both off and says so on stderr | (b) no verification by default | (a), adopted as DS-Q0 |
| DS-Q4 | The archive bound's name and default. | `archive_command_timeout_s`, `0` = off (DS-R3) | (b) a default bound; (c) a fixed constant with no key | (a), adopted as DS-Q0 |
| DS-Q5 | What a command past its bound gets. | `SIGTERM`, then `SIGKILL` after 5 s; a failure under the existing backoff | (b) `SIGKILL` at once; (c) a log line and no signal | (a), adopted as DS-Q0 |
| DS-Q6 | Does the bound apply to `archive_dir`'s copy? | no: the copy is the server's own I/O, bounded by the device, and a key named for the command says so | (b) yes | (a), adopted as DS-Q0 |
| DS-Q7 | The close measurement. | one interleaved A/B in `build-release` of a one-row and a multi-row `INSERT`, `8274c951` against the close: S3 is the one stage on a statement's path, and only on its refusal | (b) none, stated as not measured | (a), adopted as DS-Q0 |
| DS-Q8 | How S1 is tested, the tools having no cell in `ctest`. | a driver under `tools/` that starts a server with `auth = scram` and `tls = on`, runs the client against it - a good password, a bad one, a plaintext client on the TLS port, a base backup through `kds_backup.py` - and is run by hand at the stage; its output is in §7 | (b) a `ctest` cell that needs Python and OpenSSL's CLI on the host | (a), adopted as DS-Q0 |

## 5. Stages

| Stage | What | Exit |
|---|---|---|
| DS-S1 | the client's SCRAM and TLS; the two tools' flags; the driver of DS-Q8 | the driver green; the suite green |
| DS-S2 | `archive_command_timeout_s` | cells in `backup_archive_test.cpp`: a command past the bound is ended and counted, the segment unarchived, the next attempt archives; `0` waits; the suite green |
| DS-S3 | the encode refusal's byte | a cell per value kind, positional and column-listed, one row and many; the suite green |
| DS-S4 | the relation rename's byte | a cell for the name too long and for the name taken; the suite green |
| DS-S5 | the close | DS-Q7's measurement; `known-gaps.md`, the specs, the manual, the index rows |

Each stage lands carrying "overhead not measured; measured at the
milestone's close".

## 6. What DS does not do

Each was in the operator's list of ten and is left where it is:

- **A text parameter holding a single quote** (`known-gaps.md`, "The read
  and update paths (BN)"): the lexer's "No escaping, deliberately"
  (`src/parser/lexer.cpp`) is a decision about the string-literal grammar,
  and an escape moves what a fingerprint folds.
- **A positional `INSERT` after a `DROP COLUMN`** `[quiet-wrong]`: BW-Q9
  and BW-Q11 are marked.
- **A departure that raises a `SUM`** `[quiet-wrong]`: narrowing AS11
  waits for the operator, as its entry says.
- **PITR to a time, the dirty pool's refusals, the spilled value an
  `UPDATE` re-assigns, the leaked pages, page 0's second copy**: each
  moves a record, a format or a subsystem's protocol.

## 7. Stage log (CLA, appended as stages land)

### DS-S1 - the client's SCRAM and TLS (2026-11-26)

On `ds-known-gaps-contained` over `8274c951`. `tools/kwp.py` runs
SCRAM-SHA-256 before `S_HELLO` and wraps its socket in TLS 1.3;
`ckdbs_cli.py` and `kds_backup.py` take `--user`, `--tls`, `--tls-ca`,
`--tls-insecure`. `tools/ds_auth_drill.py` (DS-Q8) is 28 checks against an
`auth = scram` server, plain and `tls = on`: admitted and refused sessions,
a readonly role held to its class, a forged server-first and server
signature, both tools through both transports, a base backup verified.
**28 of 28, ten runs in a row** after the review's changes.

**Review** (`critics-developer`), no security defect; the RFC 7677 vector
reproduced byte for byte through `_scram`. Applied:
- `ckdbs_cli.py --text --user` asked for a password before refusing; the
  refusal now reads the raw arguments first.
- the server-first message is parsed in order (`r=`, `s=`, `i=`, digits
  only), as `scram.cpp`'s client does: a repeated attribute or a leading
  `m=` was let through to the signature check, fail-closed but looser than
  the grammar.
- a refused authentication and an `e=` server-final each have their own
  text; eight drill checks that accepted any exception now name theirs,
  and three were added.
- `kds_backup.py` ends a failed connect in one line, not a traceback.

Rejected: an `argparse` mutually exclusive group for `--tls-ca` /
`--tls-insecure` - one branch saved, and the refusal's wording would
become `argparse`'s.

One drill run in the reviewer's session reported a failure it did not
capture; ten runs since have not shown it. Not explained.

**Suite**: 5,224 of 5,224 at `-j8`, less `bs-nightly`, on this tree's
build inputs (S1 changes none). Overhead not measured; measured at the
milestone's close.

### DS-S2 - `archive_command_timeout_s` (2026-11-26)

On `ds-known-gaps-contained` over `5ae44e5f`. A boot key, `0` by default
and then nothing changed: the old spawn and the old blocking wait. Set, the
command leads its own process group; a 20 ms poll reads the bound and a
stop; `SIGTERM` at either, `SIGKILL` 5 s later, the group cleared once more
at the shell's exit before the exit is reaped. Refused at boot without
`archive_command`.

**Review** (`critics-developer`), three findings applied, each a change of
behaviour from the first draft:
- **a signalled command archived nothing, whatever it exited with.** The
  draft took an exit 0 read after the bound as the command's word; a
  handler that cleans up and exits 0 would have had its segment recycled
  with no copy in the archive. §3's "nothing is recycled on a killed
  command's word" now holds as written.
- **a stop gives a bounded command the grace, not the rest of its bound**:
  the wait reads the stop, where the draft would have held a shutdown for
  `bound + 5 s` on a command that ignores `SIGTERM`.
- **the group is cleared before the shell is reaped** (`waitid` with
  `WNOWAIT`), so the signal cannot reach a group that took the id over.
- the cells read a pid file only once its line is whole, and count a zombie
  as ended; cells added for the stop, the exit 0 and "`0` waits".

Rejected: refusing a value above 32 bits inside the config's `number()` -
it would change every numeric key's behaviour, outside DS; `known-gaps.md`
states the cast.

**Cells**: eight in `backup_archive_test.cpp`, one in
`config_file_test.cpp`; 13 archiver and config cells, three repeats, green.
**Suite**: the first run failed 1 of 5,229,
`DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`, the kick race
`known-gaps.md` ("Testing") names, 3 of 6 alone and untouched by this
stage; **the run on the reviewed tree is 5,232 of 5,232** at `-j8`, less
`bs-nightly`. Overhead not measured; measured at the milestone's close.

### DS-S3 - an `INSERT` value's refusal names its byte (2026-11-26)

On `ds-known-gaps-contained` over `e8597253`. `exec::EncodeRow` appends
the value's byte to a refusal the value earned (`AtValue`,
`src/exec/row_codec.cpp`), by code: `InvalidArgument`, `OutOfRange`,
`Unsupported`, `NotImplemented`. Six cells in
`tests/insert_column_list_test.cpp` (`InsertValueByteTest`): eight kinds
of bad value, positional, key-omitted and column-listed, a later row of a
multi-row statement, an expression, a NULL named once, a selected row.

**Review** (`critics-developer`), by running a server as well as reading.
Applied:
- **an `INSERT ... SELECT` over a view reported a byte in the view's
  text** (`(byte 11)` for a statement whose byte 11 is a space): a literal
  keeps its `AstValue` through the query layer and a view's body is parsed
  from its own text. `InsertSelectRows` now clears every selected value's
  offset, so a selected row's refusals name no byte. **This also changes
  two refusals older than DS** that read the same offset - the NULL
  refusal and the unique check's byte - for a selected row: they named a
  byte, right for the statement's own literal and wrong for a view's, and
  name none now.
- the NULL refusal prints no `(byte 0)` for a value no token wrote.
- the guard against a second byte is the value's kind, not a search of
  the message for `(byte `, which a quoted user string could satisfy.
- `NotImplemented` joined the codes: an over-long spill is the value's.
- `known-gaps.md` had said an `UPDATE`'s literals are refused at compile
  with their byte. False: a bare literal is refused per row by the
  encoder with none (BJ-Q3 (c)). The entry now states that as the gap.

### DS-S4 - a relation rename's new name is refused at its byte (2026-11-26)

On the same tree. The relation arm of the rename asks `NameFitsAt` before
the catalog and maps the catalog's `AlreadyExists`, `TxnConflict` and
`Unsupported` - `CheckNameFree`'s three - to the new name's byte. One cell,
`AlterTableTest.ARelationRenamesNewNameIsRefusedAtItsByte`, asserting the
code as well as the byte.

**Review** (`critics-developer`): no defect; the order of refusals is
unchanged, since the fit check was already the catalog door's first
statement. Applied: `TxnConflict` and `Unsupported` carry the byte, which
`alter.md` §7 had promised ("each with a byte") and the code did not give;
§7 now also says which of a rename's refusals carry none; the cell asserts
the status code. Rejected: one `AtByte` helper for the three sites that
build ` (byte N)` - it would rewrite the column arm, outside this stage.

**Suite, S3 and S4 together**: 5,237 of 5,237 before the reviews' changes.
On the reviewed tree, 5,238 of 5,239: the one failure is
`DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`, the kick race
of `known-gaps.md` ("Testing"), as at DS-S2. **A second run of the
reviewed tree was still running when the session was handed off, and its
result is not in this log**; the 70 cells of the touched suites are green.
Overhead not measured; measured at the milestone's close.

### DS-S5 - the close (2026-11-26)

On `ds-known-gaps-contained` at `cc46c0d85`, in the session that took §8's
handoff, on the operator's word (`raft-marks-2026-11-26.md` §6). The
section is below §8 in time and above it on the page.

**The suite §8 owed did not run green, and the close proceeds on the
operator's choice.** Three runs at `cc46c0d85`, `-j8`, less `bs-nightly`:

| Run | Passed | Failed |
|---|---|---|
| 1 | 5,238 of 5,239 | `ContentionCountersTest.TwoCoresInsertingIntoOneRelationContendWhatAnInsertTakes` |
| 2 | 5,238 of 5,239 | the same cell |
| 3 | 5,237 of 5,239 | the same cell, and `SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement` |

Both cells fail on the test binary built from `8274c951`, the commit DS
opened from, eight copies at once and the two binaries alternating: the
first 8 of 32 on each; the second 163 of 400 there and 196 of 400 at
`cc46c0d85`, a difference the rounds do not resolve (of the 50 rounds the
later binary failed more in 25, the earlier in 15, 10 tied; the last 40
alone 19, 14 and 7). DS's
engine change is on neither cell's path: a successful `INSERT` and an
`UPDATE` run none of its lines. The first cell's passes take multiples of
the rig's 5 s idle block, whole blocks waited out with the missing wake
not observed (`known-gaps.md`, "Testing"); the second fails by its retry
budget and loses no increment
(`docs/inflight/bugs/two-core-increment-starves-one-core.md`).
`DurableKickRigTest`'s cell, which failed at DS-S2 and at DS-S4, passed in
all three. **CLA put three options
to the operator - (a) read the two as the flaky cells they are and close,
the suite reported by its figures and never as green; (b) run again until
a run is green; (c) fix the cells first - and the operator chose (a).**

**DS-Q7's measurement**, `ck-tester`, `build-release`,
`bench/v3.5.0/results-ds-close-v3.4.17-151-gcc46c0d85.md`: `tools/dr_close.py
--part ab` unmodified, `8274c951` (A), a second copy of it (A2, the
control) and `cc46c0d85` (B), `cores = 1`, `relaxed`, 100,000-row
relations, 5 rounds, run twice. Server CPU, B - A, run 1 and run 2, with
the control's A2 - A:

| Statement | Relation | B - A | A2 - A |
|---|---|---|---|
| one-row `INSERT` | btree | +0.09 and +0.01 us a statement (+1.4 %, +0.2 %) | +0.18 and +0.13 us (+2.8 %, +2.0 %) |
| one-row `INSERT` | heap | +0.11 and +0.01 us a statement (+1.4 %, +0.1 %) | +0.28 and +0.26 us (+3.4 %, +3.2 %) |
| 500-row `INSERT` | btree | +0.005 and +0.003 us a row (+0.4 %, +0.2 %) | +0.002 and +0.001 us a row |
| 500-row `INSERT` | heap | +0.019 and +0.010 us a row (+0.6 %, +0.3 %) | +0.020 and +0.016 us a row |

**No delta reaches the control.** Of the driver's other phases two are
outside it in some run, each on a statement DS does not touch and left
unexplained: a walked `UPDATE` reads 0.7 to 0.8 % cheaper on B in three of
four cells, not called a gain, and a heap's 100-row range 1.5 % dearer in
run 1, above that run's control (+0.4 %), and 1.3 % in run 2, inside its
+1.4 %. Not measured: a refused `INSERT` (the one path S3 adds
instructions to), an `INSERT ... SELECT` (the loop S3 added), `strict`.
The time: about 3 minutes of builds, 162 s of runs, 5 s held at the quiet
gate. The same driver's last file, DR's close, was measured on another
host and no delta is taken against it.

**Documents.** `known-gaps.md`: the first cell's figures ("Testing"), and
what §8 found and left, corrected at S5's review - `tools/kds_backup.py`'s
doubled quote is read back by BACKUP's own word splitter, and no cell
sends one ("Backup and restore"). The second cell's figures are in its bug
file. `CLAUDE.md`: the archive bound and the tool's authentication in the
Backup row. The order's rows in `index.md` and `milestones.md`. The specs
and the manual were brought to the code by the stage that changed it (S1
to S4) and S5 changes none.

**The merge.** `origin/main` at `6c98f7c32` - DT, DU and the twelve orders
written the same day, source included - merged into the work branch at
`4055d666a`. Conflicts in `CLAUDE.md`, `milestones.md`, `index.md` and the
day's raft marks, each resolved by keeping both sides;
`command_dispatcher.cpp` merged without one. **The suite on the merged
tree**, `-j8`, less `bs-nightly`: 5,298 of 5,299, the one failure
`DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter` ("kicks skipped
1", the cell `known-gaps.md` gives five of 30 alone at `60dc593af`); then
**5,299 of 5,299**. The two cells that failed at `cc46c0d85` passed in
both.

**Review** (`critics-developer`), of S5's documents and the merge, by
reading; nothing built or run. The merged code is sound: DS's change and
DT's and DU's each arrive whole, nothing DT or DU added reads a value's
byte offset, and `EncodeRow` keeps its two callers. Six claims in S5's
documents were wrong or said more than was seen, and each is corrected:
- **the `quote()` entry was false.** §8 and S5 read the lexer; a `BACKUP`
  statement never reaches it (`SplitBackupWords`, which reads `''` as one
  quote), so the tool's doubling is the matching escape. The entry now
  says what is true: no cell sends a quote, and the round trip is not run.
- the first cell's 5 s multiples were written as skipped kicks. A second
  mechanism gives the same durations - the cell raises each script's `go`
  flag with no kick - and only an expiry singles out the writer's kick.
  The entry names both and says the missing wake was not observed.
- the second cell's rounds were quoted as the last 40 of 50.
- a second phase is outside the control in one run (a heap's 100-row
  range, run 1), where the order and the results file said one.
- two counts and an end time in the results file.
- `index.md`'s header still said DS was on its own branch.

Applied at CLA's reading of the rest: `known-gaps.md` carried each of the
two cells twice after the merge, DT's and DU's records beside DS's, and
each is now one entry - DS's figures folded into DT's entry for the first
cell and into DU's bug file for the second. Rejected: citing §7 from the
raft marks and the index row in place of the suite's figures - a mark
carries what it rests on, as the day's other marks do.

**The landing.** On the operator's word ("merge to main and push",
`raft-marks-2026-11-26.md` §6): `worktree-ds-known-gaps-contained` at
`96468568` merged `--no-ff` onto `main` at `6c98f7c32`, the merge
`f42341d3`, its tree the work branch's, and pushed with `--no-verify`.
**The hook was skipped**: the full suite had run 5,299 of 5,299 at
`4055d666a`, whose build inputs are the pushed tree's - after a run of
5,298 there, and none green at `cc46c0d85`. No tag, no version.

## 8. Handoff (2026-11-26 14:21 UTC)

The session ended here on the operator's word ("handoff"). State:

- **Landed on the work branch** `worktree-ds-known-gaps-contained`:
  DS-S1 `5ae44e5f`, DS-S2 `e8597253`, DS-S3 and DS-S4 in the commit that
  carries this section (one commit: both stages touch
  `command_dispatcher.cpp`). Nothing is merged to `main` or pushed.
- **Owed before DS-S5**: one green run of the full suite on this commit
  (`ctest --test-dir build --output-on-failure -LE bs-nightly -j8`); the
  log of the run in flight at the handoff is the session's scratchpad
  file `ds-s34-ctest3.log`, which a reboot clears.
- **DS-S5, not started**: DS-Q7's measurement - `ck-tester`, an
  interleaved A/B in `build-release` of a one-row and a multi-row
  `INSERT`, `8274c951` against the closing commit, to
  `bench/<version>/`; then the index and register rows flipped to closed,
  `CLAUDE.md`'s rows (WAL or Backup for the archive bound; the client's
  authentication under Wire protocol or Backup), `git fetch origin && git
  merge origin/main` on the work branch, and the report at the push gate.
- **`git describe --tags` reads `v3.4.17-...` in this clone**: `CLAUDE.md`
  names `v3.5.0` as a local, unpushed tag, and this clone does not have
  it. A results file takes whatever `git describe` prints.
- **Found and left**: `tools/kds_backup.py`'s `quote()` doubles a single
  quote, which the lexer does not read as an escape, so a backup target
  holding one cannot be spelled; no entry was written. (DS-S5 wrote it,
  and its review found the reading wrong: a `BACKUP` statement never
  reaches that lexer, and `SplitBackupWords` in
  `src/server/command_dispatcher.cpp` reads `''` as one quote, so the
  doubling is the matching escape.)
