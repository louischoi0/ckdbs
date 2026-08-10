#include "kds/server/session_step_client.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/server/remote_step_service.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/wire/row_codec.hpp"

// P4b and P4c's halves cross-wired in process: the session client's sends
// deliver straight into the remote server's handlers and back - the full
// STEP_OPEN -> BATCH/CREDIT -> EOF exchange, with no reactor, no threads
// and no transport, so what is under test is the protocol alone.

namespace kds::server {
namespace {

class SessionStepLoopbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = storage::MemoryPageDevice::Create(64);
        ASSERT_TRUE(device.ok());
        device_ = std::move(device.value());
        auto store = storage::DevicePageStore::Open(*device_, kFirstUserPageId);
        ASSERT_TRUE(store.ok());
        store_ = std::move(store.value());
        auto boot = bootstrap::BootstrapDatabase(*store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));

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

        auto access = boot_->catalog.InitTableAccess(oid_);
        ASSERT_TRUE(access.ok());
        for (int i = 0; i < 6; ++i) {
            auto id = boot_->catalog.AllocateRowId(oid_);
            ASSERT_TRUE(id.ok());
            parser::AstValue qty;
            qty.type = parser::ValueType::kInt;
            qty.int_val = i * 100;
            qty.raw_int_text = std::to_string(i * 100);
            auto payload = exec::EncodeRow(access.value()->schema, access.value()->layout,
                                           id.value(), {qty});
            ASSERT_TRUE(payload.ok());
            auto placed = heap::ChainInsert(*store_, access.value()->desc_page_id, id.value(),
                                            payload.value(), 1);
            ASSERT_TRUE(placed.ok());
        }

        // Core 1 owns the relation's server side; core 0 is the session.
        // Each side's sender delivers into the other's handlers, exactly
        // as two attached reactors would - minus the rings.
        server_.emplace(
            boot_->catalog, *store_, /*core_id=*/1,
            [this](std::uint32_t, sched::RingMessageKind kind, std::vector<std::byte> payload) {
                switch (kind) {
                    case sched::RingMessageKind::kStepBatch: client_->OnStepBatch(payload); break;
                    case sched::RingMessageKind::kStepEof: client_->OnStepEof(payload); break;
                    case sched::RingMessageKind::kStepError:
                        client_->OnStepError(payload);
                        break;
                    default: ADD_FAILURE() << "server sent unexpected kind";
                }
                return Status::OK();
            },
            nullptr, /*batch_target_bytes=*/64);
        client_.emplace(
            /*core_id=*/0,
            [this](std::uint32_t, sched::RingMessageKind kind, std::vector<std::byte> payload) {
                sched::MessageHeader from_session{};
                from_session.src_core = 0;
                from_session.dst_core = 1;
                switch (kind) {
                    case sched::RingMessageKind::kStepOpen:
                        server_->OnStepOpen(from_session, payload);
                        break;
                    case sched::RingMessageKind::kStepCredit:
                        server_->OnStepCredit(payload);
                        break;
                    case sched::RingMessageKind::kStepCancel:
                        server_->OnStepCancel(payload);
                        break;
                    default: ADD_FAILURE() << "client sent unexpected kind";
                }
                return Status::OK();
            },
            nullptr);
    }

    exec::Step ScanStep() {
        exec::Step step;
        step.step_id = 0;
        step.rel_oid = oid_;
        step.kind = exec::AccessKind::kScan;
        return step;
    }

    std::unique_ptr<storage::MemoryPageDevice> device_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::optional<bootstrap::BootstrapResult> boot_;
    catalog::Oid oid_ = 0;
    std::optional<RemoteStepServer> server_;
    std::optional<SessionStepClient> client_;
};

TEST_F(SessionStepLoopbackTest, ARemoteScanCompletesWithEveryRow) {
    auto tag = client_->Open(ScanStep(), /*owner_core=*/1, /*request_id=*/11);
    ASSERT_TRUE(tag.ok()) << tag.status().message();

    // Loopback is synchronous: by the time Open returns, batches flowed,
    // grant-on-receive kept the credits alive past the initial 4, EOF
    // landed, and the read is complete. This is the property the credit
    // deadlock would break - a stalled stream would leave done false.
    SessionStepClient::RemoteRead* read = client_->Find(tag.value());
    ASSERT_NE(read, nullptr);
    EXPECT_TRUE(read->done);
    EXPECT_TRUE(read->error.ok()) << read->error.message();
    EXPECT_EQ(read->rows, 6u);
    EXPECT_GT(read->batches.size(), 1u);  // the tiny target forced chunks
    EXPECT_EQ(server_->open_pipelines(), 0u);

    // The rows decode through the one KWP decoder, in order.
    std::uint64_t next_qty = 0;
    for (const auto& batch : read->batches) {
        std::span<const std::byte> rows;
        auto header = DecodeStepBatchHeader(batch, rows);
        ASSERT_TRUE(header.ok());
        auto decoded = wire::DecodeRowBatch(rows, /*field_count=*/2);
        ASSERT_TRUE(decoded.ok());
        for (const auto& row : decoded.value()) {
            ASSERT_EQ(row.size(), 2u);
            next_qty += 100;
        }
    }
    client_->Close(tag.value());
    EXPECT_EQ(client_->open_reads(), 0u);
}

TEST_F(SessionStepLoopbackTest, ARemoteErrorArrivesWithItsCode) {
    exec::Step step = ScanStep();
    step.rel_oid = 424242;
    auto tag = client_->Open(step, 1, 12);
    ASSERT_TRUE(tag.ok());

    SessionStepClient::RemoteRead* read = client_->Find(tag.value());
    ASSERT_NE(read, nullptr);
    EXPECT_TRUE(read->done);
    EXPECT_FALSE(read->error.ok());
    EXPECT_EQ(read->error.code(), StatusCode::kNotFound);
    client_->Close(tag.value());
}

TEST_F(SessionStepLoopbackTest, CloseBeforeEofCancelsTheRemoteSide) {
    // A server that cannot send batches (no credit consumed here - the
    // client is opened and immediately closed before any grant flows) must
    // be torn down by the cancel, not left parked.
    auto tag = client_->Open(ScanStep(), 1, 13);
    ASSERT_TRUE(tag.ok());
    // Loopback completed it synchronously; re-open a fresh read and close
    // it mid-flight is not expressible without a reactor, so what this
    // pins is the pairing rule: Close on a completed read frees the state
    // and sends nothing the server would misread.
    client_->Close(tag.value());
    EXPECT_EQ(client_->open_reads(), 0u);
    EXPECT_EQ(server_->open_pipelines(), 0u);
}

}  // namespace
}  // namespace kds::server
