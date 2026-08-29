#include "kds/server/range_alloc.hpp"

#include <string>

#include "kds/exec/assertion_catalog.hpp"
#include "kds/wal/log_page_handoff.hpp"

namespace kds::server {

void LogRangeDecline(Logger* log, std::uint32_t core_id, catalog::Oid rel_oid,
                     exec::RangeGate gate, std::string_view why) {
    if (log == nullptr || !log->enabled(LogLevel::kInfo)) return;
    log->Info("range", "core " + std::to_string(core_id) +
                           " will not open a range on relation oid " + std::to_string(rel_oid) +
                           ": " + std::string(exec::RangeGateName(gate)) + " (" +
                           std::string(why) + ")");
}

StatusOr<PageId> OpenRangeOnSystemCore(catalog::Catalog& catalog,
                                       storage::DevicePageStore& store, wal::WalManager* wal,
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
    // `!= kNamespacePublic`, which is the engine's idiom for the question
    // (AL7, DT3, `range_eligible.hpp`'s own scope note) and not `==
    // kNamespaceSys`: the two agree only while `sys` and `public` are the
    // only namespaces, and the difference between them is which way a
    // third one fails. A gate must fail closed.
    if (row.value().namespace_oid != catalog::kNamespacePublic) {
        LogRangeDecline(log, catalog::kSystemCore, rel_oid, exec::RangeGate::kNone,
                        "a catalog relation's pages and chain head are core 0's by construction");
        return kInvalidPageId;
    }

    auto access = catalog.InitTableAccess(rel_oid);
    if (!access.ok()) return access.status();

    // ---- R4/IS5: a carve that continues this core's own top range -------
    //
    // Every grant opened a boundary, which is right when the asker is a new
    // writer and pure waste when it is the same one again: a *single*-writer
    // relation cut its own chain in two per lease block and spent one
    // fan-in stage per 4,096 rows, for a partition with one owner on both
    // sides of it. The block lands in the top range either way - ids only
    // ascend, and that range runs to `kIdSpaceEnd` - so when this core
    // already owns the top range there is nothing to open and nothing lost
    // by not opening it.
    //
    // The empty-directory case is the same statement: the relation is one
    // range owned by `sys.tables.owner_core` (CC9), so an owner asking for
    // its own first block needs no boundary either.
    //
    // What this does **not** do is bound the ceiling on a contended
    // relation: with k cores taking turns, the top range's owner is a
    // different core almost every time, so a range still opens per block.
    // That is `workplan-insert-spreading.md` §3's arithmetic and it stands;
    // this row removes the half of it that bought nothing.
    //
    // Not a gate decline, and deliberately not counted as one: no gate
    // refused this relation, and `range_split_declines` is the evidence
    // base for which owning decision to lift first (§9e). An entry there
    // for "there was nothing to do" would corrupt exactly that reading.
    {
        const auto& ranges = access.value()->ranges;
        const std::uint32_t top_owner =
            ranges.empty() ? access.value()->owner_core : ranges.back().owner_core;
        if (top_owner == owner_core) {
            if (log != nullptr && log->enabled(LogLevel::kDebug)) {
                log->Debug("range", "core " + std::to_string(owner_core) +
                                        " already owns the top range of relation oid " +
                                        std::to_string(rel_oid) + "; its block at " +
                                        std::to_string(lo) + " opens no new boundary");
            }
            return kInvalidPageId;
        }
    }

    {
        const exec::RangeGate gate = exec::RangeEligible(*access.value(), enforcer);
        if (gate != exec::RangeGate::kNone) {
            LogRangeDecline(log, catalog::kSystemCore, rel_oid, gate, "re-checked at the durable row");
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
            LogRangeDecline(log, catalog::kSystemCore, rel_oid, exec::RangeGate::kAssertion,
                            "durable sys.assertions row, which this core's registry cannot see");
            return kInvalidPageId;
        }
    }

    // ---- CC10's steps, in CC10's order ------------------------------
    //
    // The ordering below is a correctness statement in the spec, not a
    // sequence that happened to work: **the head page is durable and
    // handed off before any row names it, and the boundary is published
    // last.** The first draft of this row wrote the rows first and
    // published inside `InsertRangeRow`, which broadcast a partition
    // before the page it partitions into existed on the device.
    auto entry_page = catalog.CreateRangeEntryPage(rel_oid, lo);
    if (!entry_page.ok()) return entry_page.status();
    const PageId head[] = {entry_page.value()};

    // Step 1. **Flush, and it is CC7's own sequence rather than a
    // precaution** (PW1c-4, the relation publish hook's `FlushPages` →
    // handoff → grant → `EvictClean`). `CreateRangeEntryPage` formatted
    // the head in *this* core's frame and nothing wrote it to the device;
    // every core has its own `DevicePageStore`, so the owner's admission
    // faults these bytes from the device and would read the id back as
    // "allocated but was never written" (`ResidentBytes`' all-zero arm) -
    // the grant would fail on arrival and the range would have a head no
    // core could write. `FlushPages` carries the free map out with the
    // page, which the owner's `RefreshFreeMapFromDevice` then needs to see
    // the id allocated at all.
    if (Status s = store.FlushPages(head); !s.ok()) {
        return s.WithContext("flushing range entry page " + std::to_string(entry_page.value()) +
                             " before its handoff");
    }

    // Step 2, and PL §9 rule 1's durability wait. A failure here leaves an
    // orphaned formatted page and **no boundary**, which is the direction
    // to fail in: a leaked page is the class DROP TABLE already accepts,
    // where a published boundary whose head was never handed off would be
    // a range its owner cannot write.
    auto handoff = wal::LogPageHandoff(wal, entry_page.value(), owner_core);
    if (!handoff.ok()) {
        return handoff.status().WithContext("handoff record for range entry page " +
                                            std::to_string(entry_page.value()));
    }
    // An unlogged store answers kNoLsn throughout - nothing to sync and
    // nothing to recover - which is what a fixture is.
    if (handoff.value() != wal::kNoLsn) {
        if (Status s = wal->EnsureDurable(handoff.value()); !s.ok()) {
            return s.WithContext("handoff record for the range entry page not durable");
        }
    }

    // Step 3, and step 5 inside it: the rows, then one publication. The
    // reply the caller sends is step 4, so the broadcast still precedes
    // the grant by the width of this return - stated rather than claimed
    // closed. Closing it needs publication deferred past a message send,
    // which is the mover's shape (CC10's migration) and RB3's to build;
    // what is closed here is the half that mattered, the announcement of a
    // boundary whose page was neither durable nor handed off.
    if (Status s = catalog.OpenRangeRows(rel_oid, lo, owner_core, entry_page.value()); !s.ok()) {
        return s;
    }

    // The departure completed on this side (the 95b45e8 review's C4, and
    // the publish hook's closing step): core 0 would otherwise keep a
    // frame of a page another core now owns, which is a stale-read window
    // and - because core 0 has no lease and so `MayWrite`s everything - a
    // later flush of that frame writing the empty image back over the
    // owner's rows. Best-effort and logged: a dirty-frame refusal here
    // would mean the flush above lied, and the range is already open.
    if (Status s = store.EvictClean(head); !s.ok() && log != nullptr &&
                                            log->enabled(LogLevel::kError)) {
        log->Error("range", "core 0 could not evict handed-off range entry page " +
                                std::to_string(entry_page.value()) + ": " + s.message());
    }
    return entry_page.value();
}

}  // namespace kds::server
