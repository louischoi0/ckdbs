#include "kds/stats/waystone_probe.hpp"

#include "kds/stats/waystone_dir.hpp"

namespace kds::stats {

StatusOr<ProbeResult> Probe(storage::PageStore& store, const WaystoneRef& ws,
                            const EpochProvider& epochs, std::uint64_t pk) {
    auto entry = LookupEntry(store, ws, pk);
    if (!entry.ok()) {
        // NotFound means the entry page was never allocated - an ordinary
        // outcome for most of a sparse id space, and a miss rather than a
        // failure. Anything else is a real problem worth reporting.
        if (entry.status().code() == StatusCode::kNotFound) return ProbeResult{};
        return entry.status();
    }

    ProbeResult result{};
    result.use_count = entry.value().use_count;

    // Four ways to be unusable, all reported identically on purpose. A
    // caller that handled three of them and forgot the fourth would read
    // whatever tuple happens to sit at a stale location.
    if ((entry.value().flags & kEntryLive) == 0) return result;
    if (entry.value().pk != pk) return result;
    if (entry.value().page_id == kInvalidPageId) return result;
    if (entry.value().page_epoch != epochs.EpochOf(entry.value().page_id)) return result;

    result.trusted = true;
    result.page_id = entry.value().page_id;
    result.slot = entry.value().slot;
    return result;
}

StatusOr<ProbeResult> ProbeAndVerify(storage::PageStore& store, const WaystoneRef& ws,
                                     const EpochProvider& epochs, std::uint64_t pk,
                                     KeystoneIdAt id_at, void* ctx) {
    auto probed = Probe(store, ws, epochs, pk);
    if (!probed.ok() || !probed.value().trusted) return probed;
    if (id_at == nullptr) {
        return Status::InvalidArgument(
            "waystone: ProbeAndVerify needs a Keystone-id reader; a trusted location "
            "may never be used without checking the tuple actually at it");
    }

    // Spec section 3.1 rule 2. The epoch check above says "nothing has
    // rearranged this page since we looked"; this says "and the thing we
    // are about to read is the thing we asked for". The second does not
    // follow from the first - an entry written before a location was ever
    // observed, or one whose epoch was never bumped by a mover that should
    // have bumped it, passes the epoch check and points somewhere wrong.
    const std::optional<std::uint64_t> found =
        id_at(store, probed.value().page_id, probed.value().slot, ctx);
    if (!found.has_value() || *found != pk) {
        // Demoted, not failed: the authoritative path still has the answer.
        ProbeResult miss{};
        miss.use_count = probed.value().use_count;
        return miss;
    }
    return probed;
}

Status Observe(storage::PageStore& store, const WaystoneRef& ws, std::uint64_t pk,
               PageId page_id, std::uint16_t slot, std::uint32_t epoch) {
    auto leaf = LookupEntryPage(store, ws.dir_root, ws.depth, pk);
    if (!leaf.ok()) return leaf.status();
    if (leaf.value() == kInvalidPageId) return Status::OK();  // uncovered; backfill's problem

    auto bytes = store.Get(leaf.value());
    if (!bytes.ok()) return bytes.status();

    auto entry = ReadEntry(std::span<const std::byte, kPageSize>(bytes.value()), EntrySlotOf(pk));
    if (!entry.ok()) return entry.status();
    // Not live, or belonging to some other pk: re-observing either would
    // resurrect a delete-mark or overwrite a neighbour's entry.
    if ((entry.value().flags & kEntryLive) == 0) return Status::OK();
    if (entry.value().pk != pk) return Status::OK();

    WaystoneEntry updated = entry.value();
    updated.page_id = page_id;
    updated.slot = slot;
    updated.page_epoch = epoch;
    // Heat is deliberately untouched: location and heat are separate facts
    // (spec section 5), and re-finding a tuple after a relayout says
    // nothing about how often it is read.
    return WriteEntry(bytes.value(), EntrySlotOf(pk), updated);
}

}  // namespace kds::stats
