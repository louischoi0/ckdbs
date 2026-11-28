# EO close, three engines: KDS, PostgreSQL 18.7 and Oracle Database Free 26ai through one Rust client

**INTERNAL ANALYSIS ONLY. NOT FOR MARKETING, NOW OR LATER.** Committed on
the operator's word of 2026-11-28 (`bench/README.md`, "The Oracle
control"). The Oracle licence's text on publishing benchmark results was
not read (HTTP 403).

The same run as `bench/v3.5.1/results-eo-close-v3.5.1-63-g9543cf720.md`
(see its Stamp). Oracle: `gvenzl/oracle-free:slim`, Oracle AI Database
26ai Free Release 23.26.4.0.0, at its defaults (Free's own cap: 2 CPU
threads, 2 GB), host network on 1521, **fresh once a round** (KDS and
PostgreSQL are fresh every cell), every process pinned to CPUs 0 to 7. Its
client is the `oracle` crate over OCI, with `libclntsh` 23.1 from the
image, string binds as `VARCHAR2`, and a fetch array and prefetch of 10,000
rows. Every Oracle check passed in every round.

## What the three-engine view adds

1. **On a single client's short reads, the three engines are close.** A
   point read (s3 `pk-user`) is 22.1 µs to 34.4 µs across engines and
   cells, and an indexed loans read 20.4 µs to 29.7 µs, with spreads up to
   62 %, so no one engine leads on them. **Oracle's client is the dearest on rows**:
   109.3 µs of decode for scenario 1's 201-bar range, against KDS's
   16.6 µs and PostgreSQL's 7.5 µs, and 6,328.6 µs against 681.5 µs for
   7,560 rows. The `oracle` crate's per-value work is the cause, not the
   server: Oracle's server CPU for that range is 49.3 µs against KDS's
   40.7 µs. So for multi-row reads, Oracle's wall clock here overstates
   its server time.
2. **Oracle's walks are ahead of KDS's on some shapes:**
   - scenario 4's board 71.4 µs against KDS's 133.8 µs (KDS `off`; KDS's
     declared Cabin gives 64.1 µs);
   - scenario 4's 10,000-row tape 66.4 µs against 132.1 µs;
   - scenario 1's day slice at 60,480 bars 450.1 µs against 666.6 µs (its
     execution 392.0 µs against 628.3 µs);
   - `agg-by-session` 0.72x.

   KDS's walk is ahead of PostgreSQL's but not of Oracle's on these.
   Whether the cause is Oracle's block format or its predicate evaluation
   was not checked; Oracle's `V$SQL_PLAN` for the board statement would
   say.
3. **KDS's read-only commit cost stands out against both**:
   - scenario 5's order status: KDS 433.4 µs, PostgreSQL 82.7 µs, Oracle
     73.8 µs;
   - its stock level: 482.7 µs, 187.8 µs, 124.6 µs.

   Oracle books no `log file sync` for these: nothing to commit.
4. **A writing commit**: Oracle's `log file sync` in scenario 5's payment
   is 210.6 µs an operation, between PostgreSQL's sampled 108.2 µs and
   KDS's 456.6 µs.
   - scenario 0, one trader: KDS `strict` 699.8 µs, PostgreSQL 643.0 µs,
     Oracle 1,163.7 µs;
   - scenario 2's booking: KDS 323.1 µs, PostgreSQL 337.7 µs, Oracle
     438.7 µs.

   KDS is ahead of Oracle on both of those, and behind both on scenario 5.
5. **Free's CPU cap is visible in every multi-client cell.** At 8
   connections Oracle books `resmgr:cpu quantum`, 41.3 µs an operation in
   scenario 1's join. Even so, Oracle at 2 threads is ahead of KDS at one
   core: 803.2 µs against 5,234.9 µs at `join-conn-8`, and 1,369.8 µs
   against 2,404.4 µs on scenario 5's four-terminal mix. That says KDS's
   one-core configuration, not a property of the engine
   (the public file, item 3).
6. **Scenario 2's customer statement**: KDS 881.1 µs, Oracle 882.8 µs,
   PostgreSQL 147.5 µs. The same join is equally slow on KDS and Oracle,
   so PostgreSQL's plan is the outlier. KDS's gap is a plan question, not
   an executor defect (public file, item 4).

## Ratios and tables

- `ratios-eo-three-engines-v3.5.1-63-g9543cf720.txt`: every phase's
  median of round means per engine arm, with KDS/PG and KDS/Oracle rate
  ratios.
- `tables-eo-three-engines-v3.5.1-63-g9543cf720.md`: `report.py
  --engines kds,pg,oracle`, every phase's percentiles, CPU, breakdown and
  events, Oracle's from `V$SESS_TIME_MODEL`, `V$SESSTAT` and
  `V$SESSION_EVENT`.
- `eo-close-v3.5.1-63-g9543cf720/timed/`: Oracle's raw JSONs.
