R = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05"
for name, old, new in (
    ("doc1.tpl.md",
     "| Tree | `git status --short` at the start: one modified tracked file, `instructions/v3.0.0/workorder-br-backup-restore.md`. Nothing under `src/`, `include/`, `tests/` or `tools/` differs from `a8110b05`; no engine file was edited at any point |",
     "| Tree | `git status --short` at 15:45 UTC, before anything was built: one modified tracked file, `instructions/v3.0.0/workorder-br-backup-restore.md`. Nothing under `src/`, `include/` or `tools/` differed from `a8110b05`, and both binaries were built from `git archive` exports of the two commits, so the worktree's later state does not reach them (at 17:35 UTC `tests/backup_archive_test.cpp` also shows modified, by the session that launched this run; this run did not touch it). No engine file was edited by this run |"),
    ("doc2.tpl.md",
     "| Tree | one modified tracked file, `instructions/v3.0.0/workorder-br-backup-restore.md`; nothing under `src/`, `include/`, `tests/`, `tools/` differs from `a8110b05`; no engine file was edited |",
     "| Tree | `git status --short` at 15:45 UTC, before anything was built: one modified tracked file, `instructions/v3.0.0/workorder-br-backup-restore.md`; nothing under `src/`, `include/` or `tools/` differed from `a8110b05`. The binaries were built from `git archive` exports of the commits, so the worktree's later state does not reach them (`tests/backup_archive_test.cpp` shows modified at 17:35 UTC, by the session that launched this run). No engine file was edited by this run |"),
):
    s = open(f"{R}/{name}").read()
    assert old in s, name
    open(f"{R}/{name}", "w").write(s.replace(old, new))
print("ok")
