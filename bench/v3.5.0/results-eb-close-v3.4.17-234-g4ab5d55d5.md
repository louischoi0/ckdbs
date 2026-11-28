# EB close, the remedies: the point read's overhead was Step's size, and at the closing commit it is +0.3 % to +1.2 %

**Thesis.** The first close measurement (`results-eb-close-v3.4.17-231-ga610d007a.md`,
at `a610d007a`) found EB's four built arms 31x to 2,989x cheaper and every
point read +1.5 % to +3.2 % dearer in server CPU. Three remedies were
applied within the milestone and measured each time against the same base
binary. **The cause was the size of `exec::Step`**: EB grew it from 744 to
800 bytes. A control built at the base commit with nothing but 48 bytes of
padding in `Step` read +0.9 % to +2.2 % on the same cells, and a plain
rebuild of the base read within 0.4 %. At the closing commit `4ab5d55d5`
(`v3.4.17-234-g4ab5d55d5`) `Step` is 744 bytes again and the point reads are:

- btree **+0.3 %** (inside noise);
- heap **+1.1 %**, where the run's own same-source control read +0.9 %;
- embedded key **+1.1 %**;
- hashed key **+1.2 %**.

That is +0.02 us to +0.10 us of 6.5 us to 8.0 us. Writes, the conflicting
`DO NOTHING` and the issued-key insert are inside noise. The built arms are
**29x to 2,413x** cheaper at the closing commit. Those cells ran beside the
Debug suite on the other CPUs, so the absolute numbers are noisier than the
first file's.

| field | value |
|---|---|
| date executed | 2026-11-27, 23:25-23:50 UTC |
| branch / commits | `worktree-eb-key-forms-in-the-executor`, worktree `eb-key-forms-in-the-executor`: base `9e7597d5d` (`v3.4.17-224-g9e7597d5d`); the first close `a610d007a`; remedies `f29dba427`, `3d7a8f6df`, `4ab5d55d5` (`v3.4.17-234-g4ab5d55d5`, the closing commit); bisect binaries at `cf3f5bb87` (S1), `78f98354b` (S2), `88164d7fb` (S3) |
| binaries | `build-release/eb-close/`: base sha256 `c4ad5c21...c84`; `base-rebuilt` (the base commit built again in `build-bisect`) `4e28c56a...b55`; `basepad` (the base commit with `std::uint64_t eb_size_probe[6]` appended to `Step`, never committed) `7e9dbe0a...249c`; `close-f29dba427` `b47cf1ca...f019`; `fix2` (the tree of `3d7a8f6df`) `eb7dfecc...b0cf`; `close-final` (the tree of `4ab5d55d5`) the `fix3` build; full hashes in each run's JSON |
| driver | `tools/eb_close.py` (written by the first close's `ck-tester` run), `--cells` per run; server, pinning and repeat design as the first file states (18 repeats a cell, rotated arm order, one measuring CPU in 2-7 a repeat, driver on CPU 1) |
| host | load 1.1-1.8 through the overhead runs; the final `G-` run shared the host with this session's Debug suite on CPUs 8-23 (`taskset`), the servers on CPUs 0-7 |
| raw output | `bench/v3.5.0/archive/eb-close-v3.4.17-231-ga610d007a/`: `eb-close-pass3-oh.json` (f29), `eb-bisect-a.json`, `eb-bisect-b.json`, `eb-pad.json`, `eb-fix2.json`, `eb-fix3-oh.json`, `eb-final-g.json` |
| prior number | the first close file, same shapes, same base binary |

## The remedies, in order

Server CPU a point read, close / base - 1, paired by repeat, 95 % t-interval:

| build | btree | heap | embedded | hashed | what it changed |
|---|---|---|---|---|---|
| `a610d007a` (first close, pass 1) | +1.5 % +-0.5 | +2.0 % +-0.4 | +2.4 % +-0.6 | +3.2 % +-0.7 | - |
| `f29dba427` | +2.0 % +-0.3 | +2.0 % +-0.6 | +2.9 % +-0.5 | +3.2 % +-0.7 | `residual` back inside one cache line (`RangeBounds` and `IndexProbe` restored; `Step` 800 to 792 bytes): **no effect** |
| S1 `cf3f5bb87` | +1.5 % +-1.7 | - | +0.2 % +-0.5 | +0.7 % +-0.6 | bisect |
| S2 `78f98354b` | +1.4 % +-0.5 | - | +1.5 % +-0.5 | +2.1 % +-0.6 | bisect: the whole-key arms built their probes on every literal read |
| S3 `88164d7fb` | +1.1 % +-0.2 | - | +2.1 % +-0.5 | +3.2 % +-0.5 | bisect |
| base + 48 bytes in `Step` | +2.2 % +-0.5 | +1.9 % +-0.3 | +1.8 % +-0.4 | +0.9 % +-0.6 | control: size alone |
| base rebuilt | -0.0 % +-0.2 | -0.4 % +-0.2 | +0.4 % +-0.4 | -0.0 % +-0.7 | control: a rebuild |
| `3d7a8f6df` (fix2) | +1.2 % +-0.4 | +1.2 % +-0.4 | +1.7 % +-0.4 | +2.2 % +-0.6 | the whole-key arms gated on a correlated equality |
| **`4ab5d55d5` (fix3, closing)** | **+0.3 % +-0.6** | **+1.1 % +-0.4** | **+1.1 % +-0.5** | **+1.2 % +-0.4** | `Step` 744 bytes: the two new key forms in one `ColdBox` |

At the closing commit the run's control (the rebuilt base) read -0.2 %,
+0.9 %, -0.4 % and -0.1 % on the same four cells. The writes, at the
closing commit, read -0.8 %, +0.2 %, +0.2 % and +0.7 %, and the controls'
intervals were 0.8 % to 1.6 % wide. The conflicting `DO NOTHING` read
+1.1 % +-1.2 and the issued-key insert +0.1 % +-0.4.

## The built arms at the closing commit

| cell | base | close | gain |
|---|---|---|---|
| R1 page by an open tuple key bound | 487 us | 16.8 us | 29x |
| J1 join by a composite index's whole key | 263,588 us | 228 us | 1,154x |
| J2 join by an embedded key | 226,517 us | 93.9 us | 2,413x |
| O1 order by an embedded key | 3,971 us | 16.2 us | 246x |

The gains are within 10 % of the first close's at `a610d007a` (31x,
1,283x, 2,989x, 252x). This run shared the host with the Debug suite, and
its control drifted -5.6 % on J1.

## What the numbers say about the engine

- **A point read's cost scales with `Step`'s size.** 48 bytes of padding
  that nothing reads cost about 0.14 us a read, about 2 %. A statement
  compiles its chain afresh every time (CX-S6: no compiled-chain cache), so
  every byte of `Step` is constructed, moved and destroyed on every point
  statement. A field that almost no step carries belongs behind one
  pointer (`base/cold_box.hpp`), not in the struct.
- **The first remedy, putting `residual` back in one cache line, changed
  nothing.** The comment on `Step` records a cache-line regression from
  JB1, but that is not what this was.
- **The rest is work a point read now does:** about 0.08 us on the
  embedded and hashed reads. On the heap read it is the same size as the
  control's own drift. It is not located further; the host has no `perf`
  or `valgrind`.

## Not measured

- A profile of the residual +1.1 % (no profiler on the host).
- The `G-` cells on a quiet host at the closing commit; the S4 shapes; more
  than one core; `strict` or `group` durability.
