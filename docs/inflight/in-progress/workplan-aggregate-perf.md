# Aggregate query performance — workplan

Tasks `AP01`-`AP06`, companion to `docs/spec/aggregate.md`. Every item here
is grounded in a measurement in `bench/results-aggregate.md` or
`bench/results-scenario1-vs-pg.md`; anything that is not is named as a
guess.

**The one-line summary of what the measurements say: the fold is not the
problem.** Against PostgreSQL 17.10 over 60,480 rows, ckdbs runs a grouped
scan at 0.36× on the global form and *wins* at 1.37× once there are 7,560
groups — so the fold's own scaling is already better than a HashAggregate's.
What is 2.7× behind is the walk underneath it, and most of that walk is
spent decoding columns nothing reads.

## Where to pick this up — state as of 2026-08-07

**Built: AP01, AP02 (partly), AP03.** Next: **AP05**, which is the largest
measured item left. AP04 and AP06 are still open; AP02's remaining half is
open and is a *decision*, not effort.

| task | state | what it got |
|---|---|---|
| AP01 | **done** (`8ab989f`) | 3.21× on `COUNT(*)` over 12 columns; 1.70× on a plain projection |
| AP02 | **partly** (`91d5c73`, `b0d461b`) | 7-9% from a lazy column name, then 19-35% from the decode-call fixes |
| AP03 | **done** (`8e0f8d5` + the `wip` before it) | fold overhead per statement +4.13 µs → +1.88 µs |
| AP04 | open | DISTINCT was +27.4% per row *before* AP02; **re-measure first** |
| AP05 | open, **next** | ~0.5 µs per group founded; +31.9 µs on a 64-group fold |
| AP06 | open | re-run the benches and the PostgreSQL comparison |

Cumulative against the state before any of it, Release build, 20,000 rows:
`SELECT COUNT(*) FROM wide` **3.95×**, `SELECT SUM(a) FROM wide` **3.08×**,
`SELECT a FROM wide` **2.59×**.

### The rule this workplan earned the hard way

**Re-measure the premise before building the fix.** Two tasks running had
their stated cause disproved by doing so:

- **AP02** blamed `AstValue` construction. It was `DecodeOneValueInto`
  building a `std::string` of the column's *name*, per column per row, for
  errors nobody reads.
- **AP03** blamed constructing an `Aggregator` per statement. Hoisting it
  changed **nothing measurable**; the cost was `RunAggregated` building a
  second `std::ostringstream`.

Both stated causes were plausible and both were wrong, because each was
written against an engine that the previous task had already changed. AP04's
+27.4% and AP05's ~0.5 µs are both pre-AP02 numbers and should be assumed
stale until re-taken.

### How to measure here

Two instruments, and the wrong one hides everything:

- **Client latency** (`tools/aggregate_benchmark.py`) for whole-scan work,
  where the engine dominates. Useless below ~20 µs: a pk lookup is ~115 µs
  end to end and ~85% of that is the Python client.
- **Server CPU** (`/proc/<pid>/stat`, fields 13-14) for per-statement fixed
  costs. Run paired A/B **interleaved**, four rounds of 20,000 statements;
  a non-interleaved pair drifts enough between server restarts to invent a
  result, which is exactly how AP03's hoist first looked like a 18% win.

And: **`./build` is Debug.** `CMakeLists.txt` defaults `CMAKE_BUILD_TYPE`
that way, and a Debug measurement is wrong in both directions - see the
header of `bench/results-aggregate.md`. Use `build-release`.

---

Execution rules, the same ones `docs/workplan-aggregate.md` states:
- Do tasks in numeric order unless "needs" says otherwise.
- Each task ships with its tests in the same change.
- **Measure on a Release build.** `CMakeLists.txt` defaults to Debug, and
  the first version of `bench/results-aggregate.md` was wrong in both
  directions because of it — see that file's header.
- **AG1 still holds.** The fold stays outside the executor. A change to
  `step_vm.cpp` here is expected (the decode path lives there) but a change
  to `AccessKind`, or an operator inside the chain, means the placement
  decision was violated rather than that the task needed it.

---

## The measurement everything below rests on

Same relation, same 20,000-row walk, Release build, one connection:

| statement | mean | reads |
|---|---:|---|
| `SELECT COUNT(*) FROM narrow` (2 columns) | 4,474 µs | 0 columns |
| `SELECT COUNT(*) FROM wide` (12 columns) | 11,990 µs | 0 columns |
| `SELECT COUNT(*) FROM wide WHERE a = 1` | **3,898 µs** | 0 columns |

The fold reads **no column** in all three. The first two differ by 2.7×
purely in relation width, and the third — which does strictly *more* logical
work than the second — is **3.1× faster than it**.

The cause is not subtle. `Step::filter_columns` is computed from the
residual alone (`step_compiler.cpp` §4), and `ChainRunner::AcceptTupleAt`
decodes that mask, tests the residual, then decodes **`~filter_columns`** for
every surviving row. So:

- `COUNT(*)` with no WHERE has an empty residual → mask 0 → every row
  survives → every row decodes **all 12 columns** into `AstValue`s that
  nothing reads.
- Adding `WHERE a = 1` puts one column in the mask and rejects 19,999 rows
  before the full decode, so only one row pays it.

**This is not aggregate-specific.** `SELECT a FROM wide` has the same defect
— it decodes 12 columns to emit 1. The fold is only what made it visible,
because a fold is the one consumer that can read *zero* columns and
therefore shows the waste undiluted.

---

## AP01 — Decode what the statement reads, not what the filter reads — **BUILT**

`Step` gains a second mask beside `filter_columns`: the columns any consumer
of the row actually reads — the projection's refs, the aggregate spec's item
and group-key refs, the next step's probe key, a sub-chain's correlation,
the trail's pk. `AcceptTupleAt`'s post-residual pass decodes
`read_columns & ~filter_columns` instead of `~filter_columns`.

For `SELECT COUNT(*) FROM t` that pass decodes **nothing** and the walk
becomes page iteration plus a counter.

Three things to get right, and the third is a decision rather than a
detail.

**The mask must be a superset of every reader**, or a row is folded from a
slot still holding the previous row's value — the exact bug the Cabin
write-hook comment in `step_vm.cpp` already documents for its own case, and
the reason `recording_here` forces a full decode today. Build it in the
compiler where every consumer is already resolved, and default it to
`kAllColumns` so a `Step` built by anything other than the compiler is slow
rather than wrong (the rule `filter_columns` already follows).

**`SELECT *` keeps `kAllColumns`.** It reads every column by definition.

**It touches AG1's chain-identity test, and that needs confirming rather
than assuming.** Spec §1 promises the chain is identical "in steps, kinds,
residuals and class" — a decode mask is none of those, so the *spec* permits
this. But `tests/aggregate_contract_test.cpp` renders `filter_columns` in
its shape comparison, so an aggregated statement and its plain twin would
diverge there. Either the rendering narrows to the spec's four, or the mask
lives somewhere the comparison does not reach. **Settle it before building,
and record which.** Narrowing the test is the likelier answer — the access
path is what AG1 is about, and a decode mask is not an access path — but
that is an argument, not a decision.

*Done when:* `SELECT COUNT(*) FROM wide` lands within noise of
`SELECT COUNT(*) FROM narrow`; the wide/narrow ratio above collapses;
`tests/aggregate_test.cpp`'s zero-allocation and NULL tests are unchanged;
the scan/probe equivalence and the five Waystone configurations are
unchanged; and the chain-identity decision above is written into the spec.

*Expected:* ~3× on a fold over a wide relation, ~2× on scenario1's
`agg-global` (9 columns, 2 read), which would move it from 0.36× to roughly
0.7× of PostgreSQL. **Every projection benefits too**, which is the larger
prize and the reason this is AP01.

*Measured* (`bench/results-aggregate.md`): **3.21×** on `COUNT(*)` over a
12-column relation, **2.00×** on `SUM`, and **1.70×** on a plain
`SELECT a` — the projection gain confirming this was never an aggregation
fix. `COUNT(*)` without a WHERE is no longer slower than with one, and
relation width costs 1.15× where it cost 2.7×.

*The decision the task said to settle, settled:* **a second field**, not a
change to `filter_columns`. That name means what the *filter* needs, which
is what the Cabin write-hook logic keys off and what the contract suite
renders; narrowing the identity test would have been the more invasive
answer. `read_columns` is simply not rendered, which conforms to spec §9.1
literally — it fixes identity on "steps, kinds, residuals, class" and a
decode mask is none of the four. The exclusion is commented in the test
rather than left to be re-derived.

## AP02 — Fold from the cell, not from an `AstValue` (needs AP01) — **PARTLY DONE**

Even with AP01, a folded column is decoded into an `AstValue` — two
`std::string` members and a tag — and then read once. `bench/results-scenario1-vs-pg.md`
already measured the general form of this: **96% of a decode was building
`AstValue`s rather than reading cells.**

For the integer aggregates the fold needs an `int64` and nothing else. Add a
path that reads the cell directly into the accumulator for
`COUNT`/`SUM`/`MIN`/`MAX` over a fixed-width integer column, leaving the
`AstValue` path for `varchar` extremes and for anything the group key needs
in its encoded form.

**Do not generalise this into an expression evaluator.** It is one function
over one cell for four functions, and the moment it grows a switch on
operators it has become the thing spec I10 keeps out of the grammar.

*Done when:* `SUM` over an int64 column costs measurably less than the same
statement's `MIN` over a varchar; the NULL table and the overflow behaviour
are unchanged; no new code path can produce a value the `AstValue` path
would not.

*What was actually found and done.* **The premise was wrong.** Reading the
code before writing any found that `DecodeOneValueInto` built a
`std::string` of the column's name on **every call** - once per column per
row - for error messages almost never produced, and `EncodeOneValue` did the
same on the write path. Building the name only where an error is built is
worth **7-9%** on every statement that decodes a column
(`bench/results-aggregate.md`), and nothing on `COUNT(*)`, which after AP01
decodes none - which is the check that the measurement is real.

*What is still open, and why it is now a design question.* Decoding one
integer column still costs ~71 ns/row over `COUNT(*)`, and that is the call
framing rather than the int path: per-row input validation, a 12-iteration
mask loop to find one set bit, and a `Status` returned per column. Two ways
on, and they are not equivalent:

  **(a) Make the decode call cheaper** - **DONE.** Two causes, both
  general. `Status` carries a `std::string` by value, so a Status-returning
  check builds one even when it passes, and `DecodeColumnsInto` ran two per
  call before reading a byte; testing the predicate first and calling the
  checker only on failure keeps every check and builds nothing on the path
  that succeeds. And the column loop tested every column's bit to find the
  one it wanted; it iterates the mask's set bits now. Worth **19-35%** on
  top of the lazy name, which takes `COUNT(*)` over twelve columns to
  **3.95x** its original cost and one column's decode from 98 ns/row to 33.

  **(b) Fold from the cell**, as this task was named - **still open, and
  the case for it is now weaker**: one column's decode is 33 ns/row, so the
  ceiling on this route has fallen with (a). It needs the raw
  payload and the layout at the `Aggregator`, which today consumes a
  `ChainFrame` - the same thing every `RowSink` consumes, which is the
  AG1 seam. Giving the fold raw bytes means either widening `RowSink` for
  every consumer or teaching the executor that aggregation exists. **Neither
  is obviously right and the measurement does not yet justify either**, so
  this stays unbuilt pending a decision, not pending effort.

## AP03 — Hoist the `Aggregator` onto the dispatcher — **DONE, and the premise was wrong again**

Measured: a fold costs **+30.1%** on a pk point lookup — ~31 µs on a 103 µs
statement — and that is almost all fixed setup, not per-row work. The
dispatcher already hoists `trail_scratch_` and `replay_scratch_` for exactly
this reason, and the commit that did it records an 18% regression on a point
join from constructing a collector per statement.

Keep one `Aggregator` per dispatcher, `Clear()` it per statement the way
`TrailCollector` is cleared, and keep the reservations.

**The lifetime rule has to survive it**: the aggregator borrows the chain's
`AggregateSpec` and `column_names`, which live only as long as the
statement. A hoisted aggregator must not hold either past `Finish` — take
them per statement rather than at construction.

*Done when:* the point-lookup fold overhead drops below 10%; an
allocation-counting test shows a second aggregated statement allocating
nothing the first did not.

*What happened.* The overhead was **+4.13 µs/stmt of server CPU** on a pk
lookup, re-measured before touching anything - the task's own +30.1% figure
had expired, because AP01 and AP02 moved the denominator and the earlier
number was taken over a client socket with 30 reps.

**Hoisting the aggregator changed nothing measurable.** Still +4.13 µs
after it. The cost was `RunAggregated` constructing a **second
`std::ostringstream`** - the caller already had one holding the heading
line, and the callee copied the header out of it and built a fresh stream.
An `ostringstream` carries a `stringbuf` and a locale; passing the caller's
buffer by reference took the overhead to **+1.88 µs**, a 54% cut.

The hoist is kept, on its own smaller merits - five fewer allocations per
aggregated statement, and it mirrors `trail_scratch_` - but it is not
credited with the improvement. **Two tasks in a row have now had their
stated cause disproved by measuring first** (AP02's was a column name, not
an `AstValue`), which is worth stating as a rule rather than a coincidence:
*re-measure the premise before building the fix, because the earlier number
was taken against an engine that has since changed.*

## AP04 — DISTINCT without building a string per row (needs AP02)

Measured: `COUNT(DISTINCT sym)` costs **+27.4%** over `COUNT(sym)` on the
same walk, with 16 distinct values over 20,000 rows — so all but 16 probes
are hits and the cost is the per-row encode and probe, not the inserts.

For a fixed-width integer column the encoded key is a tag byte plus eight
bytes of `int_val`; hashing the value directly and keeping the set keyed on
`int64` removes the string entirely. Strings keep the current path.

**§3.2's rule is what constrains this**: distinctness must mean exactly what
"same group key" means. Two representations are permitted only while they
partition identically — an int set and the encoded-string set must agree on
every value, which is a property to test rather than assert.

*Done when:* the DISTINCT overhead on an integer column drops materially;
`tests/aggregate_test.cpp`'s encoding tests (NULL against empty string,
length-prefixed keys, distinct-per-group) are unchanged and pass against
both representations.

## AP05 — Pre-size the group map (needs AP01)

Measured: about **1 µs per group founded**, which is the vector growth, the
map node and the rehashing. `sys.access_stats` records one row per access
*shape* with a `use_count`, and **nothing consumes it** — this would be its
first reader, which `docs/spec/heap-and-tuple.md` §7 has wanted since the
statistics landed.

Reserve `groups_` and `index_` from the last execution's group count for
this shape.

**It must stay a hint.** A wrong estimate may cost a rehash and may never
change a result, and the cap in AG11 still refuses rather than truncates. If
that cannot be kept trivially true, do not build it: 1 µs per group is the
smallest item on this list.

*Done when:* a repeated grouped scan founds its groups with no rehash;
deleting every access-statistics row changes no result, only speed.

## AP06 — Re-measure and correct the documents

Re-run `tools/aggregate_benchmark.py` and scenario1 against PostgreSQL on a
Release build, and update `bench/results-aggregate.md` and
`bench/results-scenario1-vs-pg.md` — including the `agg-*` table, whose
ratios are the point of the exercise.

*Done when:* both files carry post-AP01 numbers with the build stated, and
the aggregation section says what moved and what did not.

---

## Not on this list, and why

- **Pre-aggregation below a join.** Rejected by `aggregate.md` §1, not
  deferred: it needs an operator inside the chain, which means teaching the
  step-kind trust table about a step that reads no relation. Revisit only if
  measured traffic presents the shape.
- **Parallel or partial aggregation.** `Aggregator::Merge` (AG-M) is built
  and tested precisely so this is possible, and it belongs to
  `docs/spec/crosscore.md`'s pipeline rather than here — there is no second core
  running steps yet.
- **An index-driven `GROUP BY`.** There is no secondary index to drive it
  with. A Cabin is keyed by *value* and is authoritative only for observed
  ones, which answers "which rows have this value", not "what values are
  there" — the question grouping asks. Reading a Cabin as a group source
  would be trusting it for absence, which invariant 9's argument forbids for
  the same reason it forbids trusting a trail.
- **`HAVING` and `ORDER BY` push-down.** Neither exists yet; both are
  post-fold consumers and belong with the decision that adds them.
- **Making the fold beat a HashAggregate.** It already scales better with
  group count (+46% against +454% from 1 to 7,560 groups). Nothing on this
  list is aimed at the fold's asymptotics, because the measurements do not
  point there.
