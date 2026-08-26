-- ===========================================================================
-- Transactions in KDS — a runnable example
--
--     rm -rf /tmp/txn-example.db /tmp/txn-example.db.wal
--     ./build-release/kds_server /tmp/txn-example.db --port 15600 &
--     python3 tools/ckdbs_cli.py --port 15600 -f example/transaction.sql --echo
--
-- **Run it on a fresh data file.** The row ids in the comments below are the
-- ones a fresh database issues, and CREATE TABLE is idempotent while INSERT
-- is not — so a second run against the same file appends two more rows and
-- every id shifts. That is not a defect in the script: ids are unique and
-- monotonic and are never reused, which is invariant 11 doing its job.
--
-- Everything below runs on **one connection**, which is what `-f` opens, and
-- that is exactly why it can only show half the story. The half it cannot
-- show — one transaction reading while another writes — needs two
-- connections and is at the bottom of this file, with the commands to
-- reproduce it.
--
-- **Six statements below are expected to fail**, and they are the point of
-- the sections they are in — so `6 statement(s) replied ERR` at the end of
-- the run is what success looks like. Each is marked `-- EXPECT: ERR ...`:
-- one in §5 (changing the level mid-transaction), one in §5 (SERIALIZABLE),
-- and four in §6 (the bad column, then the three refusals that follow it).
--
-- Spec: docs/spec/txn.md. Command reference: docs/spec/client-manual.md §3.
-- ===========================================================================


-- ---------------------------------------------------------------------------
-- 0. Setup
--
-- The first column is the Keystone primary key and the engine issues it, so
-- INSERT never supplies one (invariant 11). BTREE makes `WHERE id = <n>` a
-- tree descent rather than a chain scan.
-- ---------------------------------------------------------------------------

CREATE TABLE accounts (id int64, owner varchar, balance int64) BTREE;

-- Autocommit: no BEGIN, so each of these is its own transaction and is
-- durable and visible the moment it replies.
INSERT INTO accounts VALUES ('alice', 1000);
INSERT INTO accounts VALUES ('bob', 500);

SELECT * FROM accounts;


-- ---------------------------------------------------------------------------
-- 1. An explicit transaction that commits
--
-- A transfer is two UPDATEs, and the point of the transaction is that no
-- other connection ever sees one without the other.
--
-- Balances are computed here rather than in SQL: `SET col = <value>` takes a
-- literal, not an expression. There is no `SET balance = balance - 100`.
-- ---------------------------------------------------------------------------

BEGIN;

UPDATE accounts SET balance = 900 WHERE id = 1;   -- alice: 1000 - 100
UPDATE accounts SET balance = 600 WHERE id = 2;   -- bob:    500 + 100

-- A transaction always sees its own uncommitted writes. Nobody else does.
SELECT * FROM accounts;

COMMIT;

SELECT * FROM accounts;


-- ---------------------------------------------------------------------------
-- 2. ROLLBACK undoes everything, in reverse
--
-- An UPDATE's bytes are put back, an INSERT's slot is retired, and a
-- DELETE's mark is cleared — each by the compensation its trail entry names
-- (docs/spec/txn.md §6). Undo pages are not freed; nothing purges.
-- ---------------------------------------------------------------------------

BEGIN;

UPDATE accounts SET balance = 0 WHERE id = 1;
INSERT INTO accounts VALUES ('carol', 250);
DELETE FROM accounts WHERE id = 2;

-- Inside the transaction: alice is 0, carol exists, bob is gone.
SELECT * FROM accounts;

ROLLBACK;

-- After: alice is 900 again, carol never existed, bob is back. Note that
-- carol's id is *spent* — ids are unique and monotonic, never gapless — so
-- the next INSERT will not reuse it.
SELECT * FROM accounts;


-- ---------------------------------------------------------------------------
-- 3. Two updates to one row in one transaction
--
-- Not a conflict: the second undo record links to the first, so ROLLBACK
-- unwinds both and lands on the **original**, not on the intermediate value.
-- ---------------------------------------------------------------------------

BEGIN;
UPDATE accounts SET balance = 111 WHERE id = 1;
UPDATE accounts SET balance = 222 WHERE id = 1;
SELECT * FROM accounts WHERE id = 1;              -- 222
ROLLBACK;
SELECT * FROM accounts WHERE id = 1;              -- 900, not 111


-- ---------------------------------------------------------------------------
-- 4. DELETE is a delete-mark, not a removal
--
-- The row's bytes stay and the slot gains a flag; the deleting transaction's
-- id goes in the tuple's writer field. That is the whole of DELETE in the
-- no-xmax model, and it is why an older snapshot still reads the row (see
-- §7 below). Physical reclamation is a purge pass that does not exist, so
-- the space is not reused.
-- ---------------------------------------------------------------------------

BEGIN;
DELETE FROM accounts WHERE id = 2;
SELECT * FROM accounts;                           -- bob is gone for me
COMMIT;
SELECT * FROM accounts;                           -- and now for everyone

-- Deleting again marks nothing: there is no visible version left to delete.
DELETE FROM accounts WHERE id = 2;                -- DELETED 0


-- ---------------------------------------------------------------------------
-- 5. Isolation levels
--
-- Three rungs, in increasing precedence: the `isolation` config key, then
-- `SET ISOLATION LEVEL` per connection, then `BEGIN ISOLATION LEVEL` for one
-- transaction. READ COMMITTED is the default, and the reason is specific to
-- this engine: under first-updater-wins with no waiting, holding one read
-- view for a whole transaction turns more concurrent writes into retryable
-- aborts.
--
-- On one connection the two levels are indistinguishable — the difference is
-- what you see of *other* connections' commits, which is §7.
-- ---------------------------------------------------------------------------

SET ISOLATION LEVEL REPEATABLE READ;
BEGIN;
SELECT * FROM accounts;

-- EXPECT: ERR — the level of a running transaction cannot change underneath
-- it, or its earlier statements become unexplainable.
SET ISOLATION LEVEL READ COMMITTED;

COMMIT;

-- Back to the session default for the rest of the file.
SET ISOLATION LEVEL READ COMMITTED;

-- A per-transaction override, which does not outlive the transaction.
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT * FROM accounts;
COMMIT;

-- EXPECT: ERR — SERIALIZABLE is out of scope, not unimplemented, and the
-- refusal says why rather than calling it an unknown word.
BEGIN ISOLATION LEVEL SERIALIZABLE;


-- ---------------------------------------------------------------------------
-- 6. A failed statement poisons the transaction
--
-- **Failure atomicity is per transaction, not per statement** (docs/spec/txn.md
-- §6). A statement that fails inside an explicit transaction does not undo
-- the ones before it — the connection enters `failed-txn` and answers only
-- ROLLBACK / ABORT / SYNC / STOP / PING until you roll back. That is a
-- deviation from SQL, which savepoints would close; there are none.
--
-- In autocommit the abort is automatic, so behaviour there *is*
-- statement-atomic.
-- ---------------------------------------------------------------------------

BEGIN;

UPDATE accounts SET balance = 777 WHERE id = 1;

-- EXPECT: ERR — no such column.
UPDATE accounts SET nosuch = 1;

-- EXPECT: ERR ×3 — the connection is now poisoned. Note that even SELECT is
-- refused: the whitelist is deliberately narrow, so a statement added to the
-- language later is refused by default rather than admitted by omission.
SELECT * FROM accounts;
INSERT INTO accounts VALUES ('dave', 1);
COMMIT;

-- The way out. It undoes the 777 as well: nothing in a poisoned transaction
-- is kept.
ROLLBACK;

SELECT * FROM accounts;                           -- alice is 900


-- ---------------------------------------------------------------------------
-- 7. What one connection cannot show
--
-- Everything above is one session. The behaviour transactions exist for —
-- what a reader sees while a writer is mid-flight — needs two, so run these
-- two transcripts side by side in two terminals against the *same* server:
--
--     python3 tools/ckdbs_cli.py --port 15600      # terminal A
--     python3 tools/ckdbs_cli.py --port 15600      # terminal B
--
-- ---- 7a. REPEATABLE READ holds one view for the whole transaction --------
--
--   A>  BEGIN ISOLATION LEVEL REPEATABLE READ;
--   A>  SELECT * FROM accounts WHERE id = 1;      -> 1,alice,900
--   B>  UPDATE accounts SET balance = 5000 WHERE id = 1;   -> UPDATED 1
--   A>  SELECT * FROM accounts WHERE id = 1;      -> 1,alice,900   (unchanged)
--   A>  COMMIT;
--   A>  SELECT * FROM accounts WHERE id = 1;      -> 1,alice,5000
--
-- Run the identical script with READ COMMITTED and A's *second* SELECT
-- returns 5000 — a read view per statement instead of per transaction. That
-- one line is the only observable difference between the two levels.
--
-- ---- 7b. An uncommitted write is invisible ------------------------------
--
--   B>  BEGIN;
--   B>  UPDATE accounts SET balance = 1 WHERE id = 1;
--   B>  SELECT * FROM accounts WHERE id = 1;      -> 1,alice,1      (its own)
--   A>  SELECT * FROM accounts WHERE id = 1;      -> 1,alice,5000   (not B's)
--   B>  ROLLBACK;
--   A>  SELECT * FROM accounts WHERE id = 1;      -> 1,alice,5000
--
-- ---- 7c. First-updater-wins, and the retry -------------------------------
--
--   A>  BEGIN;
--   B>  BEGIN;
--   A>  UPDATE accounts SET balance = 10 WHERE id = 1;   -> UPDATED 1
--   B>  UPDATE accounts SET balance = 20 WHERE id = 1;
--         -> ERR TXN_CONFLICT retryable=1 row id=1 was written by transaction <n>
--            (<n> is A's trx_id, which BEGIN printed; ids advance every run)
--
-- There is no lock and nothing to wait on: B is aborted immediately rather
-- than blocked. `retryable=1` is a compatibility surface — client libraries
-- build retry loops on that bit — and it is the one error worth
-- special-casing. B is now in `failed-txn`:
--
--   B>  ROLLBACK;
--   A>  ROLLBACK;
--   B>  BEGIN;
--   B>  UPDATE accounts SET balance = 20 WHERE id = 1;   -> UPDATED 1
--   B>  COMMIT;
--
-- ---- 7d. A connection that dies rolls back -------------------------------
--
--   B>  BEGIN;
--   B>  UPDATE accounts SET balance = 999999 WHERE id = 1;
--   B>  <close the terminal>
--   A>  SELECT * FROM accounts WHERE id = 1;      -> the old value
--
-- ---------------------------------------------------------------------------


-- ---------------------------------------------------------------------------
-- 8. What this engine does not do — worth knowing before you rely on it
--
--   * **DDL is not transactional.** CREATE TABLE inside a transaction is
--     not rolled back by ROLLBACK; catalog rows are stamped with the
--     always-visible transaction and catalog reads bypass the snapshot.
--   * **No savepoints**, so a second BEGIN is an error rather than a nested
--     transaction, and a failed statement cannot be undone on its own.
--   * **No SERIALIZABLE**, and it is out of scope rather than pending.
--   * **No waiting.** A write conflict is an immediate retryable abort;
--     there is no lock manager and no deadlock detection, because there is
--     nothing that can deadlock.
--   * **Nothing purges.** Undo pages and delete-marked rows accumulate;
--     readers are deliberately unregistered, which is what makes purge
--     impossible today.
--   * **Recovery does not exist.** An uncommitted row that survives a crash
--     reads as *committed* on the next boot (docs/spec/txn.md §8). This is a
--     stated, accepted gap, not an oversight — closing it needs a persisted
--     commit watermark, which is recovery.
-- ---------------------------------------------------------------------------

SYNC;
