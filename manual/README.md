# KDS Manual

The user-facing manual, one section per audience, each verified against
code rather than specs. Engine internals live in `docs/`; the engine-wide
gap list is `docs/inflight/known-gaps.md`.

| Section | Covers |
|---|---|
| [SQL reference](sql/sql.md) | Everything a client can say in SQL: DDL, types, DML, SELECT, transactions, introspection, deliberate refusals, exact error surfaces |
| [Server manual](server/server.md) | Building, running, the full config-key table, operating, what a restart loses |
| [Client manual](client/client.md) | The wire protocol, reply shapes, error/retry rules, bundled client tools, writing your own client |
| [Physical optimizer manual](physical-optimizer/physical-optimizer.md) | The shadow report (`SHOW RELAYOUT`), the decay score, the three enactment gates, the Cabin controller's status |
