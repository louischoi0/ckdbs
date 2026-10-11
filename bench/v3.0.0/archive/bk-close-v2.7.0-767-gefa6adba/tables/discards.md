**Rules, declared before the first run unless marked** (`archive/.../drivers/analyze_bk.py`, docstring). A finished run is
discarded if, in its own record: **(1)** a real `cc1plus`, `cmake`, `make`, `ninja` or `ld` process is in its before or after list
(*amended*: a foreign `ctest` or `kds_tests` process no longer discards on its own - the first is a sleeping parent, the second one test - and the CPU rules see their load); **(2)** the CPUs outside the server's, the driver's and 6,7 averaged more than 10 % busy
(*amended during the run*: it first counted CPUs 6,7, and at `cores = 2` counted CPU 1, which the engine pins its second
reactor to); **(2b)** a hyperthread sibling of a pinned CPU (3 and 5 at `cores = 1`; 0 and 5 at `cores = 2`, amended with
rule 2) was more than 15 % busy; **(3)** any error reply, a row count other than the expected one, or a refusal probe other
than the expected one; **(4)** the `SHOW META` control's p50 in either arm exceeds 1.04 x the cell's median (*amended*: it first
read 1.10 x, and no kept run of `cores = 1` or `cores = 2` moves between the two values). Discarded runs are moved aside with their reasons, not deleted, and the same run index is run again, up to six
times; re-running a discarded index keeps the A/B start order, which alternates by index parity. The discards below were
applied by rules that read only the host and the control, never a statement's latency. A dry run of the final rules over every kept run
of every cell discards none of them (the final 1.04 rule moved two runs, one of the `cores = 1` null pair and one of the fresh pair, which were not
re-run: those cells hold 15 runs where 16 were asked). Runs discarded under an earlier form of a rule were re-run at the same index and the re-run is the one kept; the earlier
record stays under `discarded/` with its reasons.

**`overhead-c1`**: 96 runs kept, 0 discarded.

**`overhead-c2`**: 48 runs kept, 33 discarded.
- 18 x `other-CPU busy (CPUs #,# excluded) #% > # %`
- 13 x `other-CPU busy #% > # %`
- 7 x `host_after`
- 5 x `host_before`
- 5 x `sibling CPU # busy #% > # %`
- 2 x `A ping p# # > # x #`
- 2 x `B ping p# # > # x #`

**`null-c1`**: 46 runs kept, 24 discarded.
- 25 x `sibling CPU # busy #% > # %`
- 19 x `host_after`
- 17 x `host_before`
- 17 x `other-CPU busy #% > # %`
- 15 x `A ping p# # > # x #`
- 12 x `B ping p# # > # x #`
- 1 x `other-CPU busy (CPUs #,# excluded) #% > # %`

**`null-c2`**: 48 runs kept, 2 discarded.
- 2 x `other-CPU busy (CPUs #,# excluded) #% > # %`
- 1 x `sibling CPU # busy #% > # %`

**`strict-c1`**: 18 runs kept, 18 discarded.
- 20 x `sibling CPU # busy #% > # %`
- 16 x `other-CPU busy (CPUs #,# excluded) #% > # %`
- 3 x `host_after`
- 3 x `host_before`
- 1 x `B ping p# # > # x #`

**`flip-c1`**: 29 runs kept, 5 discarded.
- 4 x `A ping p# # > # x #`
- 3 x `B ping p# # > # x #`
- 2 x `sibling CPU # busy #% > # %`
- 1 x `host_after`
- 1 x `other-CPU busy (CPUs #,# excluded) #% > # %`

**`nullB-c1`**: 16 runs kept, 0 discarded.

**`nullA-c1`**: 16 runs kept, 0 discarded.

**`fresh-c1`**: 31 runs kept, 1 discarded.
- 1 x `A ping p# # > # x #`
- 1 x `B ping p# # > # x #`

