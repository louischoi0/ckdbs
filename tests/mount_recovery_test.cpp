#include "kds/server/mount_recovery.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/stream.hpp"

// Recovery at mount (docs/workplan-wal-recovery.md RV1/RV2) - the seam that
// turned `wal::RecoverCore` from a function only tests called into the thing
// a mount runs.
//
// `wal_recovery_test.cpp` already pins the driver's own behaviour: the
// refusal without an undo phase, the phase called exactly when owed, the
// ordering of the high-water repair. **This file pins what the seam adds**,
// and each of these would pass every driver test while being wrong:
//
//   - the anchor's two fields reaching analysis. A seam that read the
//     anchor and dropped `durable_lsn` would recover happily from a stream
//     that lost the records its anchor depends on - `analysis.hpp` calls
//     that recovery's quietest failure mode.
//   - the undo phase being installed *always*, not on request. The driver
//     refuses a stream with losers and no phase; a mount that passed null
//     would be choosing that refusal over recovering, on every crash.
//   - the two caller obligations coming back as numbers - the page floor
//     the extent allocator is seeded from, and the transaction ceiling the
//     superblock owes - because a report that dropped either would leave the
//     hazard RC04 exists to close wide open, silently.

namespace kds::server {
namespace {

constexpr std::uint64_t kSegmentSize = 16 * 1024;
constexpr PageId kPage = kFirstUserPageId;

std::vector<std::byte> Bytes(std::size_t n, unsigned char fill) {
    return std::vector<std::byte>(n, static_cast<std::byte>(fill));
}

class MountRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = wal::MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());
        undo_log_.emplace(store_);
    }

    // PAGE_INIT + one heap insert under `txn_id`, then `terminal` unless it
    // is kPad - which stands for "append nothing", leaving the transaction a
    // loser. The same shape `wal_recovery_test.cpp` seeds with, so a
    // difference between the two files is a difference in the seam.
    void WriteStream(std::uint64_t txn_id, wal::RecordType terminal) {
        auto s = wal::WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok()) << s.status().message();

        std::vector<std::byte> init(wal::kPageInitPayloadSize, std::byte{0});
        const wal::PageInitPayload fields{
            /*min_key=*/1, static_cast<std::uint8_t>(PageType::kHeap), {0, 0, 0},
            /*reserved2=*/0, /*owner_oid=*/0};
        ASSERT_TRUE(wal::EncodePageInit(init, fields).ok());
        auto init_lsn = s.value()->Append({wal::RecordType::kPageInit, txn_id, kPage}, init);
        ASSERT_TRUE(init_lsn.ok()) << init_lsn.status().message();

        const auto payload = Bytes(24, 0xC1);
        std::vector<std::byte> buf(wal::kHeapWriteFixedSize + payload.size(), std::byte{0});
        const wal::HeapWritePayload hw{txn_id, /*undo_ptr=*/0, /*slot=*/0,
                                       static_cast<std::uint16_t>(payload.size())};
        auto n = wal::EncodeHeapWrite(buf, hw, payload);
        ASSERT_TRUE(n.ok()) << n.status().message();
        auto insert_lsn = s.value()->Append({wal::RecordType::kHeapInsert, txn_id, kPage},
                                            std::span(buf).first(n.value()));
        ASSERT_TRUE(insert_lsn.ok()) << insert_lsn.status().message();
        insert_lsn_ = insert_lsn.value();

        if (terminal != wal::RecordType::kPad) {
            ASSERT_TRUE(s.value()->Append({terminal, txn_id, kInvalidPageId}).ok());
        }
        ASSERT_TRUE(s.value()->Sync().ok());
    }

    // A stream that exists and holds nothing - a database opened and never
    // written to, which is the mount recovery must not charge for.
    void WriteEmptyStream() {
        auto s = wal::WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok()) << s.status().message();
        ASSERT_TRUE(s.value()->Sync().ok());
    }

    StatusOr<MountRecovery> Recover(const WalAnchorFields& anchor) {
        return RecoverCoreAtMount(/*core_id=*/0, anchor, *device_, store_, *undo_log_,
                                  /*wal=*/nullptr, /*log=*/nullptr);
    }

    std::unique_ptr<wal::MemoryLogDevice> device_;
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<txn::UndoLog> undo_log_;
    wal::Lsn insert_lsn_ = 0;
};

// ---- The anchor's fields reach analysis -----------------------------------

TEST_F(MountRecoveryTest, AZeroedAnchorScansFromTheHeadOfTheStream) {
    // Every database this engine has written before RC08 has a zeroed slot,
    // so this is not an edge case - it is the only case today.
    WriteStream(/*txn_id=*/7, wal::RecordType::kTxnCommit);

    auto r = Recover(WalAnchorFields{});
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_FALSE(r.value().empty());
    EXPECT_EQ(r.value().records, 3u);  // PAGE_INIT, insert, commit
    EXPECT_EQ(r.value().winners, 1u);
    EXPECT_EQ(r.value().losers, 0u);
    EXPECT_GT(r.value().redo_applied, 0u);

    auto page = store_.GetUnpinned(kPage);
    ASSERT_TRUE(page.ok()) << page.status().message();
    heap::PageView view(page.value());
    EXPECT_EQ(view.slot_count(), 1u);
}

TEST_F(MountRecoveryTest, TheAnchorsRedoStartNarrowsTheScan) {
    // Proves `redo_start_lsn` travels: starting at the insert's own LSN, the
    // PAGE_INIT below it is never scanned. A seam that dropped the field
    // would read 3 records here and pass every other test in this file.
    //
    // The full recovery runs first, because that is what an anchor *means*:
    // a published redo start says the pages below it were flushed, so the
    // narrowed scan finds its page already on the store. Starting a scan
    // above a PAGE_INIT whose page was never written is not a narrower
    // recovery, it is a broken one - and redo says so, `page id not found`.
    WriteStream(/*txn_id=*/7, wal::RecordType::kTxnCommit);
    auto full = Recover(WalAnchorFields{});
    ASSERT_TRUE(full.ok()) << full.status().message();
    ASSERT_EQ(full.value().records, 3u);

    WalAnchorFields anchor{};
    anchor.redo_start_lsn = insert_lsn_;
    auto r = Recover(anchor);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().records, 2u) << "the scan did not start at the anchor";
    EXPECT_EQ(r.value().redo_skipped_by_lsn, 1u) << "the insert alone, its PAGE_INIT unscanned";
}

TEST_F(MountRecoveryTest, AnAnchorPastTheDurableEndRefusesTheMount) {
    // Proves `durable_lsn` travels, and that a refusal is propagated rather
    // than logged and mounted anyway. `analysis.hpp`: a log that lost the
    // records its anchor depends on scans to zero records, byte-identical to
    // a clean shutdown - so dropping this field turns recovery's quietest
    // failure into a silent empty replay.
    WriteStream(/*txn_id=*/7, wal::RecordType::kTxnCommit);

    WalAnchorFields anchor{};
    anchor.durable_lsn = 1u << 30;
    auto r = Recover(anchor);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kCorruption) << r.status().message();
    EXPECT_EQ(store_.page_count(), 0u) << "a refused mount must not have written";
}

// ---- The undo phase is installed, always ---------------------------------

TEST_F(MountRecoveryTest, ALoserRecoversInsteadOfRefusingTheMount) {
    // The seam's whole reason for existing. `RecoverCore` with a null phase
    // refuses this stream (wal_recovery_test.cpp) - correctly, because
    // publishing a loser's writes is worse than not recovering. A mount must
    // therefore never pass null, and this is that assertion.
    WriteStream(/*txn_id=*/7, wal::RecordType::kPad);

    auto r = Recover(WalAnchorFields{});
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().losers, 1u);
    EXPECT_EQ(r.value().transactions_rolled_back, 1u);
    // No compensation: this loser's insert predates RV10's undo record in
    // the seeded stream, so its chain head is 0 and all it is owed is the
    // TXN_ABORT that stops the next recovery calling it a loser
    // (recovery_undo.cpp). What a real chain does to a real row is
    // recovery_undo_test.cpp's ten tests, not this seam's.
    EXPECT_EQ(r.value().compensations, 0u);
}

TEST_F(MountRecoveryTest, ADurableAbortOwesUndoNothing) {
    WriteStream(/*txn_id=*/7, wal::RecordType::kTxnAbort);

    auto r = Recover(WalAnchorFields{});
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().aborted, 1u);
    EXPECT_EQ(r.value().losers, 0u);
    EXPECT_EQ(r.value().transactions_rolled_back, 0u);
}

// ---- The caller's two obligations come back as numbers -------------------

TEST_F(MountRecoveryTest, ThePageFloorAndTrxCeilingAreReported) {
    WriteStream(/*txn_id=*/9000, wal::RecordType::kTxnCommit);

    auto r = Recover(WalAnchorFields{});
    ASSERT_TRUE(r.ok()) << r.status().message();

    // RC04's obligation 1: the extent allocator's search starts here, so an
    // extent cannot cover a page redo just wrote.
    EXPECT_TRUE(r.value().page_floor_raised);
    EXPECT_EQ(r.value().page_floor, kPage + 1);

    // And the ceiling the superblock owes - reported, not applied, because a
    // peer's superblock is a copy it may not write (M5).
    EXPECT_EQ(r.value().next_trx_id, 9001u);
    SuperBlock sb = SuperBlock::CreateFresh(/*now_unix_seconds=*/1);
    ASSERT_TRUE(sb.SetNextTrxId(r.value().next_trx_id).ok());
    EXPECT_EQ(sb.next_trx_id(), 9001u);
}

TEST_F(MountRecoveryTest, AnUnwrittenStreamCostsNothingAndSaysSo) {
    WriteEmptyStream();

    auto r = Recover(WalAnchorFields{});
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_TRUE(r.value().empty());
    EXPECT_EQ(r.value().records, 0u);
    EXPECT_EQ(r.value().redo_applied, 0u);
    EXPECT_FALSE(r.value().page_floor_raised)
        << "a stream naming no page must not move the allocation floor";
    EXPECT_EQ(r.value().next_trx_id, 0u);
    EXPECT_EQ(store_.page_count(), 0u);
}

// ---- RC08: the completion checkpoint bounds the next crash ---------------

// A dirty table with nothing in it, which is the honest one here: this
// fixture's records were scripted into the log rather than written through a
// store, so no page is dirty. The checkpoint then takes its redo start from its
// own CHECKPOINT_BEGIN (checkpointer.hpp), which is exactly the bound under
// test - "the next recovery starts here".
class NoDirtyPages final : public wal::CheckpointTarget {
public:
    std::vector<wal::CheckpointDirtyPage> DirtyTable() const override { return {}; }
    Status FlushPages(std::span<const PageId>) override { return Status::OK(); }
};

TEST_F(MountRecoveryTest, TheCompletionCheckpointStopsTheNextRecoveryRescanningTheStream) {
    // RC08's done-when, and the reason it matters is a measurement rather than
    // an argument: before this, a mount with no anchor scanned the whole stream
    // every time (~55-70 ms per mount over a ~1500-op log at RC11), and it grew
    // with the log forever because nothing published an anchor after replaying.
    WriteStream(/*txn_id=*/7, wal::RecordType::kTxnCommit);

    sched::ManualClock clock;
    auto manager = wal::WalManager::Open(device_.get(), clock, /*core_id=*/0);
    ASSERT_TRUE(manager.ok()) << manager.status().message();

    auto first = RecoverCoreAtMount(/*core_id=*/0, WalAnchorFields{}, *device_, store_,
                                    *undo_log_, &*manager.value(), /*log=*/nullptr);
    ASSERT_TRUE(first.ok()) << first.status().message();
    ASSERT_EQ(first.value().records, 3u);

    NoDirtyPages target;
    wal::InMemoryCheckpointAnchor published;
    ASSERT_TRUE(CheckpointAfterRecovery(/*core_id=*/0, *manager.value(), target, published,
                                        /*log=*/nullptr)
                    .ok());
    EXPECT_EQ(published.publishes(), 1u) << "the mount published no anchor";
    EXPECT_GT(published.anchor().redo_start_lsn, 0u);

    // The anchor as the superblock would carry it - which is the caller's job
    // (`SuperBlockCheckpointAnchor`), and the shape the next mount reads.
    WalAnchorFields anchor{};
    anchor.checkpoint_lsn = published.anchor().checkpoint_lsn;
    anchor.redo_start_lsn = published.anchor().redo_start_lsn;
    anchor.durable_lsn = published.anchor().durable_lsn;
    anchor.segment_no = published.anchor().segment_no;

    auto second = RecoverCoreAtMount(/*core_id=*/0, anchor, *device_, store_, *undo_log_,
                                     &*manager.value(), /*log=*/nullptr);
    ASSERT_TRUE(second.ok()) << second.status().message();

    // Only the two checkpoint records, which name no page. The heap records are
    // not skipped-by-LSN - they are **not scanned at all**, which is the
    // difference between a bounded recovery and a cheap-looking one.
    EXPECT_EQ(second.value().records, 2u);
    EXPECT_EQ(second.value().redo_applied, 0u);
    EXPECT_EQ(second.value().redo_skipped_by_lsn, 0u)
        << "the second mount re-read the records the checkpoint bounded away";
    EXPECT_LT(second.value().records, first.value().records);
}

TEST_F(MountRecoveryTest, TheAnchorItPublishesIsHonestAboutTheDurableEnd) {
    // `Analyze` refuses a mount whose stream cannot reach the anchor's durable
    // point - the check that stops a lost tail reading as a clean shutdown
    // (analysis.hpp). So an anchor this function publishes has to satisfy it,
    // or RC08 would hand the next mount a refusal.
    WriteStream(/*txn_id=*/7, wal::RecordType::kTxnCommit);

    sched::ManualClock clock;
    auto manager = wal::WalManager::Open(device_.get(), clock, /*core_id=*/0);
    ASSERT_TRUE(manager.ok()) << manager.status().message();

    NoDirtyPages target;
    wal::InMemoryCheckpointAnchor published;
    ASSERT_TRUE(CheckpointAfterRecovery(/*core_id=*/0, *manager.value(), target, published,
                                        /*log=*/nullptr)
                    .ok());

    WalAnchorFields anchor{};
    anchor.redo_start_lsn = published.anchor().redo_start_lsn;
    anchor.durable_lsn = published.anchor().durable_lsn;

    auto recovered = RecoverCoreAtMount(/*core_id=*/0, anchor, *device_, store_, *undo_log_,
                                       &*manager.value(), /*log=*/nullptr);
    EXPECT_TRUE(recovered.ok()) << recovered.status().message();
}

// ---- RC09: what an operator can read afterwards --------------------------

TEST_F(MountRecoveryTest, PhasesAreTimedWhenAClockIsSuppliedAndSayWhenTheyAreNot) {
    // `docs/spec/wal.md` §13 asks for recovery phase timings. The property worth a
    // test is not the numbers - they are wall clock - but the **flag**: four
    // zeroes from an untimed run and four zeroes from an instant one are the
    // same bytes, and an operator tuning RTO has to be able to tell them apart.
    WriteStream(/*txn_id=*/7, wal::RecordType::kTxnCommit);

    auto untimed = Recover(WalAnchorFields{});
    ASSERT_TRUE(untimed.ok()) << untimed.status().message();
    EXPECT_FALSE(untimed.value().timings.timed);
    EXPECT_EQ(untimed.value().timings.total_ns(), 0u);

    // A clock that does move, so the phases can report something. ManualClock
    // is the deterministic one this engine tests with, and it advances only
    // when told - which is exactly what makes the assertion below exact rather
    // than a race against a real clock.
    class TickingClock final : public sched::Clock {
    public:
        sched::MonoTimeNs Now() const override {
            now_ += 1000;  // 1 us per read
            return now_;
        }

    private:
        mutable sched::MonoTimeNs now_ = 0;
    };
    TickingClock clock;
    storage::InMemoryPageStore fresh{kFirstUserPageId};
    txn::UndoLog fresh_undo(fresh);
    auto timed = RecoverCoreAtMount(/*core_id=*/0, WalAnchorFields{}, *device_, fresh, fresh_undo,
                                    /*wal=*/nullptr, /*log=*/nullptr, &clock);
    ASSERT_TRUE(timed.ok()) << timed.status().message();
    EXPECT_TRUE(timed.value().timings.timed);
    // Analysis and redo both ran, so both are two clock reads apart.
    EXPECT_GT(timed.value().timings.analysis_ns, 0u);
    EXPECT_GT(timed.value().timings.redo_ns, 0u);
    // No losers in this stream, so undo never ran - and an unrun phase reports
    // zero rather than the cost of the phase beside it.
    EXPECT_EQ(timed.value().timings.undo_ns, 0u);
}

TEST_F(MountRecoveryTest, TheCompletionCheckpointReportsItsOwnDuration) {
    WriteStream(/*txn_id=*/7, wal::RecordType::kTxnCommit);

    sched::ManualClock clock(1000);
    auto manager = wal::WalManager::Open(device_.get(), clock, /*core_id=*/0);
    ASSERT_TRUE(manager.ok()) << manager.status().message();

    NoDirtyPages target;
    wal::InMemoryCheckpointAnchor published;
    sched::MonoTimeNs elapsed = 12345;  // overwritten, or the test proves nothing
    ASSERT_TRUE(CheckpointAfterRecovery(/*core_id=*/0, *manager.value(), target, published,
                                        /*log=*/nullptr, &clock, &elapsed)
                    .ok());
    // A ManualClock does not move on its own, so the honest answer is zero -
    // and zero is what a caller passing a frozen clock must get, rather than
    // the field being left at whatever it held.
    EXPECT_EQ(elapsed, 0u);
}

TEST_F(MountRecoveryTest, RecoveringTwiceIsANoOp) {
    // A crash during a mount re-runs the whole thing, so the seam has to be
    // as idempotent as the driver under it.
    WriteStream(/*txn_id=*/7, wal::RecordType::kTxnCommit);

    ASSERT_TRUE(Recover(WalAnchorFields{}).ok());
    auto page = store_.GetUnpinned(kPage);
    ASSERT_TRUE(page.ok());
    const std::vector<std::byte> after_first(page.value().begin(), page.value().end());

    auto second = Recover(WalAnchorFields{});
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(second.value().redo_applied, 0u);
    EXPECT_EQ(second.value().redo_skipped_by_lsn, 2u);

    auto again = store_.GetUnpinned(kPage);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(std::vector<std::byte>(again.value().begin(), again.value().end()), after_first);
}

// ---- RC09: RV3's audit, and what SHOW META says about it -----------------

// A store that can be told to stop serving user pages, which is what a crash
// that lost a relation's pages leaves behind. Catalog pages live below
// `kFirstUserPageId` (in_memory_page_store.hpp), so the fault keeps the catalog
// itself readable - the audit has to be able to *list* relations in order to
// report that it cannot open them.
class UserPagesGone final : public storage::PageStore {
public:
    explicit UserPagesGone(storage::InMemoryPageStore& inner) noexcept : inner_(inner) {}

    void ArmFault() noexcept { armed_ = true; }

    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override {
        return inner_.CreateAtUnpinned(page_id);
    }
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override {
        return inner_.CreateNewUnpinned();
    }
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override {
        if (Failing(page_id)) return Status::NotFound("page id not found");
        return inner_.GetUnpinned(page_id);
    }
    StatusOr<std::span<std::byte, kPageSize>> GetForReadUnpinned(PageId page_id) override {
        if (Failing(page_id)) return Status::NotFound("page id not found");
        return inner_.GetForReadUnpinned(page_id);
    }

private:
    bool Failing(PageId page_id) const noexcept { return armed_ && page_id >= kFirstUserPageId; }

    storage::InMemoryPageStore& inner_;
    bool armed_ = false;
};

class RecoveryAuditTest : public ::testing::Test {
protected:
    void SetUp() override {
        faulty_.emplace(inner_);
        auto boot = bootstrap::BootstrapDatabase(*faulty_, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, *faulty_);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    storage::InMemoryPageStore inner_{kFirstUserPageId};
    std::optional<UserPagesGone> faulty_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(RecoveryAuditTest, EveryRelationTheCatalogDescribesIsOpenedAndCounted) {
    ASSERT_EQ(Run("CREATE TABLE a (id int64, v int32)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE TABLE b (id int64, s varchar)").substr(0, 7), "CREATED");

    const MountRecovery audited =
        AuditCatalogAfterRecovery(boot_->catalog, *faulty_, MountRecovery{}, /*log=*/nullptr);
    // Exactly the two user relations. The bootstrap catalogs are listed in
    // sys.objects and have no sys.tables row (catalog/well_known.hpp), so
    // counting them here would report nine missing pages on a healthy mount -
    // which is what the first draft of this audit did.
    EXPECT_EQ(audited.relations_checked, 2u);
    EXPECT_EQ(audited.relations_missing_pages, 0u);
}

TEST_F(RecoveryAuditTest, ARelationWhosePagesTheCrashTookIsCountedRatherThanRefused) {
    // RV3's detectable half: `CREATE TABLE` is unlogged, so a crash can keep
    // the catalog row and lose the pages it points at. The audit's job is to
    // say so as a number - **not** to fail the mount, because an unopenable
    // relation is the finding rather than an error hit while producing it.
    ASSERT_EQ(Run("CREATE TABLE a (id int64, v int32)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE TABLE b (id int64, s varchar)").substr(0, 7), "CREATED");
    faulty_->ArmFault();

    const MountRecovery audited =
        AuditCatalogAfterRecovery(boot_->catalog, *faulty_, MountRecovery{}, /*log=*/nullptr);
    EXPECT_EQ(audited.relations_checked, 2u);
    EXPECT_EQ(audited.relations_missing_pages, 2u)
        << "the pages are gone and the audit reported none of them missing";
}

TEST_F(RecoveryAuditTest, ShowMetaCarriesTheRecoveryBlockOnlyWhenAReportIsInstalled) {
    // Absent, not zeroed: a dispatcher with no report has no answer about
    // recovery, and printing zeroes would be an answer - the same rule
    // SHOW ASSERTIONS follows for an absent surface.
    EXPECT_EQ(Run("SHOW META").find("recovery_records="), std::string::npos);

    MountRecovery report;
    report.records = 41;
    report.winners = 3;
    report.transactions_rolled_back = 2;
    report.relations_checked = 7;
    report.timings.timed = true;
    report.timings.redo_ns = 5000;
    dispatcher_->set_recovery(&report);

    const std::string meta = Run("SHOW META");
    EXPECT_NE(meta.find("recovery_records=41"), std::string::npos) << meta;
    EXPECT_NE(meta.find("recovery_rolled_back=2"), std::string::npos) << meta;
    EXPECT_NE(meta.find("recovery_redo_us=5"), std::string::npos) << meta;
    EXPECT_NE(meta.find("recovery_relations_checked=7"), std::string::npos) << meta;
    // RV3 closed 2026-08-19: catalog mutations are logged and DDL is a
    // real transaction, so the flags flip together - durable and recovered,
    // stated in the same breath they were disclaimed in for a week.
    EXPECT_NE(meta.find("catalog_recovered=1"), std::string::npos) << meta;
    EXPECT_NE(meta.find("ddl_durable=1"), std::string::npos) << meta;
}

TEST_F(RecoveryAuditTest, AnUntimedRecoveryPrintsNoDurations) {
    MountRecovery report;
    report.records = 5;
    dispatcher_->set_recovery(&report);

    const std::string meta = Run("SHOW META");
    EXPECT_NE(meta.find("recovery_records=5"), std::string::npos) << meta;
    EXPECT_EQ(meta.find("recovery_redo_us="), std::string::npos)
        << "an unmeasured phase must not print a duration of zero: " << meta;
}

}  // namespace
}  // namespace kds::server
