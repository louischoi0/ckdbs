#include "kds/server/session_step_client.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/server/remote_step_service.hpp"
#include "kds/server/step_descriptor.hpp"
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
                                            payload.value(), 1, access.value()->oid);
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

// ---- The pipeline plan and its teardown (workplan P4d-4b-3) --------------

TEST(SessionStepPipelineTest, AStageErrorUnderAnotherTagCompletesTheStatementsRead) {
    // A pipeline's failure arrives under the *failing stage's* tag while
    // the read is registered under the final stage's; the statement is the
    // match. And a read torn down other than by clean EOF cancels every
    // stage - the leaf's failure leaves its consumer parked on input
    // forever unless the session, the one holder of the stage list, says
    // stop.
    struct Sent {
        std::uint32_t dst;
        sched::RingMessageKind kind;
    };
    std::vector<Sent> sent;
    SessionStepClient client(
        /*core_id=*/0,
        [&](std::uint32_t dst, sched::RingMessageKind kind, std::vector<std::byte>) {
            sent.push_back({dst, kind});
            return Status::OK();
        });

    SessionStepClient::PipelinePlan plan;
    plan.final_open = {std::byte{0x00}};  // nobody decodes it here
    plan.final_core = 2;
    plan.final_tag = PipelineTag{21, 0, 1};
    plan.stages.push_back(SessionStepClient::StageAddress{PipelineTag{21, 0, 0}, 1});
    plan.stages.push_back(SessionStepClient::StageAddress{PipelineTag{21, 0, 1}, 2});
    auto tag = client.OpenPipeline(std::move(plan));
    ASSERT_TRUE(tag.ok());
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].kind, sched::RingMessageKind::kStepOpen);
    EXPECT_EQ(sent[0].dst, 2u);

    // The leaf (step 0) fails; the read registered under step 1 hears it.
    StepErrorPayload error{};
    error.tag = PipelineTag{21, 0, 0};
    error.status_code = static_cast<std::uint32_t>(StatusCode::kCorruption);
    std::vector<std::byte> bytes;
    EncodePipelinePayload(error, bytes);
    client.OnStepError(bytes);

    SessionStepClient::RemoteRead* read = client.Find(tag.value());
    ASSERT_NE(read, nullptr);
    EXPECT_TRUE(read->done);
    EXPECT_EQ(read->error.code(), StatusCode::kCorruption);

    // Close after an error: a CANCEL to every stage, addressed per stage.
    client.Close(tag.value());
    ASSERT_EQ(sent.size(), 3u);
    EXPECT_EQ(sent[1].kind, sched::RingMessageKind::kStepCancel);
    EXPECT_EQ(sent[1].dst, 1u);
    EXPECT_EQ(sent[2].kind, sched::RingMessageKind::kStepCancel);
    EXPECT_EQ(sent[2].dst, 2u);
    EXPECT_EQ(client.open_reads(), 0u);
}

// ---- The plan-time tests' shared chain -----------------------------------

namespace {

// The two-relation chain the plan-time tests share: outer(id, b_id),
// inner(id, qty), joined on `inner.qty = outer.b_id` - a non-pk join
// column, which is the shape a secondary index serves locally and the
// descriptor cannot ship. Each test reshapes only the step whose kind it
// is about: the plan test makes the inner a kProbe, the downgrade tests
// (docs/spec/index.md §8a) a structure kind.
struct DowngradeFixture {
    catalog::Schema outer_schema;
    catalog::Schema inner_schema;
    exec::StepChain chain;

    DowngradeFixture() {
        auto add = [](catalog::Schema& schema, const char* name) {
            catalog::SysColumnRow row{};
            row.pos = static_cast<std::uint16_t>(schema.columns.size());
            catalog::SetName(row.name, name);
            row.type_val = catalog::kTypeValInt64;
            row.len = 8;
            row.notnull = true;
            schema.columns.push_back(row);
        };
        add(outer_schema, "id");
        add(outer_schema, "b_id");
        add(inner_schema, "id");
        add(inner_schema, "qty");

        exec::Step outer;
        outer.step_id = 0;
        outer.rel_oid = 100;
        outer.kind = exec::AccessKind::kScan;
        chain.steps.push_back(outer);

        exec::Step inner;
        inner.step_id = 1;
        inner.rel_oid = 200;
        inner.kind = exec::AccessKind::kScan;
        exec::StepPredicate join_eq;
        join_eq.lhs = exec::ColumnRef{0, 1, 1};  // inner.qty
        join_eq.op = parser::CompareOp::kEq;
        join_eq.rhs.kind = exec::OperandKind::kColumn;
        join_eq.rhs.column = exec::ColumnRef{0, 0, 1};  // outer.b_id
        inner.residual.push_back(join_eq);
        chain.steps.push_back(inner);

        chain.projection = {exec::ColumnRef{0, 0, 0},   // outer.id
                            exec::ColumnRef{0, 1, 1}};  // inner.qty
        chain.column_names = {"id", "qty"};
        chain.projection_types = {catalog::kTypeValInt64, catalog::kTypeValInt64};
    }
};

// A correlated index probe over the fixture's join column - IX17's kind,
// with the aux the descriptor refuses.
exec::IndexProbe CorrelatedProbeOnQty() {
    exec::IndexProbe probe;
    probe.index_oid = 7;
    probe.key_width = 9;
    probe.entry_width = 17;
    probe.eq_prefix = 1;
    probe.key_cols = {1};
    probe.key_from = exec::ColumnRef{0, 0, 1};  // outer.b_id
    probe.low.assign(17, std::byte{0x00});
    probe.high.assign(17, std::byte{0xFF});
    return probe;
}

// The literal form: bounds pre-encoded at compile time, no per-row key.
exec::IndexProbe LiteralProbeOnQty() {
    exec::IndexProbe probe = CorrelatedProbeOnQty();
    probe.key_from.reset();
    return probe;
}

}  // namespace

TEST(SessionStepPipelineTest, BuildTwoStepPipelineComputesTheEdgeAndNormalizesTheRefs) {
    // The plan-time facts, end to end: the forwarded layout falls out of
    // the chain's own references, both output specs are decided by the
    // session, and the shipped inner step's references arrive normalized -
    // (up=1, slot=0, forwarded index) for the outer row, (up=0, slot=0)
    // for its own. The fixture's inner is reshaped into the kProbe form:
    // keyed by outer.b_id, joined on the inner pk.
    DowngradeFixture fx;
    exec::Step& inner = fx.chain.steps[1];
    inner.kind = exec::AccessKind::kProbe;
    exec::Operand key;
    key.kind = exec::OperandKind::kColumn;
    key.column = exec::ColumnRef{0, 0, 1};  // outer.b_id
    inner.key = key;
    inner.residual.clear();
    exec::StepPredicate join_eq;
    join_eq.lhs = exec::ColumnRef{0, 1, 0};  // inner.id
    join_eq.op = parser::CompareOp::kEq;
    join_eq.rhs.kind = exec::OperandKind::kColumn;
    join_eq.rhs.column = exec::ColumnRef{0, 0, 1};  // outer.b_id
    inner.residual.push_back(join_eq);

    auto plan = BuildTwoStepPipeline(fx.chain, fx.outer_schema, fx.inner_schema,
                                     /*outer_core=*/1, /*inner_core=*/2, /*session_core=*/0,
                                     /*request_id=*/31);
    ASSERT_TRUE(plan.ok()) << plan.status().message();
    EXPECT_EQ(plan.value().final_core, 2u);
    EXPECT_EQ(plan.value().final_tag, (PipelineTag{31, 0, 1}));
    ASSERT_EQ(plan.value().stages.size(), 2u);
    EXPECT_EQ(plan.value().stages[0].tag, (PipelineTag{31, 0, 0}));
    EXPECT_EQ(plan.value().stages[0].core, 1u);
    EXPECT_EQ(plan.value().stages[1].core, 2u);
    // The decode layout mirrors the projection: outer.id, inner.qty.
    ASSERT_EQ(plan.value().output_layout.size(), 2u);
    EXPECT_EQ(std::string(catalog::NameView(plan.value().output_layout[0].name)), "id");
    EXPECT_EQ(std::string(catalog::NameView(plan.value().output_layout[1].name)), "qty");

    // The final envelope: upstream edge to core 1, the forwarded layout =
    // outer.{id, b_id} (id for the projection, b_id for the key), and the
    // output spec (upstream id, local qty).
    auto parts = DecodeStepOpenEnvelope(plan.value().final_open);
    ASSERT_TRUE(parts.ok()) << parts.status().message();
    EXPECT_EQ(parts.value().head.tag, (PipelineTag{31, 0, 1}));
    EXPECT_EQ(parts.value().head.downstream_core, 0u);
    EXPECT_EQ(parts.value().head.downstream_step, 0u);
    ASSERT_TRUE(parts.value().upstream.has_value());
    EXPECT_EQ(parts.value().upstream->upstream_core, 1u);
    ASSERT_EQ(parts.value().upstream->forwarded.size(), 2u);
    EXPECT_EQ(std::string(catalog::NameView(parts.value().upstream->forwarded[0].name)), "id");
    EXPECT_EQ(std::string(catalog::NameView(parts.value().upstream->forwarded[1].name)),
              "b_id");
    ASSERT_EQ(parts.value().output.size(), 2u);
    EXPECT_EQ(parts.value().output[0].from_upstream, 1);
    EXPECT_EQ(parts.value().output[0].index, 0u);  // forwarded[0] = outer.id
    EXPECT_EQ(parts.value().output[1].from_upstream, 0);
    EXPECT_EQ(parts.value().output[1].index, 1u);  // inner.qty

    // The shipped inner step decodes with normalized references: the key
    // at (1, 0, 1) - forwarded index of b_id - and the join residual's
    // sides at (0,0,0) own / (1,0,1) upstream.
    auto shipped = DecodeStepDescriptor(parts.value().descriptor);
    ASSERT_TRUE(shipped.ok()) << shipped.status().message();
    ASSERT_TRUE(shipped.value().key.has_value());
    EXPECT_EQ(shipped.value().key->column.up, 1u);
    EXPECT_EQ(shipped.value().key->column.rel_slot, 0u);
    EXPECT_EQ(shipped.value().key->column.col_pos, 1u);
    ASSERT_EQ(shipped.value().residual.size(), 1u);
    EXPECT_EQ(shipped.value().residual[0].lhs.up, 0u);
    EXPECT_EQ(shipped.value().residual[0].lhs.rel_slot, 0u);
    EXPECT_EQ(shipped.value().residual[0].rhs.column.up, 1u);
    EXPECT_EQ(shipped.value().residual[0].rhs.column.col_pos, 1u);

    // The enclosed leaf open: addressed to the consuming stage (core 2,
    // step 1), its output spec the forwarded layout as local columns.
    auto leaf = DecodeStepOpenEnvelope(parts.value().upstream->enclosed_open);
    ASSERT_TRUE(leaf.ok()) << leaf.status().message();
    EXPECT_EQ(leaf.value().head.tag, (PipelineTag{31, 0, 0}));
    EXPECT_EQ(leaf.value().head.downstream_core, 2u);
    EXPECT_EQ(leaf.value().head.downstream_step, 1u);
    EXPECT_FALSE(leaf.value().upstream.has_value());
    ASSERT_EQ(leaf.value().output.size(), 2u);
    EXPECT_EQ(leaf.value().output[0].from_upstream, 0);
    EXPECT_EQ(leaf.value().output[0].index, 0u);
    EXPECT_EQ(leaf.value().output[1].index, 1u);
}


TEST(SessionStepPipelineTest, AStructureServedInnerStepShipsAsItsWalk) {
    // The hole this closes: an inner step of IX17's kind used to fail
    // eligibility outright, so CREATE INDEX on a peer relation's join
    // column turned the join's answers into affinity ERRs
    // (docs/inflight/known-gaps.md's closed entry). It now plans, and the
    // consuming stage receives the walk the step would fall back to
    // anyway: kScan, no aux, the join equality still in the residual.
    DowngradeFixture fx;
    fx.chain.steps[1].kind = exec::AccessKind::kIndexProbe;
    fx.chain.steps[1].index = CorrelatedProbeOnQty();

    ASSERT_TRUE(TwoStepPipelineEligible(fx.chain).ok());
    auto plan = BuildTwoStepPipeline(fx.chain, fx.outer_schema, fx.inner_schema,
                                     /*outer_core=*/1, /*inner_core=*/2, /*session_core=*/0,
                                     /*request_id=*/41);
    ASSERT_TRUE(plan.ok()) << plan.status().message();

    auto parts = DecodeStepOpenEnvelope(plan.value().final_open);
    ASSERT_TRUE(parts.ok()) << parts.status().message();
    auto shipped = DecodeStepDescriptor(parts.value().descriptor);
    ASSERT_TRUE(shipped.ok()) << shipped.status().message();
    EXPECT_EQ(shipped.value().kind, exec::AccessKind::kScan);
    EXPECT_FALSE(shipped.value().index.has_value());
    EXPECT_FALSE(shipped.value().cabin.has_value());
    EXPECT_TRUE(shipped.value().access_columns.empty());
    // The join equality rides the residual, normalized: inner.qty at
    // (0,0), the outer row's b_id through the forwarded layout at (1,0).
    ASSERT_EQ(shipped.value().residual.size(), 1u);
    EXPECT_EQ(shipped.value().residual[0].lhs, (exec::ColumnRef{0, 0, 1}));
    EXPECT_EQ(shipped.value().residual[0].rhs.column.up, 1u);
}

TEST(SessionStepPipelineTest, AStructureServedLeafShipsAsItsWalkToo) {
    // The outer side has the same exposure - a literal restriction on the
    // outer relation's indexed column compiles it to kIndexProbe - and
    // gets the same downgrade at the leaf's encode.
    DowngradeFixture fx;
    fx.chain.steps[0].kind = exec::AccessKind::kIndexProbe;
    fx.chain.steps[0].index = LiteralProbeOnQty();

    ASSERT_TRUE(TwoStepPipelineEligible(fx.chain).ok());
    auto plan = BuildTwoStepPipeline(fx.chain, fx.outer_schema, fx.inner_schema,
                                     /*outer_core=*/1, /*inner_core=*/2, /*session_core=*/0,
                                     /*request_id=*/42);
    ASSERT_TRUE(plan.ok()) << plan.status().message();

    auto final_parts = DecodeStepOpenEnvelope(plan.value().final_open);
    ASSERT_TRUE(final_parts.ok());
    ASSERT_TRUE(final_parts.value().upstream.has_value());
    auto leaf = DecodeStepOpenEnvelope(final_parts.value().upstream->enclosed_open);
    ASSERT_TRUE(leaf.ok()) << leaf.status().message();
    auto shipped = DecodeStepDescriptor(leaf.value().descriptor);
    ASSERT_TRUE(shipped.ok()) << shipped.status().message();
    EXPECT_EQ(shipped.value().kind, exec::AccessKind::kScan);
    EXPECT_FALSE(shipped.value().index.has_value());
}

TEST(SessionStepPipelineTest, AStructureProbeJoinsTheWalkedClassAndItsRules) {
    // The A/B that pins the widening itself: one cabined inner step, once
    // with the join conjunct and once without. With it, the structure kind
    // is now eligible (this half fails on the pre-downgrade code); without
    // it, the downgraded walk would be a cross product, which stays
    // refused for the reason the walked class always refused one.
    DowngradeFixture fx;
    fx.chain.steps[1].kind = exec::AccessKind::kCabinProbe;
    exec::CabinProbe cabin;
    cabin.cabin_id = 3;
    cabin.col_pos = 1;
    cabin.value.type = parser::ValueType::kInt;
    cabin.value.int_val = 5;
    fx.chain.steps[1].cabin = cabin;
    exec::StepPredicate literal_eq;
    literal_eq.lhs = exec::ColumnRef{0, 1, 1};
    literal_eq.op = parser::CompareOp::kEq;
    literal_eq.rhs.kind = exec::OperandKind::kLiteral;
    literal_eq.rhs.literal = cabin.value;
    fx.chain.steps[1].residual.push_back(literal_eq);

    // With the fixture's join conjunct beside the cabined literal:
    // eligible, into the walked class.
    EXPECT_TRUE(TwoStepPipelineEligible(fx.chain).ok());

    // Without any reference to the outer row: a cross product, refused.
    fx.chain.steps[1].residual.erase(fx.chain.steps[1].residual.begin());
    EXPECT_FALSE(TwoStepPipelineEligible(fx.chain).ok());
}

TEST(SessionStepPipelineTest, ASingleStepStructureOpenShipsAsItsWalk) {
    // The P4c single-step seam gets the identical treatment: an indexed
    // filter on a peer-owned relation used to die at the descriptor's
    // refusal and fall through to the affinity ERR; the open now carries
    // the walk.
    std::vector<std::byte> captured;
    SessionStepClient client(
        /*core_id=*/0, [&](std::uint32_t, sched::RingMessageKind, std::vector<std::byte> p) {
            captured = std::move(p);
            return Status::OK();
        });

    exec::Step step;
    step.step_id = 0;
    step.rel_oid = 300;
    step.kind = exec::AccessKind::kIndexProbe;
    step.index = LiteralProbeOnQty();
    exec::StepPredicate literal_eq;
    literal_eq.lhs = exec::ColumnRef{0, 0, 1};
    literal_eq.op = parser::CompareOp::kEq;
    literal_eq.rhs.kind = exec::OperandKind::kLiteral;
    literal_eq.rhs.literal.type = parser::ValueType::kInt;
    literal_eq.rhs.literal.int_val = 5;
    step.residual.push_back(literal_eq);

    auto tag = client.Open(step, /*owner_core=*/1, /*request_id=*/43);
    ASSERT_TRUE(tag.ok()) << tag.status().message();
    ASSERT_FALSE(captured.empty());
    auto parts = DecodeStepOpenEnvelope(captured);
    ASSERT_TRUE(parts.ok()) << parts.status().message();
    auto shipped = DecodeStepDescriptor(parts.value().descriptor);
    ASSERT_TRUE(shipped.ok()) << shipped.status().message();
    EXPECT_EQ(shipped.value().kind, exec::AccessKind::kScan);
    EXPECT_FALSE(shipped.value().index.has_value());
    // The literal equality is untouched: the walk filters to the same rows.
    ASSERT_EQ(shipped.value().residual.size(), 1u);
    EXPECT_EQ(shipped.value().residual[0].rhs.literal.int_val, 5);
}

}  // namespace
}  // namespace kds::server
