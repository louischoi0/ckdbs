# eqbench - one client for three engines

EO's harness (`instructions/v3.2.0/workorder-eo-three-engine-equal-client.md`,
`bench/README.md` "Three engines on one client"): one Rust binary drives KDS
(KWP/1), PostgreSQL and Oracle Database Free through the same scenario code,
the same seeded parameter sequence and the same statements, and writes one
JSON per run.

    cargo build --release                      # target/release/eqbench
    ./servers.sh pg-up 15434 [profile]         # PostgreSQL 18 at its defaults
    ./servers.sh ora-up <secret-file>          # Oracle Database Free on 1521
    ./servers.sh kds-up <binary-copy> <dir> <port> group|strict <cpus>
    eqbench --engine kds|pg|oracle ... --scenario <name> [--set k=v]... --json out.json

Oracle's client libraries are the image's own (`libclntsh` 23.1, Oracle
26ai Free): copy `lib/`, `nls/`, `oracore/`, `rdbms/mesg` and
`network/mesg` of `$ORACLE_HOME` out of `gvenzl/oracle-free:slim` with
`podman cp`, then run with `ORACLE_HOME` set to the copy and
`LD_LIBRARY_PATH=$ORACLE_HOME/lib`. Nothing of Oracle's is committed.

## What is equal, and what is not

| | KDS | PostgreSQL | Oracle |
|---|---|---|---|
| client | `src/kwp.rs`, written for the harness | `postgres` 0.19 (pure Rust) | `oracle` 0.6 over ODPI-C and OCI (C) |
| statement | a named statement, parsed once (EA's kept parse) | a prepared statement (extended protocol) | an OCI statement handle, built once |
| one execution | `C_BIND` `C_EXECUTE` `C_CLOSE` `C_SYNC` in one write | `Bind` `Execute` `Sync` in one write | one `OCIStmtExecute` round trip, fetch array and prefetch 10,000 rows |
| values | binary, decoded by `kwp.rs` | binary, decoded by `pg.rs`'s `AnyVal` | OCI's buffers, read by `ora.rs` |
| `BEGIN` | a `C_TXN_BEGIN` round trip | a `BEGIN` round trip | none: a transaction opens at its first statement |
| autocommit | the server's | the server's | OCI's commit-on-success, inside the execute's round trip |
| `COMMIT` | `C_TXN_COMMIT` | `COMMIT` | `OCITransCommit` |

The decode is timed apart from the round trip on every engine
(`client_decode_us_per_op`), and the harness thread's CPU is counted per
operation (`client_cpu_us_per_op`), so the residue of the three clients'
difference is in every cell.

## The breakdown (EO-Q8)

`src/profile.rs`'s table maps each engine's own accounting to `parse`,
`optimise`, `parse_optimise`, `execution`, `disk_read`, `commit`, `lock`,
`network`, and the harness adds `formatting_client`. A `null` is "not
reported by the engine", never 0. PostgreSQL's breakdown needs
`servers.sh pg-up <port> profile` (`pg_stat_statements`, `track_io_timing`,
a 1 ms wait sampler): it is a second pass, the timed pass running at the
defaults.

## Relation kinds

Each scenario's file opens with its relations' KDS kinds and the reason
for each, by `bench/README.md`'s rule ("What a relation is stored as").
PostgreSQL and Oracle store every relation as their own heap table,
untuned.
