-- Joins and projection (V05, V06, V17). Needs 00_setup.sql.
--
-- Every `rows:` here is the point. A join is easy to make return the right
-- *values* with the wrong multiplicity - pairing a row twice, or pairing
-- the driving relation's rows with themselves - and no substring check
-- notices that.

-- ---------------------------------------------------------------------
-- The basic pairing. Four trades, each matching exactly one account, so
-- four rows out. dormant has no trade and must not appear.
-- rows: 4
-- expect: alice,AAPL
-- expect: alice,MSFT
-- expect: bob,TSLA
-- expect: carol,NVDA
-- reject: dormant
SELECT acct.name, trade.sym FROM trade JOIN acct ON trade.acct_id = acct.id

-- Written the other way round: same pairs, different order. The driving
-- relation is whichever was written first - spec section 1 makes that a
-- client contract, not a hint.
-- rows: 4
-- expect: alice,AAPL
-- reject: dormant
SELECT acct.name, trade.sym FROM acct JOIN trade ON trade.acct_id = acct.id

-- ---------------------------------------------------------------------
-- A join whose ON column is NOT a primary key. `region` is shared by two
-- accounts and two trades on each side, so this pairs many-to-many: 2
-- emea trades x 2 emea accounts + 2 apac trades x 2 apac accounts = 8.
--
-- This is the case that separates a real nested loop from a lookup that
-- stops at the first match. A probe-shaped implementation applied here
-- would return 4.
-- rows: 8
-- expect: dormant,AAPL
SELECT acct.name, trade.sym FROM trade JOIN acct ON trade.region = acct.region

-- ---------------------------------------------------------------------
-- Three relations. Each trade joins its account, and then the account
-- joins back to trades by region - so the count is not something you can
-- get right by accident.
-- rows: 8
SELECT t.sym, a.name, u.sym FROM trade AS t JOIN acct AS a ON t.acct_id = a.id JOIN trade AS u ON u.region = a.region

-- ---------------------------------------------------------------------
-- A self-join: accounts sharing a region, excluding the row pairing with
-- itself. `a.id != b.id` is a column-to-column comparison, which the
-- grammar only gained at V15 - it exists because a correlated subquery
-- cannot be written without it.
--
-- emea is alice + dormant, apac is bob + carol, so each region yields an
-- ordered pair in both directions: 4 rows, not 2. Halving it would need a
-- `<` between two columns, which is a fine thing to try here.
-- rows: 4
-- expect: alice,dormant
-- expect: dormant,alice
-- expect: bob,carol
SELECT a.name, b.name FROM acct AS a JOIN acct AS b ON a.region = b.region WHERE a.id != b.id

-- And the same statement with `<` instead keeps one direction of each
-- pair. Two columns under an inequality is the same production, so this
-- is a real check that the operator is not special-cased to equality.
-- rows: 2
-- expect: alice,dormant
-- reject: dormant,alice
SELECT a.name, b.name FROM acct AS a JOIN acct AS b ON a.region = b.region WHERE a.id < b.id

-- ---------------------------------------------------------------------
-- Projection: exactly the columns named, in the order named.
-- expect: gold,alice
-- reject: emea
SELECT acct.tier, acct.name FROM acct WHERE id = 1

-- A column may be repeated - that is the client's business, not ours.
-- expect: alice,alice
SELECT acct.name, acct.name FROM acct WHERE id = 1

-- ---------------------------------------------------------------------
-- The refusals, each with a byte position.
-- error: ambiguous
SELECT id FROM trade JOIN acct ON trade.acct_id = acct.id

-- error: outer joins are not supported
SELECT acct.id FROM acct LEFT JOIN trade ON trade.acct_id = acct.id

-- error: named twice
SELECT acct.id FROM acct JOIN acct ON acct.id = acct.id

-- error: ambiguous across 2 relations
SELECT * FROM trade JOIN acct ON trade.acct_id = acct.id

-- error: cannot appear in FROM
SELECT * FROM (SELECT trade.id FROM trade)
