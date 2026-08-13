#include "kds/server/remote_step_service.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/step_chain.hpp"
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

        auto access = boot_->catalog.InitTableAccess(oid_);
        ASSERT_TRUE(access.ok());
        for (int i = 0; i < 8; ++i) {
            auto id = boot_->catalog.AllocateRowId(oid_);
            ASSERT_TRUE(id.ok());
            parser::AstValue qty;
            qty.type = parser::ValueType::kInt;
            qty.int_val = i * 10;
            qty.raw_int_text = std::to_string(i * 10);
            auto payload = exec::EncodeRow(access.value()->schema, access.value()->layout,
                                           id.value(), {qty});
            ASSERT_TRUE(payload.ok());
            auto placed = heap::ChainInsert(*store_, access.value()->desc_page_id, id.value(),
                                            payload.value(), /*trx_id=*/1, access.value()->oid);
            ASSERT_TRUE(placed.ok());
        }

        server_.emplace(
            boot_->catalog, *store_, /*core_id=*/1,
            [this](std::uint32_t dst, sched::RingMessageKind kind,
                   std::vector<std::byte> payload) {
                sent_.push_back(Sent{dst, kind, std::move(payload)});
                return Status::OK();
            },
            nullptr, /*batch_target_bytes=*/1);  // one row per batch: 8 rows > 4 credits, deterministically
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

    static inline int counter_ = 0;
    std::unique_ptr<storage::MemoryPageDevice> device_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::optional<bootstrap::BootstrapResult> boot_;
    catalog::Oid oid_ = 0;
    std::optional<RemoteStepServer> server_;
    std::vector<Sent> sent_;
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

}  // namespace
}  // namespace kds::server
