# An assertion grouped by the tuple key column reads before the values

**The wrong behaviour.** `CREATE ASSERTION cap ON u GROUP BY (id) CHECK
COUNT(*) <= 1` on `u (id int64)` - `id` the relation's tuple key column - is
accepted, and the next `INSERT INTO u VALUES (5)` crashes the server.
`AssertionEnforcer::AdmitInsert` hands `KeyFor`
(`src/exec/assertion_check.cpp`) the inserted values **without** the tuple
key - a span of length 0 here, `first_col_pos = 1` - and `ValueAt` reads
`values[schema_pos - first_col_pos]` with `schema_pos = 0`: an index that
wraps to `SIZE_MAX`, read and copied. A segmentation fault in
`parser::AstValue::operator=` is what the Debug build shows; another build
may read garbage instead and key the row into a wrong group.

**Verified at** `7f0fa91d` with the A1 stage-2b diff on
`worktree-ddl-build-yield`, which does not touch `src/exec/assertion_check.cpp`
or the create's column resolution; found by
`IndexBuildSliceRigTest.AnAssertionNameTakenWhileTheBuildYieldedIsRefusedBeforeAdoption`
while it was being written (it now groups `u` by another column).

**Smallest reproduction.** On any dispatcher with assertions enforced:

    CREATE TABLE u (id int64)
    CREATE ASSERTION cap ON u GROUP BY (id) CHECK COUNT(*) <= 1
    INSERT INTO u VALUES (5)

**What it costs.** A crash reachable from SQL by any session that can
declare an assertion - the worst class - and, in a build where the read
does not fault, a **quiet wrong answer**: an admission keyed on bytes that
are not the row's.

**The fix, known and unscheduled.** Either refuse a `GROUP BY` or `SUM`
column that is the tuple key at the create (`ResolveAssertionColumns`,
`src/exec/assertion_catalog.cpp`) with its byte, or have `ValueAt` take
the tuple key from where the insert path carries it. Which one is a
decision for the assertion spec's owner (`docs/spec/assertion.md` §3.1
lists the create-time refusals); neither belongs to A1.
