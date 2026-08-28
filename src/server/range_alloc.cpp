#include "kds/server/range_alloc.hpp"

#include <string>

#include "kds/exec/assertion_catalog.hpp"
#include "kds/wal/log_page_handoff.hpp"

namespace kds::server {

StatusOr<PageId> OpenRangeOnSystemCore(catalog::Catalog& catalog, storage::PageStore& store,
                                       wal::WalManager* wal,
                                       const exec::AssertionEnforcer& enforcer,
                                       catalog::Oid rel_oid, std::uint64_t lo,
                                       std::uint32_t owner_core, Logger* log) {
    // §9b's catalog-relation scope, **taken here** because RD4 declined to
    // invent a gate §6a does not list. Every one of §6a's five facts is
    // true of `sys.tables`, yet a catalog relation is categorically
    // unsplittable: its pages are core 0's by construction, its chain head
    // is a compile-time `kCatalogPage*` constant rather than a directory
    // row, and CC9 puts the directory itself in the catalog. The engine's
    // idiom for the question is the namespace (AL7, DT3).
    //
    // Asked off the `sys.tables` row and **before** the access fill, which
    // is not tidiness: a bootstrap catalog relation has no `sys.columns`
    // rows at all, so the fill fails it with "no columns for this rel_id".
    // That refusal is safe but accidental, and a scope this file states is
    // one a later change cannot remove by making the fill succeed.
    auto row = catalog.GetSysTableRow(rel_oid);
    if (!row.ok()) return row.status();
    if (row.value().namespace_oid == catalog::kNamespaceSys) {
        if (log != nullptr && log->enabled(LogLevel::kInfo)) {
            log->Info("range", "core 0 declined a range for catalog relation oid " +
                                   std::to_string(rel_oid) +
                                   ": a catalog relation's pages and chain head are core 0's by "
                                   "construction");
        }
        return kInvalidPageId;
    }

    auto access = catalog.InitTableAccess(rel_oid);
    if (!access.ok()) return access.status();

    {
        const exec::RangeGate gate = exec::RangeEligible(*access.value(), enforcer);
        if (gate != exec::RangeGate::kNone) {
            if (log != nullptr && log->enabled(LogLevel::kInfo)) {
                log->Info("range", "core 0 declined a range for relation oid " +
                                       std::to_string(rel_oid) + " at lo " + std::to_string(lo) +
                                       ": " + std::string(exec::RangeGateName(gate)) +
                                       " (re-checked at the durable row)");
            }
            return kInvalidPageId;
        }
        // The fifth gate against the durable rows, for the reason the
        // header gives: core 0's registry is silent about a peer-owned
        // relation's assertions, so `RangeEligible`'s assertion arm above
        // answered "eligible" vacuously.
        // `ListAssertionTargets` and not `AssertionsOnRelation`: the
        // question is *whether* a row exists, not what it says, and the
        // target form reads exactly the pages the walk already holds
        // rather than resolving var-heap spills a grant may not cover.
        auto assertions = exec::ListAssertionTargets(catalog, store);
        if (!assertions.ok()) return assertions.status();
        bool asserted = false;
        for (const exec::AssertionDef& def : assertions.value()) {
            if (def.target_oid == rel_oid) {
                asserted = true;
                break;
            }
        }
        if (asserted) {
            if (log != nullptr && log->enabled(LogLevel::kInfo)) {
                log->Info("range", "core 0 declined a range for relation oid " +
                                       std::to_string(rel_oid) + " at lo " + std::to_string(lo) +
                                       ": " +
                                       std::string(exec::RangeGateName(exec::RangeGate::kAssertion)) +
                                       " (durable sys.assertions row)");
            }
            return kInvalidPageId;
        }
    }

    auto entry_page = catalog.OpenRange(rel_oid, lo, owner_core);
    if (!entry_page.ok()) return entry_page.status();

    // CC10 step 2's record, logged **after** the rows so one EnsureDurable
    // covers both: LSNs are monotonic within core 0's stream, so waiting
    // on this one waits on everything logged before it. PL §9 rule 1 -
    // durable before any grant leaves - is what that wait is for, and the
    // grant is the caller's reply.
    auto handoff = wal::LogPageHandoff(wal, entry_page.value(), owner_core);
    if (!handoff.ok()) {
        return handoff.status().WithContext("handoff record for range entry page " +
                                            std::to_string(entry_page.value()));
    }
    // An unlogged store answers kNoLsn throughout - nothing to sync and
    // nothing to recover - which is what a fixture is.
    if (handoff.value() != wal::kNoLsn) {
        if (Status s = wal->EnsureDurable(handoff.value()); !s.ok()) {
            return s.WithContext("range directory row and handoff record not durable");
        }
    }
    return entry_page.value();
}

}  // namespace kds::server
