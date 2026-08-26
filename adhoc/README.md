# Ad-hoc query scripts

Editable `.sql` scripts for exercising the query language against a real
server. Adding a query and the answer it should give is two adjacent
lines — no code change, no rebuild.

```sh
adhoc/run.sh                      # every script, against a throwaway server
adhoc/run.sh --quiet              # only failures and statements with no expectation
adhoc/run.sh 02_subqueries.sql    # the fixture, then just that one
```

Exit status is 0 when every expectation held.

## Writing a query

One statement per line — the wire protocol is one line in, one line out
(`docs/spec/client-manual.md`), so a statement cannot span lines here either.
`--` starts a comment, and four spellings are directives applying to the
**next** statement:

```sql
-- expect: alice        the reply must contain this
-- reject: dormant      the reply must NOT contain this
-- error: named twice   the reply must be an ERR containing this
-- rows: 3              the reply must hold exactly this many data rows
SELECT acct.name FROM acct WHERE EXISTS (SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)
```

A statement with no directive runs and prints its reply. That is the
ad-hoc case: paste a query, look at what comes back, add an expectation
once you know what it should be.

**`rows:` earns its place.** The failures that matter in a query engine
are wrong *answers*, and the most common wrong answer is the right values
with the wrong multiplicity — a join that pairs a row twice, a subquery
predicate that filters nothing. Neither shows up as a missing substring.

## The files

| File | What it covers |
|---|---|
| `00_setup.sql` | The fixture. Always runs first; the others assume its rows. |
| `01_joins.sql` | Joins, projection, written order, the refusals (V05, V06, V17). |
| `02_subqueries.sql` | `EXISTS`/`IN`/scalar, correlated and not, NULL, cardinality (V07, V15, V18). |

The fixture is shaped so a *wrong* implementation gives a wrong answer
rather than an error — the only failure mode worth building a fixture
around. In particular `dormant` is an account with no trades: every
positive subquery form must drop it and every negative form must keep it,
so a predicate that is silently ignored keeps it in **both**.

Ids are system-generated and positional: `alice=1, bob=2, dormant=3,
carol=4`.

## Iterating on one query

`run.sh` starts and stops its own server. To keep one alive instead:

```sh
build/kds_server /tmp/scratch.db --port 15499 &
python3 tools/run_sql.py adhoc/00_setup.sql --port 15499
python3 tools/ckdbs_cli.py --port 15499 "SELECT * FROM acct"
```

## Related

- `tools/grammar_v2_check.py` — the same idea in Python, asserting *which
  refusal* each declined form gives. Use that for error surfaces, these
  for answers.
- `tools/grammar_v2_demo.py` — a narrated walkthrough of the grammar.
