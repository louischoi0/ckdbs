# Raft marks, 2026-11-20

The operator's words of 2026-11-20 that open a new matter, and what CLA
took under them.

## 1. DH written: the page image's price

| | |
|---|---|
| **Word** | *"Since the torn page heal came in, the overhead has grown a lot."* and *"Write a work order for that as an additional milestone."* (given in Korean; recorded in English) - after CLA's reading of `bench/v3.4.14/results-db-close-v3.4.14-5-gec057c06.md` in the same conversation: -11.7 % at 100,000-row updates on four cores, -25.3 % under pool pressure, the standard cells inside the floor |
| **Mark** | **DH is written, not opened**: `instructions/v3.2.0/workorder-dh-page-image-volume.md`, on `main` at `673f025d`, documents only. The letter and the order's exploratory standing (DH-Q0 (a)) are CLA's proposals, not the operator's word. Six levers are laid out (L0 nothing, L1 the hole left out, L2 compression, L3 the writeback rate independent of `cores`, L4 fewer copies under pool pressure, L5 a switch), none built or measured; DH-S0 prices them |
| **Corrected** | CLA told the operator that leaving the free space out of the image was the lowest-risk lever. The tree does not support it as said (the common header names no free region; the payload is the page verbatim; DB-R5 and DB §6 name it a payload change); the order says so in §0 |
| **Does not settle** | DH-Q0..Q6; whether DH opens; the bar (DH-Q2); whether a log format event is admissible (DH-Q3); the price on scenarios 1 to 3, which were measured at a tag that predates DB |

## 2. v3.4.15 named and tagged

| | |
|---|---|
| **Word** | *"tag 3.4.15"* - after CLA's report that DA landed on `main` at `1eeab8ce` with the hook skipped, the full suite 0 failed of 4865 at `89c26252`, whose code is the merge's (`raft-marks-2026-11-19.md` §17) |
| **Mark** | **The version of record is `v3.4.15`**, the operator's own word: an annotated tag on `main` at `1eeab8ce`, pushed by the operator from their own shell (`git push origin v3.4.15`) after the permission classifier refused CLA's push. Its message states what is built since `v3.4.14` (DF, DE narrow, DB, DA), that the superblock did not move, the suite and what was not executed, and the gaps that bound it - first DB's unruled close deltas (-11.7 %, -25.3 %), then DF's cost, DE's narrowness and DA-S3 owed |
| **Does not settle** | anything after `1eeab8ce` but this record; the ruling on DB's close deltas; DA-S3's host |
| **Recorded at** | this file; `CLAUDE.md`'s version of record; this file's row in `instructions/v3.2.0/index.md` |

## 3. DI written: a shape's statistics named by what the statement binds

| | |
|---|---|
| **Word** | *"First, it seems the priority is to tie the parsing of a query and the statistics about a query to the oid and the namespace, so that there is no quiet wrong answer."* and, after CLA laid out five items with a proposal each, *"Write the DI work order as proposed."* (given in Korean; recorded in English) - in a conversation that compared the engine with a shared cursor cache and a query store |
| **Mark** | **DI is written, not opened**: `instructions/v3.2.0/workorder-di-shape-identity.md`, in the main checkout on `main` at `f4138860` (`v3.4.15-1-gf4138860`), documents only. **"As proposed" marks four of the five items spoken, each (a)**: DI-Q1 (the binding enters the statistics key; the fetch id does not move), DI-Q2 (a statement with no binding keeps one row that says so), DI-Q4 (the Waystone opt-in's reach is not in DI), DI-Q5 (a renamed relation's plan reading `enabled` is in DI). The letter is CLA's proposal |
| **Corrected** | Two things CLA said in the conversation. (1) The word speaks of a quiet wrong answer; CLA found no path on which a statement's rows are wrong by the fetch id's text identity - the plan tables, the advice and the posteriors compare oids, and a replay checks `rel_oid`. What is wrong is `sys.statement_stats`' row, which sums two bindings of one text with no mark; the order uses `[quiet-wrong]` for that reading and says so in §0. (2) The fifth item, the identity, was spoken as "the plan key, reused". The plan key exists for a searched join block only, so it would leave every one-relation statement merged; **DI-Q3 is restated in the order and is not covered by "as proposed"** |
| **Does not settle** | DI-Q0, DI-Q3 and DI-Q6..Q11; whether DI opens; a statistics history or statistics per plan variant, which the conversation named as a later order |
| **Recorded at** | this file; the DI order's header, §0 and §4; the DI row and this file's row in `instructions/v3.2.0/index.md` |

## 4. DI widened: every path

| | |
|---|---|
| **Word** | *"Is there no such thing outside `statement_stats`? It has to be applied to every path on which this problem can occur."* (given in Korean; recorded in English) - after CLA's report that DI was written and pushed at `5915182c` |
| **Mark** | **DI covers every structure that names a statement by its text alone** (DI-R2a). **DI-Q4 is re-marked (b)**: the Waystone opt-in's reach, which §3 marked out of DI, is in it. The order is amended in the main checkout on `main` at `5915182c`, documents only: §0a, the Cabin controller's signal as DI-S2 (DI-Q12), the opt-in and the trail as DI-S3 (DI-Q13, with a format event, DI-Q11) |
| **Found** | Read at `5915182c`: the Cabin controller's execution signal is keyed by `pattern_id` alone and credits a pattern's whole heat to the `(relation, column)` seen last (`optimizer_signals.hpp:87-97`, `cabin_optimizer.cpp:239-240`), so one text over two relations prices one relation's Cabin with the other's heat. The engine, not a reader, acts on that merge; no result is wrong by it and the controller is off by default |
| **Does not settle** | DI-Q0, DI-Q3, DI-Q6..Q13 but DI-Q4; whether DI opens; that the census is complete - 40 files name the two ids and DI-S0 opens the ones this reading did not |
| **Recorded at** | this file; the DI order's header, §0a, §3, §4, §5 and §7; the DI row and this file's row in `instructions/v3.2.0/index.md` |

## 5. DJ written: connections, sessions and cursors

| | |
|---|---|
| **Word** | *"From the viewpoint of connections, cursors and connection pools, against the commercial databases (PostgreSQL, Oracle, Azure SQL), find the directions to fill in, or to improve into a design that fits kdbs better."* and, after CLA's reading of the tree and five directions, *"Write a work order for the improvement directions. And make the following options settable: a connection count limit, an idle session timeout."* (given in Korean; recorded in English) |
| **Mark** | **DJ is written, not opened**: `instructions/v3.2.0/workorder-dj-connections-sessions-cursors.md`, in the main checkout on `main` at `4d72be11` (`v3.4.15-3-g4d72be11`), documents only. **The two settings are the operator's word and are DJ-S1**, built whatever DJ-S0 finds; their names, defaults and edges (DJ-Q1..Q7) are CLA's proposals, unmarked. The other parts - a session handed back clean, a wait at the transaction's head in place of `Begin`'s refusal, a bounded reply - are CLA's directions under "the improvement directions", each gated on DJ-S0 and a mark. The letter is CLA's proposal |
| **Differs from what CLA said** | CLA told the operator an idle-session timeout was not needed, an idle session holding memory only. The word asks for the setting; it is built, and CLA proposes it off unless set (DJ-Q4 (a)), which is today's behaviour |
| **Does not settle** | DJ-Q0..Q12 - among them the two keys' names and defaults, what the cap counts (a `C_CANCEL` connection never, DJ-R2 `[quiet-wrong]`), an administrator's reserve, whether a `C_PING` resets the idle clock; whether DJ opens |
| **Recorded at** | this file; the DJ order's header and §0; the DJ row and this file's row in `instructions/v3.2.0/index.md` |


## 6. DI opened on the standing go-ahead

| | |
|---|---|
| **Word** | The operator invoked `go-ahead-achieving-milestone` with the argument `DI`: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* |
| **Mark** | **DI is opened**, on worktree `di-shape-identity` from `1c7bb774` (`v3.4.15-4-g1c7bb774`). **No item below is the operator's own mark**: each is CLA's proposal, adopted by CLA on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone). The operator's own marks stay DI-Q1, Q2, Q5 (a) and DI-Q4 (b) (§3, §4) |
| **Adopted at DI-S0** | **DI-Q0 (a)**: a census first. **DI-Q3 `[quiet-wrong]` (a), revised against the tree before it was adopted**: the binding is the distinct relations in first-declared order with the namespace the statement's unqualified names were resolved in, up to 16; the argument, and the two premises the stage's review added to it, are the order's §7, "DI-Q3 revised" |
| **Restated at DI-S0, adopted as their stages reach them** | DI-Q6 (the views gain `binding`, `namespace`, `relation_oids`, and `relations` on the statistics view); DI-Q7 (a part reclaims a gone relation's rows when a new shape meets it full, asked of the recording core's catalog); DI-Q9 (a) (no engine reader joins a sample to a statistics row); DI-Q10 (P1 as written cannot be done - no row stores its namespace - so a rename is read off the recorded oids' current names); DI-Q12 (a); DI-Q13 `[quiet-wrong]` (a) (a `DROP TABLE` ends the opt-ins naming the relation; `WAYSTONE OFF <fetch_id>` is the form that needs no relation); DI-Q8, DI-Q11 as written |
| **Adopted at DI-S1** | **DI-Q6 (a)** and **DI-Q7 (a)**, each as DI-S0 restated it (the order's §7) |
| **Adopted at DI-S2** | **DI-Q12 (a)**: the signal keyed by the pattern and its candidate together |
| **Adopted at DI-S3** | **DI-Q13 `[quiet-wrong]` (a)**, as DI-S0 restated it and with its premise read again (`WAYSTONE ON` compiles its example under the session's namespace): an opt-in names the shape under the relations its example bound, and a committed `DROP TABLE` ends it. **DI-Q11 (a)**: one format event, superblock 38, which also carries DA-S3 by DA-Q1 (a) |
| **Does not settle** | a push, a merge to `main`, a tag or a version: each waits for the operator's word |
| **Recorded at** | this file; the DI order's header, §4 and §7; the DI row and this file's row in `instructions/v3.2.0/index.md` |
