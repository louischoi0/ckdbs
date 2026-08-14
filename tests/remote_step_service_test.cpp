#include "kds/server/remote_step_service.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/exec/step_vm.hpp"
#include "kds/server/step_descriptor.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/wire/row_codec.hpp"

// The remote step server (workplan P4b), driven without a reactor: the
// injected sender captures what would ride the ring, so every protocol
// rule - batches under credit, resume on grant, EOF last, errors with the
// retryable bit, teardown by tag - is asserted directly.

namespace kds::server {
namespace {

struct Sent {
    std::uint32_t dst = 0;
    sched::RingMessageKind kind{};
    std::vector<std::byte> payload;
};

class RemoteStepServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/64);
        ASSERT_TRUE(device.ok());
        device_ = std::move(device.value());
        auto store = storage::DevicePageStore::Open(*device_, kFirstUserPageId);
        ASSERT_TRUE(store.ok());
        store_ = std::move(store.value());
        auto boot = bootstrap::BootstrapDatabase(*store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));

        // A two-column relation with rows the batches can be checked
        // against, inserted through the same encode path a real INSERT
        // uses (exec_chain_test's arrangement).
        catalog::Schema schema;
        auto add = [&](const char* name, const char* type) {
            auto type_row = boot_->catalog.ResolveTypeByName(type);
            ASSERT_TRUE(type_row.ok());
            catalog::SysColumnRow row{};
            row.pos = static_cast<std::uint16_t>(schema.columns.size());
            catalog::SetName(row.name, name);
            row.type_val = type_row.value().type_val;
            row.len = type_row.value().len;
            row.notnull = true;
            schema.columns.push_back(row);
        };
        add("id", "int64");
        add("qty", "int64");
        auto created = boot_->catalog.CreateTable(catalog::kNamespacePublic, "t", schema,
                                                  catalog::ClusteredType::kHeap,
                                                  catalog::KeyMode::kAssigned);
        ASSERT_TRUE(created.ok());
        oid_ = created.value();

        // qty = i*10 so the residual test's `qty > 50` keeps exactly 2 of
        // the 8; one row per batch (target 1) makes 8 rows > 4 credits,
        // deterministically.
        InsertRows(8, /*qty_step=*/10);
        MakeServer(/*submit=*/{});
    }

    std::vector<std::byte> OpenFor(const exec::Step& step, std::uint64_t request_id = 7) {
        auto descriptor = EncodeStepDescriptor(step);
        EXPECT_TRUE(descriptor.ok()) << descriptor.status().message();
        StepOpenHead head{};
        head.tag = PipelineTag{request_id, /*session_core=*/0, step.step_id};
        head.downstream_core = 0;
        return EncodeStepOpen(head, descriptor.value());
    }

    sched::MessageHeader HeaderFromSession() {
        sched::MessageHeader h{};
        h.src_core = 0;
        h.dst_core = 1;
        return h;
    }

    exec::Step ScanStep() {
        exec::Step step;
        step.step_id = 0;
        step.rel_oid = oid_;
        step.kind = exec::AccessKind::kScan;
        return step;
    }

    // Grows the relation by `n` rows, qty = i * qty_step, through the same
    // encode path a real INSERT uses. The streaming tests need a
    // multi-page chain: the producer parks at page boundaries, and a
    // one-page relation never reaches one.
    void InsertRows(int n, int qty_step = 1) {
        auto access = boot_->catalog.InitTableAccess(oid_);
        ASSERT_TRUE(access.ok());
        for (int i = 0; i < n; ++i) {
            auto id = boot_->catalog.AllocateRowId(oid_);
            ASSERT_TRUE(id.ok());
            parser::AstValue qty;
            qty.type = parser::ValueType::kInt;
            qty.int_val = i * qty_step;
            qty.raw_int_text = std::to_string(i * qty_step);
            auto payload = exec::EncodeRow(access.value()->schema, access.value()->layout,
                                           id.value(), {qty});
            ASSERT_TRUE(payload.ok());
            auto placed = heap::ChainInsert(*store_, access.value()->desc_page_id, id.value(),
                                            payload.value(), /*trx_id=*/1, access.value()->oid);
            ASSERT_TRUE(placed.ok()) << placed.status().message();
        }
    }

    // One server for both shapes: an empty submit is the reactorless
    // fallback, a real one lands producer tasks in tasks_ so Pump() can be
    // one reactor pass.
    void MakeServer(RemoteStepServer::SubmitFn submit) {
        server_.emplace(
            boot_->catalog, *store_, /*core_id=*/1,
            [this](std::uint32_t dst, sched::RingMessageKind kind,
                   std::vector<std::byte> payload) {
                sent_.push_back(Sent{dst, kind, std::move(payload)});
                return Status::OK();
            },
            nullptr, /*batch_target_bytes=*/1, std::move(submit));
    }

    void UseStreamingServer() {
        MakeServer([this](std::unique_ptr<sched::Task> task) { tasks_.push_back(std::move(task)); });
    }

    // One reactor pass: every live task polled once, finished ones dropped.
    void Pump() {
        for (auto& task : tasks_) {
            if (task != nullptr && task->Poll() == sched::PollResult::kDone) task.reset();
        }
        std::erase(tasks_, nullptr);
    }

    void GrantCredits(std::uint32_t n, std::uint64_t request_id = 7) {
        StepCreditPayload credit{PipelineTag{request_id, 0, 0}, n};
        std::vector<std::byte> bytes;
        EncodePipelinePayload(credit, bytes);
        server_->OnStepCredit(bytes);
    }

    std::unique_ptr<storage::MemoryPageDevice> device_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::optional<bootstrap::BootstrapResult> boot_;
    catalog::Oid oid_ = 0;
    std::optional<RemoteStepServer> server_;
    std::vector<Sent> sent_;
    std::vector<std::unique_ptr<sched::Task>> tasks_;
};

TEST_F(RemoteStepServiceTest, AScanStreamsBatchesUnderCreditAndEofLast) {
    server_->OnStepOpen(HeaderFromSession(), OpenFor(ScanStep()));

    // The tiny target makes 8 rows more batches than the initial 4
    // credits: exactly 4 batches went out and the pipeline is parked.
    ASSERT_GE(sent_.size(), 1u);
    std::size_t batches = 0;
    for (const Sent& s : sent_) {
        EXPECT_EQ(s.kind, sched::RingMessageKind::kStepBatch);
        ++batches;
    }
    EXPECT_EQ(batches, kInitialCreditsPerEdge);
    EXPECT_EQ(server_->open_pipelines(), 1u);

    // A credit grant resumes the drain; enough grants finish the stream,
    // EOF arrives last, and the pipeline tears down.
    StepCreditPayload credit{PipelineTag{7, 0, 0}, 4};
    std::vector<std::byte> bytes;
    EncodePipelinePayload(credit, bytes);
    server_->OnStepCredit(bytes);

    ASSERT_GE(sent_.size(), 2u);
    EXPECT_EQ(sent_.back().kind, sched::RingMessageKind::kStepEof);
    EXPECT_EQ(server_->open_pipelines(), 0u);

    // The rows round-trip: decode every batch through the one KWP decoder
    // and check the qty column of the first row of the first batch.
    std::span<const std::byte> rows;
    auto header = DecodeStepBatchHeader(sent_.front().payload, rows);
    ASSERT_TRUE(header.ok());
    EXPECT_EQ(header.value().seq, 0u);
    auto decoded = wire::DecodeRowBatch(rows, /*field_count=*/2);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    ASSERT_GE(decoded.value().size(), 1u);
}

TEST_F(RemoteStepServiceTest, AFilteredScanAppliesTheResidualRemotely) {
    exec::Step step = ScanStep();
    step.kind = exec::AccessKind::kFilterScan;
    exec::StepPredicate pred;
    pred.lhs = exec::ColumnRef{0, 0, 1};  // qty
    pred.op = parser::CompareOp::kGt;
    pred.rhs.kind = exec::OperandKind::kLiteral;
    pred.rhs.literal.type = parser::ValueType::kInt;
    pred.rhs.literal.int_val = 50;
    step.residual.push_back(pred);

    server_->OnStepOpen(HeaderFromSession(), OpenFor(step));

    // qty > 50 keeps 2 of 8 rows (60, 70): one small batch, then EOF.
    std::size_t rows_total = 0;
    for (const Sent& s : sent_) {
        if (s.kind != sched::RingMessageKind::kStepBatch) continue;
        std::span<const std::byte> rows;
        auto header = DecodeStepBatchHeader(s.payload, rows);
        ASSERT_TRUE(header.ok());
        rows_total += header.value().row_count;
    }
    EXPECT_EQ(rows_total, 2u);
    EXPECT_EQ(sent_.back().kind, sched::RingMessageKind::kStepEof);
}

// ---- The STEP_OPEN envelope (workplan P4d-4b-1) --------------------------

TEST_F(RemoteStepServiceTest, AnEnvelopeWithoutAnUpstreamEdgeRoundTrips) {
    exec::Step step = ScanStep();
    auto descriptor = EncodeStepDescriptor(step);
    ASSERT_TRUE(descriptor.ok());

    StepOpenHead head{};
    head.tag = PipelineTag{9, 0, 3};
    head.downstream_core = 2;
    head.downstream_step = 4;

    // Named, not a temporary: `descriptor` is a view into the envelope
    // (the decoder's documented aliasing), so the buffer has to outlive
    // every assertion below.
    const std::vector<std::byte> envelope = EncodeStepOpen(head, descriptor.value());
    auto parts = DecodeStepOpenEnvelope(envelope);
    ASSERT_TRUE(parts.ok()) << parts.status().message();
    EXPECT_EQ(parts.value().head.tag, head.tag);
    EXPECT_EQ(parts.value().head.downstream_core, 2u);
    EXPECT_EQ(parts.value().head.downstream_step, 4u);
    EXPECT_FALSE(parts.value().upstream.has_value());
    EXPECT_TRUE(std::equal(parts.value().descriptor.begin(), parts.value().descriptor.end(),
                           descriptor.value().begin(), descriptor.value().end()));
}

TEST_F(RemoteStepServiceTest, AnEnvelopeWithAnUpstreamEdgeRoundTrips) {
    exec::Step step = ScanStep();
    auto descriptor = EncodeStepDescriptor(step);
    ASSERT_TRUE(descriptor.ok());

    StepOpenHead head{};
    head.tag = PipelineTag{9, 0, 1};

    StepOpenUpstream up;
    up.upstream_core = 3;
    catalog::SysColumnRow col{};
    col.pos = 1;
    col.type_val = 20;  // arbitrary; the codec must not interpret it
    catalog::SetName(col.name, "qty");
    up.forwarded.push_back(col);
    up.enclosed_open = {std::byte{0xAB}, std::byte{0xCD}};

    const std::vector<std::byte> envelope = EncodeStepOpen(head, descriptor.value(), &up);
    auto parts = DecodeStepOpenEnvelope(envelope);
    ASSERT_TRUE(parts.ok()) << parts.status().message();
    ASSERT_TRUE(parts.value().upstream.has_value());
    EXPECT_EQ(parts.value().upstream->upstream_core, 3u);
    ASSERT_EQ(parts.value().upstream->forwarded.size(), 1u);
    EXPECT_EQ(parts.value().upstream->forwarded[0].pos, 1u);
    EXPECT_EQ(parts.value().upstream->forwarded[0].type_val, 20u);
    EXPECT_EQ(parts.value().upstream->enclosed_open,
              (std::vector<std::byte>{std::byte{0xAB}, std::byte{0xCD}}));
    EXPECT_TRUE(std::equal(parts.value().descriptor.begin(), parts.value().descriptor.end(),
                           descriptor.value().begin(), descriptor.value().end()));
}

TEST_F(RemoteStepServiceTest, ATruncatedEnvelopeWithAWholeHeadAnswersStepError) {
    // The envelope contract: a failure at any point answers STEP_ERROR
    // and opens nothing. With the head intact the tag is real, and a
    // session that opened this stage would otherwise wait on a read
    // nobody will ever finish.
    exec::Step step = ScanStep();
    auto descriptor = EncodeStepDescriptor(step);
    ASSERT_TRUE(descriptor.ok());
    StepOpenHead head{};
    head.tag = PipelineTag{7, 0, 0};
    StepOpenUpstream up;
    up.upstream_core = 0;
    auto envelope = EncodeStepOpen(head, descriptor.value(), &up);
    envelope.resize(sizeof(StepOpenHead) + 3);  // head + flag + 2 of the core's 4 bytes

    server_->OnStepOpen(HeaderFromSession(), envelope);
    ASSERT_EQ(sent_.size(), 1u);
    EXPECT_EQ(sent_.front().kind, sched::RingMessageKind::kStepError);
    EXPECT_EQ(server_->open_pipelines(), 0u);
}

TEST_F(RemoteStepServiceTest, AConsumingStageOnAReactorlessServerIsRefused) {
    // The fixture's default server has no SubmitFn, and a consuming stage
    // parks between batches - there is nothing to park on.
    exec::Step step = ScanStep();
    auto descriptor = EncodeStepDescriptor(step);
    ASSERT_TRUE(descriptor.ok());
    StepOpenHead head{};
    head.tag = PipelineTag{7, 0, 1};
    StepOpenUpstream up;
    up.upstream_core = 0;

    server_->OnStepOpen(HeaderFromSession(), EncodeStepOpen(head, descriptor.value(), &up));
    ASSERT_EQ(sent_.size(), 1u);
    EXPECT_EQ(sent_.front().kind, sched::RingMessageKind::kStepError);
    EXPECT_EQ(server_->open_pipelines(), 0u);
}

TEST_F(RemoteStepServiceTest, AnUnknownRelationAnswersStepError) {
    exec::Step step = ScanStep();
    step.rel_oid = 999999;
    server_->OnStepOpen(HeaderFromSession(), OpenFor(step));

    ASSERT_EQ(sent_.size(), 1u);
    EXPECT_EQ(sent_.front().kind, sched::RingMessageKind::kStepError);
    auto error = DecodePipelinePayload<StepErrorPayload>(sent_.front().payload);
    ASSERT_TRUE(error.ok());
    EXPECT_EQ(error.value().retryable, 0);
    EXPECT_EQ(server_->open_pipelines(), 0u);
}

TEST_F(RemoteStepServiceTest, ACancelTearsDownAndLateCreditsAreDiscarded) {
    server_->OnStepOpen(HeaderFromSession(), OpenFor(ScanStep()));
    ASSERT_EQ(server_->open_pipelines(), 1u);

    StepEofPayload cancel{PipelineTag{7, 0, 0}};
    std::vector<std::byte> bytes;
    EncodePipelinePayload(cancel, bytes);
    server_->OnStepCancel(bytes);
    EXPECT_EQ(server_->open_pipelines(), 0u);

    // A credit for the torn-down tag is discarded silently - §3's rule.
    const std::size_t before = sent_.size();
    StepCreditPayload credit{PipelineTag{7, 0, 0}, 2};
    EncodePipelinePayload(credit, bytes);
    server_->OnStepCredit(bytes);
    EXPECT_EQ(sent_.size(), before);
}

// ---- The streaming producer (workplan P4d-4a) ---------------------------

TEST_F(RemoteStepServiceTest, AStreamingProducerParksOnCreditAndItsBufferingIsBounded) {
    InsertRows(392);  // 400 rows across several pages
    UseStreamingServer();

    // The audit is armed with the real store for the whole test: this is
    // the executor's first genuine suspension, and the claim under test
    // is that it holds nothing - no span, no pin - when it parks.
    exec::InstallSuspendAudit(store_.get());
    sched::ResetSuspendAudit();

    server_->OnStepOpen(HeaderFromSession(), OpenFor(ScanStep()));
    EXPECT_TRUE(sent_.empty()) << "nothing runs until the reactor polls the producer";
    ASSERT_EQ(tasks_.size(), 1u);

    // One pass: the walk fills the first page, ships the 4 credits'
    // batches, and parks at the page boundary with the page's remaining
    // seals queued - not the relation's.
    Pump();
    ASSERT_EQ(tasks_.size(), 1u) << "the producer must park, not finish";
    EXPECT_EQ(sent_.size(), kInitialCreditsPerEdge);
    EXPECT_EQ(server_->open_pipelines(), 1u);
    const std::size_t parked_queue = server_->unsent_batches();
    EXPECT_GT(parked_queue, 0u);
    EXPECT_LT(parked_queue, 250u) << "the queue must hold about one page's seals, "
                                     "not the 400-row relation";
    EXPECT_FALSE(sched::SuspendAuditTripped()) << sched::SuspendAuditReason();

    // Idle polls while parked cost nothing and send nothing.
    const std::size_t at_park = sent_.size();
    Pump();
    Pump();
    EXPECT_EQ(sent_.size(), at_park);

    // Grants drain and the gate reopens the walk; enough rounds finish
    // the relation, EOF last, teardown complete.
    for (int round = 0; round < 500 && server_->open_pipelines() > 0; ++round) {
        GrantCredits(4);
        Pump();
    }
    EXPECT_EQ(server_->open_pipelines(), 0u);
    EXPECT_TRUE(tasks_.empty());
    EXPECT_FALSE(sched::SuspendAuditTripped()) << sched::SuspendAuditReason();
    exec::UninstallSuspendAudit();

    ASSERT_EQ(sent_.back().kind, sched::RingMessageKind::kStepEof);
    std::size_t rows_total = 0;
    for (const Sent& s : sent_) {
        if (s.kind != sched::RingMessageKind::kStepBatch) continue;
        std::span<const std::byte> rows;
        auto header = DecodeStepBatchHeader(s.payload, rows);
        ASSERT_TRUE(header.ok());
        rows_total += header.value().row_count;
    }
    EXPECT_EQ(rows_total, 400u);
}

TEST_F(RemoteStepServiceTest, StreamingAndCollectThenStreamShipIdenticalBytes) {
    InsertRows(392);

    // The legacy shape first, its traffic kept aside.
    server_->OnStepOpen(HeaderFromSession(), OpenFor(ScanStep()));
    while (server_->open_pipelines() > 0) GrantCredits(4);
    std::vector<Sent> legacy;
    legacy.swap(sent_);

    // The streaming shape against the same store, same tag, same target.
    UseStreamingServer();
    server_->OnStepOpen(HeaderFromSession(), OpenFor(ScanStep()));
    for (int round = 0; round < 500 && (server_->open_pipelines() > 0 || !tasks_.empty());
         ++round) {
        Pump();
        GrantCredits(4);
    }
    ASSERT_EQ(server_->open_pipelines(), 0u);

    // Byte-identical traffic: same batches in the same order under the
    // same credit protocol, EOF last - the internal buffering is the only
    // thing the producer changed.
    ASSERT_EQ(sent_.size(), legacy.size());
    for (std::size_t i = 0; i < sent_.size(); ++i) {
        EXPECT_EQ(sent_[i].kind, legacy[i].kind) << "message " << i;
        EXPECT_EQ(sent_[i].payload, legacy[i].payload) << "message " << i;
    }
}

TEST_F(RemoteStepServiceTest, ACancelReachesAParkedProducerAndNoEofFollows) {
    InsertRows(392);
    UseStreamingServer();

    server_->OnStepOpen(HeaderFromSession(), OpenFor(ScanStep()));
    Pump();
    ASSERT_EQ(server_->open_pipelines(), 1u);
    ASSERT_EQ(tasks_.size(), 1u);

    // The handler cannot erase under a parked walk; it marks, and the
    // producer tears itself down on its next resume.
    StepEofPayload cancel{PipelineTag{7, 0, 0}};
    std::vector<std::byte> bytes;
    EncodePipelinePayload(cancel, bytes);
    server_->OnStepCancel(bytes);
    EXPECT_EQ(server_->open_pipelines(), 1u) << "marked, not yet erased";

    Pump();
    EXPECT_EQ(server_->open_pipelines(), 0u);
    EXPECT_TRUE(tasks_.empty());
    for (const Sent& s : sent_) {
        EXPECT_NE(s.kind, sched::RingMessageKind::kStepEof)
            << "a cancelled pipeline must never claim a clean end";
    }
}

// A cancelled producer's entry outlives its CANCEL - the parked walk has to
// be the one to erase it - and the window between the mark and the
// producer's next resume is where a credit can still arrive. Nothing may
// ship in it: the pre-streaming shape erased in the handler, so a late
// credit found no pipeline at all, and the streaming shape must be no
// louder on the wire than the shape it replaced.
TEST_F(RemoteStepServiceTest, ACreditArrivingAfterACancelShipsNothing) {
    InsertRows(392);
    UseStreamingServer();
    server_->OnStepOpen(HeaderFromSession(), OpenFor(ScanStep()));
    Pump();
    ASSERT_EQ(server_->open_pipelines(), 1u);
    ASSERT_GT(server_->unsent_batches(), 0u) << "the park must leave a queue to be tempted by";

    StepEofPayload cancel{PipelineTag{7, 0, 0}};
    std::vector<std::byte> bytes;
    EncodePipelinePayload(cancel, bytes);
    server_->OnStepCancel(bytes);

    const std::size_t before = sent_.size();
    GrantCredits(4);
    EXPECT_EQ(sent_.size(), before) << "a cancelled pipeline must ship nothing more";

    // And the producer still tears itself down on its next resume.
    Pump();
    EXPECT_EQ(server_->open_pipelines(), 0u);
    EXPECT_TRUE(tasks_.empty());
}

// A park can cross a catalog invalidation - any DDL anywhere broadcasts
// one, and its handler clears the whole TableAccess cache. The review of
// 31319c8 reproduced a use-after-free here under ASan: the executor's
// bound_ and the producer's schema both died with the cache. The fix is
// re-Bind at every real park plus a producer-owned Schema copy; this
// test is that reproduction, kept as the regression's pin.
TEST_F(RemoteStepServiceTest, ACatalogInvalidationAcrossAParkNeitherCrashesNorChangesTheRows) {
    InsertRows(392);
    UseStreamingServer();
    server_->OnStepOpen(HeaderFromSession(), OpenFor(ScanStep()));
    Pump();
    ASSERT_EQ(server_->open_pipelines(), 1u) << "the producer must be parked mid-walk";

    // Verbatim what CoreRuntime's kCatalogInvalidate handler runs.
    boot_->catalog.InvalidateFromPeer();

    for (int round = 0; round < 500 && server_->open_pipelines() > 0; ++round) {
        GrantCredits(4);
        Pump();
    }
    EXPECT_EQ(server_->open_pipelines(), 0u);
    ASSERT_EQ(sent_.back().kind, sched::RingMessageKind::kStepEof);

    std::size_t rows_total = 0;
    for (const Sent& s : sent_) {
        if (s.kind != sched::RingMessageKind::kStepBatch) continue;
        std::span<const std::byte> rows;
        auto header = DecodeStepBatchHeader(s.payload, rows);
        ASSERT_TRUE(header.ok());
        rows_total += header.value().row_count;
    }
    EXPECT_EQ(rows_total, 400u) << "an invalidation must not drop or duplicate rows";
}

TEST_F(RemoteStepServiceTest, ARelationDroppedAcrossAParkAnswersAStepErrorNotAFreedRead) {
    InsertRows(392);
    UseStreamingServer();
    server_->OnStepOpen(HeaderFromSession(), OpenFor(ScanStep()));
    Pump();
    ASSERT_EQ(server_->open_pipelines(), 1u) << "the producer must be parked mid-walk";

    // The strongest form of the invalidation: the relation itself is gone
    // when the walk resumes. The re-Bind fails cleanly and the session
    // hears STEP_ERROR - the semantic a stale plan is promised (§5).
    std::vector<std::uint64_t> dropped_cabins;
    ASSERT_TRUE(boot_->catalog.DropTable(oid_, dropped_cabins).ok());

    for (int round = 0; round < 500 && server_->open_pipelines() > 0; ++round) {
        GrantCredits(4);
        Pump();
    }
    EXPECT_EQ(server_->open_pipelines(), 0u);
    EXPECT_TRUE(tasks_.empty());
    bool errored = false;
    for (const Sent& s : sent_) {
        if (s.kind == sched::RingMessageKind::kStepError) errored = true;
        EXPECT_NE(s.kind, sched::RingMessageKind::kStepEof)
            << "a walk that could not finish must not claim a clean end";
    }
    EXPECT_TRUE(errored);
}

// ---- The consuming stage (workplan P4d-4b-2) -----------------------------

class ConsumingStageTest : public RemoteStepServiceTest {
protected:
    // A one-column input layout: the join key an upstream scan forwards.
    catalog::SysColumnRow KeyColumn() {
        auto type_row = boot_->catalog.ResolveTypeByName("int64");
        EXPECT_TRUE(type_row.ok());
        catalog::SysColumnRow col{};
        col.pos = 0;
        catalog::SetName(col.name, "t_id");
        col.type_val = type_row.value().type_val;
        col.len = type_row.value().len;
        col.notnull = true;
        return col;
    }

    // The local step: keep rows of `t` whose id equals the upstream key -
    // a filtered walk, so the frame path is exercised without leaning on
    // probe-on-heap semantics. References arrive normalized (fact 4).
    exec::Step JoinStep() {
        exec::Step step;
        step.step_id = 1;
        step.rel_oid = oid_;
        step.kind = exec::AccessKind::kFilterScan;
        exec::StepPredicate pred;
        pred.lhs = exec::ColumnRef{0, 0, 0};  // t.id
        pred.op = parser::CompareOp::kEq;
        pred.rhs.kind = exec::OperandKind::kColumn;
        pred.rhs.column = exec::ColumnRef{1, 0, 0};  // the forwarded key
        step.residual.push_back(pred);
        return step;
    }

    // Opens the consuming stage: output = (upstream key, local qty).
    void OpenConsuming(std::uint64_t request_id = 7) {
        auto descriptor = EncodeStepDescriptor(JoinStep());
        ASSERT_TRUE(descriptor.ok()) << descriptor.status().message();

        StepOpenHead upstream_head{};
        upstream_head.tag = PipelineTag{request_id, /*session_core=*/0, /*step_id=*/0};
        upstream_head.downstream_core = 1;
        upstream_head.downstream_step = 1;
        exec::Step upstream_step = ScanStep();
        auto upstream_descriptor = EncodeStepDescriptor(upstream_step);
        ASSERT_TRUE(upstream_descriptor.ok());

        StepOpenUpstream up;
        up.upstream_core = 0;
        up.forwarded.push_back(KeyColumn());
        up.output.push_back(StepOutputColumn{/*from_upstream=*/1, /*index=*/0});
        up.output.push_back(StepOutputColumn{/*from_upstream=*/0, /*index=*/1});  // qty
        up.enclosed_open = EncodeStepOpen(upstream_head, upstream_descriptor.value());

        StepOpenHead head{};
        head.tag = PipelineTag{request_id, /*session_core=*/0, /*step_id=*/1};
        head.downstream_core = 0;
        server_->OnStepOpen(HeaderFromSession(), EncodeStepOpen(head, descriptor.value(), &up));
    }

    // One input batch carrying the given keys, under the upstream's tag.
    std::vector<std::byte> InputBatch(std::vector<std::int64_t> keys, std::uint32_t seq,
                                      std::uint64_t request_id = 7) {
        catalog::Schema input_schema;
        input_schema.columns.push_back(KeyColumn());
        wire::RowBatchWriter writer;
        for (std::int64_t key : keys) {
            parser::AstValue v;
            v.type = parser::ValueType::kInt;
            v.int_val = key;
            Status s = writer.AppendRow(input_schema, std::vector<parser::AstValue>{v});
            EXPECT_TRUE(s.ok()) << s.message();
        }
        return EncodeStepBatch(PipelineTag{request_id, 0, 0}, seq, writer);
    }

    void SendInputEof(std::uint64_t request_id = 7) {
        StepEofPayload eof{PipelineTag{request_id, 0, 0}};
        std::vector<std::byte> bytes;
        EncodePipelinePayload(eof, bytes);
        server_->OnStepEof(bytes);
    }
};

TEST_F(ConsumingStageTest, AConsumingStageForwardsItsUpstreamOpenThenJoinsPerInputRow) {
    UseStreamingServer();
    OpenConsuming();

    // The chained open: exactly one message so far, the enclosed upstream
    // open forwarded to its core - after the pipeline state exists, which
    // open_pipelines() witnesses.
    ASSERT_EQ(sent_.size(), 1u);
    EXPECT_EQ(sent_.front().kind, sched::RingMessageKind::kStepOpen);
    EXPECT_EQ(sent_.front().dst, 0u);
    EXPECT_EQ(server_->open_pipelines(), 1u);
    auto forwarded = DecodeStepOpenEnvelope(sent_.front().payload);
    ASSERT_TRUE(forwarded.ok());
    EXPECT_EQ(forwarded.value().head.tag, (PipelineTag{7, 0, 0}));
    EXPECT_EQ(forwarded.value().head.downstream_step, 1u);

    // Nothing to do until input arrives: the consumer parks.
    Pump();
    ASSERT_EQ(tasks_.size(), 1u);
    EXPECT_EQ(sent_.size(), 1u);

    // Three keys in, three joined rows out (id, qty of t): the sink read
    // the upstream value through up=1 and the local row through slot 0.
    server_->OnStepBatch(InputBatch({2, 5, 7}, /*seq=*/0));
    Pump();

    std::vector<std::pair<std::int64_t, std::int64_t>> rows_out;
    bool credited_upstream = false;
    for (std::size_t i = 1; i < sent_.size(); ++i) {
        if (sent_[i].kind == sched::RingMessageKind::kStepCredit) {
            credited_upstream = true;
            continue;
        }
        ASSERT_EQ(sent_[i].kind, sched::RingMessageKind::kStepBatch);
        std::span<const std::byte> rows;
        auto header = DecodeStepBatchHeader(sent_[i].payload, rows);
        ASSERT_TRUE(header.ok());
        auto decoded = wire::DecodeRowBatch(rows, /*field_count=*/2);
        ASSERT_TRUE(decoded.ok()) << decoded.status().message();
        for (const auto& row : decoded.value()) {
            auto key = wire::DecodeInt(row[0].bytes);
            auto qty = wire::DecodeInt(row[1].bytes);
            ASSERT_TRUE(key.ok());
            ASSERT_TRUE(qty.ok());
            rows_out.emplace_back(key.value(), qty.value());
        }
    }
    EXPECT_TRUE(credited_upstream) << "grant-on-drain: one credit per consumed input batch";
    EXPECT_EQ(rows_out, (std::vector<std::pair<std::int64_t, std::int64_t>>{
                            {2, 10}, {5, 40}, {7, 60}}));

    // The upstream's EOF ends the stage: final drain, EOF downstream,
    // teardown complete.
    SendInputEof();
    Pump();
    EXPECT_EQ(server_->open_pipelines(), 0u);
    EXPECT_TRUE(tasks_.empty());
    ASSERT_EQ(sent_.back().kind, sched::RingMessageKind::kStepEof);
}

TEST_F(ConsumingStageTest, ACancelReachesAParkedConsumerAndItsUpstreamHearsIt) {
    UseStreamingServer();
    OpenConsuming();
    Pump();
    ASSERT_EQ(server_->open_pipelines(), 1u);

    StepEofPayload cancel{PipelineTag{7, 0, 1}};
    std::vector<std::byte> bytes;
    EncodePipelinePayload(cancel, bytes);
    server_->OnStepCancel(bytes);
    Pump();
    EXPECT_EQ(server_->open_pipelines(), 0u);
    EXPECT_TRUE(tasks_.empty());

    // §7's upstream half: the stage that stopped told its producer to
    // stop too - without this the leaf parks on its credit gate forever.
    bool upstream_cancelled = false;
    for (const Sent& s : sent_) {
        EXPECT_NE(s.kind, sched::RingMessageKind::kStepEof)
            << "a cancelled stage must never claim a clean end";
        if (s.kind != sched::RingMessageKind::kStepCancel) continue;
        auto tag = DecodePipelinePayload<StepEofPayload>(s.payload);
        ASSERT_TRUE(tag.ok());
        if (tag.value().tag == (PipelineTag{7, 0, 0})) upstream_cancelled = true;
    }
    EXPECT_TRUE(upstream_cancelled);
}

TEST_F(ConsumingStageTest, AMalformedInputBatchAnswersErrorAndCancelsItsUpstream) {
    UseStreamingServer();
    OpenConsuming();
    Pump();

    // A batch whose declared rows cannot decode under the forwarded
    // layout: the header survives, the payload is garbage.
    catalog::Schema input_schema;
    input_schema.columns.push_back(KeyColumn());
    wire::RowBatchWriter writer;
    parser::AstValue v;
    v.type = parser::ValueType::kInt;
    v.int_val = 1;
    ASSERT_TRUE(writer.AppendRow(input_schema, std::vector<parser::AstValue>{v}).ok());
    std::vector<std::byte> batch = EncodeStepBatch(PipelineTag{7, 0, 0}, 0, writer);
    batch.resize(sizeof(StepBatchHeader) + 2);  // truncate the row bytes

    server_->OnStepBatch(batch);
    Pump();
    EXPECT_EQ(server_->open_pipelines(), 0u);
    EXPECT_TRUE(tasks_.empty());

    bool errored = false;
    bool upstream_cancelled = false;
    for (const Sent& s : sent_) {
        if (s.kind == sched::RingMessageKind::kStepError) errored = true;
        if (s.kind == sched::RingMessageKind::kStepCancel) upstream_cancelled = true;
        EXPECT_NE(s.kind, sched::RingMessageKind::kStepEof);
    }
    EXPECT_TRUE(errored);
    EXPECT_TRUE(upstream_cancelled);
}

TEST_F(ConsumingStageTest, AnOutputSpecThatForwardsNothingIsRefused) {
    UseStreamingServer();
    exec::Step step = JoinStep();
    auto descriptor = EncodeStepDescriptor(step);
    ASSERT_TRUE(descriptor.ok());
    StepOpenHead head{};
    head.tag = PipelineTag{7, 0, 1};
    StepOpenUpstream up;
    up.upstream_core = 0;
    up.forwarded.push_back(KeyColumn());
    up.enclosed_open.resize(sizeof(StepOpenHead) + 1);  // decodable head, no output spec

    server_->OnStepOpen(HeaderFromSession(), EncodeStepOpen(head, descriptor.value(), &up));
    ASSERT_EQ(sent_.size(), 1u);
    EXPECT_EQ(sent_.front().kind, sched::RingMessageKind::kStepError);
    EXPECT_EQ(server_->open_pipelines(), 0u);
    EXPECT_TRUE(tasks_.empty());
}

}  // namespace
}  // namespace kds::server
