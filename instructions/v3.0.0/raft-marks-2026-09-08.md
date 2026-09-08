# Ratification — operator mark of 2026-09-08 on AS-Q6

2026-09-08, operator (chat session, in Korean): *"[decision] [AS-Q6]:
드라이버를 바꾸고 f6ed10c도 같은 드라이버로 재측정해 새 기준선 수립 당분간
measure는 BTREE only로 진행한다"* — change the drivers, re-measure `f6ed10c`
with the same drivers to establish a new baseline, and for the time being
measurement proceeds BTREE only. Recorded by CLA on `as-q6-drivers-btree`
against `bc1040e`. Nothing here is code.

## What it marks

`workorder-as-sus1-heap-suspended.md` AS-Q6. Its mark of 2026-09-05
(`raft-marks-2026-09-05.md` §4) covered `tools/benchmark.py`'s
`--clustered heap` and the six tools sharing its shape, and left the tools
that emit an explicit `HEAP` "to be refused". AN-S5 then met that refusal on
`tools/scenario0_stockmarket.py` and `tools/scenario2_freight.py`
(`docs/inflight/known-gaps.md`, "AL-S8's scenario matrix cannot be re-run
on a post-SUS-1 engine"), and AM-S6 (AW-S5) was blocked on the same word.
This mark is the answer that entry named the operator's to give.

## What it obliges

| item | obligation |
|---|---|
| the drivers | `scenario0_stockmarket.py`'s `trades` and `user_periodic_profit`, and `scenario2_freight.py`'s `freights` and `charges`, are created `BTREE`; the PostgreSQL twins mirror it - a pk index where ckdbs now has a tree. **No storage switch is added**: "BTREE only for now" is one path, a switch would be a second path nobody measures, and the heap shape is what the AL-S8 files at `f6ed10c` measured, retrievable from git |
| the baseline | `f6ed10c`'s archived binary (`/home/cdkbs/bench-runs/al-s8-f6ed10c/kds_server`, the one AL-S8's stamp names) is re-measured with the changed drivers, on this host, into fresh `bench/v3.0.0/` files named for the driver shape. Those are the comparator for every later delta. The AL-S8 heap files stay as history and are compared against nothing: a heap number beside a btree number is two workloads, not a delta |
| the rule | until the operator says otherwise, a `bench/v3.0.0/` number is measured on BTREE relations only, and a delta is taken only against a BTREE baseline of the same driver (`bench/README.md`) |
| what it unblocks | AM-S6 (AW-S5) and AN-S5's scenario half, each as `HEAD` against the new baseline, interleaved, same host - the shape D15 permits |

## What it does not cover

Four tools still emit an explicit `HEAP` and are refused at `CREATE TABLE`
on a post-SUS-1 engine: `bulk_insert_benchmark.py`, `kwp_load_benchmark.py`,
`aggregate_benchmark.py`'s heap arm and `cabin_scope_ab_benchmark.py`'s two
heap arms. None of them is on the AL-S8 matrix or on any open stage's
measurement list; they belong to AS-S3, the tools stage the 2026-09-05 mark
sized S, and this mark's "BTREE only" is the rule that stage applies to them.
