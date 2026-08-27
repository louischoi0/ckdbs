#include "kds/exec/assertion_replay.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "kds/storage/cabin_bound_page.hpp"
#include "kds/wal/payload.hpp"

namespace kds::exec {

namespace {

using storage::cabin::BoundCabinEntry;
using storage::cabin::BoundCabinPage;
using storage::cabin::kEntryBytes;

std::string KeyOf(std::span<const std::byte> bytes) {
    std::string key;
    key.resize(bytes.size());
    if (!bytes.empty()) std::memcpy(key.data(), bytes.data(), bytes.size());
    return key;
}

// The shared half of RESERVE and BUILD: the entry onto the page, the delta
// and the entry into the directory. `must_be_reserved` is the one bit the
// two types disagree on, and checking it here is what keeps them from being
// confused on disk (wal/payload.hpp).
Status ReplayEntry(const wal::DecodedRecord& record, storage::PageStore& store,
                   AssertionReplayContext& context, bool must_be_reserved) {
    auto decoded = wal::DecodeAssertEntry(record.payload);
    if (!decoded.ok()) return decoded.status();
    const wal::DecodedAssertEntry& d = decoded.value();

    if (d.fields.entry_len != kEntryBytes) {
        return Status::Corruption("assert replay: entry is " +
                                  std::to_string(d.fields.entry_len) + " bytes, the format is " +
                                  std::to_string(kEntryBytes));
    }
    const BoundCabinEntry entry =
        storage::cabin::DecodeEntry(std::span<const std::byte, kEntryBytes>(d.entry.data(),
                                                                            kEntryBytes));
    if (entry.reserved() != must_be_reserved) {
        // A RESERVE writes a reserved entry and a BUILD a committed one, by
        // definition of the two types. Bytes that say otherwise are intact
        // and wrong - Corruption, never a guess about which side lied.
        return Status::Corruption(must_be_reserved
                                      ? "assert replay: ASSERT_RESERVE entry is not reserved"
                                      : "assert replay: ASSERT_BUILD entry is reserved");
    }
    if (!must_be_reserved && entry.departure()) {
        // The builder writes arrivals only: every row it incorporates is a
        // row that exists. A departure can arrive only through a RESERVE.
        return Status::Corruption("assert replay: ASSERT_BUILD entry is a departure");
    }

    BoundCabin* cabin = context.CabinOf(d.fields.assertion_id);
    if (cabin == nullptr) return Status::OK();  // the skip rule, header comment

    auto page = store.Get(record.header.page_id);
    if (!page.ok()) return page.status().WithContext("assert replay: fetching the entry page");
    auto opened = BoundCabinPage::Open(page.value().bytes());
    if (!opened.ok()) return opened.status().WithContext("assert replay: opening the entry page");
    BoundCabinPage& view = opened.value();

    if (d.fields.index == view.entry_count()) {
        auto at = view.Append(entry);
        if (!at.ok()) return at.status().WithContext("assert replay: appending the entry");
        if (at.value() != d.fields.index) {
            return Status::Corruption("assert replay: append landed at " +
                                      std::to_string(at.value()) + ", the record says " +
                                      std::to_string(d.fields.index));
        }
    } else if (d.fields.index < view.entry_count()) {
        // Already on the page - the page was flushed after the write and this
        // fold is restoring the directory. The bytes are the same bytes, so
        // rewriting them is the idempotent half the header documents.
        if (Status s = view.Write(d.fields.index, entry); !s.ok()) {
            return s.WithContext("assert replay: rewriting the entry");
        }
    } else {
        return Status::Corruption("assert replay: entry index " +
                                  std::to_string(d.fields.index) + " is past the append point " +
                                  std::to_string(view.entry_count()));
    }

    // The entry's inline value is the group delta (wal/payload.hpp: a COUNT
    // assertion writes 1), so the directory fold reads it and nothing else -
    // with the departure flag saying which sign it applies under.
    const std::string key = KeyOf(d.key);
    // AS6a: the record's id decides which group this is, before the fold can
    // create one under an id of its own choosing (`bound_cabin.hpp`).
    if (Status s = cabin->AdoptGroupId(key, d.fields.group_id); !s.ok()) {
        return s.WithContext("assert replay: binding the entry's group id");
    }
    Status applied = entry.departure()
                         ? cabin->ApplyDeparture(key, entry.value, record.header.page_id,
                                                 d.fields.index)
                         : cabin->Apply(key, entry.value, record.header.page_id, d.fields.index);
    return applied.WithContext("assert replay: applying the group delta");
}

Status ReplayCommit(const wal::DecodedRecord& record, storage::PageStore& store,
                    AssertionReplayContext& context) {
    auto decoded = wal::DecodeAssertCommit(record.payload);
    if (!decoded.ok()) return decoded.status();
    const wal::DecodedAssertCommit& d = decoded.value();

    if (context.CabinOf(d.fields.assertion_id) == nullptr) return Status::OK();

    auto page = store.Get(record.header.page_id);
    if (!page.ok()) return page.status().WithContext("assert replay: fetching the entry page");
    auto opened = BoundCabinPage::Open(page.value().bytes());
    if (!opened.ok()) return opened.status().WithContext("assert replay: opening the entry page");
    BoundCabinPage& view = opened.value();

    for (std::uint16_t i = 0; i < d.fields.count; ++i) {
        if (Status s = view.ClearReserved(d.index_at(i)); !s.ok()) {
            return s.WithContext("assert replay: clearing the reserved flag");
        }
    }
    // The directory is untouched on purpose: a reservation counted in the
    // aggregate from admission (§6.2), so commit moves flags and never sums.
    return Status::OK();
}

Status ReplayRollback(const wal::DecodedRecord& record, storage::PageStore& store,
                      AssertionReplayContext& context) {
    auto decoded = wal::DecodeAssertRollback(record.payload);
    if (!decoded.ok()) return decoded.status();
    const wal::DecodedAssertRollback& d = decoded.value();

    BoundCabin* cabin = context.CabinOf(d.fields.assertion_id);
    if (cabin == nullptr) return Status::OK();

    // Nothing shrinks: the orphaned slot is still the recorded leak that rides
    // on purge. What the page does take is the `kEntryOrphaned` mark a live
    // abort writes, so a replayed page and a live one agree - the linkage
    // rebuild reads these pages and nothing else, so a mark only the live path
    // wrote would make a recovered cabin disagree with a crashed-and-recovered
    // one (`docs/spec/assertion.md` §7).
    //
    // The (page_id, index) pair is the entry's *name* here, which Unapply
    // confirms against the group's list. The envelope's flag byte says whether
    // the reservation was a departure, which decides the sign the compensation
    // restores under.
    auto page = store.Get(record.header.page_id);
    if (!page.ok()) return page.status().WithContext("assert replay: fetching the entry page");
    auto opened = BoundCabinPage::Open(page.value().bytes());
    if (!opened.ok()) return opened.status().WithContext("assert replay: opening the entry page");
    if (Status s = opened.value().MarkOrphaned(d.fields.index); !s.ok()) {
        return s.WithContext("assert replay: marking the entry orphaned");
    }

    const std::string key = KeyOf(d.key);

    // **The linkage may legitimately be absent, and then it is restored for
    // `Unapply` to take away again.** The rebuild that runs before this fold
    // walks the cabin's pages and *skips* an entry the page already marks
    // orphaned (`assertion_recover.cpp`) - which is this entry exactly when the
    // mark reached the device before the crash, the ordinary shape of a crash
    // during a later checkpoint. The compensation must still happen: the base
    // snapshot was taken while the reservation was live, so it counts this
    // delta, and without the subtraction the header keeps a reservation that
    // aborted. `Unapply`'s missing-pair NotFound is a name check for the *live*
    // abort path, where the pair comes from the transaction's own reservation
    // list and its absence is a defect; a rebuild is entitled not to have
    // restored it, and failing here failed the whole recovery pass.
    const std::pair<PageId, std::uint16_t> at{record.header.page_id, d.fields.index};
    if (const GroupHeader* header = cabin->Find(key);
        header != nullptr &&
        std::find(header->entries.begin(), header->entries.end(), at) ==
            header->entries.end()) {
        if (Status s = cabin->AttachEntry(header->group_id, at.first, at.second); !s.ok()) {
            return s.WithContext("assert replay: relinking an entry the page walk skipped");
        }
    }

    const bool departure =
        (record.header.flags & wal::kAssertRollbackFlagDeparture) != 0;
    Status undone = departure ? cabin->UnapplyDeparture(key, d.fields.delta, at.first, at.second)
                              : cabin->Unapply(key, d.fields.delta, at.first, at.second);
    return undone.WithContext("assert replay: compensating a reservation");
}

}  // namespace

bool IsAssertionRecord(wal::RecordType type) noexcept {
    switch (type) {
        case wal::RecordType::kAssertReserve:
        case wal::RecordType::kAssertCommit:
        case wal::RecordType::kAssertRollback:
        case wal::RecordType::kAssertBuild:
        case wal::RecordType::kAssertDrop:
            return true;
        default:
            return false;
    }
}

Status ReplayAssertionRecord(const wal::DecodedRecord& record, storage::PageStore& store,
                             AssertionReplayContext& context) {
    switch (record.type()) {
        case wal::RecordType::kAssertReserve:
            return ReplayEntry(record, store, context, /*must_be_reserved=*/true);
        case wal::RecordType::kAssertBuild:
            return ReplayEntry(record, store, context, /*must_be_reserved=*/false);
        case wal::RecordType::kAssertCommit:
            return ReplayCommit(record, store, context);
        case wal::RecordType::kAssertRollback:
            return ReplayRollback(record, store, context);
        case wal::RecordType::kAssertDrop: {
            auto decoded = wal::DecodeAssertDrop(record.payload);
            if (!decoded.ok()) return decoded.status();
            context.Drop(decoded.value().assertion_id);
            return Status::OK();
        }
        default:
            return Status::InvalidArgument(std::string("assert replay: not an assertion record: ") +
                                           wal::RecordTypeName(record.type()));
    }
}

}  // namespace kds::exec
