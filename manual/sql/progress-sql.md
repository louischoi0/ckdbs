# Progress — manual/sql.md (task 5939e5a7344b4ece)

- Run began 2026-08-09.
- Step 1: this file. Done.
- Step 2: grammar verified against `include/kds/parser/ast.hpp` (full read),
  `src/parser/parser.cpp` (full read), `src/parser/lexer.cpp` (keyword table),
  `src/server/command_dispatcher.cpp` (dispatch table, HandleBegin/Commit/
  Rollback/SetIsolation, InsertInner, ErrorReply), `src/catalog/catalog.cpp`
  (type registry), `src/exec/step_compiler.cpp` (join/access-kind rule). Done.
- Step 3: manual/sql.md written — DDL, types, DML (DELETE exists — the prior
  "no SQL DELETE" finding is stale), SELECT (incl. no-pagination finding),
  transactions, introspection (complete SHOW list from code). Done.
- Step 4: refusals section written. Done.
- Step 5: error-surface section written from ErrorReply + verified messages. Done.

Findings for the report:
1. Task brief said assertions are "ENFORCING NOTHING" — stale; AST07 enforces
   on the write paths (verified call sites). Documented as enforcing, with the
   restart/enforcing=0 caveat.
2. SQL DELETE exists (ParseDelete/DeleteStmt) — prior finding stale.
3. Pagination: none in SQL; KWP portal suspension specified but only the frame
   codec is built (specified-but-unbuilt).
4. Per-transaction durability class: specified in protocol.md, unreachable
   from the text protocol — config key only (specified-but-unbuilt).
5. Undocumented-by-spec code behavior: bare `CREATE TABLE <name>` (no column
   list) legacy dispatcher path creating an empty-schema relation.
6. `CABIN AUTO` parses and is stored but nothing consumes it.
7. `IN (value list)` unbuilt (V08 half-open); reported as "expected a subquery".
