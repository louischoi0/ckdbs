#include "kds/exec/assertion_recover.hpp"

#include <cstring>
#include <map>
#include <set>
#include <span>
#include <string>

#include "kds/storage/cabin_bound_page.hpp"
#include "kds/wal/log_scanner.hpp"
#include "kds/wal/payload.hpp"

namespace kds::exec {
namespace {

using storage::cabin::BoundCabinPage;

// Walks one cabin's page chain and attaches every entry to the group its
// `group_id` names - AS6a's linkage rebuild, and the reason the entry carries an
// id at all.
//
// Bounded by the cabin's own pages. An entry whose group the snapshot did not
// carry is **left unattached rather than failing**: it belongs to a group created
// after the checkpoint, and the fold that follows is what creates that group.
// Attaching it then is not this walk's job, because the fold re-appends the
// linkage itself when it applies the record.
StatusOr<std::uint64_t> AttachEntriesFromPages(storage::PageStore& store, PageId root,
                                               BoundCabin& cabin) {
    std::uint64_t attached = 0;
    PageId page_id = root;
    // The same cycle bound the chain walks elsewhere use: a damaged link must be
    // a reported Corruption and never a hang.
    for (std::uint32_t steps = 0; page_id != kInvalidPageId; ++steps) {
        if (steps > (1u << 20)) {
            return Status::Corruption("assertion recovery: cabin page chain from " +
                                      std::to_string(root) +
                                      " exceeds the maximum length; the links may form a cycle");
        }
        auto page = store.GetForRead(page_id);
        if (!page.ok()) {
            return page.status().WithContext("assertion recovery: cabin page " +
                                             std::to_string(page_id));
        }
        // `Open` is what proves this is a kCabinBound page; a page of another
        // class here means the root or a link is not what it claims.
        auto view = BoundCabinPage::Open(
            std::span<std::byte, kPageSize>(const_cast<std::byte*>(page.value().data()),
                                           kPageSize));
        if (!view.ok()) {
            return view.status().WithContext("assertion recovery: cabin page " +
                                             std::to_string(page_id));
        }

        const std::uint16_t count = view.value().entry_count();
        for (std::uint16_t index = 0; index < count; ++index) {
            auto entry = view.value().Read(index);
            if (!entry.ok()) return entry.status();
            if (entry.value().group_id == 0) {
                // Written before AS6a, so there is nothing to attribute it by.
                // Skipped rather than guessed: the alternative is inventing the
                // allocation order the id exists to replace.
                continue;
            }
            Status s = cabin.AttachEntry(entry.value().group_id, page_id, index);
            if (s.ok()) {
                ++attached;
            } else if (s.code() != StatusCode::kNotFound) {
                return s;
            }
        }
        page_id = view.value().next_page_id();
    }
    return attached;
}

// The fold's context over a fixed set of cabins, plus the "has a base" rule.
class RecoveryContext final : public AssertionReplayContext {
public:
    void Add(std::uint64_t assertion_id, BoundCabin* cabin) { cabins_[assertion_id] = cabin; }

    void MarkBased(std::uint64_t assertion_id) { based_.insert(assertion_id); }
    bool based(std::uint64_t assertion_id) const { return based_.count(assertion_id) != 0; }

    BoundCabin* CabinOf(std::uint64_t assertion_id) override {
        auto it = cabins_.find(assertion_id);
        return it == cabins_.end() ? nullptr : it->second;
    }

    void Drop(std::uint64_t assertion_id) override {
        cabins_.erase(assertion_id);
        based_.erase(assertion_id);
    }

private:
    std::map<std::uint64_t, BoundCabin*> cabins_;
    std::set<std::uint64_t> based_;
};

}  // namespace

StatusOr<AssertionRecoveryReport> RecoverAssertions(
    wal::LogDevice& device, std::uint32_t core_id, wal::Lsn from_lsn,
    storage::PageStore& store, std::span<const RecoverableAssertion> assertions, Logger* log) {
    AssertionRecoveryReport report;
    if (assertions.empty()) {
        return report;  // nothing to recover, and no scan to pay for
    }

    RecoveryContext context;
    std::map<std::uint64_t, const RecoverableAssertion*> by_id;
    for (const RecoverableAssertion& a : assertions) {
        if (a.cabin == nullptr) {
            return Status::InvalidArgument("assertion recovery: assertion " +
                                          std::to_string(a.assertion_id) + " has no cabin");
        }
        context.Add(a.assertion_id, a.cabin);
        by_id[a.assertion_id] = &a;
        report.assertions.push_back(AssertionRecoveryResult{a.assertion_id, false, 0, 0, 0});
    }

    auto result_for = [&report](std::uint64_t id) -> AssertionRecoveryResult* {
        for (AssertionRecoveryResult& r : report.assertions) {
            if (r.assertion_id == id) return &r;
        }
        return nullptr;
    };

    auto visit = [&](const wal::DecodedRecord& record) -> Status {
        if (record.type() == wal::RecordType::kAssertSnapshot) {
            auto decoded = wal::DecodeAssertSnapshot(record.payload);
            if (!decoded.ok()) return decoded.status();
            const std::uint64_t id = decoded.value().fields.assertion_id;

            auto known = by_id.find(id);
            if (known == by_id.end()) {
                // A snapshot for an assertion the caller did not ask about -
                // dropped since, or belonging to a relation the crash lost
                // (RV3). Skipped, exactly as the fold skips an unknown id.
                return Status::OK();
            }
            AssertionRecoveryResult* r = result_for(id);
            BoundCabin& cabin = *known->second->cabin;

            for (const wal::SnapshotGroupEntry& group : decoded.value().groups) {
                const std::string key(reinterpret_cast<const char*>(group.key.data()),
                                      group.key.size());
                if (Status s = cabin.RestoreGroup(group.group_id, key, group.count, group.sum);
                    !s.ok()) {
                    return s.WithContext("assertion recovery: restoring assertion " +
                                         std::to_string(id));
                }
                ++r->groups_restored;
            }

            // AS6a's step 3, here rather than after the scan: every entry page
            // is already redone, and doing it at the snapshot means the fold
            // below sees a directory that is whole.
            auto attached = AttachEntriesFromPages(store, known->second->root_page_id, cabin);
            if (!attached.ok()) return attached.status();
            r->entries_attached += attached.value();
            r->recovered = true;
            context.MarkBased(id);
            return Status::OK();
        }

        if (!IsAssertionRecord(record.type())) {
            return Status::OK();  // every other record belongs to another phase
        }

        // Which assertion this record names, without decoding a payload this
        // function does not own: every assertion payload starts with the id.
        if (record.payload.size() < sizeof(std::uint64_t)) {
            return Status::Corruption("assertion recovery: " +
                                      std::string(wal::RecordTypeName(record.type())) +
                                      " payload is too short to name an assertion");
        }
        std::uint64_t id = 0;
        std::memcpy(&id, record.payload.data(), sizeof(id));

        if (by_id.count(id) == 0) {
            return Status::OK();  // not ours; the fold's own skip rule agrees
        }
        if (!context.based(id)) {
            // No snapshot yet for this assertion, so there is no base to fold
            // onto and folding would under-count. Counted and skipped; the
            // assertion stays unrecovered and its caller leaves it unenforcing.
            ++report.records_without_a_base;
            return Status::OK();
        }

        if (Status s = ReplayAssertionRecord(record, store, context); !s.ok()) {
            return s;
        }
        if (AssertionRecoveryResult* r = result_for(id); r != nullptr) {
            ++r->records_folded;
        }
        return Status::OK();
    };

    auto scanned = wal::ScanLog(device, core_id, from_lsn, visit);
    if (!scanned.ok()) {
        // The visitor's own failures come back here: ScanLog returns a
        // visitor's non-ok Status unchanged, which is why this needs no second
        // error channel (log_scanner.hpp's veto rule).
        return scanned.status().WithContext("assertion recovery");
    }

    if (log != nullptr) {
        for (const AssertionRecoveryResult& r : report.assertions) {
            if (!r.recovered) {
                log->Error("recovery",
                           "assertion " + std::to_string(r.assertion_id) +
                               " found no group snapshot at or after the last checkpoint, so it "
                               "cannot enforce until it is rebuilt (docs/feat-assertion.md §7)");
                continue;
            }
            log->Info("recovery", "assertion " + std::to_string(r.assertion_id) + ": " +
                                      std::to_string(r.groups_restored) +
                                      " group(s) restored, " +
                                      std::to_string(r.entries_attached) +
                                      " entr(ies) relinked, " + std::to_string(r.records_folded) +
                                      " record(s) folded");
        }
    }
    return report;
}

}  // namespace kds::exec
