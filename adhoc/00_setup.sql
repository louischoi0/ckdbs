CREATE TABLE acct (id int64, name varchar, tier varchar, region varchar) BTREE
CREATE TABLE trade (id int64, acct_id int64, sym varchar, region varchar) BTREE

-- alice = 1
INSERT INTO acct VALUES ('alice', 'gold', 'emea')
INSERT INTO acct VALUES ('bob', 'silver', 'apac')
INSERT INTO acct VALUES ('dormant', 'gold', 'emea')
INSERT INTO acct VALUES ('carol', 'bronze', 'apac')

-- alice has two, bob one, carol one, dormant none.
INSERT INTO trade VALUES (1, 'AAPL', 'emea')
INSERT INTO trade VALUES (1, 'MSFT', 'emea')
INSERT INTO trade VALUES (2, 'TSLA', 'apac')
INSERT INTO trade VALUES (4, 'NVDA', 'apac')

SELECT * FROM acct
SELECT * FROM trade
