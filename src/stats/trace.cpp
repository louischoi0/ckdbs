#include "kds/stats/trace.hpp"

#include <sstream>
#include <vector>

namespace kds::stats {

const char* LayerName(Layer layer) noexcept {
    switch (layer) {
        case Layer::kRequest: return "request";
        case Layer::kParse: return "parse";
        case Layer::kCatalog: return "catalog";
        case Layer::kPlan: return "plan";
        case Layer::kExecute: return "execute";
        case Layer::kHeap: return "heap";
        case Layer::kBufferPool: return "buffer_pool";
        case Layer::kPageIo: return "page_io";
        case Layer::kWalAppend: return "wal_append";
        case Layer::kWalFlush: return "wal_flush";
        case Layer::kCheckpoint: return "checkpoint";
        case Layer::kReply: return "reply";
        case Layer::kMaxLayer: break;
    }
    return "?";
}

std::uint16_t TraceContext::Open(Layer layer) noexcept {
    if (count_ >= kMaxSpansPerTrace) {
        // **Counted, never allocated** (§6). The trace is incomplete from
        // here on and `complete()` says so, which is what keeps a missing
        // node from reading as a fast subtree.
        ++dropped_;
        return Span::kNoParent;
    }
    const std::uint16_t index = count_++;
    Span& span = spans_[index];
    span.layer = layer;
    span.parent = open_;
    span.start_ns = Now();
    span.end_ns = span.start_ns;
    span.detail = 0;
    open_ = index;
    return index;
}

void TraceContext::Close(std::uint16_t index, sched::MonoTimeNs end_ns,
                         std::uint64_t detail) noexcept {
    if (index >= count_) return;
    spans_[index].end_ns = end_ns;
    spans_[index].detail = detail;
    // Pop to this span's parent. `SpanScope` is RAII and non-copyable, so
    // closes are strictly nested and this is the whole of the bookkeeping;
    // a close out of order would leave `open_` naming a closed span, which
    // is why the type does not let one happen.
    open_ = spans_[index].parent;
}

const TraceContext* TraceSink::Find(std::uint64_t id) const noexcept {
    for (const TraceContext& trace : traces_) {
        if (trace.id() == id) return &trace;
    }
    return nullptr;
}

namespace {

// Child time of `index`, so self-time is its duration minus this. Direct
// children only: a grandchild's time is already inside its parent's.
sched::MonoTimeNs ChildNs(std::span<const Span> spans, std::uint16_t index) {
    sched::MonoTimeNs total = 0;
    for (std::size_t i = 0; i < spans.size(); ++i) {
        if (spans[i].parent == index) total += spans[i].duration_ns();
    }
    return total;
}

void RenderSubtree(std::ostringstream& os, std::span<const Span> spans, std::uint16_t index,
                   int depth) {
    const Span& span = spans[index];
    const sched::MonoTimeNs child = ChildNs(spans, index);
    const sched::MonoTimeNs self = span.duration_ns() >= child ? span.duration_ns() - child : 0;
    os << "\\n";
    for (int i = 0; i <= depth; ++i) os << "  ";
    os << LayerName(span.layer) << " " << span.duration_ns() / 1000 << "us self="
       << self / 1000 << "us";
    if (span.detail != 0) os << " detail=" << span.detail;
    for (std::size_t i = 0; i < spans.size(); ++i) {
        if (spans[i].parent == index) {
            RenderSubtree(os, spans, static_cast<std::uint16_t>(i), depth + 1);
        }
    }
}

}  // namespace

std::string RenderTrace(const TraceContext& trace) {
    std::ostringstream os;
    os << "trace=" << trace.id() << " cmd=\"" << trace.command() << "\" total="
       << trace.total_ns() / 1000 << "us";
    if (!trace.complete()) {
        // Said in the reply, not only in a field: a reader who does not
        // know the tree is short will read the missing time as fast.
        os << " incomplete dropped_spans=" << trace.dropped();
    }
    const std::span<const Span> spans = trace.spans();
    for (std::size_t i = 0; i < spans.size(); ++i) {
        if (spans[i].parent == Span::kNoParent) {
            RenderSubtree(os, spans, static_cast<std::uint16_t>(i), 0);
        }
    }
    return os.str();
}

std::string RenderTraceList(const TraceSink& sink) {
    std::ostringstream os;
    os << "id,total_us,spans,command";
    for (const TraceContext& trace : sink.traces()) {
        os << "\\n" << trace.id() << "," << trace.total_ns() / 1000 << ","
           << trace.spans().size() << (trace.complete() ? "" : "+dropped") << ","
           << trace.command();
    }
    return os.str();
}

}  // namespace kds::stats
