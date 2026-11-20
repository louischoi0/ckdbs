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

## 6. CV opened on the standing go-ahead

| | |
|---|---|
| **Word** | `/go-ahead-achieving-milestone CV` - the standing word *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session"*, given for CV |
| **Mark** | **CV is opened**, on worktree `cv-ordered-index-walk` from `f4138860` (`v3.4.15-1-gf4138860`). **No CV-Q item is the operator's own mark.** Each is **adopted on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone)**, as CLA proposed it: CV-Q0..Q6 each (a), CV-Q3 (a) as revised at CV-S0 (a list-filled prefix is no ordered candidate), and CV-Q7 (a), raised at CV-S0 and written into the order's §4 with its options before it was adopted. The two `[quiet-wrong]` items, CV-Q3 (the index choice's ties) and CV-Q7 (no string order key: IX6 truncates), were adopted after their premises were re-read on the tree at `f4138860` (the order's §7, CV-S0) |
| **Does not settle** | a push, a merge to `main`, a tag, a version; the options the standing go-ahead did not take (CV-Q0 (b), CV-Q1 (b), CV-Q2 (b), CV-Q3 (b), CV-Q4 (b)/(c), CV-Q5 (b), CV-Q6 (b), CV-Q7 (b)), which stay open to the operator's own mark |
| **Recorded at** | this file; the order's header, §4 and §7; its row in `instructions/v3.2.0/index.md` |

## 7. DK written: the btree descent and the immutable low key

| | |
|---|---|
| **Word** | *"Is there room to optimize, using the property that a page's `min_key` is immutable, so that a btree descent goes down without holding a latch or a lock? When the page has been split it would have to retry."* and, after CLA's reading of the tree, *"For now, put it as an exploratory milestone and write the work order."* (given in Korean; recorded in English) |
| **Mark** | **DK is written, not opened**: `instructions/v3.2.0/workorder-dk-descent-low-key.md`, in the main checkout on `main` at `1c7bb774` (`v3.4.15-4-g1c7bb774`), documents only. **Exploratory is the operator's word (DK-Q0)**; the letter and the candidates are CLA's proposals: D1 a write descent's leaf fetched exclusive at once from the parent's `level`, D2 a leaf that moves right instead of restarting, D3 the internal levels read optimistically - which is CW's C3a and is built only under CW (DK-Q1 (a), unmarked); D4, the low key as the only validation, rejected on paper |
| **Found** | CW (`workorder-cw-optimistic-page-read.md`, written 2026-11-17, not opened) already holds the latch-free internal-node read as its C3a, and excludes B-link moves (CW §6); DK does not restate its protocol. The secondary tree's leaf stores no low key: its bound is the right sibling's first entry, fixed only while no entry is removed (`index_tree.cpp:60-64`), which DD would touch |
| **Corrected** | In the conversation CLA said a failed internal node could be re-read in place rather than from the root. It is sound, the leaf's check catching any stray left, but it saves a root restart only once D2 is built, since an internal node has no low key or right link to move along (DK-Q6) |
| **Does not settle** | DK-Q1..Q9; whether DK opens; whether CW opens |
| **Recorded at** | this file; the DK order's header and §0; the DK row and this file's row in `instructions/v3.2.0/index.md` |

## 8. v3.4.16 named and tagged

| | |
|---|---|
| **Word** | *"tag 3.4.16"* - after CLA's report that CV landed on `main` at `fda024cf` with the hook skipped on the word "push no verify", the full suite 0 failed of 4897 at `7d11ba44`, whose code is the merge's |
| **Mark** | **The version of record is `v3.4.16`**, the operator's own word: an annotated tag on `main` at `0078e149` (CV's merge `fda024cf` and DK's documents above it). Its message states what is built since `v3.4.15` (CV), that the superblock did not move, the suite and what was not executed, and the gaps that bound it - first the unmeasured many-leaf reverse walk, then the reverse walk's blocking leaf reads, the unseeded one-sided bound and the decline's memory, with v3.4.15's carried gaps |
| **Does not settle** | anything after `0078e149` but this record |
| **Recorded at** | this file; `CLAUDE.md`'s version of record; this file's row in `instructions/v3.2.0/index.md` |

## 9. The statistics rule recorded, and DL written

| | |
|---|---|
| **Word** | *"It seems we have to set the principle that statistics must not, in any case, line up a write or a read (query execution)."* - after CLA's account of DA's page-11 hold (-11.6 % of throughput at `cores = 8`, `bench/v3.4.14/results-da-s0-premise-v3.4.14-30-g673f025d.md`) - and, after CLA's reading of the tree and a five-clause draft, *"For now record it as a rule; in the end strictly, and for now establish this rule through a separate milestone. Write the work order of the milestone that will establish this rule."* (given in Korean; recorded in English) |
| **Mark** | **The rule is recorded**: `docs/rules/rules.md` §3a, the operator's sentence, binding on every statistics path added or changed from 2026-11-20. **Its end state is strict** - a record waits for no reader of the statistic, not even one copy of its own part - the operator's word, DL-Q1. **DL is written, not opened**: `instructions/v3.2.0/workorder-dl-statistics-queue-no-statement.md`, in the main checkout on `main` at `1aaafe1b` (`v3.4.16-1-g1aaafe1b`), documents only; it is not exploratory. What does not meet the rule today is listed in `docs/inflight/known-gaps.md` ("Statistics that can queue a statement"). The letter, the five clauses the rule is read as, the census and the stages are CLA's proposals |
| **Found** | Read at `1aaafe1b`: every successful `SELECT` with an identity takes the Cabin controller's collector latch, one for the instance, whether or not `cabin_optimizer` is on (`optimizer_signals.cpp:68-81`, `command_dispatcher.cpp:10719`) - page 11's shape with a shorter section, unmeasured, and inside both arms of every `cores = 8` cell DA measured; a Cabin probe's hit takes three shared latches to count itself; the per-core parts wait for a reader's copy under a `std::mutex`, which DF-R2 and DA-R3 allow and the strict form does not |
| **Differs from what CLA said** | CLA's draft carried a clause "a switched-off consumer collects nothing". It is a cost, not a queue; it is not in the rule and is DL-Q3. The rule gained a clause the draft lacked: a statement's own read of a statistic waits for no writer (clause (d), the plan-statistics publish latch). CLA's census in the conversation was a first reading of `src/stats/`; the order says so and DL-S0 is the census |
| **Does not settle** | DL-Q0 and DL-Q2..Q10 - among them what counts as a statistic at the edges (Waystone trails, the controller's view), the strict shape a store takes, whether the collector runs while its consumer is off, wait-free words every core writes, whether a gathering task must give way; whether DL opens; the clauses' wording, which a mark amends |
| **Recorded at** | this file; `docs/rules/rules.md` §3a; `docs/inflight/known-gaps.md`; the DL order's header and §0; the DL row and this file's row in `instructions/v3.2.0/index.md` |

## 10. DL amended: a switched-off consumer collects nothing

| | |
|---|---|
| **Word** | *"Add the principle 'a switched-off consumer collects nothing' to that milestone too."* (given in Korean; recorded in English) - after CLA's report that DL was written and that the clause had been left out of the rule as a cost matter and made an open item, DL-Q3 |
| **Mark** | **The principle is a rule DL establishes**: recorded as the second entry of `docs/rules/rules.md` §3a - where a setting turns off the only consumer of a statistic, a statement's path records nothing for it, and a switch-on starts from no evidence - and **DL-Q3 is marked (a)**, the operator's word. The order is amended in the main checkout on `main` at `1aaafe1b`, before its first commit, documents only: DL-R10, DL-R11, a table of consumers with a switch (§1.2), DL-S1's first half, a test per switch in DL-S5 |
| **CLA's reading, to be corrected if wrong** | The word says "add to the milestone"; CLA also recorded it in `rules.md`, beside the first rule and not inside it, since a principle a milestone establishes is a rule of the tree. It adds no switch (DL-Q11 (a), unmarked): a store a `sys` view reports keeps BO-Q8's "no switch" |
| **Found** | Read at `1aaafe1b`: the collector's S1-S3 are the one case that breaks it - with `cabin_optimizer` off, the default, every fingerprinted `SELECT` still builds an identity for the collector, walks its chain for a candidate and notes under the instance's latch (`command_dispatcher.cpp:10261-10265`, `:10683-10719`); S4, the access statistics and ASH already ask their switch |
| **Does not settle** | DL-Q11 (whether the rule adds switches), DL-Q12 (whether any `join_order` value turns the planner's observations' consumer off), DL-Q13 (evidence already collected at a switch-off); DL-Q0, DL-Q2 and DL-Q4..Q10 as before; whether DL opens |
| **Recorded at** | this file; `docs/rules/rules.md` §3a; `docs/inflight/known-gaps.md`; the DL order's header, §0, §1.2, §3, §4, §5 and §7; the DL row and this file's row in `instructions/v3.2.0/index.md` |

## 11. The audit's dead code: nine survivors removed

| | |
|---|---|
| **Word** | *"Among the audit results, is anything left that was verified and not processed?"* and, after CLA listed what is left - two logic defects and two dead-code survivors waiting for a ruling, nine dead-code survivors needing none, five duplication survivors - *"Start with the nine dead-code findings that need no ruling."* (given in Korean; recorded in English) |
| **Mark** | **G43, G45, G46, G48, G49, G51, G52, G54 and G55 are removed**, each as the verifier of 2026-11-17 refined it, on worktree `audit-dead-code-g43-g55` at `8f5d4388`, from `1c7bb774`. No statement's answer, refusal, lock or record changes; the mount's recovery line loses ", skipped-not-dirty N" |
| **Landed** | *"Wrap up the work so far for now; it has to be pushed with no verify."* (given in Korean; recorded in English) - said while the merged tree was building. The branch is merged to `main` with `--no-ff` and pushed with `--no-verify`: **the hook was skipped, and the merged tree's suite was not executed before the push**. What ran: 4,863 of 4,863 on `8f5d4388`'s build inputs, before `origin/main` (`b0a824db`: CV, `v3.4.16`, DK, DL) was merged at `755e9000` with one conflict in `index_tree.cpp`. The simulation corpus was stopped at 17 passing runs. The overhead is not measured. **Run after the push, the suite passed 4,895 of 4,895 on the tree `main` carries at `1cd689ae`** |
| **Outside the word** | One cell the suite showed flaky was corrected: `DropTableReclaimServingTest`'s two-core cell required every dropped page to be unallocated after a remount, and a still-serving writer takes a freed id (`page.md` §5). It failed with the old redo filter restored too. CLA changed its expectation to "free, or no longer the dropped relation's" on its own judgment; the operator may want it back as it was with an entry under `docs/inflight/bugs/` |
| **Does not settle** | G44 and G47 (a ruling each: `page.md` §3's list of `PageRef`'s surface; whether `page_mgr` moves to a test target or is deleted with `wal.md` and `page.md` reworded); G31, G39 with G53, G50; G26-R and G21-R; whether the five duplication survivors are taken up; the overhead of the audit's fixes, not measured; the 15 unread slices |
| **Recorded at** | this file; `docs/pending/whole-engine-audit-second-run-twelve-slices.md` §13 and its header; its row in `docs/pending/README.md`; this file's row in `instructions/v3.2.0/index.md` |

## 12. DM written: a leaf's upper bound

| | |
|---|---|
| **Word** | *"And what if it were managed by a `max_key` instead of the `min_key`?"*, then *"The pros and cons, and the cost and effect, of adding the upper bound."*, and, after CLA's account and its question whether to add the two forms to DK as candidates, *"For now add them as candidates, and write it as an exploratory milestone."* (given in Korean; recorded in English) |
| **Mark** | **DM is written, not opened**: `instructions/v3.2.0/workorder-dm-leaf-upper-bound.md`, in the main checkout on `main` at `b0a824db` (`v3.4.16-2-gb0a824db`), documents only. **Exploratory is the operator's word (DM-Q0)**, and so is that the upper bound's two forms are candidates: U1, the right sibling's low key remembered in a core-local memo, and U2, the bound kept on the page, a format event. Replacing `min_key` by an immutable `max_key` is recorded as rejected on paper |
| **CLA's reading, to be corrected if wrong** | The word can be read as "add them to DK" or as "write a milestone for them". CLA did both in the smallest way: DM is an order of its own, because its question ends at a format event and DK promises none, and DK's §2 names the two as D5 and D6 and points at DM. DM-Q1 (b) folds them into DK and deletes DM, if that is what was meant |
| **Found** | Read at `b0a824db`: a coverage check fetches the right sibling with the leaf held (`btree.cpp:98-114`, `index_tree.cpp:75-95`), paid by nearly every secondary index insert, by a named key landing mid-chain, by a lookup's miss and by a write lookup; nothing makes that sibling resident, so under pool pressure it is a device read under an exclusive hold. A range walk reads one leaf past its range. **And a correction to DK**: CV's `BtreeFetchLeafAtOrBelow` and `IndexFetchLeafAtOrBelow` already walk right along the chain, so DK's D2 is an existing shape and DK's §1, read before CV merged, did not say so; DK's stage log is amended |
| **Corrected** | CLA's figure of "0.1-0.3 us a fetch, about 1 % of an insert an index" is an estimate; the order says so and DM-S0 measures it. No sibling fetch has been counted or priced in any results file |
| **Does not settle** | DM-Q1..Q9 - among them whether DM stands beside DK, the bars, whether a format event is admissible and when (DM-R6: never without the operator's word), U2's reach and place, whether the B-link internal node is anybody's; whether DM opens |
| **Recorded at** | this file; the DM order's header and §0; the DK order's §2 and §7; the DM and DK rows and this file's row in `instructions/v3.2.0/index.md` |

## 13. DN written: the heap redefined again, an Oracle-style heap

| | |
|---|---|
| **Word** | *"I am going to redefine the heap table concept once more. I intend to name an Oracle-style table a heap table. That is, rows are stored at random by key but in order by rowid. Range search by key is supported through an intermediate index. A key access always goes through the index and then a lookup happens, but a full table scan scans the data pages directly with no index."* Then, to CLA's five questions: *"1. (b), assuming DD comes first."*; *"2. Where there is no PK, the engine issues a Keystone internally to tell rows apart (the rowid a value computed from the data page, so that it can be computed back in one step)."*; *"2. The key's identity: a multi-column PK, a PK of other types supported, and where no PK is created, the engine-issued Keystone in its place (in that case not seen by `SELECT *` in SQL, but its value can be read by naming the column, e.g. `keyidentity`)."*; *"3. No global order guaranteed."*; *"If the user sets no PK or makes it an int64, the engine-issued Keystone is used."* Then, asked whether a single `int64` PK is itself the Keystone, named or issued: *"(a) is right, write it."* (given in Korean; recorded in English) |
| **Mark** | **DN is written, not opened**: `instructions/v3.2.0/workorder-dn-heap-rowid.md`, in the main checkout on `main` at `d091d096` (`v3.4.15-19-gd091d096`), documents only. **The operator's marks, DN-D1..D5**: a slot is reused only once every index entry naming it is removed, DD the prerequisite (D1); the Keystone is the row's identity and the rowid its address, computed from `(page, slot)` and back in one step (D2); a PK of any column count and type, and with none the issued Keystone, hidden from `*` and readable by an explicit name (D3); no global insertion order (D4); a single `int64` PK is the Keystone (D5). The letter and the order's exploratory standing (DN-Q0) are CLA's proposals |
| **Reverses** | BZ's definition of 2026-10-17 (`raft-marks-2026-10-17.md` §1): the heap's storage, and "the hidden key is not nameable from SQL" (`$!a`), which DN-D3 replaces |
| **Does not settle** | DN-Q0..Q10 - among them the hidden Keystone's name, whether a hidden-Keystone heap always has a Keystone index, whether an entry carries the Keystone beside the rowid `[quiet-wrong]`, a foreign key to a heap, a BZ heap on an existing volume; the amended invariants 2, 3, 4 and 11 (DN-R9); whether DN opens; whether DD opens first |
| **Recorded at** | this file; the DN order's header and §0; the DN row and this file's row in `instructions/v3.2.0/index.md` |

## 14. DN: the name reserved, and invariants 2 to 4 the btree's

| | |
|---|---|
| **Word** | *"The name for reading it (`keyidentity` or the like) as a reserved word."* and *"Invariants 2 to 4 are btree-only."* (given in Korean; recorded in English) - answers to two of the conflicts CLA listed before DN was written, given as it was pushed at `4defff7a` |
| **Mark** | **DN-D6: the hidden Keystone's name is a reserved word** - no identifier may take it. **DN-D7: invariants 2, 3 and 4 bind a btree page and no heap data page.** The order is amended in the main checkout on `main` at `4defff7a`, documents only: §0's table, §1.4, DN-R5, DN-R9, DN-Q1 |
| **Not done at the mark** | `CLAUDE.md`'s and `heap-and-tuple.md` §8's invariants are not edited: both state what is built, no heap data page exists, and 2 and 3 name no page class today, so whether a catalog chain page relies on either is read first (DN-S0) and the text written at DN-S1 |
| **Does not settle** | the word's spelling ("or the like"; CLA proposes `keyidentity`) and its edges (DN-Q1: what it reads on a relation with a declared Keystone, a volume that already declares the name, the fingerprint's bump rule); invariant 11's wording for a heap; DN-Q0 and DN-Q2..Q10; whether DN opens |
| **Recorded at** | this file; the DN order's header, §0, §1.4, §3, §4 and §7; this file's row in `instructions/v3.2.0/index.md` |

## 15. DN-D7 revised: the `min_key` invariants bind a heap too

| | |
|---|---|
| **Word** | *"But even for a heap, the `min_key` invariant is kept (applied)."* (given in Korean; recorded in English) - straight after §14's "invariants 2 to 4 are btree-only" |
| **Mark** | **DN-D7 as revised: invariants 2 and 3 bind a heap data page; invariant 4's order is the btree leaf's alone, and a heap data page is unordered.** CLA reads "the `min_key` invariant" as 2 and 3 together. The order is amended in the main checkout on `main` at `e3b770e6`, documents only: on a heap invariant 3 becomes a rule of placement (DN-R10) - a row goes only to a page whose `min_key` is at or below its Keystone |
| **What it costs, as read** | An issued Keystone always passes (the mark is above every key placed, so a reused slot takes a higher one). A named `int64` key (DN-D5) lower than every open page's `min_key` cannot be placed on them; keys named in descending order would open a page a row under CLA's proposal for what the field is (DN-Q11 (a)) |
| **Does not settle** | DN-Q11 - what a heap page's `min_key` is; whether the reading of "the `min_key` invariant" as 2 and 3 is the operator's; DN-Q0..Q10 as before; whether DN opens |
| **Recorded at** | this file; the DN order's header, §0, §1.4, §3, §4 and §7; this file's row in `instructions/v3.2.0/index.md` |

## 16. A note in DD: read again with DN's consumer

| | |
|---|---|
| **Word** | *"Leave a note so that DD is looked at again from that viewpoint."* (given in Korean; recorded in English) - after CLA's report that DN makes DD its prerequisite, and that DD's premise, weak as an index's growth, has under DN a consumer it did not have: without entry removal a heap never reuses a slot |
| **Mark** | **The note is left**: `workorder-dd-index-entry-purge.md` §0a, with a pointer in its header and in DN §1.5, in the main checkout on `main` at `9c7d3b2e`, documents only. Eight items for DD-S0 to read again: DD-Q0's verdict rule has no arm for DN; DD-Q1 (a) keeps a delete-marked row's head entry; DD-Q3 (a) pins one slot a leaf of every index; DD-Q4 (a) makes space reuse an administrator's statement; DD purges an index where DN needs to know a slot is named by no index; DD-R1's byte-equality is about another entry; DD-S0's cells measure no heap; and CV's reverse index walk now leans on "no entry is removed" |
| **Does not settle** | anything in DD: no DD-Q is marked, no rule or stage changed, and each of the eight is a question for DD-S0, not a finding; whether DD or DN opens; the order CLA proposes, DN-S0's census before DD-S0 |
| **Recorded at** | this file; DD's header, §0a and §7; DN §1.5; the DD row and this file's row in `instructions/v3.2.0/index.md` |

## 17. DH opened on the standing go-ahead

| | |
|---|---|
| **Word** | The operator invoked `go-ahead-achieving-milestone` with the argument `DH`: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* (the skill's standing word) |
| **Mark** | **DH is open**, on worktree `dh-page-image-volume` from `f4138860` (`v3.4.15-1-gf4138860`). **Every DH-Q item is CLA's proposal adopted under that word; none is the operator's own per-item mark**: DH-Q0 (a) exploratory; DH-Q1 (a), cut to the budget; DH-Q2 (a), which sets **no bar** and leaves it to the operator with the close's numbers; DH-Q3 (a) `[quiet-wrong]`, its premise re-read and then measured by a spike before the format event was admitted; DH-Q4 (a) `[quiet-wrong]`, its premise re-read against the checkpointer and the anchor's fold; DH-Q5 (a); DH-Q6 (a). Items CLA added and adopted the same way: DH-Q7 (the byte counter comes with the packed image), DH-Q8 (DH-S0's A arm is main with the image debt never paid, not the pre-DB commit - a revision of the stage as written), DH-Q9 (L3 then L2 are built; L1 and L4 dropped; L5 unbuilt), DH-Q10 `[quiet-wrong]` (the cadence is the instance's: one run a period, the cores in turn), DH-Q11 `[quiet-wrong]` (a new record kind 35 for the packed image, kind 10 untouched, no version bump), DH-Q12 (zero suppression by word, no dependency), DH-Q13 (only the write fetch's image is packed) |
| **Does not settle** | the bar (DH-Q2); any push, merge to `main`, tag or version; the options not taken, each open to the operator's own mark |
| **Recorded at** | this file; the order's header, §4.1 and §7; the order's row in `instructions/v3.2.0/index.md` |

## 18. DH closed on the operator's word, its two costs accepted

| | |
|---|---|
| **Word** | *"For now, end the test right now and close the milestone; push no verify."* and, a few minutes later, *"Accept the 21 % that remains under pool pressure and the recovery of 0.19 s to 0.72 s."* (given in Korean; recorded in English) - after CLA's interim report of the close's measurement: B (`22818e44`) faster than A (`f4138860`) by 6.2 % and 15.2 % at four and eight cores on 100,000-row updates, inside the floor at one core, 13.0 % faster under pool pressure on B's first round and still 20.7 % under the no-image bound there, and a mount after a crash at eight cores of 0.72 s against 0.19 s, one round |
| **Mark** | **DH is closed**, the operator's own word, on worktree `dh-page-image-volume`; its closing code is `22818e44`. **The two costs are accepted, the operator's own mark, and that answers DH-Q2**: no percentage was named. With the second round the pressure cell reads B 19.5 % under the bound (2,887 against 3,585 stmt/s), not 20.7 %; the word was spoken on the first |
| **Skipped on that word** | the rest of the close's measurement (stopped at 06:48 UTC: `select-pk`, `insert`, scenario 2, the four-core pressure cell, `strict`, the second recovery round - "no cell slower" is not claimed); `critics-developer`'s review of the close step; the suite on the merged tree (`18f2e339`, which brought CV and the audit's removals), last run 0 failed of 4,883 before the merge; the pre-push hook |
| **Does not settle** | a tag or a version; the options DH-Q0..Q13 did not take, each still open to the operator's own mark, DH-Q11's format event among them (`wal.md` §5.2 as amended); whether the unmeasured cells are measured later |
| **Recorded at** | this file; the order's header, §4.1 and §7 (DH-S3); `bench/v3.4.15/results-dh-close-v3.4.15-5-g22818e44.md`; `known-gaps.md` (WAL, Testing); the DH row and this file's row in `instructions/v3.2.0/index.md` |

## 19. DI opened on the standing go-ahead

| | |
|---|---|
| **Word** | The operator invoked `go-ahead-achieving-milestone` with the argument `DI`: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* |
| **Mark** | **DI is opened**, on worktree `di-shape-identity` from `1c7bb774` (`v3.4.15-4-g1c7bb774`). **No item below is the operator's own mark**: each is CLA's proposal, adopted by CLA on the operator's standing go-ahead of 2026-11-20 (go-ahead-achieving-milestone). The operator's own marks stay DI-Q1, Q2, Q5 (a) and DI-Q4 (b) (§3, §4) |
| **Adopted at DI-S0** | **DI-Q0 (a)**: a census first. **DI-Q3 `[quiet-wrong]` (a), revised against the tree before it was adopted**: the binding is the distinct relations in first-declared order with the namespace the statement's unqualified names were resolved in, up to 16; the argument, and the two premises the stage's review added to it, are the order's §7, "DI-Q3 revised" |
| **Restated at DI-S0, adopted as their stages reach them** | DI-Q6 (the views gain `binding`, `namespace`, `relation_oids`, and `relations` on the statistics view); DI-Q7 (a part reclaims a gone relation's rows when a new shape meets it full, asked of the recording core's catalog); DI-Q9 (a) (no engine reader joins a sample to a statistics row); DI-Q10 (P1 as written cannot be done - no row stores its namespace - so a rename is read off the recorded oids' current names); DI-Q12 (a); DI-Q13 `[quiet-wrong]` (a) (a `DROP TABLE` ends the opt-ins naming the relation; `WAYSTONE OFF <fetch_id>` is the form that needs no relation); DI-Q8, DI-Q11 as written |
| **Adopted at DI-S1** | **DI-Q6 (a)** and **DI-Q7 (a)**, each as DI-S0 restated it (the order's §7) |
| **Adopted at DI-S2** | **DI-Q12 (a)**: the signal keyed by the pattern and its candidate together |
| **Adopted at DI-S3** | **DI-Q13 `[quiet-wrong]` (a)**, as DI-S0 restated it and with its premise read again (`WAYSTONE ON` compiles its example under the session's namespace): an opt-in names the shape under the relations its example bound, and a committed `DROP TABLE` ends it. **DI-Q11 (a)**: one format event, superblock 38, which also carries DA-S3 by DA-Q1 (a) |
| **Adopted at DI-S4** | **DI-Q10 (a)** as DI-S0 restated it; and two items the stage reached and wrote into the order first: **DI-Q14 (a)** (a column's rename is established where the statement writes the column by its binding, and an unqualified one reads as before) and **DI-Q15 (a)** (a plan that can match nothing reads `invalid`, for either origin, and restricts no drop) |
| **Re-adopted at the close** | **DI-Q7 (c), in place of (a)**: the merge of `origin/main` at the close brought the operator's rule of §9 - a record does nothing but record, no scan, nothing a statement waits on - and DI-S1's reclaim scanned a part and read the catalog on the record of the statement that met the part full. It is taken out; a dropped relation's rows stay until a restart, as every row did before DI. CLA's reading of the rule against its own stage, not the operator's word on DI-Q7 |
| **Adopted at the close** | **DI-Q8 (a)**: no cell slower outside its own spread, no factor |
| **Does not settle** | a push, a merge to `main`, a tag or a version: each waits for the operator's word. Whether DI-S2's re-keyed collector, which the rule of §9 also reaches and DL owns, should have waited for DL |
| **Recorded at** | this file; the DI order's header, §4 and §7; the DI row and this file's row in `instructions/v3.2.0/index.md` |

## 20. DI closed and landed: push no verify

| | |
|---|---|
| **Word** | *"For now, finish every process, do the close and push no verify."* - after CLA's progress report on DI: S0 to S4 committed, `origin/main` merged, DI-S1's reclaim taken out, the close measured, the merged tree's suite green and `scripts/sim.sh` running - and, after CLA said it read "finish" as letting the simulator run to its end, *"Stop the simulator and push right away."* (both given in Korean; recorded in English) |
| **Mark** | **DI is closed and landed**: the work branch merged to `main` with `--no-ff` and pushed with the hook skipped. The suite the hook would run had run on the pushed build inputs (0 failed of 4933 (2 disabled, 2 skipped), on worktree `di-shape-identity`, the merged tree with the reclaim out); `scripts/sim.sh`: stopped on the operator's word at 30 of its 280 runs, none of the 30 failing - not a run of the corpus on this tree, and not a pass |
| **Skipped on the operator's word** | the pre-push hook, and the rest of the simulator corpus: it is not run on the closing tree and is not reported as a pass. **And the full suite on the tree as pushed**: `origin/main` had moved to `799a767d` with code (DH, the audit's dead code) and was merged again at the push; the merged tree was built and 111 cells run on it, the WAL golden log re-pinned for DA-S3 and DH-S1b together among them, and the suite's figure above is the tree before that merge |
| **Does not settle** | a tag, a version; the items §19 leaves open (DI-S2's re-keyed collector under the rule of §9; DI-Q7 (c) as CLA's reading of that rule against its own stage) |
| **Recorded at** | this file; the DI order's header and §7; the DI row and this file's row in `instructions/v3.2.0/index.md` |

## 21. v3.4.17 named and tagged

| | |
|---|---|
| **Word** | *"tag 3.4.17"* - after CLA's report that DI landed on `main` at `fba5529f` with the hook skipped, that the suite on that tree had been stopped on the operator's word before it reported, and that nothing further was to push |
| **Mark** | **The version of record is `v3.4.17`**, the operator's own word: an annotated tag on `main` at `fba5529f` (DI's merge, over DH's `799a767d` and the audit's dead-code removals). Its message states what is built since `v3.4.16` (DI, DA-S3, DH, the audit's removals, the statistics rules), the format event (superblock 38: a `v3.4.16` volume does not mount), **that the full suite was not executed on the tagged tree** - last run 0 failed of 4933 at `4459df5c`, before DI's second merge; the tagged tree built and 111 cells passed - and the gaps that bound it: DI's unresolved lean on a write, a dropped relation's statistics rows kept until a restart, the collector left to DL, what a plan out of reach does not establish, DH's stopped close measurement and its two accepted costs, and `v3.4.16`'s carried gaps |
| **Does not settle** | anything after `fba5529f` but this record; whether the suite is run on the tagged tree |
| **Recorded at** | this file; `CLAUDE.md`'s version of record; this file's row in `instructions/v3.2.0/index.md` |
