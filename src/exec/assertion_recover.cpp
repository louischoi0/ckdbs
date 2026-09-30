#include "kds/exec/assertion_recover.hpp"

#include <cstring>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "kds/storage/cabin_bound_page.hpp"
#include "kds/storage/heap/heap_chain.hpp"
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
// Peak pins (MG03): 1 - the chain walk holds each page only while reading
// its entries, and the ref is reassigned on the next hop.
StatusOr<std::uint64_t> AttachEntriesFromPages(storage::PageStore& store, PageId root,
                                               BoundCabin& cabin) {
    // Collected first, attached in one batch: resolving each entry's group id
    // individually is O(entries x groups) (`bound_cabin.hpp`).
    std::vector<BoundCabin::ScannedEntry> scanned;
    PageId page_id = root;
    // The same cycle bound the chain walks elsewhere use: a damaged link must be
    // a reported Corruption and never a hang.
    for (std::uint32_t steps = 0; page_id != kInvalidPageId; ++steps) {
        // The same bound `AdoptChain` walks under, named rather than repeated:
        // one chain, one cycle guard.
        if (steps > heap::kMaxChainPages) {
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
        //
        // `GetForRead` already hands back a mutable `span<byte, kPageSize>` -
        // the read-only promise is by contract, not by type (page_store.hpp) -
        // so the span goes straight in. This walk calls only entry_count(),
        // Read() and next_page_id(), none of which write.
        auto view = BoundCabinPage::Open(page.value().bytes());
        if (!view.ok()) {
            return view.status().WithContext("assertion recovery: cabin page " +
                                             std::to_string(page_id));
        }

        const std::uint16_t count = view.value().entry_count();
        for (std::uint16_t index = 0; index < count; ++index) {
            auto entry = view.value().Read(index);
            if (!entry.ok()) return entry.status();
            if (entry.value().orphaned()) {
                // The reservation that wrote it aborted, so it is no group's -
                // `Unapply` dropped the linkage on the live side and this is
                // how a scan that reads only pages reaches the same answer.
                // Without the mark the two are indistinguishable, and a cabin
                // whose history holds one pre-checkpoint abort re-attaches an
                // entry the live directory had dropped: the aggregate stays
                // right (snapshot + folded deltas) but §5.2's
                // `VerifyAgainstEntries` proof reports Corruption for a
                // directory that is correct. That is the AS6b decision taken
                // 2026-08-12 (`docs/spec/assertion.md` §7).
                continue;
            }
            if (entry.value().group_id == 0) {
                // A page entry written before AS6a: AST04 wrote that word as a
                // literal zero, so there is nothing to attribute it by. Skipped
                // rather than guessed - the alternative is inventing the
                // allocation order the id exists to replace.
                continue;
            }
            scanned.push_back(BoundCabin::ScannedEntry{entry.value().group_id, page_id, index});
        }
        page_id = view.value().next_page_id();
    }
    return cabin.AttachEntries(scanned);
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

    // Runs still being read: a cabin's snapshot is **chunked** (`wal/payload.hpp`),
    // and a run is a base only once all `chunk_count` of its chunks have been
    // seen, in order (AY-S8, AY-R7). Its groups are held here, not restored,
    // until then - a run that never completes is discarded whole, so a partial
    // one leaves nothing in the cabin for a whole one after it to collide with
    // in `RestoreGroup`. Keys are copied: the record's bytes do not outlive the
    // visit.
    struct PendingGroup {
        std::uint32_t group_id = 0;
        std::string key;
        std::int64_t count = 0;
        std::int64_t sum = 0;
    };
    struct PendingRun {
        std::uint16_t chunk_count = 0;
        std::uint16_t next_index = 0;
        std::vector<PendingGroup> groups;
    };
    std::map<std::uint64_t, PendingRun> pending;

    auto discard = [&](std::uint64_t id) {
        if (pending.erase(id) == 0) return;
        if (AssertionRecoveryResult* r = result_for(id); r != nullptr) {
            ++r->partial_runs_discarded;
        }
    };

    // The run is whole: restore its groups, rebuild the linkage from the
    // cabin's own pages, once, and mark the assertion foldable. **Once and not
    // once per chunk** - a later chunk's walk would see the earlier chunks'
    // groups already restored and attach their entries a second time, which is
    // the duplicate §7's `VerifyAgainstEntries` exists to catch. And **before
    // the fold**, because a reservation made before the checkpoint and rolled
    // back after it needs its (page, index) pair present for `Unapply` to find
    // (AS6a's own note) - which this walk supplies only while the abort's mark
    // has not reached the device. Once it has, the skip below drops the pair and
    // `ReplayRollback` is what puts it back (AS6b, `assertion_replay.cpp`); the
    // ordering here is still required, because that is the *other* half of the
    // same note.
    auto close_base = [&](std::uint64_t id, const PendingRun& run) -> Status {
        const RecoverableAssertion* a = by_id.at(id);
        AssertionRecoveryResult* r = result_for(id);
        for (const PendingGroup& group : run.groups) {
            if (Status s = a->cabin->RestoreGroup(group.group_id, group.key, group.count,
                                                  group.sum);
                !s.ok()) {
                return s.WithContext("assertion recovery: restoring assertion " +
                                     std::to_string(id));
            }
            ++r->groups_restored;
        }
        auto attached = AttachEntriesFromPages(store, a->root_page_id, *a->cabin);
        if (!attached.ok()) return attached.status();
        r->entries_attached += attached.value();
        r->recovered = true;
        context.MarkBased(id);
        return Status::OK();
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
            if (context.based(id)) {
                // **A later checkpoint's snapshot, and it is skipped.** The scan
                // starts at the anchor's `redo_start_lsn`, at or below the
                // `CHECKPOINT_BEGIN` whose run gave the base, so every later
                // run in range - a later completed checkpoint's, or one a crash
                // cut, the anchor being published only at Complete() - is one
                // this skip meets. Restoring them would hit
                // `RestoreGroup`'s duplicate-id refusal and fail the whole pass,
                // leaving every assertion unenforcing after a routine crash.
                //
                // Skipping is not a loss: the base already loaded plus every
                // `ASSERT_*` record folded onto it since is the same state the
                // later snapshot describes, because an assertion mutation is
                // logged whether or not a checkpoint follows it.
                return Status::OK();
            }
            const wal::AssertSnapshotPayload& fields = decoded.value().fields;

            // A chunk 0 opens a run, and a run still open for the same
            // assertion is one that stopped short - `LogAssertionSnapshot`
            // failing an append mid-run, or a crash before a later writer's run
            // - so it is discarded. Any other chunk continues the open run or
            // belongs to one whose start is not in range (the scan can begin at
            // a page record between two chunks); a run it cannot finish is
            // discarded and the chunk with it.
            if (fields.chunk_index == 0) {
                discard(id);
                pending[id].chunk_count = fields.chunk_count;
            } else if (auto open = pending.find(id);
                       open == pending.end() || open->second.chunk_count != fields.chunk_count ||
                       open->second.next_index != fields.chunk_index) {
                discard(id);
                return Status::OK();
            }

            PendingRun& run = pending[id];
            for (const wal::SnapshotGroupEntry& group : decoded.value().groups) {
                run.groups.push_back(PendingGroup{
                    group.group_id,
                    std::string(reinterpret_cast<const char*>(group.key.data()), group.key.size()),
                    group.count, group.sum});
            }
            if (++run.next_index < run.chunk_count) {
                return Status::OK();  // AS6a's step 3 waits for the rest of the run
            }
            auto whole = pending.extract(id);
            return close_base(id, whole.mapped());
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
            //
            // **A BUILD record takes this arm too, on purpose** - a genesis
            // rule ("born empty, so its first BUILD is the base") was built
            // here on 2026-08-19 and deleted the same day by review: the
            // publish-time ASSERT_SNAPSHOT (`assertion_catalog.cpp`, AS6a)
            // already covers a declaration born after the last checkpoint -
            // even an empty one gets a snapshot record - and a genesis base
            // would make the `based(id)` skip above *discard* that better,
            // complete snapshot whenever the range ever began mid-build,
            // adopting an under-counted directory as enforcing. The one case
            // genesis covered that this refusal does not is a torn,
            // unacknowledged CREATE (builds durable, publish snapshot lost),
            // where the honest answer is exactly this `enforcing=0`.
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
    // A run the scan ended inside is not a base (AY-Q7): a crash cut it, and
    // the groups it did not reach would restore as absent - an under-count an
    // admission check admits past. Its assertion stays unrecovered and its
    // relation's writes are refused, which is recoverable; the under-count was
    // not. A run whose last chunk made it is already closed, whatever followed.
    while (!pending.empty()) discard(pending.begin()->first);

    // The linkage the walk and the fold both attached, reconciled once - see
    // `BoundCabin::DedupeEntryLinkage`. Without it §7's `VerifyAgainstEntries`
    // double-counts an entry written after the checkpoint into a group that
    // existed at it, and answers Corruption on a directory that is in fact
    // correct.
    for (const RecoverableAssertion& a : assertions) {
        if (AssertionRecoveryResult* r = result_for(a.assertion_id);
            r != nullptr && r->recovered) {
            r->duplicate_links_dropped = a.cabin->DedupeEntryLinkage();
        }
    }

    if (log != nullptr) {
        for (const AssertionRecoveryResult& r : report.assertions) {
            if (!r.recovered) {
                const std::string found =
                    r.partial_runs_discarded == 0
                        ? " found no group snapshot at or after the last checkpoint"
                        : " found only group snapshots cut short (" +
                              std::to_string(r.partial_runs_discarded) + " run(s) discarded)";
                log->Error("recovery", "assertion " + std::to_string(r.assertion_id) + found +
                                           ", so it cannot enforce until it is dropped and "
                                           "created again (docs/spec/assertion.md §7)");
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
