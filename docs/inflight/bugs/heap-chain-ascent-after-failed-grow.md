# H9 — an errored page grow leaves a heap chain out of ascending order

**Symptom.** The sim's integrity sweep reports
`[chain-order] page 191: relation 't2': min_key 78 does not exceed a
predecessor page's max id 183` (`sim/integrity.cpp:211`), preceded by
`page-fail-grow` at op 1418 and three further armed page faults.

**Reproduce.**
`build-release/ckdbs-sim --seed 20260826003 --ops 1500 --iterations 2
--mode clean --profile uniform --faults io --fault-rate 40`; the
`zipfian` profile of the same seed fails identically. Found 2026-08-26
on worktree `g1-peer-page-id-not-found` at `449804e`, on a fresh
date-derived seed.

**Mechanism — not yet known.** An errored grow appears to leave the
chain's page order non-ascending; which write is left half-applied is
unread. Establish that before proposing a fix.

**Why it matters.** `heap-and-tuple.md` §3.1b's tail append, its
page-wise ordering and its **tail-page-only duplicate check** all rest
on that ascent. The third is the dangerous one: a page opening below an
id already on a predecessor admits a duplicate pk with no error at all.
Invariant 11's refusal of a named key below the high-water mark is
justified by the same ascent.

**What it is not.** Not caused by the change it was found against —
the cell fails identically with that change compiled out. Not a
recovery defect: `mode=clean` injects no crash, so the fault path alone
produces it. Not reachable by a peer-side fix — the sim mounts core 0
only (`sim/instance.cpp` passes `core_id=0`, installs no lease).

**Closing.** The seed is deliberately absent from
`tests/testdata/sim_seeds.txt`, which is regression-mandatory on every
build; whoever lands the fix adds it there with the test.
