// **`WAYSTONE ON` / `WAYSTONE OFF`** (CF-S1b,
// `instructions/v3.2.0/workorder-cf-waystone-premise.md` §8.3, §8.5): the
// opt-in by example, its refusal table, its durability, and the mount's
// renewal of an opted pattern's directory.
//
// Nothing records until a shape is opted in, and the opt-in is one bit on
// the shape's own `sys.patterns` row - so the cells here are about that bit:
// who may set it, what shapes may carry it, when an answer about it is
// durable, and what a mount does with the directory beside it.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <unistd.h>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/base/log.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/parser/parser.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/server/restore.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/stats/waystone_dir.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/txn/manager.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "reclaim_census.hpp"
#include "sim/instance.hpp"
#include "view_text.hpp"

namespace kds::server {
namespace {

namespace fs = std::filesystem;

// The first byte position `text` names (`byte N`), if any.
std::optional<std::size_t> ByteOf(const std::string& text) {
    const std::size_t at = text.find("byte ");
    if (at == std::string::npos) return std::nullopt;
    return static_cast<std::size_t>(std::strtoull(text.c_str() + at + 5, nullptr, 10));
}

std::uint64_t FetchIdOf(const std::string& sql) {
    const auto fp = parser::FingerprintOf(sql);
    EXPECT_TRUE(fp.has_value()) << sql;
    return fp.has_value() ? fp->fetch_id : 0;
}

std::string Hex(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
    return buf;
}

// How many rows a `SHOW PATTERNS` reply reads opted (CF-S1c's count, as the
// view states it).
std::size_t ViewOpted(const std::string& patterns_reply) {
    std::size_t opted = 0;
    for (const std::string& v : testing_view::ReadViewText(patterns_reply).Column("waystone")) {
        if (v == "1") ++opted;
    }
    return opted;
}

// A store that watches its waystone directories being made: whether any
// `sys.patterns` page is pinned at that moment, an action to run there - a
// concurrent `WAYSTONE ON` landing between the opt-in's unheld read and
// its hold - and a refusal standing for a full pool. Everything else is
// the in-memory store's.
class DirectoryWatchStore final : public storage::PageStore {
public:
    explicit DirectoryWatchStore(storage::InMemoryPageStore& inner) : inner_(inner) {}

    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override {
        return inner_.CreateAtUnpinned(page_id);
    }
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override {
        return inner_.CreateNewUnpinned();
    }
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override {
        // A write fetch of one page refused once `passes` of them were
        // served - so a WAYSTONE statement's hold can be let through and the
        // write it makes under the hold refused - while every read of the
        // page is served.
        if (refuse_write_of_ != kInvalidPageId && page_id == refuse_write_of_) {
            if (write_passes_ == 0) {
                ++write_refusals_;
                return Status::IoError("injected: the page cannot be written");
            }
            --write_passes_;
        }
        return inner_.GetUnpinned(page_id);
    }
    StatusOr<std::span<std::byte, kPageSize>> GetForReadUnpinned(PageId page_id) override {
        if (refuse_pattern_reads_ && page_id == catalog::kCatalogPagePatterns) {
            return Status::IoError("injected: sys.patterns cannot be read");
        }
        return inner_.GetForReadUnpinned(page_id);
    }
    void PinFrame(PageId page_id, storage::PinMode) noexcept override {
        if (page_id == catalog::kCatalogPagePatterns) ++pattern_pins_;
    }
    void UnpinFrame(PageId page_id) noexcept override {
        if (page_id == catalog::kCatalogPagePatterns) --pattern_pins_;
    }

    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewHeaderlessUnpinned()
        override {
        ++directories_made_;
        if (pattern_pins_ != 0) ++made_under_a_pattern_pin_;
        if (refuse_) {
            return Status::ResourceExhausted("injected: the pool has no free frame");
        }
        if (on_create_) {
            auto action = std::move(on_create_);  // disarmed first: it may create too
            on_create_ = nullptr;
            action();
        }
        return CreateNewUnpinned();
    }

    void OnCreate(std::function<void()> action) { on_create_ = std::move(action); }
    void Refuse(bool refuse) { refuse_ = refuse; }
    // Refuses every write fetch of `page_id` after the next `passes`;
    // `kInvalidPageId` disarms it.
    void RefuseWritesOf(PageId page_id, int passes = 0) {
        refuse_write_of_ = page_id;
        write_passes_ = passes;
        write_refusals_ = 0;
    }
    int write_refusals() const { return write_refusals_; }
    void RefusePatternReads(bool refuse) { refuse_pattern_reads_ = refuse; }
    int directories_made() const { return directories_made_; }
    int made_under_a_pattern_pin() const { return made_under_a_pattern_pin_; }

private:
    storage::InMemoryPageStore& inner_;
    int pattern_pins_ = 0;
    int directories_made_ = 0;
    int made_under_a_pattern_pin_ = 0;
    bool refuse_ = false;
    PageId refuse_write_of_ = kInvalidPageId;
    int write_passes_ = 0;
    int write_refusals_ = 0;
    bool refuse_pattern_reads_ = false;
    std::function<void()> on_create_;
};

class WaystoneOptInTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        boot_->catalog.SetLogger(&logger_);
        // A transaction manager and the lock family, so the cells about
        // transactions and borrows have both - the table into the manager
        // as well as the dispatcher, as production wires it (AO-S6c).
        auto table = txn::LockTable::Create(/*core_count=*/1);
        ASSERT_TRUE(table.ok()) << table.status().message();
        locks_ = std::move(table.value());
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr, /*visibility=*/nullptr,
                     /*core=*/0, locks_.get());
        recorder_.emplace(boot_->catalog, store_);
        MakeDispatcher(/*record=*/true, /*replay=*/true);

        Ok("CREATE TABLE t (id int64, v int64) BTREE");
        Ok("CREATE TABLE u (id int64, w int64) BTREE");
        Ok("CREATE INDEX t_by_v ON t (v)");
        for (int i = 1; i <= 5; ++i) {
            Ok("INSERT INTO t VALUES (" + std::to_string(i * 10) + ")");
            Ok("INSERT INTO u VALUES (" + std::to_string(i * 100) + ")");
        }
    }

    void MakeDispatcher(bool record, bool replay) {
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), record ? &*recorder_ : nullptr, replay,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);
        dispatcher_->set_locks(locks_.get());
    }

    // Fails `session`'s open transaction: a named key that is taken.
    void Fail(Session& session) {
        ASSERT_EQ(Out("BEGIN", &session).response.rfind("ERR", 0), std::string::npos);
        ASSERT_EQ(Out("INSERT INTO t VALUES (1, 10)", &session).response.rfind("ERR", 0), 0u);
        ASSERT_TRUE(session.failed()) << "the premise moved: the refusal did not fail it";
    }

    DispatchOutcome Out(const std::string& line, Session* session = nullptr) {
        return dispatcher_->Dispatch(line, session);
    }
    std::string Run(const std::string& line) { return Out(line).response; }
    void Ok(const std::string& line) {
        const std::string reply = Run(line);
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << line << " -> " << reply;
    }

    // A refusal with its code and the byte of the offending token.
    void ExpectRefused(const std::string& line, StatusCode code, std::size_t byte,
                       Session* session = nullptr) {
        const DispatchOutcome out = Out(line, session);
        EXPECT_EQ(out.status.code(), code) << line << " -> " << out.response;
        EXPECT_EQ(ByteOf(out.response), std::optional<std::size_t>(byte))
            << line << " -> " << out.response;
    }

    std::optional<catalog::SysPatternRow> RowOf(std::uint64_t fetch_id) {
        auto row = boot_->catalog.GetSysPatternRow(fetch_id);
        if (!row.ok()) return std::nullopt;
        return row.value();
    }
    bool Opted(std::uint64_t fetch_id) {
        const auto row = RowOf(fetch_id);
        return row.has_value() && (row->flags & catalog::kPatternWaystone) != 0;
    }
    // A lapsed opted row - another fingerprint version's - planted on page 9.
    void PlantLapsed(std::uint64_t fetch_id) {
        auto bytes = store_.Get(catalog::kCatalogPagePatterns);
        ASSERT_TRUE(bytes.ok());
        heap::PageView page(bytes.value().bytes());
        catalog::SysPatternRow row{};
        row.oid = 700'000 + fetch_id;
        row.fetch_id = fetch_id;
        row.fingerprint_version = parser::kFingerprintVersion + 1;
        row.waystone_root = kInvalidPageId;
        row.flags = catalog::kPatternWaystone;
        auto encoded = row.Encode();
        ASSERT_TRUE(page.InsertTuple(encoded, catalog::kBootstrapXid).ok());
    }
    // A lapsed opted row of `fetch_id` on a page of its own, chained after
    // the last page of `sys.patterns`, so a walk meets it after every row
    // page 9 holds. Answers the page's id.
    PageId ChainLapsedPage(std::uint64_t fetch_id) {
        PageId last = catalog::kCatalogPagePatterns;
        for (;;) {
            auto bytes = store_.Get(last);
            EXPECT_TRUE(bytes.ok());
            if (!bytes.ok()) return kInvalidPageId;
            const PageId next = heap::PageView(bytes.value().bytes()).next_page_id();
            if (next == kInvalidPageId) break;
            last = next;
        }
        auto created = store_.CreateNew();
        EXPECT_TRUE(created.ok());
        if (!created.ok()) return kInvalidPageId;
        const PageId tail = created.value().first;
        auto fresh = heap::PageView::CreateEmpty(created.value().second.bytes(), 0,
                                                 catalog::kSysPatternsTable);
        EXPECT_TRUE(fresh.ok());
        if (!fresh.ok()) return kInvalidPageId;
        catalog::SysPatternRow row{};
        row.oid = 800'000 + fetch_id;
        row.fetch_id = fetch_id;
        row.fingerprint_version = parser::kFingerprintVersion + 1;
        row.waystone_root = kInvalidPageId;
        row.flags = catalog::kPatternWaystone;
        auto encoded = row.Encode();
        EXPECT_TRUE(fresh.value().InsertTuple(encoded, catalog::kBootstrapXid).ok());
        auto link = store_.Get(last);
        EXPECT_TRUE(link.ok());
        if (!link.ok()) return kInvalidPageId;
        heap::PageView(link.value().bytes()).set_next_page_id(tail);
        return tail;
    }

    storage::InMemoryPageStore inner_{kFirstUserPageId};
    DirectoryWatchStore store_{inner_};
    MemoryLogSink sink_;
    ManualWallClock clock_{1'700'000'000};
    Logger logger_{&sink_, clock_, LogLevel::kTrace};
    std::optional<bootstrap::BootstrapResult> boot_;
    // Above the manager, which gives its borrows back through it at teardown.
    std::unique_ptr<txn::LockTable> locks_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<stats::TrailRecorder> recorder_;
    std::optional<CommandDispatcher> dispatcher_;
};

const std::string kPoint = "SELECT * FROM t WHERE id = 3";

// ---- The surface ---------------------------------------------------------

TEST_F(WaystoneOptInTest, OnAndOffRoundTripAndASecondOfEitherIsUnchanged) {
    const std::string id = std::to_string(FetchIdOf(kPoint));
    EXPECT_EQ(Run("WAYSTONE ON " + kPoint),
              "OK WAYSTONE ON fetch_id=" + id + " waystone_recording=on waystone_replay=on");
    EXPECT_TRUE(Opted(FetchIdOf(kPoint)));
    // The unheld pre-read finds the row rooted, so a second ON makes no
    // page: one made here would be unused and leak on every redundant ON.
    const int made = store_.directories_made();
    EXPECT_EQ(Run("WAYSTONE ON " + kPoint),
              "OK WAYSTONE ON fetch_id=" + id +
                  " waystone_recording=on waystone_replay=on unchanged");
    EXPECT_EQ(store_.directories_made(), made) << "a redundant ON made a directory page";
    for (const std::string& line : sink_.lines) {
        EXPECT_EQ(line.find("leaked by WAYSTONE ON"), std::string::npos) << line;
    }
    EXPECT_EQ(Run("waystone off " + kPoint), "OK WAYSTONE OFF fetch_id=" + id);
    EXPECT_FALSE(Opted(FetchIdOf(kPoint)));
    EXPECT_EQ(Run("WAYSTONE OFF " + kPoint), "OK WAYSTONE OFF fetch_id=" + id + " unchanged");

    // A shape never opted, under both spellings: no row, nothing written.
    const std::string other = "SELECT v FROM t WHERE id = 1 LIMIT 1";
    const std::string other_id = std::to_string(FetchIdOf(other));
    EXPECT_EQ(Run("WAYSTONE OFF " + other), "OK WAYSTONE OFF fetch_id=" + other_id + " unchanged");
    EXPECT_EQ(Run("WAYSTONE OFF " + other_id),
              "OK WAYSTONE OFF fetch_id=" + other_id + " unchanged");
    EXPECT_FALSE(RowOf(FetchIdOf(other)).has_value()) << "an OFF wrote a row";
}

// The example's `fetch_id` is the bare SELECT's - the prefix moves no
// fingerprint, as ANALYZE's does not - and OFF takes it in ANALYZE's hex or
// in the view's decimal.
TEST_F(WaystoneOptInTest, OffTakesTheFetchIdInHexAndInDecimal) {
    const std::uint64_t fetch_id = FetchIdOf(kPoint);
    ASSERT_EQ(Run("WAYSTONE ON " + kPoint).rfind("OK WAYSTONE ON fetch_id=" +
                                                 std::to_string(fetch_id) + " ",
                                                 0),
              0u);
    const std::string analyzed = Run("ANALYZE " + kPoint);
    ASSERT_NE(analyzed.find(" fetch_id=0x" + Hex(fetch_id)), std::string::npos) << analyzed;

    EXPECT_EQ(Run("WAYSTONE OFF 0x" + Hex(fetch_id)),
              "OK WAYSTONE OFF fetch_id=" + std::to_string(fetch_id));
    EXPECT_FALSE(Opted(fetch_id));
    ASSERT_EQ(Run("WAYSTONE ON " + kPoint).rfind("OK WAYSTONE ON", 0), 0u);
    EXPECT_EQ(Run("WAYSTONE OFF " + std::to_string(fetch_id)),
              "OK WAYSTONE OFF fetch_id=" + std::to_string(fetch_id));
    EXPECT_FALSE(Opted(fetch_id));
}

// `OFF <select>` only fingerprints: it clears what ON set after the
// relation is gone, and through a `?` the literal form shares a fetch with.
TEST_F(WaystoneOptInTest, OffByExampleNeedsNoRelationAndTakesAPlaceholder) {
    const std::string id = std::to_string(FetchIdOf(kPoint));
    ASSERT_EQ(Run("WAYSTONE ON " + kPoint).rfind("OK WAYSTONE ON", 0), 0u);
    Ok("DROP TABLE t");
    EXPECT_EQ(Run("WAYSTONE OFF SELECT * FROM t WHERE id = ?"), "OK WAYSTONE OFF fetch_id=" + id);
    EXPECT_FALSE(Opted(FetchIdOf(kPoint)));
}

// A lapsed opted row - another fingerprint version's - is reached by number.
TEST_F(WaystoneOptInTest, OffByNumberReachesALapsedRow) {
    constexpr std::uint64_t kLapsed = 0x5151;
    PlantLapsed(kLapsed);
    EXPECT_EQ(Run("WAYSTONE OFF " + std::to_string(kLapsed)),
              "OK WAYSTONE OFF fetch_id=" + std::to_string(kLapsed));
    auto rows = boot_->catalog.ListPatterns();
    ASSERT_TRUE(rows.ok());
    for (const catalog::SysPatternRow& row : rows.value()) {
        if (row.fetch_id == kLapsed) EXPECT_EQ(row.flags & catalog::kPatternWaystone, 0);
    }
}

// ---- What ON admits -------------------------------------------------------

// Drawn at the shape: a SELECT with no tuple key equality has no step a
// trail serves under any plan, for good - `Unsupported`, at its SELECT.
// One that admits a descent and compiles to none now is accepted: a
// negative literal leaves `id = -5` a scan this plan, under the fetch a
// positive literal shares.
TEST_F(WaystoneOptInTest, AShapeWithNoTupleKeyEqualityIsRefusedAndANegativeLiteralIsNot) {
    ExpectRefused("WAYSTONE ON SELECT * FROM t WHERE v = 30", StatusCode::kUnsupported, 12);
    ExpectRefused("WAYSTONE ON SELECT * FROM t", StatusCode::kUnsupported, 12);
    ExpectRefused("WAYSTONE ON SELECT a.v FROM t AS a JOIN u AS b ON a.v = b.w",
                  StatusCode::kUnsupported, 12);
    // A range on the key is never a lookup, and the key against its own
    // relation's column binds nothing before the step that reads both.
    ExpectRefused("WAYSTONE ON SELECT * FROM t WHERE id > 3", StatusCode::kUnsupported, 12);
    ExpectRefused("WAYSTONE ON SELECT * FROM t WHERE id = v", StatusCode::kUnsupported, 12);
    EXPECT_FALSE(RowOf(FetchIdOf("SELECT * FROM t WHERE v = 30")).has_value());

    const std::string negative = "SELECT * FROM t WHERE id = -5";
    ASSERT_EQ(FetchIdOf(negative), FetchIdOf(kPoint)) << "the premise moved";
    const std::string analyzed = Run("ANALYZE " + negative);
    ASSERT_EQ(analyzed.find("Lookup"), std::string::npos) << "the premise moved: " << analyzed;
    EXPECT_EQ(Run("WAYSTONE ON " + negative).rfind("OK WAYSTONE ON", 0), 0u);
    EXPECT_TRUE(Opted(FetchIdOf(kPoint)));
    // And the run-time guard keeps the scan from recording.
    for (int i = 0; i < 3; ++i) Run(negative);
    EXPECT_EQ(recorder_->stats().trails_written, 0u);

    // A join on the key admits a probe whichever relation drives, and
    // whichever side of the equality the key is written on; a key equality
    // inside a subquery - uncorrelated, or against an enclosing row - is a
    // descent of the example's too (§8.3: "in the example or a subquery of
    // it").
    for (const char* admitted :
         {"SELECT a.v FROM t AS a JOIN u AS b ON a.id = b.id",
          "SELECT a.v FROM t AS a JOIN u AS b ON a.v = b.id",
          "SELECT * FROM t WHERE v IN (SELECT w FROM u WHERE id = 3)",
          "SELECT * FROM t WHERE EXISTS (SELECT w FROM u WHERE u.id = t.v)"}) {
        EXPECT_EQ(Run(std::string("WAYSTONE ON ") + admitted).rfind("OK WAYSTONE ON", 0), 0u)
            << admitted;
    }
}

// ---- The refusal table (§8.3), one cell per row ----------------------------

TEST_F(WaystoneOptInTest, TheRoleGateAnswersBeforeEveryOtherGate) {
    Session writer;
    writer.set_role(Role::kReadWrite);
    // A failed transaction too, so the order is pinned: role first.
    Fail(writer);
    const DispatchOutcome out = Out("WAYSTONE ON " + kPoint, &writer);
    EXPECT_EQ(out.response.rfind("ERR permission: WAYSTONE needs admin", 0), 0u) << out.response;
    EXPECT_FALSE(ByteOf(out.response).has_value()) << out.response;
    EXPECT_FALSE(Opted(FetchIdOf(kPoint)));
}

TEST_F(WaystoneOptInTest, AFailedTransactionAnswersWithItsOwnRefusalBeforeTheHandler) {
    Session admin;
    Fail(admin);
    for (const std::string line : {"WAYSTONE ON " + kPoint, std::string("WAYSTONE OFF 1")}) {
        const DispatchOutcome out = Out(line, &admin);
        EXPECT_EQ(out.response.rfind("ERR current transaction is aborted", 0), 0u)
            << line << " -> " << out.response;
    }
    EXPECT_FALSE(Opted(FetchIdOf(kPoint)));
}

// CF-O-Q7 (a): autocommit only, refused before anything is read, so the
// transaction it meets is not poisoned - it still reads and commits.
TEST_F(WaystoneOptInTest, InsideATransactionItIsNotImplementedAndPoisonsNothing) {
    Session admin;
    ASSERT_EQ(Out("BEGIN", &admin).response.rfind("ERR", 0), std::string::npos);
    ExpectRefused("WAYSTONE ON " + kPoint, StatusCode::kNotImplemented, 0, &admin);
    ExpectRefused("WAYSTONE OFF " + kPoint, StatusCode::kNotImplemented, 0, &admin);
    EXPECT_FALSE(admin.failed()) << "the refusal poisoned the transaction";
    EXPECT_EQ(Out("SELECT v FROM t WHERE id = 3", &admin).response, "v\\n30");
    EXPECT_EQ(Out("COMMIT", &admin).response.rfind("ERR", 0), std::string::npos);
    EXPECT_FALSE(Opted(FetchIdOf(kPoint)));
}

TEST_F(WaystoneOptInTest, AMissingOrUnknownSecondWordIsInvalidAtThatWord) {
    ExpectRefused("WAYSTONE", StatusCode::kInvalidArgument, 8);
    ExpectRefused("WAYSTONE MAYBE " + kPoint, StatusCode::kInvalidArgument, 9);
    ExpectRefused("WAYSTONE ON", StatusCode::kInvalidArgument, 11);
}

TEST_F(WaystoneOptInTest, OnTakesNoNumberAndOffRefusesAMalformedOne) {
    ExpectRefused("WAYSTONE ON 12345", StatusCode::kInvalidArgument, 12);
    ExpectRefused("WAYSTONE OFF 0xZZ", StatusCode::kInvalidArgument, 13);
    ExpectRefused("WAYSTONE OFF 12ab", StatusCode::kInvalidArgument, 13);
    ExpectRefused("WAYSTONE OFF 0x", StatusCode::kInvalidArgument, 13);
    ExpectRefused("WAYSTONE OFF 0x10000000000000000", StatusCode::kInvalidArgument, 13);
    ExpectRefused("WAYSTONE OFF 18446744073709551616", StatusCode::kInvalidArgument, 13);
    ExpectRefused("WAYSTONE OFF -1", StatusCode::kInvalidArgument, 13);
}

// The parser's own code, its byte shifted by the prefix.
TEST_F(WaystoneOptInTest, AnExampleThatDoesNotParseIsTheParsersRefusalAtItsByte) {
    const std::string bad = "SELECT * FROM t WHERE";
    const auto bare = parser::Parse(bad);
    ASSERT_FALSE(bare.ok());
    const auto at = ByteOf(bare.status().message());
    ASSERT_TRUE(at.has_value()) << bare.status().message();
    ExpectRefused("WAYSTONE ON " + bad, bare.status().code(), *at + 12);
    ExpectRefused("WAYSTONE OFF " + bad, bare.status().code(), *at + 13);
}

TEST_F(WaystoneOptInTest, AWriteOrAQueryLayerStatementIsNotImplementedAtItsLeadingWord) {
    for (const char* write : {"INSERT INTO t VALUES (1)", "UPDATE t SET v = 1 WHERE id = 1",
                              "DELETE FROM t WHERE id = 1",
                              "WITH x AS (SELECT * FROM t) SELECT * FROM x"}) {
        ExpectRefused(std::string("WAYSTONE ON ") + write, StatusCode::kNotImplemented, 12);
        ExpectRefused(std::string("WAYSTONE OFF ") + write, StatusCode::kNotImplemented, 13);
    }
}

TEST_F(WaystoneOptInTest, AnyOtherLeadingWordIsInvalidAnalyzeIncluded) {
    ExpectRefused("WAYSTONE ON ANALYZE " + kPoint, StatusCode::kInvalidArgument, 12);
    ExpectRefused("WAYSTONE OFF SHOW PATTERNS", StatusCode::kInvalidArgument, 13);
}

TEST_F(WaystoneOptInTest, AnExampleThatDoesNotCompileIsTheCompilersRefusalAtItsByte) {
    const std::string bad = "SELECT * FROM nosuch WHERE id = 1";
    const DispatchOutcome bare = Out(bad);
    ASSERT_FALSE(bare.status.ok()) << bare.response;
    const auto at = ByteOf(bare.response);
    ASSERT_TRUE(at.has_value()) << bare.response;
    ExpectRefused("WAYSTONE ON " + bad, bare.status.code(), *at + 12);
}

// A SELECT only the query layer reads has no step chain to record.
TEST_F(WaystoneOptInTest, ASelectTheNativePathDeclinesIsNotImplemented) {
    const std::string layered = "SELECT DISTINCT v FROM t WHERE id = 3";
    const auto native = parser::Parse(layered);
    ASSERT_FALSE(native.ok()) << "the premise moved: the native grammar reads DISTINCT";
    const DispatchOutcome out = Out("WAYSTONE ON " + layered);
    EXPECT_EQ(out.status.code(), StatusCode::kNotImplemented) << out.response;
    EXPECT_EQ(ByteOf(out.response), ByteOf(native.status().message()).value() + 12)
        << out.response;
}

// CF-O-Q11 (a): refused at the `?`, which OFF accepts (above).
TEST_F(WaystoneOptInTest, APlaceholderIsNotImplementedAtItsByte) {
    const std::string line = "WAYSTONE ON SELECT * FROM t WHERE id = ?";
    ExpectRefused(line, StatusCode::kNotImplemented, line.find('?'));
}

// A full pool refuses the directory's page before page 9 is touched.
TEST_F(WaystoneOptInTest, ADirectoryPageAFullPoolRefusesIsResourceExhaustedAtByteZero) {
    store_.Refuse(true);
    ExpectRefused("WAYSTONE ON " + kPoint, StatusCode::kResourceExhausted, 0);
    store_.Refuse(false);
    EXPECT_FALSE(RowOf(FetchIdOf(kPoint)).has_value()) << "a refused ON wrote its row";
}

// **§8.3's `TxnConflict` row, against the tree**: a bind borrow is a read
// borrow (`read_borrow.hpp`), and a refused one is recorded and never
// fails its statement - the defence is one-directional, a DDL waits for a
// reader and a reader does not wait for a DDL (`txn.md` §5). So the
// example compiles while a DDL holds the relation `X`, as the same SELECT
// runs, and the opt-in is written. Pinned so a change to that rule meets
// this cell.
TEST_F(WaystoneOptInTest, ABindBorrowRefusedIsRecordedAndTheOptInStillRuns) {
    auto oid = boot_->catalog.FindTableOidByName(catalog::kNamespacePublic, "t");
    ASSERT_TRUE(oid.ok());
    txn::LockHoldings ddl;
    constexpr std::uint64_t kDdl = 9'000'001;
    ASSERT_TRUE(locks_->Acquire(kDdl, txn::LockKey::Relation(oid.value()),
                                txn::LockMode::kExclusive, ddl)
                    .value()
                    .granted);
    EXPECT_EQ(Run(kPoint), "id,v\\n3,30") << "the premise moved: a reader waited for the DDL";
    EXPECT_EQ(Run("WAYSTONE ON " + kPoint).rfind("OK WAYSTONE ON", 0), 0u);
    locks_->Release(kDdl, ddl);
    EXPECT_TRUE(Opted(FetchIdOf(kPoint)));
}

// ---- The directory, the hold and the leak ---------------------------------

// The page is made with no `sys.patterns` page pinned - never under page
// 9's hold - and a concurrent ON that rooted the row between the unheld
// read and the hold leaves this one's page unused, logged as leaked.
TEST_F(WaystoneOptInTest, TheDirectoryIsMadeBeforeTheHoldAndAnUnusedOneIsLoggedAsLeaked) {
    const std::uint64_t fetch_id = FetchIdOf(kPoint);
    auto theirs = stats::CreateDirPage(store_);
    ASSERT_TRUE(theirs.ok());
    store_.OnCreate([&] {
        auto wrote = boot_->catalog.SetPatternWaystone(fetch_id, catalog::kStmtClassUnclassified,
                                                       /*on=*/true, theirs.value());
        ASSERT_TRUE(wrote.ok()) << wrote.status().message();
    });
    const int before = store_.directories_made();
    const std::string reply = Run("WAYSTONE ON " + kPoint);
    EXPECT_EQ(store_.directories_made(), before + 1);
    EXPECT_EQ(store_.made_under_a_pattern_pin(), 0) << "a directory was made under page 9";
    EXPECT_NE(reply.find(" unchanged"), std::string::npos) << reply;
    EXPECT_EQ(RowOf(fetch_id)->waystone_root, theirs.value()) << "the second root replaced the first";
    bool leaked = false;
    for (const std::string& line : sink_.lines) {
        if (line.find("[waystone]") != std::string::npos &&
            line.find("leaked by WAYSTONE ON") != std::string::npos) {
            leaked = true;
        }
    }
    EXPECT_TRUE(leaked) << "the unused page was not logged";
}

// `waystone_recording` off: no recorder, so nothing would record into a
// directory - ON sets the bit, makes none, and says what the ceilings are.
TEST_F(WaystoneOptInTest, WithRecordingOffAndReplayOnOnMakesNoDirectory) {
    MakeDispatcher(/*record=*/false, /*replay=*/true);
    const int before = store_.directories_made();
    const std::string reply = Run("WAYSTONE ON " + kPoint);
    EXPECT_EQ(reply, "OK WAYSTONE ON fetch_id=" + std::to_string(FetchIdOf(kPoint)) +
                         " waystone_recording=off waystone_replay=on");
    EXPECT_EQ(store_.directories_made(), before);
    const auto row = RowOf(FetchIdOf(kPoint));
    ASSERT_TRUE(row.has_value());
    EXPECT_NE(row->flags & catalog::kPatternWaystone, 0);
    EXPECT_FALSE(catalog::HasWaystoneDirectory(*row));
}

// ---- ANALYZE and sys.meta ---------------------------------------------------

// A dispatcher built with no optimizer surface, and a statement nobody
// opted: ANALYZE still names its pattern and its fetch.
TEST_F(WaystoneOptInTest, AnalyzeOfANotOptedSelectStillPrintsItsIds) {
    const auto fp = parser::FingerprintOf(kPoint);
    ASSERT_TRUE(fp.has_value());
    const std::string analyzed = Run("ANALYZE " + kPoint);
    EXPECT_NE(analyzed.find(" pattern_id=0x" + Hex(fp->pattern_id)), std::string::npos)
        << analyzed;
    EXPECT_NE(analyzed.find(" fetch_id=0x" + Hex(fp->fetch_id)), std::string::npos) << analyzed;
}

TEST_F(WaystoneOptInTest, SysMetaCarriesTheCeilingsAndTheOptedCount) {
    const std::string before = Run("SHOW META");
    EXPECT_EQ(testing_view::MetaField(before, "waystone_recording"), "on") << before;
    EXPECT_EQ(testing_view::MetaField(before, "waystone_replay"), "on") << before;
    EXPECT_EQ(testing_view::MetaField(before, "waystone_opted"), "0") << before;
    ASSERT_EQ(Run("WAYSTONE ON " + kPoint).rfind("OK WAYSTONE ON", 0), 0u);
    EXPECT_EQ(testing_view::MetaField(Run("SHOW META"), "waystone_opted"), "1");
    ASSERT_EQ(Run("WAYSTONE OFF " + kPoint).rfind("OK WAYSTONE OFF", 0), 0u);
    EXPECT_EQ(testing_view::MetaField(Run("SHOW META"), "waystone_opted"), "0");
}

// ---- The gate and the opted count (CF-S1c) --------------------------------

// **A not-opted `SELECT` pays nothing of Waystone's**: with nothing opted it
// fills no pattern map and is never sighted; with another shape opted it is
// one hash miss in the filled map - no fill, no scan, no sighting.
TEST_F(WaystoneOptInTest, ANotOptedSelectFillsNoPatternMapAndIsNeverSighted) {
    // ANALYZE gives the statement an identity, as a server's optimizer
    // surface gives every `SELECT` one: the collector's gate is the opt-in,
    // not the identity.
    const auto run_not_opted = [&] {
        for (int i = 0; i < 3; ++i) EXPECT_EQ(Run(kPoint), "id,v\\n3,30");
        for (int i = 0; i < 2; ++i) Run("ANALYZE " + kPoint);
    };
    run_not_opted();
    EXPECT_EQ(boot_->catalog.cache_stats().pattern_fills, 0u) << "nothing opted, and a fill ran";
    EXPECT_EQ(recorder_->stats().sightings, 0u) << "a shape nobody opted was sighted";

    const std::string other = "SELECT w FROM u WHERE id = 3";
    ASSERT_EQ(Run("WAYSTONE ON " + other).rfind("OK WAYSTONE ON", 0), 0u);
    Run(other);
    Run(other);
    const std::uint64_t fills = boot_->catalog.cache_stats().pattern_fills;
    const std::uint64_t sightings = recorder_->stats().sightings;
    ASSERT_EQ(fills, 1u) << "the opted shape's first ask did not fill, or filled twice";
    ASSERT_EQ(sightings, 2u) << "the opted shape was not sighted";

    run_not_opted();
    EXPECT_EQ(boot_->catalog.cache_stats().pattern_fills, fills) << "a not-opted miss scanned";
    EXPECT_EQ(recorder_->stats().sightings, sightings) << "a not-opted shape was sighted";
    EXPECT_EQ(recorder_->stats().write_failures, 0u);
    // And the control still replays.
    EXPECT_NE(Run("ANALYZE " + other).find("replays=1"), std::string::npos);

    // A scan-only statement asks no map - invariant 9 keeps Waystone off
    // it - so even as a generation's first statement it fills nothing.
    const std::uint64_t settled = boot_->catalog.cache_stats().pattern_fills;
    Ok("CREATE TABLE w (id int64) BTREE");  // the word moves: a new generation
    Ok("SELECT * FROM u");
    EXPECT_EQ(boot_->catalog.cache_stats().pattern_fills, settled) << "a scan-only SELECT filled";
    Run(other);
    EXPECT_EQ(boot_->catalog.cache_stats().pattern_fills, settled + 1) << "the premise moved";
}

// **A failed lookup is counted, not silent** (CF-S1c): a `sys.patterns`
// read the gate's fill cannot make leaves the statement unrecorded and its
// answer unchanged, and shows in the recorder's `write_failures`.
TEST_F(WaystoneOptInTest, AFillThatCannotReadIsCountedAndChangesNoReply) {
    const std::string other = "SELECT w FROM u WHERE id = 3";
    ASSERT_EQ(Run("WAYSTONE ON " + other).rfind("OK WAYSTONE ON", 0), 0u);
    const std::string reply = Run(other);
    Ok("CREATE TABLE w (id int64) BTREE");  // a new generation: the next ask fills
    store_.RefusePatternReads(true);
    EXPECT_EQ(Run(other), reply);
    EXPECT_EQ(Run(other), reply);
    store_.RefusePatternReads(false);
    EXPECT_EQ(recorder_->stats().write_failures, 2u) << "a failed fill went uncounted";
}

// **The count is the view's**, through every way a WAYSTONE statement can
// end: an ON, a second ON, an OFF, an OFF of a lapsed row (no move - the
// view reads it false), an ON whose insert failed under page 9's hold (no
// move), and an OFF whose walk failed after clearing one row (one move, and
// the word moves). `sys.meta` and the catalog read the same word, and the
// mount's renewal sets it from the chain. An overwrite whose record fails
// is `WaystoneDurabilityTest`'s, which has a log to fail.
TEST_F(WaystoneOptInTest, TheOptedCountMatchesTheViewThroughEveryWrite) {
    const auto expect_matches = [&](const std::string& when, std::size_t want) {
        const std::size_t view = ViewOpted(Run("SHOW PATTERNS"));
        EXPECT_EQ(view, want) << when;
        EXPECT_EQ(boot_->catalog.opted_patterns(), view) << when;
        EXPECT_EQ(testing_view::MetaField(Run("SHOW META"), "waystone_opted"),
                  std::to_string(view))
            << when;
    };
    const std::string other = "SELECT w FROM u WHERE id = 3";
    expect_matches("at the start", 0);
    ASSERT_EQ(Run("WAYSTONE ON " + kPoint).rfind("OK WAYSTONE ON", 0), 0u);
    expect_matches("after ON", 1);
    ASSERT_NE(Run("WAYSTONE ON " + kPoint).find(" unchanged"), std::string::npos);
    expect_matches("after an unchanged ON", 1);
    ASSERT_EQ(Run("WAYSTONE ON " + other).rfind("OK WAYSTONE ON", 0), 0u);
    expect_matches("after a second shape's ON", 2);
    ASSERT_EQ(Run("WAYSTONE OFF " + kPoint).rfind("OK WAYSTONE OFF", 0), 0u);
    expect_matches("after OFF", 1);

    constexpr std::uint64_t kLapsed = 0x6161;
    PlantLapsed(kLapsed);
    expect_matches("with a lapsed row planted", 1);
    ASSERT_EQ(Run("WAYSTONE OFF " + std::to_string(kLapsed)),
              "OK WAYSTONE OFF fetch_id=" + std::to_string(kLapsed));
    expect_matches("after OFF <fetch_id> of a lapsed row", 1);

    // **A write that fails after the decision moves nothing**: page 9's
    // hold is let through and the write made under it refused. An ON of a
    // shape's first row makes three write fetches of page 9 - the hold, the
    // walk for a current row, the insert - and the third is refused.
    const std::string third = "SELECT * FROM t WHERE id = 3 LIMIT 1";
    store_.RefuseWritesOf(catalog::kCatalogPagePatterns, /*passes=*/2);
    const DispatchOutcome insert = Out("WAYSTONE ON " + third);
    const int insert_refusals = store_.write_refusals();
    store_.RefuseWritesOf(kInvalidPageId);
    EXPECT_FALSE(insert.status.ok()) << "the premise moved: " << insert.response;
    EXPECT_EQ(insert_refusals, 1) << "the premise moved: the refusal missed the insert: "
                                  << insert.response;
    expect_matches("after an ON whose insert failed", 1);
    EXPECT_FALSE(RowOf(FetchIdOf(third)).has_value());

    // **OFF's walk failing after it cleared an earlier row**: a lapsed opted
    // row of `other`'s fetch_id on a page chained after page 9, whose write
    // fetch is refused. The current row on page 9 was cleared and counted
    // down, the lapsed one was never reached, and the word moves for the
    // write that stands, or every cache would keep the bit it cleared.
    const PageId tail = ChainLapsedPage(FetchIdOf(other));
    ASSERT_NE(tail, kInvalidPageId);
    const std::uint64_t version = boot_->catalog.catalog_version();
    store_.RefuseWritesOf(tail);
    const DispatchOutcome partial = Out("WAYSTONE OFF " + other);
    const int partial_refusals = store_.write_refusals();
    store_.RefuseWritesOf(kInvalidPageId);
    EXPECT_FALSE(partial.status.ok()) << "the premise moved: " << partial.response;
    EXPECT_EQ(partial_refusals, 1) << "the premise moved: the walk never reached the tail";
    EXPECT_GT(boot_->catalog.catalog_version(), version) << "a partial OFF moved no word";
    expect_matches("after an OFF that failed past its first row", 0);
    EXPECT_FALSE(Opted(FetchIdOf(other)));
    EXPECT_FALSE(Opted(FetchIdOf(kPoint)));

    ASSERT_EQ(Run("WAYSTONE ON " + kPoint).rfind("OK WAYSTONE ON", 0), 0u);
    ASSERT_TRUE(DiscardAdvisoryState(boot_->catalog, /*waystone_recording=*/true).ok());
    expect_matches("after the mount's renewal", 1);
}

// ---- The mount's discard, and the cache in front of it -----------------------

// `[quiet-wrong]` without the renewal's cache drop: a pattern entry filled
// before the discard keeps the previous run's root, whose children can be
// ids handed to logged pages since. Filled here by recording through it,
// then the discard the mount runs, then the entry asked again.
TEST_F(WaystoneOptInTest, APatternCachedBeforeTheDiscardAnswersTheRenewedRoot) {
    ASSERT_EQ(Run("WAYSTONE ON " + kPoint).rfind("OK WAYSTONE ON", 0), 0u);
    Run(kPoint);
    Run(kPoint);
    ASSERT_NE(Run("ANALYZE " + kPoint).find("replays=1"), std::string::npos);
    auto cached = boot_->catalog.FindOptedPattern(FetchIdOf(kPoint));
    ASSERT_TRUE(cached.ok());
    const PageId old_root = cached.value()->waystone_root;

    ASSERT_TRUE(DiscardAdvisoryState(boot_->catalog, /*waystone_recording=*/true).ok());

    auto after = boot_->catalog.FindOptedPattern(FetchIdOf(kPoint));
    ASSERT_TRUE(after.ok());
    EXPECT_NE(after.value()->waystone_root, old_root) << "the cache kept the previous root";
    EXPECT_EQ(after.value()->waystone_root, RowOf(FetchIdOf(kPoint))->waystone_root);
    EXPECT_EQ(Run("ANALYZE " + kPoint).find("replays="), std::string::npos)
        << "a trail survived the discard";
}

// §8.3 Mount: a renewal whose create fails clears the row's root, keeps its
// bit, logs the failure, and still opens `[quiet-wrong]` if the root were
// kept - the previous run's, whose children can be ids handed to logged
// pages since.
TEST_F(WaystoneOptInTest, ARenewalWhoseCreateFailsClearsTheRootAndKeepsTheBit) {
    const std::uint64_t fetch_id = FetchIdOf(kPoint);
    ASSERT_EQ(Run("WAYSTONE ON " + kPoint).rfind("OK WAYSTONE ON", 0), 0u);
    ASSERT_TRUE(catalog::HasWaystoneDirectory(*RowOf(fetch_id))) << "the premise moved";

    store_.Refuse(true);
    const Status renewed = DiscardAdvisoryState(boot_->catalog, /*waystone_recording=*/true);
    store_.Refuse(false);
    ASSERT_TRUE(renewed.ok()) << "a failed create refused the mount: " << renewed.message();

    const auto row = RowOf(fetch_id);
    ASSERT_TRUE(row.has_value());
    EXPECT_NE(row->flags & catalog::kPatternWaystone, 0) << "the bit went with the directory";
    EXPECT_FALSE(catalog::HasWaystoneDirectory(*row)) << "the previous run's root was kept";
    auto cached = boot_->catalog.FindOptedPattern(fetch_id);
    ASSERT_TRUE(cached.ok()) << cached.status().message();
    EXPECT_FALSE(cached.value()->has_waystone_directory()) << "the cache kept the root";
    bool logged = false;
    for (const std::string& line : sink_.lines) {
        if (line.find("[waystone]") != std::string::npos &&
            line.find("has no directory this run") != std::string::npos) {
            logged = true;
        }
    }
    EXPECT_TRUE(logged) << "the failed create was not logged";
}

// ---- Durability of the answer (§8.3) -----------------------------------------

constexpr std::uint64_t kSegmentSize = 1 << 20;

class WaystoneDurabilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto page_device = storage::MemoryPageDevice::Create(64, 64);
        ASSERT_TRUE(page_device.ok());
        device_ = std::move(page_device.value());
        auto log_device = wal::MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(log_device.ok());
        log_device_ = std::move(log_device.value());
        auto wal = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0);
        ASSERT_TRUE(wal.ok()) << wal.status().message();
        wal_ = std::move(wal.value());
        auto store = storage::DevicePageStore::Open(*device_, storage::FrameCapacity{4096},
                                                    kFirstUserPageId);
        ASSERT_TRUE(store.ok());
        store_ = std::move(store.value());
        store_->SetWalGate(wal_.get());
        auto boot = bootstrap::BootstrapDatabase(*store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        boot_->catalog.SetWal(wal_.get());
        recorder_.emplace(boot_->catalog, *store_);
        for (const auto rung : {wal::DurabilityClass::kRelaxed, wal::DurabilityClass::kGroup}) {
            dispatchers_.emplace_back(std::make_unique<CommandDispatcher>(
                boot_->superblock, boot_->catalog, *store_, /*log=*/nullptr, /*clock=*/nullptr,
                wal_.get(), rung, exec::Budget(), &*recorder_));
        }
        ASSERT_EQ(Relaxed("CREATE TABLE t (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
        ASSERT_EQ(Relaxed("INSERT INTO t VALUES (10)").rfind("INSERTED", 0), 0u);
    }

    std::string Relaxed(const std::string& line) {
        return dispatchers_[0]->Dispatch(line).response;
    }
    std::string Group(const std::string& line) { return dispatchers_[1]->Dispatch(line).response; }
    // The statement without the wait, so the park is what is read: with no
    // writer thread, `RequestSyncNow` syncs inline and a parked answer is
    // already durable by the time `Dispatch` returns, so durability alone
    // cannot tell a parked answer from one that never parked.
    DispatchOutcome Staged(std::size_t rung, const std::string& line) {
        return dispatchers_[rung]->DispatchAndStage(line, nullptr);
    }

    sched::ManualClock clock_;
    std::unique_ptr<storage::MemoryPageDevice> device_;
    std::unique_ptr<wal::MemoryLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<stats::TrailRecorder> recorder_;
    std::vector<std::unique_ptr<CommandDispatcher>> dispatchers_;
};

// `relaxed` answers at once; `group`'s OK waits for its record; and an
// `unchanged` at `group` waits for the record of whichever statement set
// the state it reports - here the relaxed ON, not yet synced.
TEST_F(WaystoneDurabilityTest, AnUnchangedAnswerAtGroupParksUntilTheStateItReportsIsDurable) {
    const std::string sql = "SELECT * FROM t WHERE id = 1";
    const DispatchOutcome relaxed = Staged(0, "WAYSTONE ON " + sql);
    ASSERT_EQ(relaxed.response.rfind("OK WAYSTONE ON", 0), 0u) << relaxed.response;
    EXPECT_EQ(relaxed.pending_lsn, wal::kNoLsn) << "a relaxed answer parked";
    const wal::Lsn relaxed_bound = wal_->appended_lsn() - 1;
    ASSERT_FALSE(wal_->IsDurable(relaxed_bound)) << "the relaxed ON was synced; nothing to wait on";

    const DispatchOutcome unchanged = Staged(1, "WAYSTONE ON " + sql);
    ASSERT_NE(unchanged.response.find(" unchanged"), std::string::npos) << unchanged.response;
    EXPECT_NE(unchanged.pending_lsn, wal::kNoLsn)
        << "an unchanged answer at group did not park on the state it reports";
    EXPECT_GE(unchanged.pending_lsn, relaxed_bound)
        << "an unchanged answer at group parked short of the record that set its state";
    EXPECT_TRUE(wal_->IsDurable(relaxed_bound));

    const std::string other = "SELECT v FROM t WHERE id = 1 LIMIT 1";
    const DispatchOutcome on = Staged(1, "WAYSTONE ON " + other);
    ASSERT_EQ(on.response.rfind("OK WAYSTONE ON", 0), 0u) << on.response;
    EXPECT_NE(on.pending_lsn, wal::kNoLsn) << "group's OK did not park";
    EXPECT_GE(on.pending_lsn, wal_->appended_lsn() - 1) << "group's OK parked short of its record";
    const DispatchOutcome off = Staged(1, "WAYSTONE OFF " + other);
    ASSERT_EQ(off.response.rfind("OK WAYSTONE OFF", 0), 0u) << off.response;
    EXPECT_NE(off.pending_lsn, wal::kNoLsn) << "group's OFF did not park";
    EXPECT_GE(off.pending_lsn, wal_->appended_lsn() - 1) << "group's OFF parked short of its record";
    // And the wait itself: `Dispatch` answers only once the bound is durable.
    ASSERT_EQ(Group("WAYSTONE ON " + other).rfind("OK WAYSTONE ON", 0), 0u);
    EXPECT_TRUE(wal_->IsDurable(wal_->appended_lsn() - 1)) << "group's OK came back early";
}

// **The opted count moves only after the row's write succeeded** (CF-S1c):
// with the log fail-stopped, an ON over an existing row and an OFF each
// decide, change the page and fail at their record - the decision made,
// the write not - and neither moves the count. The page keeps a change no
// record covers, which is fail-stop's state: nothing is written back and
// the restart recounts from the chain.
TEST_F(WaystoneDurabilityTest, AWriteWhoseRecordFailsMovesNoOptedCount) {
    const std::string sql = "SELECT * FROM t WHERE id = 1";
    const std::string other = "SELECT v FROM t WHERE id = 1 LIMIT 1";
    ASSERT_EQ(Relaxed("WAYSTONE ON " + sql).rfind("OK WAYSTONE ON", 0), 0u);
    ASSERT_EQ(Relaxed("WAYSTONE OFF " + sql).rfind("OK WAYSTONE OFF", 0), 0u);
    ASSERT_EQ(Relaxed("WAYSTONE ON " + other).rfind("OK WAYSTONE ON", 0), 0u);
    ASSERT_EQ(boot_->catalog.opted_patterns(), 1u);

    // Something staged, and its drain refused: the stream stops, and every
    // later append is refused.
    const std::vector<std::byte> filler(16, std::byte{0x5a});
    ASSERT_TRUE(wal_->Append({wal::RecordType::kHeapInsert, wal::kNoTxnId, kInvalidPageId, 0},
                             filler)
                    .ok());
    log_device_->FailNextWrite(Status::IoError("injected: the log cannot be written"));
    ASSERT_FALSE(wal_->Flush().ok());
    ASSERT_TRUE(wal_->stopped()) << "the premise moved: the log did not stop";

    const std::string on = Relaxed("WAYSTONE ON " + sql);
    EXPECT_EQ(on.rfind("ERR", 0), 0u) << "the premise moved: " << on;
    EXPECT_EQ(boot_->catalog.opted_patterns(), 1u) << "an ON whose record failed moved the count";
    const std::string off = Relaxed("WAYSTONE OFF " + other);
    EXPECT_EQ(off.rfind("ERR", 0), 0u) << "the premise moved: " << off;
    EXPECT_EQ(boot_->catalog.opted_patterns(), 1u) << "an OFF whose record failed moved the count";
}

// ---- Across a mount ------------------------------------------------------------

struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("kds_wo_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::string Reply(Expeditor& db, const std::string& line) {
    return db.dispatcher().Dispatch(line).response;
}

// What a mount leaves of the opt-in: the row and its bit, a directory or
// none, and whether the first run replays.
struct AfterMount {
    bool opted = false;
    PageId root = kInvalidPageId;
    PageId cached_root = kInvalidPageId;
    bool replayed = false;
};

AfterMount Inspect(Expeditor& db, const std::string& sql) {
    AfterMount out;
    auto row = db.catalog().GetSysPatternRow(FetchIdOf(sql));
    EXPECT_TRUE(row.ok()) << row.status().message();
    if (!row.ok()) return out;
    out.opted = (row.value().flags & catalog::kPatternWaystone) != 0;
    if (catalog::HasWaystoneDirectory(row.value())) out.root = row.value().waystone_root;
    auto cached = db.catalog().FindOptedPattern(FetchIdOf(sql));
    if (cached.ok() && cached.value()->has_waystone_directory()) {
        out.cached_root = cached.value()->waystone_root;
    }
    out.replayed = Reply(db, "ANALYZE " + sql).find("replays=") != std::string::npos;
    return out;
}

// A live run that opted `sql` in, recorded its trail and saw it replay;
// answers the directory the run recorded into.
PageId OptInAndRecord(Expeditor& db, const std::string& sql) {
    for (const std::string& line : {std::string("CREATE TABLE ws (id int64, v int64) BTREE"),
                                    std::string("INSERT INTO ws VALUES (5)"),
                                    "WAYSTONE ON " + sql, sql, sql}) {
        const std::string reply = Reply(db, line);
        EXPECT_EQ(reply.rfind("ERR", 0), std::string::npos) << line << " -> " << reply;
    }
    EXPECT_NE(Reply(db, "ANALYZE " + sql).find("replays=1"), std::string::npos)
        << "nothing was recorded; the cell proves nothing";
    auto row = db.catalog().GetSysPatternRow(FetchIdOf(sql));
    EXPECT_TRUE(row.ok());
    return row.ok() ? row.value().waystone_root : kInvalidPageId;
}

const std::string kMounted = "SELECT * FROM ws WHERE id = 1";

// **The bit survives a crash at `group`, and the trail does not**; and the
// renewal runs after recovery `[quiet-wrong]`: the opt-in's records are
// redone first, so the row the renewal roots is the recovered one - ordered
// the other way, redo would rewrite the previous run's root over the fresh
// one, and the next trail would write through a directory whose children
// may be ids the cursor has handed to logged pages since.
//
// The image is built for that: `sys.patterns`' page is on the device as it
// stood before the opt-in, so the row comes back from the log alone, while
// the recorded directory - its trail included - and its allocation are
// written back before the crash, so a mount that renewed before redo would
// find no row, and redo would then point the row at a trail that replays.
TEST(WaystoneMountTest, ACrashAtGroupKeepsTheBitRenewsTheDirectoryAndLosesTheTrail) {
    TempDir live, image;
    PageId recorded = kInvalidPageId;
    {
        auto opened = reclaim_census::Mount(live.path, 1, wal::DurabilityClass::kGroup);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        for (const char* line : {"CREATE TABLE ws (id int64, v int64) BTREE",
                                 "INSERT INTO ws VALUES (5)"}) {
            ASSERT_EQ(Reply(db, line).rfind("ERR", 0), std::string::npos) << line;
        }
        reclaim_census::CleanShutdownCheckpoint(db);
        for (const std::string& line : {"WAYSTONE ON " + kMounted, kMounted, kMounted}) {
            const std::string reply = Reply(db, line);
            ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << line << " -> " << reply;
        }
        ASSERT_NE(Reply(db, "ANALYZE " + kMounted).find("replays=1"), std::string::npos)
            << "nothing was recorded; the cell proves nothing";
        auto row = db.catalog().GetSysPatternRow(FetchIdOf(kMounted));
        ASSERT_TRUE(row.ok());
        recorded = row.value().waystone_root;
        ASSERT_TRUE(db.store().WriteBack(std::span<const PageId>(&recorded, 1)).ok());
        ASSERT_TRUE(db.store().PersistMaps().ok());
        reclaim_census::CrashImage(live.path, image.path);
    }
    auto mounted = reclaim_census::Mount(image.path, 1, wal::DurabilityClass::kGroup);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    CurrentCoreGuard as(0);
    const AfterMount after = Inspect(*mounted.value(), kMounted);
    EXPECT_TRUE(after.opted) << "the opt-in did not survive the crash";
    EXPECT_NE(after.root, kInvalidPageId) << "the opted row was given no directory";
    EXPECT_NE(after.root, recorded) << "the previous run's directory survived the mount";
    EXPECT_EQ(after.cached_root, after.root);
    EXPECT_FALSE(after.replayed) << "a trail survived the crash";
}

// `waystone_recording` off at the mount, `waystone_replay` on: the renewal
// clears the root and makes none, and the bit stays for a later mount.
TEST(WaystoneMountTest, AMountWithRecordingOffMakesNoDirectoryAndKeepsTheBit) {
    TempDir live;
    {
        auto opened = reclaim_census::Mount(live.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        CurrentCoreGuard as(0);
        ASSERT_NE(OptInAndRecord(*opened.value(), kMounted), kInvalidPageId);
        reclaim_census::CleanShutdownCheckpoint(*opened.value());
    }
    // This mount logs to a file, so the boot line - one of the three places
    // §8.3's defaults say the ceilings and the opted count are told - is read
    // back. `reclaim_census::Mount`'s config with the log file set.
    const fs::path log_path = live.path / "boot.log";
    {
        Expeditor::Config config;
        config.waystone_recording = false;
        config.buffer_pool_frames = 4096;  // required (BE-R3)
        config.data_file = (live.path / "kds.db").string();
        config.wal_dir = (live.path / "wal").string();
        config.log_file = log_path.string();
        config.cores = 1;
        config.debug_text_port = 0;
        config.checkpoint_interval_ns = 0;
        auto mounted = Expeditor::Open(config, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(mounted.ok()) << mounted.status().message();
        CurrentCoreGuard as(0);
        const AfterMount after = Inspect(*mounted.value(), kMounted);
        EXPECT_TRUE(after.opted);
        EXPECT_EQ(after.root, kInvalidPageId) << "a directory was made with recording off";
        const std::string meta = Reply(*mounted.value(), "SHOW META");
        EXPECT_EQ(testing_view::MetaField(meta, "waystone_recording"), "off") << meta;
        EXPECT_EQ(testing_view::MetaField(meta, "waystone_replay"), "on") << meta;
        EXPECT_EQ(testing_view::MetaField(meta, "waystone_opted"), "1") << meta;
        EXPECT_EQ(ViewOpted(Reply(*mounted.value(), "SHOW PATTERNS")), 1u)
            << "the mount's count is not the view's";
        reclaim_census::CleanShutdownCheckpoint(*mounted.value());
    }
    {
        std::ifstream log(log_path);
        ASSERT_TRUE(log.good()) << "the mount wrote no log at " << log_path;
        bool told = false;
        for (std::string line; std::getline(log, line);) {
            if (line.find("[waystone]") != std::string::npos &&
                line.find("waystone_recording=off waystone_replay=on waystone_opted=1") !=
                    std::string::npos) {
                told = true;
            }
        }
        EXPECT_TRUE(told) << "the boot line did not tell the ceilings and the opted count";
    }
    // Recording on again: the mount roots the row it left without one.
    auto mounted = reclaim_census::Mount(live.path);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    CurrentCoreGuard as(0);
    EXPECT_NE(Inspect(*mounted.value(), kMounted).root, kInvalidPageId);
}

// **A boot that runs no renewal counts the chain itself** (CF-S1c): the sim's
// `skip_recovery` boot skips `DiscardAdvisoryState`, and with it the walk
// that sets the opted count, so without its own counting walk the count
// would stay 0 over the opted row and no `SELECT` would ever ask for it.
TEST(WaystoneSimBootTest, ASkipRecoveryBootCountsTheOptedRowsItFinds) {
    auto opened = sim::SimInstance::Create({.skip_recovery = true, .waystone = true});
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    sim::SimInstance& db = *opened.value();
    const std::string sql = "SELECT v FROM ws WHERE id = 1";
    for (const std::string& line : {std::string("CREATE TABLE ws (id int64, v int64) BTREE"),
                                    std::string("INSERT INTO ws VALUES (5)"),
                                    "WAYSTONE ON " + sql}) {
        const std::string reply = db.Execute(line);
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << line << " -> " << reply;
    }
    ASSERT_EQ(db.catalog().opted_patterns(), 1u);
    ASSERT_TRUE(db.CleanShutdown().ok());
    ASSERT_TRUE(db.Reboot().ok());

    EXPECT_EQ(db.catalog().opted_patterns(), 1u) << "the skip_recovery boot left the count unset";
    EXPECT_EQ(ViewOpted(db.Execute("SHOW PATTERNS")), 1u);
    EXPECT_EQ(testing_view::MetaField(db.Execute("SHOW META"), "waystone_opted"), "1");
    // And the opted row is asked for: no renewal ran, so it kept the root
    // its ON made (a skip_recovery boot discards nothing), and it records.
    db.Execute(sql);
    db.Execute(sql);
    EXPECT_NE(db.Execute("ANALYZE " + sql).find("replays=1"), std::string::npos);
}

}  // namespace
}  // namespace kds::server
