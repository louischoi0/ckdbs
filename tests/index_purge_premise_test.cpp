#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/index/index_tree.hpp"
#include "kds/txn/manager.hpp"

// DD-S0's premise test (`instructions/v3.2.0/workorder-dd-index-entry-purge.md`
// DD-R1): **for every version, the entry a rebuild owes it is byte-equal to
// an entry the write path appended**, so a purge whose judge is the
// rebuild's walk never takes out an entry some version still needs.
//
// The oracle is a second index of the same definition built by `CREATE
// INDEX` after the history: it holds the entries the rebuild's walk owes
// every version a reader can still reach. Each of them must be in the
// write-maintained twin, byte for byte - or, where it is not, the test
// names the shape so the judge can be told about it.

namespace kds::server {
namespace {

using Entry = std::vector<std::byte>;

class PurgePremiseTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*mgr_);
    }

    std::string Run(const std::string& sql, Session* s = nullptr) {
        return dispatcher_->Dispatch(sql, s).response;
    }
    void Ok(const std::string& sql, Session* s = nullptr) {
        const std::string out = Run(sql, s);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }

    // Every entry of index `name` on relation `table`, as stored.
    std::vector<Entry> Entries(const std::string& table, const std::string& name) {
        auto oid = boot_->catalog.ResolveRelation("", table, catalog::kNamespacePublic);
        EXPECT_TRUE(oid.ok()) << oid.status().message();
        auto access = boot_->catalog.InitTableAccess(oid.value());
        EXPECT_TRUE(access.ok()) << access.status().message();
        const catalog::TableAccess& ta = *access.value();
        for (const auto& ix : ta.indexes) {
            if (ix.name != name) continue;
            auto root = exec::SlotRoot(store_, ta.anchor_page_id, ix.index_oid);
            EXPECT_TRUE(root.ok()) << root.status().message();
            index::IndexLayout layout;
            layout.key_width = ix.key_width;
            layout.covered_width = static_cast<std::uint16_t>(ix.entry_width - ix.key_width -
                                                              index::kIndexTupleKeyWidth);
            std::vector<Entry> out;
            Status walked = index::IndexVisit(
                store_, root.value(), layout, storage::PageAccess::kRead,
                [&](PageId, index::IndexLeafView& leaf,
                    std::uint16_t at) -> StatusOr<storage::VisitControl> {
                    auto e = leaf.Entry(at);
                    if (!e.ok()) return e.status();
                    out.emplace_back(e.value().begin(), e.value().end());
                    return storage::VisitControl::kContinue;
                });
            EXPECT_TRUE(walked.ok()) << walked.message();
            return out;
        }
        ADD_FAILURE() << "no index " << name << " on " << table;
        return {};
    }

    // The rebuilt twin's entries the maintained one lacks, byte for byte.
    std::vector<Entry> Missing(const std::string& table, const std::string& maintained,
                               const std::string& rebuilt) {
        const std::vector<Entry> have = Entries(table, maintained);
        const std::set<Entry> held(have.begin(), have.end());
        std::vector<Entry> missing;
        for (const Entry& e : Entries(table, rebuilt)) {
            if (held.count(e) == 0) missing.push_back(e);
        }
        return missing;
    }

    // Of `missing`, the entries no entry of `maintained` serves by the
    // judge's own comparison (`exec::SameOwedEntry`).
    std::size_t Unserved(const std::string& table, const std::string& maintained,
                         const std::vector<Entry>& missing) {
        auto oid = boot_->catalog.ResolveRelation("", table, catalog::kNamespacePublic);
        auto access = boot_->catalog.InitTableAccess(oid.value());
        const catalog::TableAccess& ta = *access.value();
        const catalog::TableAccess::IndexRef* ix = nullptr;
        for (const auto& i : ta.indexes) {
            if (i.name == maintained) ix = &i;
        }
        EXPECT_NE(ix, nullptr);
        const std::vector<Entry> have = Entries(table, maintained);
        std::size_t unserved = 0;
        for (const Entry& e : missing) {
            const bool served = std::any_of(have.begin(), have.end(), [&](const Entry& h) {
                return exec::SameOwedEntry(ta, *ix, h, e);
            });
            if (!served) ++unserved;
        }
        return unserved;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

// A churned history over every column kind an entry carries: an integer key,
// a string key, a composite key, inline covered columns and a spilled one.
void Churn(const std::function<void(const std::string&)>& ok,
           const std::function<void(const std::string&, Session*)>& ok_in) {
    const std::string longa(40, 'a');
    const std::string longb(40, 'b');
    for (int i = 1; i <= 60; ++i) {
        ok("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 7) + ", " +
           std::to_string(i) + ", 's" + std::to_string(i % 5) + "', '" + longa + "', 1.5)");
    }
    ok("UPDATE t SET k = k + 100 WHERE id <= 10");          // a key to fresh values
    ok("UPDATE t SET k = k - 100 WHERE id <= 5");           // and back again
    ok("UPDATE t SET c = c + 1 WHERE id > 10 AND id <= 20");  // an inline covered column
    ok("UPDATE t SET c = c WHERE id > 20 AND id <= 25");      // the same value
    ok("UPDATE t SET note = '" + longa + "' WHERE id > 25 AND id <= 30");  // same, spilled
    ok("UPDATE t SET note = '" + longb + "' WHERE id > 30 AND id <= 35");  // new, spilled
    ok("UPDATE t SET s = 's9' WHERE id > 35 AND id <= 40");   // the string key
    ok("UPDATE t SET d = 1.50 WHERE id > 40 AND id <= 45");  // a decimal, spelled again
    ok("DELETE FROM t WHERE id > 55");
    ok("PURGE FROM t WHERE id > 58");
    Session s;
    ok_in("BEGIN", &s);
    ok_in("INSERT INTO t VALUES (100, 1, 1, 'x', '" + longb + "', 2.0)", &s);
    ok_in("UPDATE t SET k = 99, note = '" + longb + "' WHERE id = 41", &s);
    ok_in("ROLLBACK", &s);
}

TEST_F(PurgePremiseTest, ARebuiltTwinHoldsNoEntryTheWriteMaintainedIndexLacks) {
    Ok("CREATE TABLE t (id int64, k int64, c int64, s varchar(16), note varchar(16), "
       "d decimal(10,2)) BTREE");
    Ok("CREATE INDEX m_k ON t (k)");
    Ok("CREATE INDEX m_kc ON t (k) COVERING (c)");
    Ok("CREATE INDEX m_ks ON t (k, s)");
    Ok("CREATE INDEX m_s ON t (s)");
    Ok("CREATE INDEX m_note ON t (k) COVERING (note)");
    Ok("CREATE INDEX m_kd ON t (k) COVERING (d)");
    Churn(
        [&](const std::string& q) { Ok(q); },
        [&](const std::string& q, Session* s) { Ok(q, s); });
    Ok("CREATE INDEX r_k ON t (k)");
    Ok("CREATE INDEX r_kc ON t (k) COVERING (c)");
    Ok("CREATE INDEX r_ks ON t (k, s)");
    Ok("CREATE INDEX r_s ON t (s)");
    Ok("CREATE INDEX r_note ON t (k) COVERING (note)");
    Ok("CREATE INDEX r_kd ON t (k) COVERING (d)");

    EXPECT_TRUE(Missing("t", "m_k", "r_k").empty());
    EXPECT_TRUE(Missing("t", "m_kc", "r_kc").empty());
    EXPECT_TRUE(Missing("t", "m_ks", "r_ks").empty());
    EXPECT_TRUE(Missing("t", "m_s", "r_s").empty());
    EXPECT_TRUE(Missing("t", "m_kd", "r_kd").empty());
    // **DD-S0's finding**: the spilled covered column breaks the premise as
    // DD-R1 wrote it. A same-value assignment of a spilled `note` (rows 26
    // to 30) moves nothing by value, so the write path appends nothing,
    // while the version's cell names a new spill: the entry the rebuild
    // owes it is in no tree. The judge's comparison serves each of them
    // from the entry the row already had.
    const std::vector<Entry> note = Missing("t", "m_note", "r_note");
    EXPECT_EQ(note.size(), 5u) << "the rows that assigned their spilled value again";
    EXPECT_EQ(Unserved("t", "m_note", note), 0u);
}

}  // namespace
}  // namespace kds::server
