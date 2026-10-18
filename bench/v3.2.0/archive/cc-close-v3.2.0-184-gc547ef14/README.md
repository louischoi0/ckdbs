Raw files of `bench/v3.2.0/results-cc-close-v3.2.0-184-gc547ef14.md`. No data files or WAL segments.

- `R1/` and `R2/`: the two full runs (harness `cc_close_run.py`; launched by `run.sh` / `run2.sh`, watched by `watch.sh` / `watch2.sh`).
  `R1/` also holds the partial runs that share its directory: `overhead2` and `overhead3` (`overhead2.py`, `overhead3.py`),
  `extra` and `extra-uniq` (`extra_plan_vs_code.py`), `gain2`, `gain3`, `gain4` (`gain2.py`, `gain3.py`, `gain4.py`) and `restart2` (`restart2.py`).
  Big per-block files (per-statement latencies in microseconds) are gzipped.
- `servers/`: the config and stderr of every server that was started.
- `harness/`: the drivers, wrappers, analysis scripts (`render.py`, `final.py`, `analyze*.py`) and `mkdoc.py` with its template, which
  regenerates the results file from these files. Paths inside are the run's absolute paths under `/home/cdkbs/bench-runs/cc-close/`.
