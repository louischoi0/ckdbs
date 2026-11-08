# Raw outputs of CP-S0 (v3.4.7-1-gcc54a296)

Results file: `../../results-cp-s0-premise-v3.4.7-1-gcc54a296.md`.

- `pass1/`, `pass2/`: the two passes' per-cell directories (JSON summaries, driver stdout, `sys` view dumps `*.views.txt/.json`, manifests, timelines, `host.log`, console logs). `pass1/m2/census.json.gz` and `pass1/m3/s0-post.views.json.gz` are gzipped. Pass 1's `status.txt` was overwritten by pass 2's; `pass2/status.txt` is pass 2's.
- `m5-pass2/`: M5's cells (pass 2); `m5-attempt1/run.txt`: the record of the lost first attempt.
- `scripts/`: the run scripts, launcher (`m1m4_srv.py`), view readers, watchdog, host sampler, `build-baseline.log`.
- `patches/`: the two measurement-build patches (never landed).
- `tools-at-run/`: copies of the two new drivers and the rig cell **as run**. The committed `tools/cp_s0_release_order.py` differs after the stage's review: its `--protocol kwp` mode was deleted (the text mode, the one S0 ran, is unchanged), so `scripts/m6_run_m6_script.sh`'s `--protocol text` argument is accepted only by the copy here; `tools/cp_s0_fence.py` lost docstring and a helper, no flag. The committed rig cell differs in a comment, one `KickUntil` and one deleted redundant `EXPECT`.
- Not archived: data files, WAL segments, server logs (`*.kdb.log`), M2's `wal-0-1.log`.
