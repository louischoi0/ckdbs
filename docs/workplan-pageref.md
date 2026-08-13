# The PageRef Migration — Workplan (MG01–MG06)

Status: **READY FOR EXECUTION. Nothing here is built.**
Spec: `docs/page.md` §3 (S2 PageRef, and §16-7's "PageStore v2 migration",
which this document is the plan for), `docs/spec-eviction.md` (normative for
everything eviction-side). Related: `docs/workplan-eviction.md` (whose every
task is gated on this), `docs/known-gaps.md` ("Buffer-pool eviction is built
but disarmed").

This workplan exists because the task is large, mechanical in the bulk, and
**dangerous exactly where it stops being mechanical** — and because three
different summaries of it have now circulated with three different sizes. §1
fixes the size with the command that produced it.

---

## 0. The defect, stated once

`PageStore::Get()`, `GetForRead()`, `CreateAt()` and `CreateNew()` return a
raw `std::span<std::byte, kPageSize>` into a buffer-pool frame. The contract
in `page_store.hpp` promises only read-your-writes; it models no pin, so the
span is valid **only while nothing evicts**. Eviction (EVT01-EVT03, EVT06) is
built and disarmed: nothing calls the sweep, because the moment it runs,
every live span in the engine is a potential use-after-free.

The destination exists: `DevicePageStore::PageRef`
(`device_page_store.hpp:399`) — move-only, copy deleted, construction pins,
destruction unpins, `bytes()` explicit, `MarkDirty()` and `Release()` present
— with `PinnedGet` / `PinnedGetForRead` beside it, and it is already tested
(`tests/eviction_test.cpp`, `APageRefPinsForItsLifetimeAndUnpinsExactlyOnce`).
What has never moved is the callers.

**The migration is all-or-nothing in effect** (`docs/txn.md` §8's rule,
restated by `feat-index.md` §13): a half-migrated engine pins some frames and
not others, so eviction still cannot be armed and the work has bought
nothing. §4 stages the *commits* without staging the *guarantee*.

## 1. The size, measured — do not re-estimate, re-run

Counted 2026-08-13 on `fix-errors-1` at `60e79ee`, by exactly this:

```
grep -rn "\.Get(\|->Get(" src/ include/            # 74
grep -rn "GetForRead(" src/ include/                # 47
grep -rn "\.CreateAt(\|->CreateAt(\|\.CreateNew(\|->CreateNew(" src/ include/   # 27
                                                    # engine total: 148
# tests/: 127 + 25 + 93 = 245 more; tree-wide ≈ 393 raw matches,
# of which some are definitions, comments and MemoryLogDevice noise —
# known-gaps.md's "~257 call sites" is the right order of magnitude.
```

Three corrections this table makes to numbers already in circulation:

- A review agent counted **72** by counting `Get(` alone in `src/`+`include/`.
  That misses `GetForRead` — which returns the *same* mutable span with the
  same lifetime bug ("the read-only promise is by contract, not by type",
  `assertion_recover.cpp`) — and misses the two creation verbs. The real
  engine-side set is **~148, twice that estimate**.
- `known-gaps.md`'s ~257 was a tree-wide count including tests. Right order,
  wrong denominator for planning engine work.
- **Tests migrate too.** A test holding a raw span while the sweep runs is a
  test that crashes only under the debug poisoner (MG05), so the 245
  test-side sites are real work, just parallelizable and low-risk.

The count is a snapshot. **MG01's first deliverable makes it obsolete**: once
the return type changes, the compiler enumerates the set exactly, and grep is
retired for this purpose.

## 2. The one rule that must survive every review

**No implicit conversion from `PageRef` to `span`, ever.**
`workplan-eviction.md` already states why, and it goes here too because it
will be proposed again in review as an ergonomic courtesy:

```cpp
auto s = store.Get(id).value();   // with implicit conversion this compiles…
                                  // …and s dangles the moment the temporary
                                  // PageRef unpins. The exact bug eviction
                                  // introduces, minted at every call site.
```

Access is `ref.bytes()`, explicitly, on a named handle whose lifetime is
visible in the code. A reviewer asking for the conversion is asking to
convert a compile-time enumeration back into a latent use-after-free.

## 3. The three shapes of a call site

Every one of the ~148 sites is one of three shapes. Classify before editing;
only the third involves design.

**Shape A — fetch, use, discard (the bulk, mechanical).**

```cpp
// before                                   // after
auto page = store.Get(id);                  auto page = store.PinnedGet(id);
if (!page.ok()) return page.status();       if (!page.ok()) return page.status();
auto view = HeapPage::Open(page.value());   auto view = HeapPage::Open(page.value().bytes());
```

The handle lives to the end of the scope, which is what the old code was
already (wrongly) assuming about the span. Nothing to think about beyond
`GetForRead` → `PinnedGetForRead` + `MarkDirty()` at the sites the comment
in `PageRef` anticipates: a read fetch that turned out to write.

**Shape B — a span held across another fetch (the latent bugs, audit each).**
Btree split holding parent and child; the heap chain walk holding current
and next; `varheap::ChainAppend`'s root-to-tail walk; the assertion linkage
scan. Today these are *already wrong in principle* — two live spans, and any
future eviction between the fetches invalidates the first. Converted, they
hold two `PageRef`s, which is correct — **and which is why MG04 must decide
the pin budget before this shape is converted**, because "hold N pins across
a walk" is exactly what can exhaust a small pool. Every Shape-B site gets a
comment naming how many pins it holds at peak and why that is bounded.

**Shape C — a span stored in a struct or returned (ownership changes).**
The stored span becomes a stored `PageRef` (moving the struct to move-only),
or the design changes so the span is re-fetched per use. Enumerate these
sites in MG01's compile-error list and bring each to review individually;
there are few, and each is an ownership decision, not a conversion.

## 4. Tasks

**MG01 — flip the seam, enumerate by compiler.** On the migration branch,
change `Get`/`GetForRead`/`CreateAt`/`CreateNew` on the `PageStore`
*interface* to return `StatusOr<PageRef>` (the base-class `PageRef` shape:
`InMemoryPageStore` and every other store implements pin/unpin as no-ops —
their frames never move, so a no-op pin is a *true* statement, not a stub).
Keep the old spellings alive for exactly the length of the branch as
`GetUnpinned()` etc., marked `[[deprecated]]`, so subsystems convert one
commit at a time with the tree green throughout. Deliverable: the complete
compile-error list, committed as the checklist the remaining tasks burn down.

**MG02 — Shape A conversions, one subsystem per commit.** Order by blast
radius, smallest first: catalog, undo/txn, heap, varheap, btree, index,
cabin/assertion, exec steps, server. Each commit compiles and passes the
suite; each names its subsystem so review is per-contract, not per-600-line
diff.

**MG03 — Shape B and C, each with its pin-count comment (B) or ownership
note (C).** This is the review-heavy tranche; `critics-developer` per
commit, per the session workflow, with the specific question "is this
site's peak pin count bounded, and by what".

**MG04 — the pin budget, decided not discovered.** EV8 already decides pool
*exhaustion* (bounded cooperative retry, then `ResourceExhausted`, no
waiting, ever) and EV6 already gives scans the ring so they never hold pins
proportional to relation size. What no document yet states is the
**per-operation pin ceiling** — the number that makes EV8's retry a rare
event rather than a steady state. Proposal to confirm or amend in
`spec-eviction.md`: an operation holds at most **4** simultaneous pins
(btree descent's worst honest case: parent + child during a split, ±1 for
an index maintaining alongside); anything needing more goes through the
scan ring or is redesigned. A `static_assert`-style debug counter enforces
it: exceeding the ceiling in a debug build is an abort naming the site.

**MG05 — the poisoner, written before eviction is armed.** A clean compile
proves lifetimes for no one. Debug-build `DevicePageStore` poisons every
reclaimed frame (memset `0xEF`) before reuse, and the full suite runs with
eviction armed and the resident limit set brutally low (every fetch a
potential reclaim), under ASan. Every surviving raw-span assumption becomes
a deterministic crash naming its site. This suite run — not the compile —
is the migration's acceptance gate.

**MG06 — delete the escape hatch, arm, close the gaps.** Delete
`GetUnpinned()` and friends; wire the sweep calls that
`workplan-eviction.md` EVT03/EVT06 left dangling; run the eviction crash
matrix and BLK07, which stop being vacuous at this exact commit; strike the
"built but disarmed" entry in `known-gaps.md` and flip the CLAUDE.md
milestone row, per the maintenance rule.

## 5. Already decided elsewhere — do not re-open here

- **Pinned classes** (EV3, as amended by the finding in
  `device_page_store.hpp`): fixed catalog pages by *id range*, Bound Cabin
  pages by *kind*. A `PageRef` pin is absolute on top of that.
- **Scan resistance** (EV6): bulk scans use the ring, don't bump usage.
- **Exhaustion** (EV8): cooperative retry then `ResourceExhausted`. MG04's
  ceiling is an *addition* under EV8, not a change to it.
- **No partial migration** (`txn.md` §8): the staging in §4 stages commits,
  never the guarantee — eviction stays off until MG06.

## 6. Acceptance

1. Zero raw-span accessors on `PageStore` — the type system, not grep.
2. MG05's poisoned-frame suite green under ASan with eviction armed and a
   minimal resident limit.
3. Every Shape-B site carries its peak-pin comment; the MG04 debug counter
   never fires in the suite.
4. The eviction crash matrix and BLK07 run and pass — for the first time
   non-vacuously.
5. `bench/`: interleaved A/B against the pre-migration commit, release
   build. Pin/unpin on the hot path is two atomic ops per fetch; the number
   to publish is the per-statement delta, and the honest expectation is
   "small but nonzero" — report it, do not bury it.

## 7. Open decisions — flag, don't assume

- MG04's ceiling value (4 is a proposal from the btree's shape, not a
  measurement).
- Whether `CreateAt`/`CreateNew` return pinned (symmetric, assumed here) or
  keep raw spans on the argument that a just-created page cannot be a sweep
  victim — rejected by default because it re-opens the two-accessor world,
  but cheap to revisit at MG01 if the conversions say otherwise.
- Whether the test tranche (245 sites) converts wholesale in MG02's rhythm
  or rides behind the poisoner and converts only what crashes. Wholesale is
  assumed; the shortcut is listed because it is tempting and its cost —
  tests that pass while modeling the pre-eviction world — should be priced
  by whoever takes it.
