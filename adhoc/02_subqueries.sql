-- Predicate-position subqueries (V07, V15, V18). Needs 00_setup.sql.
--
-- `dormant` is the account with no trades, and it is the whole point of
-- this file: it is the row that every positive form must drop and every
-- negative form must keep. A subquery predicate that is silently ignored
-- keeps it in *both*, which is why almost every check below pins a row
-- count as well as a name.

-- ---------------------------------------------------------------------
-- EXISTS / NOT EXISTS, correlated. The pair must partition the relation:
-- 3 + 1 = 4 accounts.
-- rows: 3
-- expect: alice
-- expect: bob
-- expect: carol
-- reject: dormant
SELECT acct.name FROM acct WHERE EXISTS (SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)

-- rows: 1
-- expect: dormant
-- reject: alice
SELECT acct.name FROM acct WHERE NOT EXISTS (SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)

-- ---------------------------------------------------------------------
-- EXISTS / NOT EXISTS, uncorrelated. The answer cannot vary per row, so
-- it is all four or none - and the sub-chain is evaluated once, before
-- the outer relation is opened at all.
-- rows: 4
SELECT acct.name FROM acct WHERE EXISTS (SELECT trade.id FROM trade)

-- rows: 0
SELECT acct.name FROM acct WHERE NOT EXISTS (SELECT trade.id FROM trade)

-- A false uncorrelated EXISTS: no rows, and `acct` is never opened.
-- rows: 0
SELECT acct.name FROM acct WHERE EXISTS (SELECT trade.id FROM trade WHERE trade.sym = 'NOSUCH')

-- ---------------------------------------------------------------------
-- IN / NOT IN. Same partition as EXISTS above, reached a different way -
-- if these two disagree with the EXISTS pair, one of them is wrong.
-- rows: 3
-- reject: dormant
SELECT acct.name FROM acct WHERE id IN (SELECT acct_id FROM trade)

-- rows: 1
-- expect: dormant
SELECT acct.name FROM acct WHERE id NOT IN (SELECT acct_id FROM trade)

-- NOT IN over an EMPTY subquery is true for every row. This is the case
-- that separates "empty" from "contains a NULL": with no rows there is no
-- NULL either, so the answer is TRUE rather than UNKNOWN.
-- rows: 4
SELECT acct.name FROM acct WHERE id NOT IN (SELECT acct_id FROM trade WHERE trade.sym = 'NOSUCH')

-- IN against a set that matches nothing: no rows, and no error.
-- rows: 0
SELECT acct.name FROM acct WHERE id IN (SELECT acct_id FROM trade WHERE trade.sym = 'NOSUCH')

-- ---------------------------------------------------------------------
-- Scalar subqueries. Exactly one row compares; two is a runtime error,
-- never a first-row pick - a first-row pick would make the answer depend
-- on physical page order.
-- rows: 1
-- expect: bob
SELECT acct.name FROM acct WHERE id = (SELECT acct_id FROM trade WHERE trade.sym = 'TSLA')

-- Zero rows is NULL, and a comparison against NULL is never true - under
-- `=` *and* under `!=`. A two-valued implementation would return all four
-- rows for the second one, which is the bug the tri-state prevents.
-- rows: 0
SELECT acct.name FROM acct WHERE id = (SELECT acct_id FROM trade WHERE trade.sym = 'NOSUCH')

-- rows: 0
SELECT acct.name FROM acct WHERE id != (SELECT acct_id FROM trade WHERE trade.sym = 'NOSUCH')

-- Four trades, so this scalar subquery is over-cardinal.
-- error: more than one row
SELECT acct.name FROM acct WHERE id = (SELECT acct_id FROM trade)

-- Correlated, so single-valued per outer row where the uncorrelated form
-- was not. bob and carol have exactly one trade; alice has two, which
-- makes *her* row the violation - so the whole statement fails.
-- error: more than one row
SELECT acct.name FROM acct WHERE id = (SELECT acct_id FROM trade WHERE trade.acct_id = acct.id)

-- ---------------------------------------------------------------------
-- Comparison against an inline NULL - the only NULL reachable from
-- stored data today (spec I16). Never true, either direction.
-- rows: 0
SELECT acct.name FROM acct WHERE tier = NULL

-- rows: 0
SELECT acct.name FROM acct WHERE tier != NULL

-- ---------------------------------------------------------------------
-- Subqueries alongside ordinary predicates, and inside a join.
-- alice and dormant are gold; only alice has trades.
-- rows: 1
-- expect: alice
SELECT acct.name FROM acct WHERE tier = 'gold' AND EXISTS (SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)

-- A subquery over a joined statement: the correlation reaches the
-- relation it names, not whichever was most recently bound.
-- rows: 4
SELECT a.name, t.sym FROM trade AS t JOIN acct AS a ON t.acct_id = a.id WHERE EXISTS (SELECT trade.id FROM trade WHERE trade.acct_id = a.id)

-- ---------------------------------------------------------------------
-- Nesting. A subquery inside a subquery, correlated two levels out.
-- rows: 3
-- reject: dormant
SELECT acct.name FROM acct WHERE EXISTS (SELECT trade.id FROM trade WHERE trade.acct_id = acct.id AND EXISTS (SELECT acct.id FROM acct WHERE acct.tier = 'gold'))

-- Past the depth cap: declined, with the cap named.
-- error: nesting deeper than 4
SELECT acct.name FROM acct WHERE EXISTS (SELECT trade.id FROM trade WHERE EXISTS (SELECT trade.id FROM trade WHERE EXISTS (SELECT trade.id FROM trade WHERE EXISTS (SELECT trade.id FROM trade WHERE EXISTS (SELECT trade.id FROM trade)))))

-- ---------------------------------------------------------------------
-- UPDATE takes the same predicates, through the same evaluator.
-- dormant is the only account with no trades.
-- expect: UPDATED 1
UPDATE acct SET tier = 'none' WHERE NOT EXISTS (SELECT trade.id FROM trade WHERE trade.acct_id = acct.id)

-- rows: 1
-- expect: dormant,none
SELECT acct.name, acct.tier FROM acct WHERE tier = 'none'

-- And the accounts that do have trades were untouched.
-- rows: 1
-- expect: alice
SELECT acct.name FROM acct WHERE tier = 'gold'

-- ---------------------------------------------------------------------
-- Forms the language declines outright.
-- error: a subquery used as a value must project exactly one column
SELECT acct.name FROM acct WHERE id IN (SELECT * FROM trade)

-- error: expected a subquery
SELECT acct.name FROM acct WHERE id IN (1, 2)

-- error: common table expressions
WITH x AS (SELECT trade.id FROM trade) SELECT x.id FROM x

-- error: NOT is supported only
SELECT acct.name FROM acct WHERE NOT id = 1

-- [I14 — OPEN] An aggregate still gives a bare syntax error rather than a
-- positioned refusal. Answering it either way would pick one of I14's two
-- open options, so it stays this way until I14 is decided.
-- error: expected 'FROM'
SELECT acct.name FROM acct WHERE id = (SELECT COUNT(*) FROM trade)
