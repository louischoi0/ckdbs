-- Fixture for the ad-hoc scripts. Run this first; the others assume it.
--
-- The shape is chosen so that a *wrong* implementation produces a wrong
-- answer rather than an error, which is the only failure mode worth
-- building a fixture around:
--
--   * every account has a different number of trades - 2, 1, 0, 1 - so a
--     join that pairs wrongly changes the row count, and a subquery that
--     filters nothing returns 4 rows instead of 3.
--   * `dormant` has no trades at all, which is the row EXISTS must drop
--     and NOT EXISTS must keep. A predicate that is silently ignored
--     keeps it in both, and that is the bug this catches.
--   * two accounts share a tier and two share a region, so an unqualified
--     name is genuinely ambiguous and a join on a non-pk column pairs
--     more than one row.
--   * ids are system-generated and start at 1, so `acct` is 1..4 in
--     insertion order: alice=1, bob=2, dormant=3, carol=4.

CREATE TABLE acct (id int64, name varchar, tier varchar, region varchar)
CREATE TABLE trade (id int64, acct_id int64, sym varchar, region varchar)

-- alice = 1
INSERT INTO acct VALUES ('alice', 'gold', 'emea')
-- bob = 2
INSERT INTO acct VALUES ('bob', 'silver', 'apac')
-- dormant = 3, deliberately without a single trade
INSERT INTO acct VALUES ('dormant', 'gold', 'emea')
-- carol = 4
INSERT INTO acct VALUES ('carol', 'bronze', 'apac')

-- alice has two, bob one, carol one, dormant none.
INSERT INTO trade VALUES (1, 'AAPL', 'emea')
INSERT INTO trade VALUES (1, 'MSFT', 'emea')
INSERT INTO trade VALUES (2, 'TSLA', 'apac')
INSERT INTO trade VALUES (4, 'NVDA', 'apac')

-- Sanity: the fixture is what the rest of these scripts assume.
-- rows: 4
-- expect: alice
-- expect: dormant
SELECT * FROM acct

-- rows: 4
SELECT * FROM trade
