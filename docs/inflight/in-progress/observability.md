# KDS Observability — Tracing & Inspection

A proposal for per-request tracing and inspection. **Nothing here is decided and nothing is implemented.** `[PROPOSED]` marks this document's default; `[OPEN]` must not be assumed. Consistent with `docs/rules/rules.md` (§3 shared-nothing, §4 injectable clock/IO), `docs/spec/sched.md` (reactor phases, scheduling groups), and `docs/spec/wal.md` §13.

---

## 1. What problem this solves

Today there is no way to attribute time. A slow `SELECT` is slow somewhere between the socket read, the parser, the catalog lookup, the heap scan, the row decode, and the reply write — and the only tool is a stopwatch around the whole thing. The engine already has the two things a tracer needs and no tracer to use them: an injected `sched::Clock` (`rules.md` §4) and a reactor with named scheduling groups (`sched.md` §4).

The design goal is narrow: **one trace per request, a span per layer, zero cost when disabled, and no allocation on the hot path when enabled.**

## 1a. What exists today — counters on `SHOW META`, not tracing

Nothing in this document is built. What *is* built, and is worth naming
here so this proposal is not read as the engine's only instrumentation, is
a growing set of **per-core counters printed by `SHOW META`** — fields on
an existing command, not a subsystem. They answer "how much" and never
"where the time went", which is exactly the gap §1 describes.

- **Per-scheduling-group accounting** against reactor wall time
  (`sched_wall_us`, `sched_<group>_polled_us` / `_polls` / `_consumed_us`),
  added 2026-08-26 by the statement-shipping pretasks' T4 — so the time
  charged to no group is computable from outside the process
  (`docs/spec/sched.md` §4).
- **The idle policy and the wake path**, added 2026-08-26/27 by the v2.3.0
  order's RW1-RW3 and its D7: `sched_idle_blocks`, `sched_wake_race_skips`,
  `sched_parked_idle_blocks`, `sched_idle_block_us`, `sched_wakes_sent`,
  `sched_wakes_received`, `sched_spurious_wakes`. The one that changes what
  can be *read* rather than adding a count is `sched_idle_block_us`: with
  it, `sched_wall_us - sum(sched_*_polled_us) - sched_idle_block_us` is the
  time charged to nobody that was not sleep, which is what the bullet above
  could not separate — an arrival core measured at 79.5% sleep and 10.3%
  unaccounted work where the two used to arrive as one 90% lump
  (`docs/spec/sched.md` §7).
- **Cross-core write refusals** (`cross_core_write_refusals` and its keyed
  detail), the population a 2PC decision would be made from
  (`docs/spec/crosscore.md` §6).
- **Statement shipping**, both halves, added 2026-08-26 by SS4: what a core
  shipped and what came back (`shipped_statements`, `shipped_replies`,
  `shipped_refusals`, `shipped_wait_us_max`, and the anomaly counters that
  should stay 0), and what a core ran for others (`shipped_executed`,
  `shipped_running`, `shipped_deduped`, `shipped_unanswerable`,
  `shipped_early_evictions`). Field-by-field in `docs/spec/client-manual.md` §3.

Every one of them is **core-local** and **absent rather than zeroed where
the thing it counts is not wired** — a rule worth keeping if this proposal
is ever built, because a zero and an absence answer different questions.

## 2. Non-goals

- **Not a logging framework.** No severity levels, no formatters, no sinks, no `printf`-style call sites scattered through engine code. A log line inside a page-latch critical section is a latency bug waiting to happen. If a subsystem needs to report a condition, that is a `Status` (`rules.md` §1), not a log.
- **Not distributed tracing.** No W3C tracecontext, no span export protocol, no sampling policy negotiated with a collector. One process, one core today.
- **Not always-on.** Production default is off. The `[OPEN]` question of whether a low-rate always-on mode is worth it is deliberately left open (§9).
- **Not a profiler.** It attributes time to *layers the engine names*, not to functions. `perf` already does functions better.

## 3. Model — trace, span, layer `[PROPOSED]`

Three concepts, deliberately no more:

- **Trace** — one client request, end to end. Created when `TcpServer` frames a complete command line; closed when the reply is written. Carries a `TraceId` (core-local monotonic `uint64_t`; global uniqueness is `[OPEN]` and not needed while requests are core-local).
- **Span** — one layer's slice of that trace. Nests: a span records its parent, so the tree reconstructs where the time went rather than just a flat list of totals.
- **Layer** — a fixed, enumerated component. **Enumerated, not a string**: a string tag means an allocation and a hash per span, and layer names are a closed set the engine controls. Adding one is a one-line enum change.

```
enum class Layer : uint8_t {
    kRequest,      // whole command, the root span
    kParse,        // src/parser
    kCatalog,      // catalog lookups, schema build
    kPlan,         // query-optimizer, when it exists
    kExecute,      // src/exec - row codec, WHERE evaluation
    kHeap,         // heap page scan / insert / overwrite
    kBufferPool,   // frame lookup, pin, latch wait
    kPageIo,       // PageDevice read/write/sync
    kWalAppend,    // record append into the ring
    kWalFlush,     // ring drain + device sync
    kCheckpoint,   // system-group checkpoint work
    kReply,        // response encode + socket write
};
```

A span records: `layer`, `parent_index`, `start_ns`, `end_ns`, and one `uint64_t` `detail` whose meaning is per-layer (rows scanned for `kHeap`, bytes for `kPageIo`, LSN for `kWalFlush`). One opaque integer rather than a key-value bag — a bag is where an inspection tool turns into a serialization format.

## 4. Placement in the architecture

```
                       ┌──────────────────────────────┐
   client request ───► │ TcpServer::DrainCommands     │  opens the trace
                       └──────────────┬───────────────┘
                                      │  TraceContext& threaded down
                       ┌──────────────▼───────────────┐
                       │ CommandDispatcher            │  span kRequest
                       │  ├─ parser::Parse            │  span kParse
                       │  ├─ Catalog::InitTableAccess │  span kCatalog
                       │  ├─ exec::EncodeRow/Decode   │  span kExecute
                       │  ├─ heap::PageView           │  span kHeap
                       │  └─ PageStore::Get           │  span kBufferPool → kPageIo
                       └──────────────┬───────────────┘
                                      │  trace closed, appended to
                       ┌──────────────▼───────────────┐
                       │ TraceSink (core-local ring)  │  fixed capacity, drop-oldest
                       └──────────────┬───────────────┘
                                      │  read by
                       ┌──────────────▼───────────────┐
                       │ SHOW TRACE / SHOW TRACE <id> │  inspection commands
                       └──────────────────────────────┘
```

The `system`-group work (checkpointer, WAL drain) is **not** part of any request's trace — it is not caused by one request and attributing it to whichever request was unlucky enough to be in flight would be a lie. It gets its own traces, rooted at `kCheckpoint`, which is also how a developer sees a checkpoint stalling the foreground.

## 5. Threading the context — the one real design cost `[PROPOSED]`

A tracer has to reach the call sites. Three ways, and the choice matters more than anything else here:

| Approach | Cost | Verdict |
|---|---|---|
| **Explicit `TraceContext&` parameter** | every traced signature grows a parameter | **Proposed.** Honest about the dependency, works with the reactor's cooperative yields, no hidden state. |
| Thread-local current-span stack | zero signature churn | **Rejected.** A cooperative task that yields mid-span leaves the TLS stack describing the wrong task. This is exactly the bug a thread-per-core reactor makes easy to write and impossible to find. |
| Return-value plumbing (`StatusOr<T>` carries timings) | no new parameters | **Rejected.** Conflates a result with its instrumentation. |

The parameter is passed as `TraceContext*` — `nullptr` means untraced, which is the whole disabled path and costs one predictable branch.

To keep it from being a wall of churn, only **layer entry points** take it, not every function: `CommandDispatcher::Handle*`, `Catalog::InitTableAccess`, `PageStore::Get`, `PageView::InsertTuple`, `WalManager::Append`. Roughly a dozen signatures, not hundreds.

## 6. Zero-cost-when-off `[PROPOSED]`

```
class TraceContext;   // opaque; nullptr == disabled

// RAII span. Constructor reads the clock and pushes; destructor reads it
// and pops. A null context makes both no-ops.
class SpanScope {
  public:
    SpanScope(TraceContext* ctx, Layer layer) noexcept;
    ~SpanScope();
    void set_detail(uint64_t d) noexcept;
};
```

Call site:

```
SpanScope span(ctx, Layer::kHeap);
...
span.set_detail(rows_scanned);
```

- **Disabled:** `ctx == nullptr` → constructor and destructor are two predicted-not-taken branches. No clock read (a `clock_gettime` even via vDSO is ~20 ns and would show up in a tuple-scan loop).
- **Enabled:** two clock reads and a push into a **preallocated** per-trace span array. Fixed capacity (`kMaxSpansPerTrace`, `[PROPOSED]` 64); overflow increments a dropped-span counter rather than allocating. Same discipline as the WAL ring: bounded, no allocation, honest about loss.

Whether spans compile out entirely under a build flag, versus staying runtime-switchable, is `[OPEN]` — runtime-switchable is far more useful for a development tool and CLA would default to it, but the decision belongs with whoever owns the release build.

## 7. Sink and retrieval `[PROPOSED]`

- **`TraceSink`** — a core-local ring of completed traces, fixed capacity (`[PROPOSED]` 256), **drop-oldest**. Advisory data, exactly like Waystone: losing it costs insight, never correctness, so it must never apply backpressure to the request path. This is the opposite of the WAL's rule (`wal.md` §2) and the difference is worth being explicit about.
- **Inspection commands**, in the same `\n`-escaped one-line convention as `SHOW PAGE` (`docs/spec/client-manual.md`):
  - `TRACE ON` / `TRACE OFF` — toggles collection for the session.
  - `SHOW TRACES` — the ring: one line per trace, `id / command / total_us`.
  - `SHOW TRACE <id>` — the span tree, indented, with self-time and child-time separated. Self-time is the number that finds the culprit; total time only tells you which subtree to open next.
  - `SHOW TRACE STATS` — per-layer aggregate over the ring: count, total, mean, max. What you look at first.

Sketch of the intended output:

```
ckdbs> SHOW TRACE 41
trace=41 cmd="SELECT * FROM acct WHERE bal > 10" total=412us
  kRequest              412us  self=6us
    kParse               31us  self=31us
    kCatalog             88us  self=12us
      kBufferPool        76us  self=4us
        kPageIo          72us  self=72us   detail=8192B
    kExecute            281us  self=19us
      kHeap             262us  self=262us  detail=1204rows
    kReply                6us  self=6us
```

## 8. Why not just add timing to the existing stats structs

`WalStats` and `CheckpointStats` already exist and already count things. They are **process-lifetime counters**, not per-request attribution — they answer "how many syncs since boot", never "which part of *this* query was slow". Both are wanted; neither substitutes for the other. The rule proposed: counters that an operator would page on live in the subsystem's own `*Stats`; per-request attribution lives here and is off by default.

## 9. Open decisions — do not assume

- Whether tracing is compile-time removable or runtime-only (§6).
- `kMaxSpansPerTrace`, trace-ring capacity, and what a dropped span does to a trace's credibility (mark the trace incomplete vs report partial).
- Global vs core-local `TraceId` once multi-core lands — and whether a trace can span cores at all, which is tied to the `[OPEN]` cross-core transaction question (`wal.md` §3).
- Whether `SHOW TRACE*` survives the move to KWP/1 (`docs/spec/protocol.md`) as protocol messages, or stays a text-protocol debug affordance and disappears with it.
- Sampling: fraction-based, slow-request-only (trace everything, keep only over-threshold), or manual. Slow-request-only is the most useful default for a development tool and the most complex to get right, since the decision to keep comes after the cost of collecting.
- Whether `kPageIo` spans should distinguish cache hit from device read — the two differ by ~4 orders of magnitude and averaging them together produces a number that describes nothing.

## 10. Suggested build order

1. `Layer`, `SpanScope`, `TraceContext`, `TraceSink` + unit tests against a `ManualClock` — deterministic, no I/O, asserts nesting and overflow behavior.
2. Thread the context through `CommandDispatcher` only. One layer of spans (`kRequest`, `kParse`, `kExecute`) is already useful and proves the plumbing.
3. `SHOW TRACES` / `SHOW TRACE <id>` / `TRACE ON|OFF`.
4. Push spans down into catalog, page store, heap.
5. `SHOW TRACE STATS`, and `kCheckpoint`/`kWalFlush` traces for `system`-group work.

Steps 1–3 are the ones that pay; 4 onward is incremental and can stop wherever it stops being worth the signature churn.
