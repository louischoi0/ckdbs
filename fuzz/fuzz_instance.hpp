#pragma once

// A fresh in-memory instance per fuzz input (BS-S5,
// instructions/v3.0.0/workorder-bs-test-hardening.md): one core, no log,
// a fixed schema - two relations, an index, a foreign key, a few rows -
// built through the dispatcher as a client would; no Cabin store. Fresh per input so
// a crash replays from its input alone (BS-R3); kwp_session_test.cpp's
// fixture, without gtest.

#include <optional>
#include <string>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"

namespace kds::fuzz {

class Instance {
public:
    Instance() {
        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/4000);
        if (!boot.ok()) __builtin_trap();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);
        for (const char* sql : {
                 "CREATE TABLE t (id int64, v int64, s varchar NULL, d decimal(10,2) NULL, "
                 "ts timestamp NULL) BTREE SPARE 2",
                 "CREATE TABLE c (id int64, pid int64 REFERENCES t, w int32) BTREE",
                 "CREATE INDEX tv ON t (v)",
                 "INSERT INTO t VALUES (1, 10, 'a', 1.50, '2026-10-01 01:00:00'), "
                 "(2, 20, 'b', NULL, NULL), (3, 30, NULL, 3.25, '2026-10-02 00:00:00')",
                 "INSERT INTO c VALUES (1, 1, 7), (2, 1, 8), (3, 3, 9)",
             }) {
            const std::string reply = Run(sql);
            if (reply.rfind("ERR", 0) == 0) __builtin_trap();  // the fixture is wrong
        }
    }

    std::string Run(std::string_view sql) { return dispatcher_->Dispatch(sql, &session_).response; }
    server::CommandDispatcher& dispatcher() { return *dispatcher_; }

private:
    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<server::CommandDispatcher> dispatcher_;
    server::Session session_{txn::IsolationLevel::kReadCommitted};
};

}  // namespace kds::fuzz
