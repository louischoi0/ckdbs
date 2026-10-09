# Ratification marks — 2026-10-09

**The operator's words of 2026-10-08 and 2026-10-09**, recorded by CLA on
`worktree-ba-open-marks` (§1) (`v2.7.0-*`; the v3.0.0 tag is not cut). All
of it is **verbatim**: the words as typed in the session, or the option the
operator chose where CLA asked with options.

## 1. BA opened, BA-Q0..Q13 marked one by one

| | |
|---|---|
| **Word** | *"BA 중단된 작업부터 진행해줘 BA를 마무리하려고해"* ("continue BA from where it stopped; I want to finish BA"), 2026-10-08. Asked how far, the operator chose *"Q 항목을 하나씩 검토"* ("review the Q items one by one"). Each item was then put with its proposal, restated where the tree at `c47fbeff` had moved (`workorder-ba-parallelism.md` §1.14), and answered: Q0, Q1, Q2, Q3, Q4, Q5, Q6, Q7, Q11, Q12, Q13 by choosing the recommended option; Q8, Q9 and Q10 with *"제안대로 진행"* / *"BA-Q8 제안 대로 진행"* ("proceed as proposed") after the operator asked what invariant 11 says, whether a pk below the highest can be inserted, why the WAL split waits, and how PostgreSQL's move-right works |
| **Mark** | **BA-Q0** as proposed. **BA-Q1** as restated (BA-S1b struck; P12's checkpoint half BI's under BI-Q1 (a)). **BA-Q2** as proposed. **BA-Q3 (b)**. **BA-Q4** as proposed. **BA-Q5** as restated (the hit path partitioned over BE's slots, the global structures under one latch, BG-S2 first). **BA-Q6 (b)**. **BA-Q7** as proposed. **BA-Q8** as restated, with a btree named key below the cursor taking no page 7. **BA-Q9** not now, the first half restated to BC. **BA-Q10** as proposed, its cost restated. **BA-Q11** as proposed, with BF-R9's epoch the minimum over suspended statements. **BA-Q12** as proposed, migration left to `docs/pending/`. **BA-Q13** as proposed, §1.0 restated |
| **Does not settle** | BI-Q1 (BI's own); a question a stage raises; the push; the census's materiality verdicts, which BA-Q1 makes the gate for every fix stage but S15 |
| **Recorded at** | `workorder-ba-parallelism.md`'s header, §1.14, §3's BA-S11 and BA-S13 rows, §4's mark column and §6 ("BA opened"); `index.md`'s BA row |

## 2. BA run to its close, its stage questions settled by CLA's proposal

| | |
|---|---|
| **Word** | Asked whether *"BA를 마무리하려고해"* lets every stage run without its own word, the operator chose *"계속 진행, 질문은 CLA 제안대로"* ("keep going; questions as CLA proposes"); asked which commit is A for BA's close A/B, the operator chose *"d43845a0 (BA 오픈 시점)"* ("d43845a0, where BA opened"). Then `/go-ahead-achieving-milestone` |
| **Mark** | BA-S2 to BA-S17 run in §5's order on `worktree-ba-open-marks` without a word per stage; a question a stage raises is settled by CLA's proposal, recorded as adopted under this go-ahead. BA-S17's overhead A/B takes A = `d43845a0`, so BA-S1's and BA-S1c's code is outside it |
| **Does not settle** | the push, a tag or a version; BI-Q1, which is BI's; a decision outside BA's scope |
| **Recorded at** | `workorder-ba-parallelism.md`'s header, §3's intro, §5 item 5 and §6; `index.md`'s BA row |
