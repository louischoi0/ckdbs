# A nested uncorrelated subquery is dropped

**What is wrong.** Consider a predicate-position subquery that sits inside
another subquery and references nothing outside itself, such as an inner
`EXISTS` or `NOT EXISTS`. That subquery is **never evaluated**. The
statement then answers as if its predicate were absent.

**Verified at** `010b8e0`, on `worktree-ap-s4-function-catalog`, by running
the statements below. AP did not introduce it. The code it rests on
predates AP.

**Reproduction.** Use relations `t` and `u`, where both have ids 1..6 and
`u` has no row with id 999.

    SELECT id FROM t WHERE EXISTS
      (SELECT id FROM u WHERE u.id = t.id AND EXISTS (SELECT id FROM u WHERE id = 999))

- **Should answer** no rows, because the inner `EXISTS` is false.
- **Answers** all six rows.
- The plan shows the outer `EXISTS`'s sub-chain with one step and no inner
  sub-chain at all.
- The `NOT EXISTS (... id = 1)` form also answers all six rows, where the
  correct answer is none.

**Cause, by reading.** `CompileBlock` hoists a block's uncorrelated
`EXISTS`/`NOT EXISTS` sub-chains into its own chain's `hoisted` list
(`src/exec/step_compiler.cpp`, the placement pass). When that block is
itself a sub-chain, the caller keeps only `inner.value().steps`
(`sub.steps = std::move(inner.value().steps)`). It does this at both
lowering sites, `CompileWhere` and `CompileBlock`. `SubChain` has no
`hoisted` field, so the inner block's hoisted list is discarded.
Only the top-level chain's `hoisted` is ever executed.

**What it costs.** A **quiet wrong answer**: rows are returned that the
predicate excludes, or excluded that it admits, with no error. The shape is
any subquery nested inside a subquery whose predicate does not correlate to
an enclosing row. The parser admits nesting to `kMaxSubqueryDepth` (4).
Nothing in the execution suite runs a nested uncorrelated subquery. The
golden corpus parses one (`tests/testdata/parser_corpus.txt`) but does not
execute it.

**The fix, not yet scheduled.** Carry the inner block's `hoisted`
sub-chains with the `SubChain` and run them once per execution of that
sub-chain. Alternatively, attach them to the sub-chain's first step as
placed sub-chains. Either way, add a cell for `EXISTS` and `NOT EXISTS` at
depth 2 and 3, compared with the answer computed by hand. **Owner: none.**
Found by AP-S4's review and outside AP's scope.
