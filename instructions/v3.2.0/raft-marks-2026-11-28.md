# Raft marks - 2026-11-28

The operator's words of 2026-11-28 (UTC), recorded in English.

## 1. v3.5.1 named, tagged and pushed

| | |
|---|---|
| **Word** | *"tag 3.5.1"*, to the report of DX's landing, then *"push 3.5.1"* |
| **Mark** | **The version is `v3.5.1`, the operator's own word**: an annotated tag on `main` at `b95be2e86` (`origin/main` at the moment of tagging), **pushed to `origin`**. Its message states what is built since `v3.5.0` (DD, DL, DK, DN, DO, DP, DQ, BS, BG, DR, DS, DU, DT, DW, DV, DZ, EA, DX, EB; DG, DM, CW and EE abandoned), superblock 41, the suite (5,417 of 5,417 at `10cb82a27`, whose build inputs are the tagged tree's; not executed on the tagged commit itself; `tsan`, `bs-nightly` and the full `sim.sh` corpus not executed) and the gaps that bound it. **Found at the tag**: `v3.5.0` and `v3.4.18`, recorded as local tags (`raft-marks-2026-11-20.md` §25, §26), exist neither in this repository nor on `origin`, so `git describe` below `v3.5.1` reads from `v3.4.17`; they were not re-created |
| **Does not settle** | whether `v3.5.0` and `v3.4.18` are re-created at `07f4552a` and `56d57f16` |
| **Recorded at** | this file; `CLAUDE.md`'s version of record; this file's row in `instructions/v3.2.0/index.md` |

§2's first word was given on 2026-11-27 and ran on the 28th.

## 2. The PostgreSQL control run of scenarios 1 to 4, asked ten hours ahead; its results file landed with the hook skipped

| | |
|---|---|
| **Word** | *"In ten hours, run the Pg bench for scenarios 1, 2, 3 and 4, to be used as the control group"*, given at about 13:57 UTC on 2026-11-27; then *"when it finishes, write the results file with ck-tester"*; then, at the push gate, *"merge to main with no-verify and push"* |
| **Mark** | **The run was scheduled as a user systemd timer and ran on 2026-11-28**: attempt 1 (00:16:21 UTC; the timer was armed at 14:01:53 UTC on the 27th and due at 00:01:53, so fourteen minutes late, the delay's cause unverified) ran no cell because another project's container held port 15433; CLA moved it to port 15434 and started attempt 2 at once, **36 cells, every return code 0, 00:18:31 to 00:32:03 UTC**, PostgreSQL 18.7 at its defaults in a container on the host network, server on CPUs 0 to 7. **The results file was written by `ck-tester`, reviewed by `critics-developer` (sixteen errors of text corrected, every table matching its JSON) and landed at `f8c42bb09`** (`bench/v3.5.0/results-pg-control-scenarios-v3.4.17-202-g481d101aa.md`) |
| **Skipped, on the word** | the pre-push hook; no suite ran, and the branch carried `bench/` only |
| **CLA's, not the operator's** | the choice of a container over a native server (the host has none), of PostgreSQL 18 over the 17 image already on the host, of the cells (those of `v3.3.0-33-g9682b725`'s PostgreSQL run, unchanged) and of the re-run on another port |
| **Does not settle** | a KDS run to set beside it; a tag or a version |

## 3. A PostgreSQL-KDS comparison from the existing numbers, pushed with the hook skipped, and corrected by CLA the same day

| | |
|---|---|
| **Word** | *"PG and kdbs performance comparison"*; to CLA's question of how the KDS side was to be had - a fresh run past the 20-minute budget, a cut run inside it, or the existing pinned numbers at `6c98f7c32` - **the operator chose "compare at once with the existing numbers"**; then *"commit it as a comparison file and push with no-verify"* |
| **Mark** | **The comparison is a file of no measurement of its own**, `bench/v3.5.0/comparison-pg-kds-scenarios-v3.4.17-166-g6c98f7c32.md`, landed at `cce938aad` |
| **Corrected** | **Two of the three leads its first version gave PostgreSQL were CLA's readings and are not supported by the tree**: the KDS scenario drivers time a client that decodes and renders every field while the PostgreSQL twins of scenarios 2 to 4 time one that counts rows (measured once, at `e81ac7cd`; scenario 1's twin decodes and joins every field), and the commit's sync is at parity in the archived JSON (135.5 µs against 138.2 µs). The corrected file landed at `b95be2e86`. **The correction's push is CLA's reading of the same word** ("push the comparison file"), not a second word |
| **Skipped, on the word** | the pre-push hook, both times; no suite ran and no `critics-developer` review was run on the file; documents only |
| **Does not settle** | whether the matched KDS cells are measured on `main` (EF-S2 proposes it) |

## 4. A milestone asked for each of the comparison's three slow items

| | |
|---|---|
| **Word** | *"and write a milestone for each of the slow issues 1, 3 and 4"* - the comparison's items 1 (a short statement's fixed cost), 3 (the durable commit path) and 4 (a full-scan aggregate), as its first version numbered them |
| **Mark** | Three orders are to be written. Before any was, CLA's reading of the tree for their premises found items 1 and 3 unsupported (§3's correction), and CLA put the form of the three to the operator (§5) |

## 5. EF, EG and EH written re-scoped; none opened

| | |
|---|---|
| **Word** | CLA asked which form the milestones take: (i) three, each re-scoped to what the tree supports; (ii) two only, item 3 unregistered; (iii) three as first asked, each stating its broken premise. The first asking was dismissed with *"ask again"*; asked again, **the operator chose (i), "three, re-scoped"**, which CLA had recommended |
| **Mark** | **Three orders are written and none is opened**, on worktree `pg-kds-comparison-orders` from `b95be2e86` (`v3.4.17-255-gb95be2e86`), documents only: **EF** (the scenario drivers on an equal client, the matched cells measured again, a short read's server cost only behind a gate), **EG** (one client's durable write beyond its sync: `result-insert` attributed, the one-core inline sync booked, a cut only behind a gate) and **EH** (what the native fold pays for a row) |
| **Does not settle** | whether any of the three is opened, or in what order; any `-Q` item, each of which stands as CLA's proposal; EF-Q5's lift of the stage budget; a push of the orders, a merge to `main`, a tag or a version |
| **Recorded at** | this file; each order's header; their rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 6. The transaction surveyed under the operator's premise of a microservice environment

| | |
|---|---|
| **Word** | *"kdbs is a DB optimized for modern software in a microservice environment. Considering these characteristics, summarize the constraints, the differences and the strengths of the transaction as currently implemented"* |
| **Mark** | A survey, given in conversation in the main checkout on `main` at `72580aa04` (`v3.5.1-6-g72580aa04`) and written to no file: eight strengths, twelve constraints (C1 to C12), a table against PostgreSQL, four recommendations, read from `docs/spec/`, `docs/inflight/known-gaps.md` and the manual, no code read and nothing run. The hour of the word was not read from a clock; the session's clock read 2026-11-28 01:50 UTC when the orders of §7 were begun, and the word is recorded under that day. **CLA reported the premise as contradicting the tree before answering**: `CLAUDE.md` states the purpose as OLTP for financial systems (CF-Q6 (b)), and no file under `docs/`, `manual/` or `CLAUDE.md` carries the word "microservice" (`grep -rni`, 0 lines). The survey also found `docs/spec/client-manual.md:304-306` ("there is no lock to wait on") at odds with `docs/spec/txn.md` §5 ("Detection is followed by a wait, not a refusal"); not corrected |
| **Does not settle** | whether the purpose statement gains the service environment (EI-Q5) - **the concept itself reached `CLAUDE.md` the same day by another session**, as a working rule on `origin/main` at `3aa8a141e` that leaves "What This Project Is" to bound the scope; this session's finding of 0 lines is true of `72580aa04` and not of that commit; the `client-manual.md` sentence, and a second one of that file found by §7's review (`:322-326`, a write "runs to completion whatever happens to the connection") |
| **Recorded at** | this file; EI's §0 |

## 7. EI, EJ, EK and EL written from the survey's C1, C2, C3 and C8; none opened

| | |
|---|---|
| **Word** | *"write milestones for C1, C2, C3 and C8; depending on the case, write them as exploratory or proof-of-concept milestones"* |
| **Mark** | **Four orders are written and none is opened**, on worktree `service-txn-orders` from `72580aa04` (`v3.5.1-6-g72580aa04`), documents only. **EI** (C1, no two-phase commit): exploratory - whether a transaction may stop in doubt and survive a restart, the null hypothesis that it may not. **EJ** (C2, no change feed and no replication): exploratory, two proofs of concept behind its census - a feed in commit order and a standby from the archive. **EK** (C3, no `SERIALIZABLE`): exploratory - the write skew that assertion groups, foreign-key shares, unique keys and `FOR UPDATE` leave open; it re-reads AR0 D1 and D8 (`raft-marks-2026-09-28.md` §4, §5) and assumes neither moved. **EL** (C8, a retry can run twice): a proof of concept taking up CN-5 at its opening - a token row in the work's transaction, then the engine's one true `UNKNOWN_OUTCOME`. The kind of each is CLA's choice under the operator's "depending on the case"; the letters are the first four free after EH. **The orders were reviewed the day they were written** (`critics-developer`, at `5d8dd1ddf`) and corrected: the first writing of three of them reasoned from a claim the specs contradict - EL's recipe can answer `COMMITTED` of a commit not yet durable, EK said `FOR UPDATE` claims no absent row and would have let a lower bound admit falsely, EI said a prepared transaction's rows stay claimed by their headers alone - and each order's stage log says what was found. **The survey's C8 was also wider than the tree**: a dropped connection does end a walked write at its pause (`protocol.md` §10), which `client-manual.md` does not say |
| **Does not settle** | whether any of the four is opened, or in what order; any `-Q` item, each of which stands as CLA's proposal; AR0 D1 and D8, which stand as marked; CN-5's ten items; a push of the orders, a merge to `main`, a tag or a version |
| **Recorded at** | this file; each order's header; their rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 8. EM written: the statement front end's repeated work, cut only where no cut can change an answer; not opened

| | |
|---|---|
| **Word** | After CLA's reading of which SQL layers are cached, whether globally unique table names would help, and whether parsing could stop repeating: *"write a milestone that explores only the improvements here with no quiet-wrong risk, proves the concept, and implements once the gates pass"* |
| **Mark** | **EM is written and not opened** (written as EI; renamed EM at the sync with `origin/main` at `1983ae5a7`, where §7's EI to EL already stood), on worktree `front-end-parse-reuse-order` from `72580aa04` (`v3.5.1-6-g72580aa04`), documents only: five candidates (C1 one lex for the unnamed text statement, C2 a parked statement's parse kept, C3 a multi-variant plan's arguments from the parse, C4 a positive namespace name cache, C5 a whole-text parse cache explored only), an admission test under which a candidate's worst failure is a refusal, a slower plan or a debug abort, and gates G0 (admission), G1 (proof of concept), G2 (implementation) and G3 (close) |
| **CLA's, not the operator's** | the candidates, the admission test (EM-Q2), the bar (EM-Q1), what §0.1 excludes (auto-prepare by shape, a compiled-chain cache, skipping the first compile, globally unique names) |
| **Does not settle** | whether EM is opened; any EM-Q item, each of which stands as CLA's proposal; a merge to `main`, a push, a tag or a version |
| **Recorded at** | this file; EM's header; its rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 9. EF, EG and EH pushed to `main` with the hook skipped

| | |
|---|---|
| **Word** | *"push no verify"*, at CLA's push gate for the three orders |
| **Mark** | **The orders landed on `main` at `72580aa04`** (`v3.5.1-6-g72580aa04`), merged `--no-ff` from `worktree-pg-kds-comparison-orders` and pushed with `--no-verify`: the three orders, their index and register rows, this file's §2 to §5 and the comparison file's corrected claim about scenario 1's twin |
| **Skipped, on the word** | the pre-push hook; no suite ran on the tree; documents only, no build input |
| **Does not settle** | whether any of the three is opened (§10 does) |

## 10. EF, EG and EH taken up in that order on the standing go-ahead; EF opened, EF-Q0..Q7 adopted

| | |
|---|---|
| **Word** | *"Proceed with the milestones in order, as proposed, with /go-ahead-achieving-milestone"*. The skill it names carries the operator's standing word: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* "The order proposed" is CLA's recommendation of the preceding report: EF first, then EG, then EH |
| **Mark** | **EF is opened** on worktree `ef-equal-client-scenario-drivers` (branch `worktree-ef-equal-client-scenario-drivers`) from `72580aa04` (`v3.5.1-6-g72580aa04`). **EF-Q0..Q7 are adopted as CLA proposed, (a) in each, on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone)** - CLA's proposals, not per-item marks by the operator. **EF-Q5 (a) lifts the 20-minute stage budget for EF-S2 by that adoption**, which is not the operator's own word for that measurement; the order's "Marks" says so. EG and EH follow EF's close, each opened and recorded when it is reached |
| **CLA's reading** | the skill's scope is one milestone; the operator's word names three in an order, and CLA reads it as the skill run once per milestone in that order, each stopping at its own push gate |
| **Does not settle** | a push, a merge to `main`, a tag or a version; any item of EG or EH before it is opened |
| **Recorded at** | this file; `workorder-ef-equal-client-scenario-drivers.md` (header, §4 "Marks"); EF's rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 11. EF closed on the standing go-ahead; EF-Q8 written and adopted, S3 not opened

| | |
|---|---|
| **Word** | the standing go-ahead of §10; no further word |
| **Mark** | **EF is closed on the work branch at EF-S2, with EF-S3 not opened**: EF-Q1 (a)'s condition - `pk` or the 200-row tape behind PostgreSQL by more than the larger spread, through equal clients on `main` - is not met on the matched run (`bench/v3.5.1/results-ef-s2-matched-v3.5.1-6-g72580aa04.md`). **EF-Q8 was written at EF-S2 and adopted (b) on the same go-ahead**: scenario 1's KDS statements go through `ServerConnection.rendered`, which revises EF-Q7 (a) for that side; CLA's proposal, not the operator's mark |
| **CLA's, not the operator's** | the reading of the gate (the 200-row tape it names carries no claim, PostgreSQL's runs spreading 49.4 %, KDS the faster in every round; `pk` is inside the spread; a booking's point reads and scenario 1's lookup are read beside them, and two short reads outside the gate - `cargo-lookup` at `strict`, `books-by-genre` over 200 unindexed loans - are behind by under 1 µs, less than the client CPU EF-R1 leaves unequal); the three runs in place of one and the 36 minutes they took under EF-Q5 (a); timed writes moved to the counting client after the stage's review; the rule added to `bench/README.md` |
| **Does not settle** | a push, a merge to `main`, a tag or a version; the opening of EG, which follows under §10's word |
| **Recorded at** | this file; `workorder-ef-equal-client-scenario-drivers.md` (header, §4 "Marks", §7); EF's rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 12. EG opened on the standing go-ahead, on EF's unlanded close; EG-Q0..Q5 adopted

| | |
|---|---|
| **Word** | the standing go-ahead of §10, which names EG after EF; no further word |
| **Mark** | **EG is opened** on worktree `eg-one-client-durable-write` (branch `worktree-eg-one-client-durable-write`) from `d50d304f8` (`v3.5.1-26-gd50d304f8`), the close of EF's work branch. **EG-Q0..Q5 are adopted as CLA proposed, (a) in each, on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone)** - CLA's proposals, not per-item marks by the operator |
| **CLA's reading, revised from §10** | §10 said each milestone stops at its own push gate. **EF's gate is not waited at**: the word names three milestones to be worked in order in one session, so EG is opened on a branch from EF's close and EF's push gate is reported with the others at the end. Nothing is pushed or merged to `main` by this |
| **Does not settle** | a push, a merge to `main`, a tag or a version; DY-Q2, which EG-S0 gives a new price and does not decide |
| **Recorded at** | this file; `workorder-eg-one-client-durable-write.md` (header, §4 "Marks", §7); EG's rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 13. EG closed on the standing go-ahead after EG-S1; EG-Q6 written and adopted, no cut opened

| | |
|---|---|
| **Word** | the standing go-ahead of §10; no further word |
| **Mark** | **EG is closed on the work branch**: EG-S0 attributed scenario 1's `result-insert` to the breadth of a sync while a bulk load's segment stays live (traced: two `fdatasync` calls a commit, about 60 µs on the untouched segment); EG-S1 booked the one-core `strict` commit's inline sync under `commit.wal_flush`; the close's A/B found no cost it can tell from noise. **EG-Q6 was written at EG-S1's review and adopted (a)**: EG-R3 bounds the commit path, not its statistics |
| **CLA's, not the operator's** | **the reading of EG-Q1**: its condition is met taken alone (the insert is behind and the cause is in the engine), and CLA let EG-R2 bound it, so no cut was built and the narrowed sync stays DY-Q2's with a new price beside it; the reading of EG-R3 (EG-Q6); the close with two A/B cells carrying no claim, one of them the eight-core cell EG-R3 asks for |
| **Does not settle** | a push, a merge to `main`, a tag or a version; DY-Q2; the opening of EH, which follows under §10's word |
| **Recorded at** | this file; `workorder-eg-one-client-durable-write.md` (header, §4 "Marks", §7); EG's rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 14. EH opened on the standing go-ahead, on EG's unlanded close; EH-Q0..Q6 adopted

| | |
|---|---|
| **Word** | the standing go-ahead of §10, which names EH after EG; no further word |
| **Mark** | **EH is opened** on worktree `eh-native-fold-per-row` (branch `worktree-eh-native-fold-per-row`) from `01c6c741c` (`v3.5.1-35-g01c6c741c`), the close of EG's work branch. **EH-Q0..Q6 are adopted as CLA proposed, (a) in each, on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone)** - CLA's proposals, not per-item marks by the operator. The premises of its three `[quiet-wrong]` rules (EH-R2, EH-R3, EH-R7) were re-read against the tree before the opening, as the order's "Marks" records |
| **Does not settle** | a push, a merge to `main`, a tag or a version |
| **Recorded at** | this file; `workorder-eh-native-fold-per-row.md` (header, §4 "Marks"); EH's rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 15. EH closed on the standing go-ahead after S1, S2 and Sy

| | |
|---|---|
| **Word** | the standing go-ahead of §10; no further word |
| **Mark** | **EH is closed on the work branch** at `8db2923d4` (`v3.5.1-53-g8db2923d4`): a global fold takes its rows from the page's bytes (S1), the group table is one arena under an open-addressing table (S2), and an aggregated walk yields on an aggregator of its own (Sy). EH-Q1's bar is met within the spread |
| **CLA's, not the operator's** | **a regression of CLA's own, found by the close and removed before it**: the pool size bound added at the Sy review's advice cost a 60,480-group fold 1.6x; the pool is now bounded by count alone, and the closing binary is measured again against the opening one with no shape dearer. The reviews' cuts not taken are recorded in the order |
| **Does not settle** | a push, a merge to `main`, a tag or a version, for EF, EG or EH |
| **Recorded at** | this file; `workorder-eh-native-fold-per-row.md` (header, §4 "Marks", §7); EH's rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |

## 16. EF, EG and EH pushed to `main` with the hook skipped

| | |
|---|---|
| **Word** | *"push no verify"*, at CLA's push gate for the three milestones |
| **Mark** | **EF, EG and EH land on `main`** in one `--no-ff` merge of `worktree-eh-native-fold-per-row`, which carries EF's and EG's branches beneath EH's, pushed with `--no-verify` |
| **Skipped, on the word** | the pre-push hook. The suite was last run at `8db2923d4`, the last commit with a build input: 5,437 of 5,438, the one failure the two-core insert rig the stage logs name; it was not a green run, and the push does not report it as one |
| **Does not settle** | a tag or a version |
| **Recorded at** | this file; the three orders' landed rows in `instructions/milestones.md` |

