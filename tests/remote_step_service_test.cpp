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
#include "kds/txn/manager.hpp"
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
                                                  catalog::ClusteredType::kHeap);
        ASSERT_TRUE(created.ok());
        oid_ = created.value();

        // qty = i*10 so the residual test's `qty > 50` keeps exactly 2 of
        // the 8; one row per batch (target 1) makes 8 rows > 4 credits,
        // deterministically.
        InsertRows(8, /*qty_step=*/10);
        MakeServer(/*submit=*/{});

        // **The suspension audit, over the pipeline's own park points.**
        // It is what turns "a coroutine never suspends holding a page"
        // from prose into a detected violation (P4d-3c), and it was wired
        // only into `CoreRuntime::Run` and `Expeditor::Serve` - neither of
        // which these tests call, so every park this file exercises went
        // unaudited, including the one P4d-4c added *inside* a consuming
        // stage's input row. Installed against this fixture's own store,
        // because pins are per-store.
        sched::ResetSuspendAudit();  // thread-local: another test may have tripped it
        exec::InstallSuspendAudit(store_.get());
    }

    void TearDown() override {
        // **The audit records, it does not abort** - so installing it
        // proves nothing without reading it back. Every test in this
        // fixture is checked here, which is what makes the parked-walk
        // tests an actual audit of the park rather than of the answer.
        EXPECT_FALSE(sched::SuspendAuditTripped())
            << "a coroutine suspended holding a page: " << sched::SuspendAuditReason();
        sched::ResetSuspendAudit();
        // Owed at teardown: the audit is thread-local and the store it
        // reads is not immortal.
        exec::UninstallSuspendAudit();
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

TEST_F(RemoteStepServiceTest, AStandaloneOutputSpecRoundTripsWithAndWithoutAnUpstream) {
    // P4d-4b-3: the output section stands beside the upstream half, so a
    // *leaf* can carry one - the shape the forwarded layout rides on.
    exec::Step step = ScanStep();
    auto descriptor = EncodeStepDescriptor(step);
    ASSERT_TRUE(descriptor.ok());
    StepOpenHead head{};
    head.tag = PipelineTag{9, 0, 0};
    const std::vector<StepOutputColumn> output{StepOutputColumn{0, 1}, StepOutputColumn{0, 0}};

    const std::vector<std::byte> leaf =
        EncodeStepOpen(head, descriptor.value(), nullptr, output);
    auto leaf_parts = DecodeStepOpenEnvelope(leaf);
    ASSERT_TRUE(leaf_parts.ok()) << leaf_parts.status().message();
    EXPECT_FALSE(leaf_parts.value().upstream.has_value());
    ASSERT_EQ(leaf_parts.value().output.size(), 2u);
    EXPECT_EQ(leaf_parts.value().output[0].from_upstream, 0);
    EXPECT_EQ(leaf_parts.value().output[0].index, 1u);
    EXPECT_EQ(leaf_parts.value().output[1].index, 0u);
    EXPECT_TRUE(std::equal(leaf_parts.value().descriptor.begin(),
                           leaf_parts.value().descriptor.end(), descriptor.value().begin(),
                           descriptor.value().end()));

    StepOpenUpstream up;
    up.upstream_core = 3;
    up.forwarded.push_back(catalog::SysColumnRow{});
    up.enclosed_open = {std::byte{0x01}};
    const std::vector<StepOutputColumn> mixed{StepOutputColumn{1, 0}, StepOutputColumn{0, 1}};
    const std::vector<std::byte> chained =
        EncodeStepOpen(head, descriptor.value(), &up, mixed);
    auto chained_parts = DecodeStepOpenEnvelope(chained);
    ASSERT_TRUE(chained_parts.ok()) << chained_parts.status().message();
    ASSERT_TRUE(chained_parts.value().upstream.has_value());
    ASSERT_EQ(chained_parts.value().output.size(), 2u);
    EXPECT_EQ(chained_parts.value().output[0].from_upstream, 1);
    EXPECT_TRUE(std::equal(chained_parts.value().descriptor.begin(),
                           chained_parts.value().descriptor.end(), descriptor.value().begin(),
                           descriptor.value().end()));
}

TEST_F(RemoteStepServiceTest, ALeafHonorsItsOutputSpec) {
    // The stage seals exactly the spec'd columns, in spec order - what
    // lets the session plan a forwarded layout narrower than the row.
    exec::Step step = ScanStep();
    auto descriptor = EncodeStepDescriptor(step);
    ASSERT_TRUE(descriptor.ok());
    StepOpenHead head{};
    head.tag = PipelineTag{7, 0, 0};
    head.downstream_core = 0;
    const std::vector<StepOutputColumn> output{StepOutputColumn{0, 1}};  // qty only

    server_->OnStepOpen(HeaderFromSession(),
                        EncodeStepOpen(head, descriptor.value(), nullptr, output));
    // One row per batch (target 1): 8 batches need two full grants under
    // the 4-credit ceiling.
    GrantCredits(4);
    GrantCredits(4);

    std::vector<std::int64_t> qty_out;
    for (const Sent& s : sent_) {
        if (s.kind != sched::RingMessageKind::kStepBatch) continue;
        std::span<const std::byte> rows;
        auto header = DecodeStepBatchHeader(s.payload, rows);
        ASSERT_TRUE(header.ok());
        auto decoded = wire::DecodeRowBatch(rows, /*field_count=*/1);
        ASSERT_TRUE(decoded.ok()) << decoded.status().message();
        for (const auto& row : decoded.value()) {
            auto qty = wire::DecodeInt(row[0].bytes);
            ASSERT_TRUE(qty.ok());
            qty_out.push_back(qty.value());
        }
    }
    EXPECT_EQ(qty_out, (std::vector<std::int64_t>{0, 10, 20, 30, 40, 50, 60, 70}));
    EXPECT_EQ(sent_.back().kind, sched::RingMessageKind::kStepEof);
}

TEST_F(RemoteStepServiceTest, ALeafOutputSpecNamingAnUpstreamIsRefused) {
    exec::Step step = ScanStep();
    auto descriptor = EncodeStepDescriptor(step);
    ASSERT_TRUE(descriptor.ok());
    StepOpenHead head{};
    head.tag = PipelineTag{7, 0, 0};
    const std::vector<StepOutputColumn> output{StepOutputColumn{1, 0}};  // no upstream exists

    server_->OnStepOpen(HeaderFromSession(),
                        EncodeStepOpen(head, descriptor.value(), nullptr, output));
    ASSERT_EQ(sent_.size(), 1u);
    EXPECT_EQ(sent_.front().kind, sched::RingMessageKind::kStepError);
    EXPECT_EQ(server_->open_pipelines(), 0u);
}

TEST_F(RemoteStepServiceTest, AnEnvelopeTruncatedInsideItsOutputSpecAnswersStepError) {
    exec::Step step = ScanStep();
    auto descriptor = EncodeStepDescriptor(step);
    ASSERT_TRUE(descriptor.ok());
    StepOpenHead head{};
    head.tag = PipelineTag{7, 0, 0};
    const std::vector<StepOutputColumn> output{StepOutputColumn{0, 1}};
    auto envelope = EncodeStepOpen(head, descriptor.value(), nullptr, output);
    // head + upstream flag + output flag + count + 2 of the entry's 5.
    envelope.resize(sizeof(StepOpenHead) + 1 + 1 + 4 + 2);

    server_->OnStepOpen(HeaderFromSession(), envelope);
    ASSERT_EQ(sent_.size(), 1u);
    EXPECT_EQ(sent_.front().kind, sched::RingMessageKind::kStepError);
    EXPECT_EQ(server_->open_pipelines(), 0u);
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

// ---- The stage's read view (crosscore.md CC4) ----------------------------

TEST_F(RemoteStepServiceTest, AStageReadsAtLatestCommittedAndNotAnInFlightWriter) {
    // The gap the 4b-3 review priced, closed: a stage used to execute with
    // `snapshot=nullptr`, which the executor reads as "every writer
    // visible" - so a concurrent *uncommitted* INSERT on the owning core
    // was streamed to the session. CC4 says the owning core's latest
    // committed snapshot, and the owning core mints it locally.
    txn::TrxIdSequence ids(boot_->superblock);
    txn::UndoLog undo(*store_, /*wal=*/nullptr);
    txn::TransactionManager mgr(ids, undo, *store_, /*wal=*/nullptr);

    // A live transaction, and a row written under its id but never
    // committed - exactly what a peer's concurrent INSERT looks like to a
    // stage opening a moment later.
    auto writer = mgr.Begin(txn::IsolationLevel::kReadCommitted);
    ASSERT_TRUE(writer.ok()) << writer.status().message();
    auto access = boot_->catalog.InitTableAccess(oid_);
    ASSERT_TRUE(access.ok());
    auto id = boot_->catalog.AllocateRowId(oid_);
    ASSERT_TRUE(id.ok());
    parser::AstValue qty;
    qty.type = parser::ValueType::kInt;
    qty.int_val = 999;
    qty.raw_int_text = "999";
    auto payload = exec::EncodeRow(access.value()->schema, access.value()->layout, id.value(),
                                   {qty});
    ASSERT_TRUE(payload.ok());
    auto placed = heap::ChainInsert(*store_, access.value()->desc_page_id, id.value(),
                                    payload.value(), writer.value()->id(), access.value()->oid);
    ASSERT_TRUE(placed.ok()) << placed.status().message();

    // A server wired to that manager mints its view when the stage opens.
    server_.emplace(
        boot_->catalog, *store_, /*core_id=*/1,
        [this](std::uint32_t dst, sched::RingMessageKind kind, std::vector<std::byte> p) {
            sent_.push_back(Sent{dst, kind, std::move(p)});
            return Status::OK();
        },
        nullptr, /*batch_target_bytes=*/1, RemoteStepServer::SubmitFn{}, &mgr);
    server_->OnStepOpen(HeaderFromSession(), OpenFor(ScanStep()));
    GrantCredits(4);
    GrantCredits(4);

    std::vector<std::int64_t> qty_out;
    for (const Sent& s : sent_) {
        if (s.kind != sched::RingMessageKind::kStepBatch) continue;
        std::span<const std::byte> rows;
        auto header = DecodeStepBatchHeader(s.payload, rows);
        ASSERT_TRUE(header.ok());
        auto decoded = wire::DecodeRowBatch(rows, /*field_count=*/2);
        ASSERT_TRUE(decoded.ok()) << decoded.status().message();
        for (const auto& row : decoded.value()) {
            auto v = wire::DecodeInt(row[1].bytes);
            ASSERT_TRUE(v.ok());
            qty_out.push_back(v.value());
        }
    }
    // The eight committed rows, and not the in-flight writer's 999.
    EXPECT_EQ(qty_out, (std::vector<std::int64_t>{0, 10, 20, 30, 40, 50, 60, 70}));
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

    // The walked inner of P4d-4c: `t.qty >= <upstream key>`, which one
    // input row of key 0 matches on every row of the relation - so a
    // multi-page relation makes the walk cross page boundaries with
    // output already sealed, which is the only way to reach the gate.
    exec::Step WalkJoinStep() {
        exec::Step step;
        step.step_id = 1;
        step.rel_oid = oid_;
        step.kind = exec::AccessKind::kScan;
        exec::StepPredicate pred;
        pred.lhs = exec::ColumnRef{0, 0, 1};  // t.qty
        pred.op = parser::CompareOp::kGte;
        pred.rhs.kind = exec::OperandKind::kColumn;
        pred.rhs.column = exec::ColumnRef{1, 0, 0};  // the forwarded key
        step.residual.push_back(pred);
        return step;
    }

    // Opens the consuming stage: output = (upstream key, local qty).
    void OpenConsuming(std::uint64_t request_id = 7) { OpenConsumingWith(JoinStep(), request_id); }

    void OpenConsumingWith(const exec::Step& step, std::uint64_t request_id = 7) {
        auto descriptor = EncodeStepDescriptor(step);
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
        up.enclosed_open = EncodeStepOpen(upstream_head, upstream_descriptor.value());

        // The output spec stands beside the upstream section (P4d-4b-3):
        // (upstream key, local qty), in the order the downstream decodes.
        const std::vector<StepOutputColumn> output{
            StepOutputColumn{/*from_upstream=*/1, /*index=*/0},
            StepOutputColumn{/*from_upstream=*/0, /*index=*/1}};

        StepOpenHead head{};
        head.tag = PipelineTag{request_id, /*session_core=*/0, /*step_id=*/1};
        head.downstream_core = 0;
        server_->OnStepOpen(HeaderFromSession(),
                            EncodeStepOpen(head, descriptor.value(), &up, output));
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
    up.enclosed_open.resize(sizeof(StepOpenHead) + 2);  // decodable head, no output spec

    server_->OnStepOpen(HeaderFromSession(), EncodeStepOpen(head, descriptor.value(), &up));
    ASSERT_EQ(sent_.size(), 1u);
    EXPECT_EQ(sent_.front().kind, sched::RingMessageKind::kStepError);
    EXPECT_EQ(server_->open_pipelines(), 0u);
    EXPECT_TRUE(tasks_.empty());
}

TEST_F(ConsumingStageTest, AnEnclosedOpenAddressingAnotherStageIsRefused) {
    // The downstream fields of the enclosed head must name *this* stage -
    // a plan wired to the wrong consumer would stream rows to a tag that
    // never opened (P4d-4b-3's read of `downstream_step`).
    UseStreamingServer();
    auto descriptor = EncodeStepDescriptor(JoinStep());
    ASSERT_TRUE(descriptor.ok());

    StepOpenHead upstream_head{};
    upstream_head.tag = PipelineTag{7, 0, 0};
    upstream_head.downstream_core = 1;
    upstream_head.downstream_step = 2;  // not this stage's step id (1)
    auto upstream_descriptor = EncodeStepDescriptor(ScanStep());
    ASSERT_TRUE(upstream_descriptor.ok());

    StepOpenUpstream up;
    up.upstream_core = 0;
    up.forwarded.push_back(KeyColumn());
    up.enclosed_open = EncodeStepOpen(upstream_head, upstream_descriptor.value());
    const std::vector<StepOutputColumn> output{StepOutputColumn{1, 0}};

    StepOpenHead head{};
    head.tag = PipelineTag{7, 0, 1};
    head.downstream_core = 0;
    server_->OnStepOpen(HeaderFromSession(),
                        EncodeStepOpen(head, descriptor.value(), &up, output));
    ASSERT_EQ(sent_.size(), 1u);
    EXPECT_EQ(sent_.front().kind, sched::RingMessageKind::kStepError);
    EXPECT_EQ(server_->open_pipelines(), 0u);
    EXPECT_TRUE(tasks_.empty());
}

// ---- P4d-4c: the gated inner walk ---------------------------------------

TEST_F(ConsumingStageTest, TheRowTouchBudgetBoundsTheWholeStageNotOneInputRow) {
    // The 4c review's finding: the stage passed a *fresh* budget into
    // every input row's run, and ExecuteAsync seeds its counter from the
    // limit it is handed - so the count restarted per row and a walked
    // inner had no statement-wide bound at all. That is exactly the n²
    // shape `exec/budget.hpp` exists to refuse, and the local path does
    // refuse it, so the pipeline was answering a statement its local twin
    // errors on.
    //
    // A budget of 12 against the fixture's 8 rows: one input row's walk
    // spends 8, so a per-row budget would let every row through forever.
    // Per stage, the second row's walk crosses the ceiling.
    server_.emplace(
        boot_->catalog, *store_, /*core_id=*/1,
        [this](std::uint32_t dst, sched::RingMessageKind kind, std::vector<std::byte> p) {
            sent_.push_back(Sent{dst, kind, std::move(p)});
            return Status::OK();
        },
        nullptr, /*batch_target_bytes=*/1,
        [this](std::unique_ptr<sched::Task> task) { tasks_.push_back(std::move(task)); },
        /*txns=*/nullptr, exec::Budget(12));
    OpenConsumingWith(WalkJoinStep());
    Pump();

    // Three input rows, each matching all 8 rows: 24 decodes against 12.
    // Credit goes to *this stage's* tag (step 1) - `GrantCredits` addresses
    // the leaf's (step 0), which lives on the other core and is discarded
    // here by §3's rule.
    server_->OnStepBatch(InputBatch({0, 0, 0}, /*seq=*/0));
    auto grant = [&](std::uint32_t n) {
        StepCreditPayload credit{PipelineTag{7, 0, 1}, n};
        std::vector<std::byte> bytes;
        EncodePipelinePayload(credit, bytes);
        server_->OnStepCredit(bytes);
    };
    for (int i = 0; i < 200 && !tasks_.empty(); ++i) {
        grant(kInitialCreditsPerEdge);
        Pump();
    }

    bool errored = false;
    bool upstream_cancelled = false;
    for (const Sent& s : sent_) {
        if (s.kind == sched::RingMessageKind::kStepError) {
            errored = true;
            auto error = DecodePipelinePayload<StepErrorPayload>(s.payload);
            ASSERT_TRUE(error.ok());
            EXPECT_EQ(error.value().status_code,
                      static_cast<std::uint32_t>(StatusCode::kResourceExhausted));
        }
        if (s.kind == sched::RingMessageKind::kStepCancel) upstream_cancelled = true;
        EXPECT_NE(s.kind, sched::RingMessageKind::kStepEof)
            << "a stage that blew its budget must never claim a clean end";
    }
    EXPECT_TRUE(errored) << "the stage ran past its budget without refusing";
    EXPECT_TRUE(upstream_cancelled) << "§7: a stopping stage owes its upstream a cancel";
    EXPECT_EQ(server_->open_pipelines(), 0u);
    EXPECT_TRUE(tasks_.empty());
}

TEST_F(ConsumingStageTest, AWalkedInnerParksAtItsPageBoundaryAndResumesUnderCredit) {
    // 4c's whole claim: a *walked* inner parks at its own page boundaries
    // under the downstream credit, so the stage buffers a page's matches
    // instead of every match of one input row. Needs a multi-page
    // relation - `RunWalkStep` consults the gate only after a page with a
    // successor, so a one-page walk never reaches it, which is why every
    // existing consuming-stage test (8 rows, one page) leaves the gate
    // untouched.
    InsertRows(1592);  // 1600 rows over roughly eight pages
    UseStreamingServer();
    OpenConsumingWith(WalkJoinStep());
    Pump();
    ASSERT_EQ(server_->open_pipelines(), 1u);

    server_->OnStepBatch(InputBatch({0}, /*seq=*/0));
    SendInputEof();

    // Pumped with no credit ever granted to *this* stage: the walk must
    // park, and what it holds while parked is the bound under test.
    std::size_t high_water = 0;
    for (int i = 0; i < 64; ++i) {
        Pump();
        high_water = std::max(high_water, server_->unsent_batches());
        if (tasks_.empty()) break;
    }
    EXPECT_FALSE(tasks_.empty()) << "the walk ran to completion holding no credit";
    // One 8 KiB page holds about 200 of these rows; the relation holds
    // 1600. A bound around a page is the claim, and anything approaching
    // the relation is the unbounded shape 4c exists to have removed.
    EXPECT_LT(high_water, 300u) << "the stage sealed far more than a page's matches: "
                                << high_water;

    // Now drain: every grant lets the parked walk make progress, and all
    // 1600 rows arrive in relation order.
    auto grant = [&](std::uint32_t n) {
        StepCreditPayload credit{PipelineTag{7, 0, 1}, n};
        std::vector<std::byte> bytes;
        EncodePipelinePayload(credit, bytes);
        server_->OnStepCredit(bytes);
    };
    for (int i = 0; i < 4000 && !tasks_.empty(); ++i) {
        grant(kInitialCreditsPerEdge);
        Pump();
    }
    EXPECT_TRUE(tasks_.empty()) << "the pipeline never converged";
    EXPECT_EQ(server_->open_pipelines(), 0u);

    std::vector<std::int64_t> qty_out;
    bool saw_eof = false;
    for (const Sent& s : sent_) {
        if (s.kind == sched::RingMessageKind::kStepEof) saw_eof = true;
        if (s.kind != sched::RingMessageKind::kStepBatch) continue;
        std::span<const std::byte> rows;
        auto header = DecodeStepBatchHeader(s.payload, rows);
        ASSERT_TRUE(header.ok());
        auto decoded = wire::DecodeRowBatch(rows, /*field_count=*/2);
        ASSERT_TRUE(decoded.ok()) << decoded.status().message();
        for (const auto& row : decoded.value()) {
            auto key = wire::DecodeInt(row[0].bytes);
            auto qty = wire::DecodeInt(row[1].bytes);
            ASSERT_TRUE(key.ok());
            ASSERT_TRUE(qty.ok());
            EXPECT_EQ(key.value(), 0) << "the upstream row's slot changed under the park";
            qty_out.push_back(qty.value());
        }
    }
    EXPECT_TRUE(saw_eof);
    // The fixture's eight rows (qty = i*10) then this test's 1592 (qty = i),
    // in relation order - which is what proves the park resumed the walk
    // where it left off rather than restarting or skipping it.
    std::vector<std::int64_t> expected;
    for (int i = 0; i < 8; ++i) expected.push_back(i * 10);
    for (int i = 0; i < 1592; ++i) expected.push_back(i);
    EXPECT_EQ(qty_out, expected);
}

TEST_F(ConsumingStageTest, ACancelReachesAWalkParkedInsideItsInputRow) {
    // The one teardown window 4c opened: before it, a cancel could only
    // land between input rows, because the inner ran to completion inside
    // one synchronous call. Now it can land with the walk parked *inside*
    // a row - the outer frame filled, the writer half full, the decoded
    // input batch still being iterated - and everything the resume then
    // touches has to still be there.
    InsertRows(1592);
    UseStreamingServer();
    OpenConsumingWith(WalkJoinStep());
    Pump();
    server_->OnStepBatch(InputBatch({0}, /*seq=*/0));
    for (int i = 0; i < 8 && !tasks_.empty(); ++i) Pump();
    ASSERT_FALSE(tasks_.empty()) << "the walk did not park, so nothing is cancelled mid-walk";
    ASSERT_GT(server_->unsent_batches(), 0u);

    StepEofPayload cancel{PipelineTag{7, 0, 1}};
    std::vector<std::byte> bytes;
    EncodePipelinePayload(cancel, bytes);
    server_->OnStepCancel(bytes);
    for (int i = 0; i < 8 && !tasks_.empty(); ++i) Pump();

    EXPECT_TRUE(tasks_.empty()) << "the parked walk never saw the cancel";
    EXPECT_EQ(server_->open_pipelines(), 0u);
    bool upstream_cancelled = false;
    for (const Sent& s : sent_) {
        EXPECT_NE(s.kind, sched::RingMessageKind::kStepEof)
            << "a cancelled stage must never claim a clean end";
        if (s.kind != sched::RingMessageKind::kStepCancel) continue;
        auto tag = DecodePipelinePayload<StepEofPayload>(s.payload);
        ASSERT_TRUE(tag.ok());
        if (tag.value().tag == (PipelineTag{7, 0, 0})) upstream_cancelled = true;
    }
    EXPECT_TRUE(upstream_cancelled) << "crosscore.md §7: the upstream was left parked";
}

}  // namespace
}  // namespace kds::server
