# R4/IS7 — insert spreading on one relation, two writer cores

**Measured 2026-08-29 in worktree `v2.6.0-insert-spreading-1`, on the
commit this file lands with** (`git describe --tags` reads
`v2.2.1-127-g<this commit>`; the last operator tag on this line is
`v2.2.1`, and `v2.6.0` is the operator-named version this directory is
opened for). `build-release`, driver `bench/spread_insert_probe.py`.

## What is honest about this run, first

**The host has 2 CPUs**, and the engine refuses `cores` above the
hardware count. So this is `cores = 2` — core 0 plus **one** peer — and
the k = 1..4 writer-core sweep the workplan's IS7 row asks for **was not
run**. What follows is a two-way result, not a scaling curve, and it must
not be quoted as one.

The workload is one relation written concurrently from both cores'
sessions, 4,000 rows each, 8,000 placed. `placement = creating` (the
relation is core 0's, so the peer is a foreign writer to it — the shape
`crosscore.md` §6b describes), `peer_listeners = on`, `range_size_ids =
4096` on the spread arm and `0` on the control. The two arms differ in
that one key and nothing else.

## The numbers

| arm | inserts/s | wall s | rows | client retries |
|---|---|---|---|---|
| C concentrated, `durability = group` | 749 | 10.68 | 8,000 | 0 |
| **S spread, `durability = group`** | **848** | 9.43 | 8,000 | 1 |
| C concentrated, `durability = relaxed` | 20,832 | 0.38 | 8,000 | 0 |
| **S spread, `durability = relaxed`** | **23,622** | 0.34 | 8,000 | 2 |

**group: 1.132×. relaxed: 1.134×.**

## What the run says, and the prediction it corrected

**The two ratios agree to within 0.2%, and that is the finding.** §8 of
`workplan-insert-spreading.md` predicted the `group` arm would come out
flat-to-negative, on `known-gaps.md`'s measured result that spreading
writers over cores divides the group-commit batch and caps each core at
the volume's single-stream `fdatasync` rate. That prediction is **not
borne out at two cores**, and the reason it is not is visible in the same
table: the absolute rates differ by 28× between the durability arms while
the *ratio* does not move. Whatever spreading buys here, it is buying it
somewhere other than the commit path — which is what the relaxed arm was
included to isolate, and it turns out both arms isolate it.

The likeliest reading, stated as a reading rather than a result: at
`cores = 2` there is one peer, so the batch is divided in two at most, and
the ~13% that spreading saves — the peer no longer shipping every INSERT
to core 0 and waiting on the owner's reply — is larger than what half a
batch costs. The v2.1.0 finding was taken at **three** writer cores, where
the division is sharper. **That reading is untested here and needs the
host IS7 could not have.**

## The pump's client-visible cost

**One retry, per core, ever** — 1 on the group arm and 2 on the relaxed
one, against 8,000 rows. That is R4/IS1's contract holding exactly as
written: the peer's first INSERT into a foreign relation is refused, the
refusal leaves a row-id lease demand behind it, the drain tick turns that
into a range, and no later statement pays anything. `rowid_refill` reads
`requests=1 grants=1` on the peer in the spread arm and `0/0` in the
control, which is the mechanism firing once and then never again.

Worth recording because it changes a classification: `tools/multicore_benchmark.py`
lists the cross-core write refusal under `PERMANENT_TEXTS` — correct when
written, and false with spreading armed, where it is the *transient*
refusal a retry clears.

## What this run does not measure

- **k > 2.** The sweep, and with it the shape of the curve. Not run; the
  host cannot.
- **§3's fan-in ceiling.** 8,000 rows at `range_size_ids = 4096` opens two
  ranges, nowhere near `kMaxFanInUpstreams = 64`. The ceiling stays
  arithmetic, not measured.
- **The range-size sweep (D6's value).** One value, 4,096.
- **Reads over a spread relation.** Write-only workload.
- **Per-statement overhead A/B**, which the operator amendment of
  2026-08-24 suspends for v2-stage development.
