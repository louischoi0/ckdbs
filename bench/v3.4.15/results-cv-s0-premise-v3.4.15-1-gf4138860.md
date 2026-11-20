# CV-S0 - the premise: `ORDER BY ... LIMIT` today against the work an ordered walk does

Measured 2026-11-20 at `v3.4.15-1-gf4138860` (`f4138860`, worktree
`cv-ordered-index-walk`, before any CV code), for
`instructions/v3.2.0/workorder-cv-ordered-index-walk.md` §5 (CV-S0) and
CV-Q0's bar.

## Setup

- Binary: `build-release/kds_server` built at `f4138860` in the worktree,
  copied to the run directory and hashed: sha256 `7217d0eea81a83f2...`
  (in full in the archive's `s0.json`).
- Data file on `/dev/nvme0n1p1`, ext4 (`/home/louis/ck-tmp/cv-s0/run`);
  never tmpfs. Port 15641, chosen.
- Server pinned to CPU 4, `cores = 1`, `durability = group`,
  `buffer_pool_frames = 65536` (the whole relation resident), driver on
  CPUs 0-3, one connection.
- Relation `cv_t (id int64, user_id int64, created_at int64, a int64)
  BTREE`, 110,100 rows: user 1 owns 100, user 2 10,000, user 3 100,000,
  shuffled through the tuple key order; `created_at` a random
  permutation. Index `cv_uc (user_id, created_at)`.
- Driver: `tools/cv_s0_premise.py`, its `--help` the arms' definition.
  2 repeats x 2.0 s per arm, arm order rotated and reversed per repeat,
  after a 0.3 s warm-up per arm.
- Host load per cell (in the archive): loadavg 2.85 to 3.46, `cc1plus` 0
  in every cell of the run reported here.

## The arms

- **T** - today's statement, the shape CV elides. **T2** - T again, the
  noise floor.
- **P** - the proxy of the walk CV would run: T with a two-sided bound that
  leaves exactly the rows an ordered walk reads (`created_at BETWEEN <the
  user's 20th greatest> AND <above every stamp>`, `id BETWEEN <max - 19>
  AND <max>`), run today as an index range or a tuple key range: 20 rows
  resolved (1,044 examined for the OFFSET cell) and a 20-row heap. It pays
  a sort of 20 the walk will not, and not the reverse walk's re-descent per
  leaf (CV-Q1 (a)).
- **D** - one tuple key point `SELECT`: an upper bound on what one
  re-descent adds, because it also carries a parse, a reply and a round
  trip.

`ANALYZE` (archive) confirms each arm: T over a user is `IndexProbe` with
`examined` = the user's rows; P is `IndexRange` with `examined=20`; T over
the tuple key is `Scan` with `examined=110100`; P is `Range` with
`examined=50` (`examined=1044` under `OFFSET 1000`). Every cell's T and P
returned the same rows.

## Results (mean of the 2 repeats; p0 the minimum)

| cell | arm | p0 | p25 | p50 | p75 | p99 | server CPU / stmt |
|---|---|---|---|---|---|---|---|
| latest 20, user of 100 | T | 138.8 µs | 150.7 µs | 167.6 µs | 202.6 µs | 283.2 µs | 115.7 µs |
| | T2 | 139.0 µs | 147.3 µs | 183.2 µs | 213.4 µs | 272.4 µs | 115.3 µs |
| | P | 87.3 µs | 94.2 µs | 115.7 µs | 140.3 µs | 191.1 µs | 58.9 µs |
| latest 20, user of 10,000 | T | 6,918.4 µs | 7,186.9 µs | 7,337.8 µs | 8,072.5 µs | 9,173.4 µs | 7,565.3 µs |
| | T2 | 6,949.5 µs | 7,256.6 µs | 7,389.9 µs | 7,529.2 µs | 10,219.0 µs | 7,363.7 µs |
| | P | 87.5 µs | 92.7 µs | 101.0 µs | 133.4 µs | 192.3 µs | 55.5 µs |
| latest 20, user of 100,000 | T | 68,129.0 µs | 70,350.5 µs | 71,309.2 µs | 72,916.0 µs | 84,339.0 µs | 72,500.0 µs |
| | T2 | 67,412.9 µs | 68,727.4 µs | 75,205.2 µs | 77,847.1 µs | 85,439.0 µs | 74,175.9 µs |
| | P | 88.5 µs | 94.6 µs | 103.3 µs | 140.2 µs | 195.2 µs | 57.7 µs |
| `ORDER BY id DESC LIMIT 20` | T | 34,059.3 µs | 38,151.8 µs | 38,987.6 µs | 40,469.3 µs | 45,960.9 µs | 39,223.3 µs |
| | T2 | 34,066.1 µs | 35,954.8 µs | 39,652.2 µs | 40,400.9 µs | 44,555.2 µs | 38,290.9 µs |
| | P | 74.5 µs | 80.0 µs | 88.3 µs | 119.8 µs | 174.5 µs | 40.4 µs |
| `... DESC LIMIT 20 OFFSET 1000` | T | 42,888.2 µs | 44,991.0 µs | 46,647.3 µs | 48,420.7 µs | 54,841.0 µs | 46,860.4 µs |
| | T2 | 42,818.1 µs | 46,000.3 µs | 47,509.1 µs | 49,522.5 µs | 57,810.3 µs | 47,657.8 µs |
| | P | 458.3 µs | 513.8 µs | 543.3 µs | 585.2 µs | 981.8 µs | 466.3 µs |
| `ORDER BY id LIMIT 20 OFFSET 1000` (elided today) | T | 149.5 µs | 212.3 µs | 216.4 µs | 223.2 µs | 303.7 µs | 131.3 µs |
| | P | 174.6 µs | 240.5 µs | 244.6 µs | 251.8 µs | 354.4 µs | 160.4 µs |
| one point descent | D | 46.0 to 48.5 µs | 50.8 to 69.0 µs | 54.7 to 70.1 µs | 66.2 to 71.7 µs | 95.8 to 122.5 µs | 25.1 to 29.2 µs |

The noise floor (|T - T2| at p50) is 2 % to 5 % for every cell but the
100,000-row user (5.5 %).

**Wait breakdown: not taken.** Every arm is CPU on one resident core: the
server's CPU per statement is within 10 % of its p50 latency in every T
cell, and no read reaches the device (the relation fits the pool).

## CV-Q0's bar, read

The bar (CV-Q0 (a), adopted on the standing go-ahead): "latest 20 of a user
with 10k rows" and `ORDER BY id DESC LIMIT 20` over 100k rows must both be
faster than today's top-N.

- **Latest 20 of a user with 10,000 rows: 7,337.8 µs today, 101.0 µs for
  the proxy at p50 (server CPU 7,565.3 µs against 55.5 µs).** The ordered
  walk reads one or two index leaves where today's reads 10,000 entries
  and resolves every one; a re-descent per index leaf, priced at no more
  than D's 54.7 µs (and D carries a round trip the walk does not), leaves
  it more than 40 times faster.
- **`ORDER BY id DESC LIMIT 20`: 38,987.6 µs today, 88.3 µs for the proxy
  at p50.** Twenty rows sit in one leaf here, so the reverse walk makes one
  descent where the proxy makes one; D's bound is again far below the
  gap.

**Both are faster by more than an order of magnitude: the bar is met, and
CV proceeds to CV-S1.**

## What the numbers say about the engine

Today's cost is linear in what matches, not in what is returned: a user's
latest 20 costs about 0.7 µs per row the user owns (7.3 ms at 10,000, 71 ms
at 100,000), because IX8a collects every entry, sorts the tuple keys and
resolves each one before the heap keeps 20. The proxy is flat at about
100 µs whatever the user's size. `ORDER BY id DESC` pays the whole walk
(110,100 rows examined, 776 leaves) for 20 rows, where the ascending form,
already elided, pays 1,020.

## What was cut, and what was discarded

- A spike of the reverse descent was not built: P and D stand in for it,
  as stated above. CV-S4 measures S0's arms again with the built walks.
- **A first run was discarded**: its proxies used one-sided bounds
  (`created_at >= ...`, `id >= ...`), which enter neither tree - `ANALYZE`
  showed `examined=10000` and `Scan` for them - so P measured T. It is kept
  in the archive as `s0-oneside-proxies-discarded.json`; the driver now
  refuses a proxy that does not narrow. That run also overlapped another
  session's `cmake --build` (DH's), which this run did not.
- Time spent: one build of `build-release` (about 5 minutes) and two driver
  runs of about 2 minutes each; no hold.

Raw output: `bench/v3.4.15/archive/cv-s0-premise-v3.4.15-1-gf4138860/`.
