-- The reads behind the wait-event panel (CJ's WE11): active sessions over
-- time by class, top events, top fingerprints and a fingerprint-to-event
-- drill-down. Data sources are `sys.ash` and `sys.wait_stats` only; no new
-- protocol frame. A `sys` view answers plain filtered selects (no GROUP BY, no
-- aliases: found at WE11), so the client does the counting: `tools/ash_panel.py`
-- runs these over KWP and tallies the samples. `tests/ash_panel_queries_test.cpp`
-- runs every one against the engine, so a column rename in a view breaks the
-- test, not the panel.
--
-- A statement is `-- name: <name>` then SQL up to its `;`. `:since_us` is the
-- only placeholder.

-- name: newest_sample
SELECT sample_ts_us FROM sys.ash ORDER BY sample_ts_us DESC LIMIT 1;

-- name: samples
-- One row per sample. Each core samples at its own instant, so the client
-- buckets `sample_ts_us` by the sampling interval: a bucket's rows per class
-- are that class's active sessions across cores; the other three panels are
-- tallies of the same rows.
SELECT sample_ts_us, class, event, fingerprint
FROM sys.ash
WHERE sample_ts_us >= :since_us
ORDER BY sample_ts_us;

-- name: cumulative_waits
SELECT event, class, count, total_us, max_us
FROM sys.wait_stats
WHERE core IS NULL
ORDER BY total_us DESC;
