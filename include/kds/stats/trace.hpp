#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <utility>

#include "kds/sched/clock.hpp"

// Per-request attribution — the instrument `docs/inflight/in-progress/observability.md`
// specifies, built to its §10 steps 1-3 and no further (work order H, H6).
//
// ---- Why this exists, and why now ----------------------------------------
//
// **Three times a missing instrument has blocked an attribution**, and §8a
// names all three: M3 could not decompose `shipped_statement_us`, RP8 could
// not separate two candidate explanations for why three sequential syncs do
// not cost 3x, and RR2's finding is recorded as *likely* because nothing can
// tell a queued wake from a slow leg from outside the process. §8a's own
// conclusion is that a span per protocol leg would have settled RP8's
// question in one run and RR2's in the run that raised it.
//
// It is built while the measurement work is blocked on hardware because
// that is exactly when it is cheapest: it makes the runs that follow read
// their answers instead of inferring them.
//
// ---- What is deliberately not here ---------------------------------------
//
// Steps 4-5 of §10 - spans pushed down into the catalog, page store and
// heap, `SHOW TRACE STATS`, and traces for `system`-group work. The doc
// says steps 1-3 "are the ones that pay" and that the rest "can stop
// wherever it stops being worth the signature churn". So `Layer` enumerates
// the whole set (adding one later must not renumber the ones a stored trace
// used) while only the request-level ones are ever emitted today.
//
// **Cross-core traces are not decided here.** §9 ties that to `wal.md` §3's
// cross-core question. A `TraceId` is core-local monotonic, which is
// sufficient while a trace covers one core's work and prejudges nothing.
//
// ---- Two of §9's decisions, taken to build this much ---------------------
//
// **Runtime-switchable, not compile-time removable.** A development tool
// that needs a rebuild to answer a question is one nobody uses during the
// run that raised it. The off path is a null pointer test - see
// `SpanScope` - so the cost of keeping it switchable is one predictable
// branch, which is the budget §6 sets.
//
// **Sampling is manual** (`TRACE ON` / `TRACE OFF`), not fraction-based and
// not slow-request-only. §9 calls slow-request-only the most useful default
// and the most complex, since the decision to keep comes *after* the cost
// of collecting - and manual is what makes zero-cost-when-off trivially
// true rather than argued, because nothing is collected at all. It also
// defers the hard choice to a caller with an opinion, which does not exist
// yet. Reversible: a sampler decides whether to hand out a `TraceContext`,
// and every call site below already handles not getting one.
//
// Concurrency: everything here is core-local and single-threaded, like the
// reactor that owns it. A `TraceContext` belongs to one request; a
// `TraceSink` to one core.

namespace kds::stats {

// A fixed, enumerated component. **Enumerated rather than a string** for
// §3's reason: a string tag is an allocation and a hash per span, and layer
// names are a closed set the engine controls.
//
// The values are stable: a stored trace names layers by number, so a later
// build inserting one in the middle would relabel history. Append only.
enum class Layer : std::uint8_t {
    kRequest = 0,     // whole command, the root span
    kParse = 1,       // src/parser
    kCatalog = 2,     // catalog lookups, schema build
    kPlan = 3,        // query optimizer, when it exists
    kExecute = 4,     // src/exec - row codec, WHERE evaluation
    kHeap = 5,        // heap page scan / insert / overwrite
    kBufferPool = 6,  // frame lookup, pin, latch wait
    kPageIo = 7,      // PageDevice read/write/sync
    kWalAppend = 8,   // record append into the ring
    kWalFlush = 9,    // ring drain + device sync
    kCheckpoint = 10, // system-group checkpoint work
    kReply = 11,      // response encode + socket write
    kMaxLayer = 12,
};

const char* LayerName(Layer layer) noexcept;

// One layer's slice of one trace. `parent` is an index into the trace's own
// span array, so the tree reconstructs without pointers - which is what lets
// the array be a fixed-size member rather than an allocation.
struct Span {
    Layer layer = Layer::kRequest;
    std::uint16_t parent = kNoParent;
    sched::MonoTimeNs start_ns = 0;
    sched::MonoTimeNs end_ns = 0;
    // One opaque integer whose meaning is per-layer - rows for `kHeap`,
    // bytes for `kPageIo`, an LSN for `kWalFlush`. **One integer and not a
    // key-value bag**, §3's rule: a bag is where an inspection tool turns
    // into a serialization format.
    std::uint64_t detail = 0;

    static constexpr std::uint16_t kNoParent = 0xFFFF;

    sched::MonoTimeNs duration_ns() const noexcept {
        return end_ns >= start_ns ? end_ns - start_ns : 0;
    }
};

// §6's proposed cap. Overflow drops the span and counts it rather than
// allocating - the WAL ring's discipline: bounded, no allocation, honest
// about loss.
inline constexpr std::size_t kMaxSpansPerTrace = 64;

// One client request's spans, end to end.
//
// **A dropped span makes the trace incomplete, and it says so** - §9 leaves
// "mark incomplete vs report partial" open and this takes the first,
// because a span tree missing a node reads as a *fast* subtree rather than
// an absent one, which is the one way an instrument can lie about the thing
// it was built to measure.
class TraceContext {
public:
    TraceContext(std::uint64_t id, const sched::Clock* clock, std::string command)
        : id_(id), clock_(clock), command_(std::move(command)) {}

    std::uint64_t id() const noexcept { return id_; }
    const std::string& command() const noexcept { return command_; }
    std::span<const Span> spans() const noexcept { return {spans_.data(), count_}; }
    std::uint32_t dropped() const noexcept { return dropped_; }
    bool complete() const noexcept { return dropped_ == 0; }

    // The whole request's duration - the root span's, or 0 before one is
    // closed.
    sched::MonoTimeNs total_ns() const noexcept {
        return count_ == 0 ? 0 : spans_[0].duration_ns();
    }

private:
    friend class SpanScope;

    // Opens a span under whatever is currently open. Returns its index, or
    // `Span::kNoParent` when the trace is full.
    std::uint16_t Open(Layer layer) noexcept;
    void Close(std::uint16_t index, sched::MonoTimeNs end_ns, std::uint64_t detail) noexcept;
    sched::MonoTimeNs Now() const noexcept { return clock_ == nullptr ? 0 : clock_->Now(); }

    std::uint64_t id_ = 0;
    const sched::Clock* clock_ = nullptr;
    std::string command_;
    std::array<Span, kMaxSpansPerTrace> spans_{};
    std::uint16_t count_ = 0;
    std::uint16_t open_ = Span::kNoParent;  // the innermost open span
    std::uint32_t dropped_ = 0;
};

// RAII span. **A null context makes both ends no-ops**, which is the whole
// disabled path: two predicted-not-taken branches and, crucially, **no
// clock read** - a `clock_gettime` even through the vDSO is ~20 ns and
// would show up in a tuple-scan loop (§6).
class SpanScope {
public:
    SpanScope(TraceContext* ctx, Layer layer) noexcept
        : ctx_(ctx), index_(ctx == nullptr ? Span::kNoParent : ctx->Open(layer)) {}

    ~SpanScope() {
        if (ctx_ == nullptr || index_ == Span::kNoParent) return;
        ctx_->Close(index_, ctx_->Now(), detail_);
    }

    SpanScope(const SpanScope&) = delete;
    SpanScope& operator=(const SpanScope&) = delete;

    void set_detail(std::uint64_t d) noexcept { detail_ = d; }

private:
    TraceContext* ctx_ = nullptr;
    std::uint16_t index_ = Span::kNoParent;
    std::uint64_t detail_ = 0;
};

// A core-local ring of completed traces, fixed capacity, **drop-oldest**.
//
// Advisory exactly as Waystone is: losing a trace costs insight and never
// correctness, so it must never apply backpressure to the request path.
// §7 states that this is the opposite of the WAL's rule and is worth being
// explicit about - so it is stated here too, at the type that does it.
class TraceSink {
public:
    static constexpr std::size_t kDefaultCapacity = 256;

    explicit TraceSink(std::size_t capacity = kDefaultCapacity) : capacity_(capacity) {}

    void Add(TraceContext trace) {
        if (capacity_ == 0) return;
        while (traces_.size() >= capacity_) traces_.pop_front();
        traces_.push_back(std::move(trace));
    }

    const std::deque<TraceContext>& traces() const noexcept { return traces_; }
    const TraceContext* Find(std::uint64_t id) const noexcept;
    std::uint64_t NextId() noexcept { return ++next_id_; }
    void Clear() noexcept { traces_.clear(); }

private:
    std::size_t capacity_;
    std::deque<TraceContext> traces_;
    std::uint64_t next_id_ = 0;
};

// The span tree, indented, as `SHOW TRACE <id>` renders it. **Self-time is
// separated from total**, which is §7's point: self-time finds the culprit,
// total only says which subtree to open next.
std::string RenderTrace(const TraceContext& trace);

// One line per trace: id, command, total. `SHOW TRACES`.
std::string RenderTraceList(const TraceSink& sink);

}  // namespace kds::stats
