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

## 6. EI written: the statement front end's repeated work, cut only where no cut can change an answer; not opened

| | |
|---|---|
| **Word** | After CLA's reading of which SQL layers are cached, whether globally unique table names would help, and whether parsing could stop repeating: *"write a milestone that explores only the improvements here with no quiet-wrong risk, proves the concept, and implements once the gates pass"* |
| **Mark** | **EI is written and not opened**, on worktree `front-end-parse-reuse-order` from `72580aa04` (`v3.5.1-6-g72580aa04`), documents only: five candidates (C1 one lex for the unnamed text statement, C2 a parked statement's parse kept, C3 a multi-variant plan's arguments from the parse, C4 a positive namespace name cache, C5 a whole-text parse cache explored only), an admission test under which a candidate's worst failure is a refusal, a slower plan or a debug abort, and gates G0 (admission), G1 (proof of concept), G2 (implementation) and G3 (close) |
| **CLA's, not the operator's** | the candidates, the admission test (EI-Q2), the bar (EI-Q1), what §0.1 excludes (auto-prepare by shape, a compiled-chain cache, skipping the first compile, globally unique names) |
| **Does not settle** | whether EI is opened; any EI-Q item, each of which stands as CLA's proposal; a merge to `main`, a push, a tag or a version |
| **Recorded at** | this file; EI's header; its rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |
