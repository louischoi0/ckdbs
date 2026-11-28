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

## 6. EF, EG and EH pushed to `main` with the hook skipped

| | |
|---|---|
| **Word** | *"push no verify"*, at CLA's push gate for the three orders |
| **Mark** | **The orders landed on `main` at `72580aa04`** (`v3.5.1-6-g72580aa04`), merged `--no-ff` from `worktree-pg-kds-comparison-orders` and pushed with `--no-verify`: the three orders, their index and register rows, this file's §2 to §5 and the comparison file's corrected claim about scenario 1's twin |
| **Skipped, on the word** | the pre-push hook; no suite ran on the tree; documents only, no build input |
| **Does not settle** | whether any of the three is opened (§7 does) |

## 7. EF, EG and EH taken up in that order on the standing go-ahead; EF opened, EF-Q0..Q7 adopted

| | |
|---|---|
| **Word** | *"Proceed with the milestones in order, as proposed, with /go-ahead-achieving-milestone"*. The skill it names carries the operator's standing word: *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* "The order proposed" is CLA's recommendation of the preceding report: EF first, then EG, then EH |
| **Mark** | **EF is opened** on worktree `ef-equal-client-scenario-drivers` (branch `worktree-ef-equal-client-scenario-drivers`) from `72580aa04` (`v3.5.1-6-g72580aa04`). **EF-Q0..Q7 are adopted as CLA proposed, (a) in each, on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone)** - CLA's proposals, not per-item marks by the operator. **EF-Q5 (a) lifts the 20-minute stage budget for EF-S2 by that adoption**, which is not the operator's own word for that measurement; the order's "Marks" says so. EG and EH follow EF's close, each opened and recorded when it is reached |
| **CLA's reading** | the skill's scope is one milestone; the operator's word names three in an order, and CLA reads it as the skill run once per milestone in that order, each stopping at its own push gate |
| **Does not settle** | a push, a merge to `main`, a tag or a version; any item of EG or EH before it is opened |
| **Recorded at** | this file; `workorder-ef-equal-client-scenario-drivers.md` (header, §4 "Marks"); EF's rows in `instructions/v3.2.0/index.md` and `instructions/milestones.md` |
