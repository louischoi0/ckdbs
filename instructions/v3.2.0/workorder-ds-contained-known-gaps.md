# Work order DS - four contained known gaps: the backup tool behind authentication, a bound on a hung archive command, and two refusals that name no byte

Status: **WRITTEN and OPENED 2026-11-26 on the operator's standing
go-ahead** (`raft-marks-2026-11-26.md` §2), on worktree
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
