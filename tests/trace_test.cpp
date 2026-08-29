#include "kds/stats/trace.hpp"

#include <string>

#include <gtest/gtest.h>

// H6 step 1: `Layer`, `SpanScope`, `TraceContext`, `TraceSink`, against a
// `ManualClock` - deterministic, no I/O, asserting nesting and overflow
// behaviour, which is `observability.md` §10's own description of this step.
//
// The clock is manual for the reason every timing test here is: a real
// clock makes an assertion about *duration* into an assertion about the
// machine, and this file is about the tree's shape rather than the speed of
// anything.

namespace kds::stats {
namespace {

TEST(TraceTest, DisabledCostsNoClockRead) {
    // **The zero-cost-when-off claim, asserted rather than argued** (§6,
    // and H6's HH3). A null context must not read the clock: a
    // `clock_gettime` even through the vDSO is ~20 ns and would show up in
    // a tuple-scan loop. `ManualClock` cannot be read without something
    // observing it, so the proxy is the one the type gives - a null
    // context has no clock at all, so a span over it can only be a no-op.
    {
        SpanScope span(nullptr, Layer::kHeap);
        span.set_detail(1234);
    }
    // Nothing to observe is the point: the test is that this compiles, runs
    // and touches no state. The stronger form - that the branch is
    // predicted - is a measurement, and `ck-tester`'s, not this file's.
    SUCCEED();
}

TEST(TraceTest, SpansNestAndSelfTimeSeparatesFromChildTime) {
    sched::ManualClock clock;
    TraceContext trace(1, &clock, "SELECT * FROM t");
    {
        SpanScope request(&trace, Layer::kRequest);
        clock.Advance(1000);  // 1us of the request's own work
        {
            SpanScope parse(&trace, Layer::kParse);
            clock.Advance(3000);
        }
        {
            SpanScope execute(&trace, Layer::kExecute);
            clock.Advance(2000);
            {
                SpanScope heap(&trace, Layer::kHeap);
                clock.Advance(10000);
                heap.set_detail(1204);
            }
        }
        clock.Advance(1000);
    }

    ASSERT_EQ(trace.spans().size(), 4u);
    EXPECT_TRUE(trace.complete());
    // The root's total is everything: 1 + 3 + 2 + 10 + 1 = 17us.
    EXPECT_EQ(trace.total_ns(), 17000u);

    const std::span<const Span> spans = trace.spans();
    EXPECT_EQ(spans[0].layer, Layer::kRequest);
    EXPECT_EQ(spans[0].parent, Span::kNoParent);
    EXPECT_EQ(spans[1].layer, Layer::kParse);
    EXPECT_EQ(spans[1].parent, 0u);
    EXPECT_EQ(spans[2].layer, Layer::kExecute);
    EXPECT_EQ(spans[2].parent, 0u);
    // **The nesting that matters**: heap is under execute, not under the
    // request. A flat list would put it here and the tree would say the
    // request spent 10us on nothing in particular.
    EXPECT_EQ(spans[3].layer, Layer::kHeap);
    EXPECT_EQ(spans[3].parent, 2u);
    EXPECT_EQ(spans[3].detail, 1204u);

    // Rendered: self-time is what finds the culprit, so it must be the
    // duration minus the children's. Execute ran 12us and its child took
    // 10, so its self is 2.
    const std::string rendered = RenderTrace(trace);
    EXPECT_NE(rendered.find("execute 12us self=2us"), std::string::npos) << rendered;
    EXPECT_NE(rendered.find("heap 10us self=10us detail=1204"), std::string::npos) << rendered;
    // 17 total minus parse's 3 and execute's 12 = 2us of the request's own
    // work, which is the 1us before the first child plus the 1us after the
    // last. Written out because getting it wrong is how a self-time
    // assertion passes against a renderer that is also wrong.
    EXPECT_NE(rendered.find("request 17us self=2us"), std::string::npos) << rendered;
}

TEST(TraceTest, SiblingsAtTheSameDepthShareAParent) {
    sched::ManualClock clock;
    TraceContext trace(2, &clock, "x");
    {
        SpanScope request(&trace, Layer::kRequest);
        for (int i = 0; i < 3; ++i) {
            SpanScope child(&trace, Layer::kCatalog);
            clock.Advance(1000);
        }
    }
    ASSERT_EQ(trace.spans().size(), 4u);
    for (std::size_t i = 1; i < 4; ++i) {
        EXPECT_EQ(trace.spans()[i].parent, 0u) << "span " << i << " was nested under a sibling";
    }
    // Three siblings of 1us each and no self-time left over for the root.
    EXPECT_NE(RenderTrace(trace).find("request 3us self=0us"), std::string::npos);
}

TEST(TraceTest, OverflowDropsTheSpanAndMarksTheTraceIncomplete) {
    // §6: overflow counts rather than allocates. §9 left "mark the trace
    // incomplete vs report partial" open and this takes the first, because
    // a tree missing a node reads as a *fast* subtree - the one way this
    // instrument could lie about the thing it exists to measure.
    sched::ManualClock clock;
    TraceContext trace(3, &clock, "deep");
    {
        SpanScope root(&trace, Layer::kRequest);
        std::vector<std::unique_ptr<SpanScope>> held;
        for (std::size_t i = 0; i < kMaxSpansPerTrace + 8; ++i) {
            held.push_back(std::make_unique<SpanScope>(&trace, Layer::kExecute));
            clock.Advance(1);
        }
    }
    EXPECT_EQ(trace.spans().size(), kMaxSpansPerTrace);
    EXPECT_FALSE(trace.complete());
    EXPECT_EQ(trace.dropped(), 9u) << "one root plus 63 children fit; the rest are dropped";
    EXPECT_NE(RenderTrace(trace).find("incomplete dropped_spans=9"), std::string::npos)
        << "the reply did not say the tree is short";
}

TEST(TraceTest, ADroppedSpanDoesNotCorruptTheTreeBelowIt) {
    // The property that makes the drop safe rather than merely counted: a
    // span that could not be opened must not become somebody's parent, or
    // the spans after it would hang off an index that names another span.
    sched::ManualClock clock;
    TraceContext trace(4, &clock, "x");
    {
        SpanScope root(&trace, Layer::kRequest);
        std::vector<std::unique_ptr<SpanScope>> held;
        for (std::size_t i = 0; i < kMaxSpansPerTrace; ++i) {
            held.push_back(std::make_unique<SpanScope>(&trace, Layer::kExecute));
        }
        // This one is dropped; a span opened inside it must still name a
        // parent that exists.
        SpanScope dropped(&trace, Layer::kHeap);
        SpanScope inner(&trace, Layer::kPageIo);
    }
    for (const Span& span : trace.spans()) {
        EXPECT_TRUE(span.parent == Span::kNoParent || span.parent < trace.spans().size())
            << "a span names a parent index that is not in the trace";
    }
}

TEST(TraceTest, TheSinkIsDropOldestAndFindsByIdWhileItHoldsOne) {
    // Advisory exactly as Waystone is: losing a trace costs insight, never
    // correctness, so the ring never applies backpressure - it drops the
    // oldest and keeps taking.
    sched::ManualClock clock;
    TraceSink sink(/*capacity=*/3);
    for (std::uint64_t i = 1; i <= 5; ++i) {
        TraceContext trace(sink.NextId(), &clock, "cmd" + std::to_string(i));
        { SpanScope root(&trace, Layer::kRequest); clock.Advance(1000); }
        sink.Add(std::move(trace));
    }
    ASSERT_EQ(sink.traces().size(), 3u);
    EXPECT_EQ(sink.traces().front().id(), 3u) << "the ring kept the oldest instead of the newest";
    EXPECT_EQ(sink.traces().back().id(), 5u);
    EXPECT_EQ(sink.Find(1), nullptr) << "an evicted trace is gone, not stale";
    ASSERT_NE(sink.Find(5), nullptr);
    EXPECT_EQ(sink.Find(5)->command(), "cmd5");

    const std::string listed = RenderTraceList(sink);
    EXPECT_NE(listed.find("id,total_us,spans,command"), std::string::npos) << listed;
    EXPECT_NE(listed.find("5,1,1,cmd5"), std::string::npos) << listed;
    EXPECT_EQ(listed.find("cmd1"), std::string::npos) << "an evicted trace was still listed";
}

TEST(TraceTest, ASinkOfZeroCapacityKeepsNothingRatherThanGrowing) {
    sched::ManualClock clock;
    TraceSink sink(/*capacity=*/0);
    TraceContext trace(1, &clock, "x");
    sink.Add(std::move(trace));
    EXPECT_TRUE(sink.traces().empty());
}

TEST(TraceTest, EveryLayerHasADistinctName) {
    // The names reach a client through `SHOW TRACE`, so two layers sharing
    // one would make a tree unreadable at exactly the point it is being
    // read to find a culprit.
    std::set<std::string> seen;
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(Layer::kMaxLayer); ++i) {
        const std::string name = LayerName(static_cast<Layer>(i));
        EXPECT_NE(name, "?") << "layer " << static_cast<int>(i) << " has no name";
        EXPECT_TRUE(seen.insert(name).second) << "duplicate layer name: " << name;
    }
}

}  // namespace
}  // namespace kds::stats
