-- ===========================================================================
-- Assertions in KDS — a runnable example
--
--     rm -rf /tmp/assertion-example.db /tmp/assertion-example.db.wal
--     ./build-release/kds_server /tmp/assertion-example.db --port 15601 &
--     python3 tools/ckdbs_cli.py --port 15601 -f example/assertion.sql --echo
--
-- **Run it on a fresh data file.** The assertion ids and row ids in the
-- comments are the ones a fresh database issues; CREATE is idempotent-ish
-- (it refuses a duplicate name) but INSERT is not, so a second run against
-- the same file appends rows and shifts every id.
--
-- **This file needs a build at superblock format 13 or later.** Assertions
-- added `sys.assertions` on fixed page 14, so an older data file does not
-- mount at all and says so, naming both versions. That refusal is the
-- feature working.
--
-- ---------------------------------------------------------------------------
-- READ THIS BEFORE YOU RELY ON ANYTHING BELOW
--
-- `CREATE ASSERTION` is built through workplan task AST03: a declaration is
-- **parsed, validated and recorded**. It is **not enforced**. There is no
-- Bound Cabin (AST04) and no write-path check (AST07), so §2 of this file
-- inserts seven rows into a group whose declared bound is five and every one
-- of them succeeds.
--
-- The engine says so rather than letting you discover it: both `CREATE
-- ASSERTION` and `SHOW ASSERTIONS` report `enforcing=0`. When AST06 publishes
-- a Bound Cabin root it becomes `enforcing=1`, and §2 of this file starts
-- failing — which is the intended future diff.
-- ---------------------------------------------------------------------------
--
-- **Nineteen statements below are expected to fail**, and they are the point
-- of the sections they are in — so `19 statement(s) replied ERR` at the end
-- of the run is what success looks like. Each is marked `-- EXPECT: ERR ...`:
-- nine in §4 (the reserved forms), five in §5 (the wrong ones), four in §6
-- (what only the catalog can answer), and one in §7 (dropping twice).
--
-- Spec: docs/spec/assertion.md.
-- Command reference: docs/spec/client-manual.md.
-- ===========================================================================


-- ---------------------------------------------------------------------------
-- 0. Setup
--
-- The first column is the Keystone primary key and the engine issues it, so
-- INSERT never supplies one (invariant 11).
-- ---------------------------------------------------------------------------

CREATE TABLE purchases (id int64, user_id int64, product_id int64, amount int64, note varchar) BTREE;


-- ---------------------------------------------------------------------------
-- 1. Declaring one
--
-- The syntax is deliberately **not** SQL-92's `CHECK (<search condition>)`.
-- SQL-92 admits an arbitrary predicate, and that generality is one of the two
-- reasons no major DBMS ships the statement: a general predicate has to be
-- re-evaluated on every write. Here the grammar *is* the supported class —
-- a relation, a group-column list, one of two aggregates, an integer upper
-- bound — which is what makes the check O(1) against a per-group running
-- total instead of a re-evaluation (decision AS2).
--
-- The example is the spec's own (§3.2).
-- ---------------------------------------------------------------------------

CREATE ASSERTION user_product_purchase_limit
  ON purchases
  GROUP BY (user_id, product_id)
  CHECK COUNT(*) <= 5;

-- The other supported aggregate. The SUM column must be int64 — see §6.
CREATE ASSERTION daily_spend_cap
  ON purchases
  GROUP BY (user_id)
  CHECK SUM(amount) <= 1000;

-- `def=` is the declaration read back out of the catalog, byte for byte.
-- It is the *only* stored form: the group columns, the aggregate, the
-- operator and the bound are recovered by re-parsing this text, never stored
-- decoded beside it (the sys.pattern_defs model, AS10). Two consequences —
-- the GROUP BY list has no cap at all, because a longer list costs text and
-- not a wider catalog row; and there is exactly one canon, so nothing can
-- drift out of step with it.
SHOW ASSERTIONS;


-- ---------------------------------------------------------------------------
-- 2. `enforcing=0` is literal
--
-- The bound above is COUNT(*) <= 5 for one (user_id, product_id) group.
-- Here are seven rows in that group. All seven succeed.
--
-- Nothing is wrong: AST04 (the Bound Cabin that would hold the running
-- aggregate) and AST07 (the admission check on the write path) are not built.
-- This section exists so that fact is demonstrated rather than assumed.
-- ---------------------------------------------------------------------------

INSERT INTO purchases VALUES (41, 7, 100, 'buy-1');
INSERT INTO purchases VALUES (41, 7, 100, 'buy-2');
INSERT INTO purchases VALUES (41, 7, 100, 'buy-3');
INSERT INTO purchases VALUES (41, 7, 100, 'buy-4');
INSERT INTO purchases VALUES (41, 7, 100, 'buy-5');
INSERT INTO purchases VALUES (41, 7, 100, 'buy-6');   -- over the bound
INSERT INTO purchases VALUES (41, 7, 100, 'buy-7');   -- over the bound

-- 7 rows against a bound of 5, and 700 against a cap of 1000.
-- When AST07 lands, rows 6 and 7 become AssertionViolation and this query
-- answers 5 / 500.
SELECT user_id, product_id, COUNT(*), SUM(amount) FROM purchases GROUP BY user_id, product_id;


-- ---------------------------------------------------------------------------
-- 3. Two operators, one enforced ceiling
--
-- `<` and `<=` are the whole of it (AS11 as revised 2026-08-08), and both map
-- onto `aggregate <= N` **exactly** — neither reinterprets what was written:
--
--     CHECK COUNT(*) <= 5   ->  count <= 5
--     CHECK COUNT(*) <  5   ->  count <= 4
--
-- The reduction happens once, in the parser, so no later stage re-derives it
-- and no two stages can disagree about what `<` meant.
--
-- `=` used to be here, accepted as a third spelling of `count <= N` "for
-- syntactic familiarity". It is refused now — see §4.
-- ---------------------------------------------------------------------------

CREATE ASSERTION strictly_under ON purchases GROUP BY (product_id) CHECK COUNT(*) < 5;
DROP ASSERTION strictly_under;


-- ---------------------------------------------------------------------------
-- 4. Reserved and refused — `Unsupported`, each at its own byte
--
-- These are forms the engine *understands* and declines. They parse, so the
-- answer names the decision instead of being a syntax error pointing at some
-- unrelated token — and the grammar will not shift on the day one of them
-- lands.
-- ---------------------------------------------------------------------------

-- AS11. This is the refusal that pays for a whole write path: a lower bound
-- would have to be re-checked on DELETE and on every decreasing UPDATE, which
-- is exactly why v1 leaves DELETE with no assertion check at all.
-- EXPECT: ERR lower-bound assertions (>) are not supported
CREATE ASSERTION lower ON purchases GROUP BY (user_id) CHECK COUNT(*) > 5;

-- AS11 as revised 2026-08-08, and the most interesting refusal in this file.
-- `=` parsed, and was documented as meaning `aggregate <= N`. That is
-- withdrawn: documenting a reinterpretation does not make it honest, and a
-- client reading `CHECK COUNT(*) = 5` would reasonably expect a group of
-- three rows to be a violation. Enforcing it as written needs the lower-bound
-- half, so `=` costs exactly what `>=` costs and is refused beside it.
-- EXPECT: ERR equality assertions (=) are not supported
CREATE ASSERTION exactly ON purchases GROUP BY (user_id) CHECK COUNT(*) = 5;

-- EXPECT: ERR lower-bound assertions (>=) are not supported
CREATE ASSERTION lower2 ON purchases GROUP BY (user_id) CHECK SUM(amount) >= 5;

-- AS3. `NOT DEFERRABLE` is refused too, even though it names the behaviour
-- v1 actually has — accepting it would turn it into a promise, and the
-- decision reserves the whole timing clause rather than half of it.
-- EXPECT: ERR 'DEFERRABLE' is reserved and not supported
CREATE ASSERTION defer1 ON purchases GROUP BY (user_id) CHECK COUNT(*) <= 5 DEFERRABLE;

-- EXPECT: ERR constraint timing clauses (NOT DEFERRABLE / NOT VALID) are not supported
CREATE ASSERTION defer2 ON purchases GROUP BY (user_id) CHECK COUNT(*) <= 5 NOT DEFERRABLE;

-- AS7. There is no build-it-later mode: CREATE scans, or CREATE fails.
-- EXPECT: ERR constraint timing clauses (NOT DEFERRABLE / NOT VALID) are not supported
CREATE ASSERTION novalid ON purchases GROUP BY (user_id) CHECK COUNT(*) <= 5 NOT VALID;

-- §10. MIN and MAX are not incrementally maintainable under deletion without
-- extra structure, and AVG is not a bound.
-- EXPECT: ERR max bounds are out of scope for assertions
CREATE ASSERTION peak ON purchases GROUP BY (user_id) CHECK MAX(amount) <= 5;

-- EXPECT: ERR avg bounds are out of scope for assertions
CREATE ASSERTION mean ON purchases GROUP BY (user_id) CHECK AVG(amount) <= 5;

-- COUNT(<column>) counts non-NULLs, which is a *different* aggregate.
-- Reading it as COUNT(*) would enforce a bound nobody wrote.
-- EXPECT: ERR an assertion's cardinality bound is written COUNT(*)
CREATE ASSERTION cnt ON purchases GROUP BY (user_id) CHECK COUNT(amount) <= 5;

-- A distinct aggregate needs per-value multiplicity, which a group header
-- does not carry.
-- EXPECT: ERR DISTINCT is not supported in an assertion's CHECK
CREATE ASSERTION dis ON purchases GROUP BY (user_id) CHECK COUNT(DISTINCT amount) <= 5;


-- ---------------------------------------------------------------------------
-- 5. Wrong, rather than reserved — `InvalidArgument`
--
-- The distinction is deliberate. §4's forms could be enforced and are not, so
-- they say which decision they are waiting on. These could never be enforced
-- by anything, so they are simply wrong.
-- ---------------------------------------------------------------------------

-- `!=` names no ceiling in either direction.
-- `!=` names no ceiling in either direction. Distinct from `=` and `>`, which
-- name a constraint the engine understands and declines — there is no decision
-- pending here.
-- EXPECT: ERR '!=' is not a bound
CREATE ASSERTION neq ON purchases GROUP BY (user_id) CHECK COUNT(*) != 5;

-- Degenerate (§3.1). A group *exists* only because it holds at least one row,
-- so its count is at least 1 and any ceiling below 1 declares a relation that
-- may never be written to again. Both spellings an accepted operator can
-- still produce are caught. (`= 0`, the spelling the spec named, is now
-- refused one step earlier — by the operator itself.)
-- EXPECT: ERR can never admit a row
CREATE ASSERTION zero2 ON purchases GROUP BY (user_id) CHECK COUNT(*) <= 0;
-- EXPECT: ERR can never admit a row
CREATE ASSERTION zero3 ON purchases GROUP BY (user_id) CHECK COUNT(*) < 1;

-- The same argument deliberately does **not** extend to SUM: an int64 column
-- may hold negative values, so no non-negative bound is provably
-- unsatisfiable, and refusing one would be inventing a restriction the spec
-- does not state. This is accepted.
CREATE ASSERTION sum_zero ON purchases GROUP BY (user_id) CHECK SUM(amount) <= 0;
DROP ASSERTION sum_zero;

-- Literals only, non-negative (§3.1, TY3 conservatism). One predicate answers
-- both halves, which is why the message names both.
-- EXPECT: ERR an assertion's bound is a non-negative integer literal
CREATE ASSERTION negbound ON purchases GROUP BY (user_id) CHECK COUNT(*) <= -1;


-- ---------------------------------------------------------------------------
-- 6. What only the catalog can answer
--
-- Everything in §§4-5 was decidable from the statement's text alone, so the
-- parser answered it with a byte offset. These four need a schema, so they
-- are the catalog's — and they still carry the byte the parser recorded for
-- the name in question.
-- ---------------------------------------------------------------------------

-- EXPECT: ERR no relation named 'nosuchtable'
CREATE ASSERTION a1 ON nosuchtable GROUP BY (user_id) CHECK COUNT(*) <= 1;

-- EXPECT: ERR relation 'purchases' has no column 'nope'
CREATE ASSERTION a2 ON purchases GROUP BY (user_id, nope) CHECK COUNT(*) <= 1;

-- The SUM column must be int64. `note` is a varchar.
-- EXPECT: ERR an assertion's SUM column must be int64
CREATE ASSERTION a3 ON purchases GROUP BY (user_id) CHECK SUM(note) <= 1;

-- Names are unique, and the comparison is case-insensitive like every other
-- object name — two spellings of one name must not become two assertions.
-- EXPECT: ERR assertion "DAILY_SPEND_CAP" already exists
CREATE ASSERTION DAILY_SPEND_CAP ON purchases GROUP BY (product_id) CHECK COUNT(*) <= 9;


-- ---------------------------------------------------------------------------
-- 7. Dropping
--
-- The catalog row is **retired**, not delete-marked. Catalog reads have no
-- snapshot to filter a mark against, so a marked row would still be found by
-- name and re-creating the same name would collide with a row nobody can see.
-- ---------------------------------------------------------------------------

DROP ASSERTION daily_spend_cap;

-- Dropping what is not there is an error, not a silent success.
-- EXPECT: ERR no assertion named 'daily_spend_cap'
DROP ASSERTION daily_spend_cap;

-- And the name is genuinely free again — which is what proves the row was
-- retired rather than marked.
CREATE ASSERTION daily_spend_cap ON purchases GROUP BY (user_id) CHECK SUM(amount) <= 2000;

SHOW ASSERTIONS;


-- ---------------------------------------------------------------------------
-- 8. Nothing is reserved
--
-- `ASSERTION`, `CHECK` and `GROUP` reach the lexer as ordinary identifiers,
-- matched by text only where the grammar expects them. So they are still
-- usable as table and column names, no token sequence lexes differently than
-- it did before this feature, and `kFingerprintVersion` did not move — which
-- means no stored waystone was retired by adding the statement.
-- ---------------------------------------------------------------------------

CREATE TABLE assertion (id int64, check int64, group int64);
INSERT INTO assertion VALUES (10, 20);
SELECT check, group FROM assertion WHERE check = 10;

CREATE ASSERTION assertion ON assertion GROUP BY (check) CHECK COUNT(*) <= 3;
DROP ASSERTION assertion;


-- ---------------------------------------------------------------------------
-- 9. What this does not do — worth knowing before you rely on it
--
--   * **Nothing is enforced.** AST01-AST03 are built: the spec revision, the
--     grammar, the catalog. The Bound Cabin (AST04), its WAL records (AST05),
--     the CREATE-time build scan (AST06) and the write-path reservation
--     protocol (AST07) are not. `enforcing=0` is the honest report of that.
--   * **AST04 is blocked**, and not merely unstarted: its pinned-page
--     discipline ("Bound Cabin pages are exempt from eviction") needs an
--     eviction policy to be exempt from, and the buffer-pool frame
--     reclamation policy is an open decision. There is no eviction in this
--     engine at all.
--   * **`CREATE ASSERTION` does not scan the relation**, so a declaration
--     made over already-violating data succeeds today. AS7 says it must fail;
--     that is AST06.
--   * **Upper bounds only** (AS11 as revised), which is what makes DELETE
--     check-free. `>`, `>=` and `=` are all reserved, not
--     pending-with-a-date; `=` is refused because reading it as `<=` would
--     enforce something other than what was written, and enforcing it as
--     written needs the lower-bound half.
--   * **One relation per assertion** (AS8). Multi-relation assertions are
--     blocked on the cross-core commit protocol, which is itself reserved.
--   * **No WHERE-scoped or HAVING-style assertions**, no `AVG`/`MIN`/`MAX`
--     bounds, no uint64 SUM.
--   * **`DROP TABLE` does not consult assertions** — because there is no
--     DROP TABLE. §8.3's RESTRICT rule is implemented as a predicate
--     (`AssertionsOnRelation`) with no call site; wiring it is one call at a
--     statement that does not exist yet.
--   * **The declaration is unlogged**, like every other DDL in this engine,
--     so it survives a crash only as far as the last SYNC.
-- ---------------------------------------------------------------------------

SYNC;
