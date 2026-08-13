#include "kds/stats/trail_store.hpp"

#include "kds/stats/waystone_dir.hpp"

namespace kds::stats {

namespace {

// One executor observation as the on-disk entry format.
//
// `page_epoch` is the executor's observed value narrowed to the entry
// format's u32 (workplan PX04; the layout did not change - the field was
// always here, written 0 while the engine had no epoch). The truncation is
// this layer's decision to make, and it is safe: a wrap needs 2^32
// relayouts of one page, and even a wrap collision only readmits the
// location to the Keystone-id check (R4's pairing rule).
WaystoneEntry EntryOf(const exec::TouchedTuple& touched) noexcept {
    WaystoneEntry entry{};
    entry.pk = touched.pk;
    entry.rel_oid = touched.rel_oid;
    entry.page_id = touched.page_id;
    entry.page_epoch = static_cast<std::uint32_t>(touched.page_epoch);
    entry.slot = touched.slot;
    entry.step_id = touched.step_id;
    // Never inferred from `pk != 0`: a never-written entry reads back
    // all-zero, and pk 0 with page_id 0 are both plausible-looking values
    // (waystone.hpp).
    entry.flags = kWaystoneEntryValid;
    entry.reserved = 0;
    return entry;
}

// Whether the page already holds exactly the trail `touched` describes.
//
// Deliberately strict: same instance, same count, and every field of every
// entry equal in the same order. Anything less would let a page "match"
// while pointing somewhere else, which is the one mistake this cannot
// afford to make - a false match skips the write and leaves a stale trail
// behind, and a stale trail is what the replay contract exists to survive
// rather than something to manufacture on purpose.
//
// `recorded_ts` is **not** compared: it changes on every execution by
// construction, so comparing it would make every trail look different and
// buy nothing. The cost is that a trail whose contents never change keeps
// its original timestamp, which is a retention input and best-effort by
// contract (waystone.hpp).
bool TrailIsUnchanged(std::span<const std::byte, kPageSize> page, const InstanceKey& key,
                      std::span<const exec::TouchedTuple> touched) {
    if (!WaystonePageHolds(page, key)) return false;

    const WaystoneHeader header = ReadWaystoneHeader(page);
    if (header.entry_count != touched.size()) return false;

    for (std::size_t i = 0; i < touched.size(); ++i) {
        auto stored = ReadWaystoneEntry(page, i);
        if (!stored.ok()) return false;
        const WaystoneEntry want = EntryOf(touched[i]);
        const WaystoneEntry& got = stored.value();
        if (got.pk != want.pk || got.rel_oid != want.rel_oid || got.page_id != want.page_id ||
            got.slot != want.slot || got.step_id != want.step_id ||
            got.page_epoch != want.page_epoch || got.flags != want.flags) {
            return false;
        }
    }
    return true;
}

}  // namespace

Status WriteTrail(storage::PageStore& store, PageId root, int depth, const InstanceKey& key,
                  std::span<const exec::TouchedTuple> touched, std::uint64_t recorded_ts) {
    if (touched.empty()) {
        // An empty trail is not a trail. Writing one would replace a
        // populated trail with nothing, which is a worse answer than
        // leaving the previous execution's - and the caller has no reason
        // to ask for it.
        return Status::InvalidArgument("waystone: refusing to record an empty trail");
    }
    if (touched.size() > kMaxTrailEntries) {
        // Refused whole, never truncated. See trail_store.hpp: a truncated
        // trail is indistinguishable from a complete one to every reader.
        return Status::OutOfSpace("waystone: a trail of " + std::to_string(touched.size()) +
                                  " entries exceeds the " + std::to_string(kMaxTrailEntries) +
                                  "-entry page limit; not recorded");
    }

    auto page_id = LookupOrCreateWaystonePage(store, root, depth, key);
    if (!page_id.ok()) return page_id.status();

    // ---- The unchanged trail, which is the steady state --------------------
    //
    // Once an instance is hot, every execution re-records - and finds
    // exactly what it recorded last time. Nothing moves a tuple: the
    // fixed-length rule stops an UPDATE migrating one (invariant 13) and
    // nothing relayouts, so a stable query over stable data produces the
    // same entries at the same addresses forever.
    //
    // Rewriting them costs a **dirtied page**, and a dirtied page costs a
    // device write at the next flush. Comparing first costs a read that
    // does not dirty anything. Measured on tools/join_benchmark.py, the
    // rewrite was most of an 18% p50 regression on a point join; this
    // removes it for the case that dominates.
    //
    // Read through GetForRead(), which is the whole point - a Get() here
    // would dirty the page just to look at it and defeat the exercise.
    if (auto existing = store.GetForRead(page_id.value());
        existing.ok() && TrailIsUnchanged(existing.value().bytes(), key, touched)) {
        return Status::OK();
    }

    auto bytes = store.Get(page_id.value());
    if (!bytes.ok()) return bytes.status();

    // A freshly allocated target reads back as PageType::kInvalid - the
    // directory leaves it zeroed rather than formatted, deliberately
    // (waystone_dir.hpp), because formatting is the writer's business and
    // so is what to do about an occupant.
    if (!WaystonePageHolds(bytes.value().bytes(), key) && !TrailDisplacesOnCollision()) {
        // Under a *drop* policy this is where a colliding instance gives
        // up. Unreachable while the policy is displace; kept so the branch
        // the other choice needs is written down rather than described.
        return Status::AlreadyExists("waystone: the target page holds another instance's trail");
    }

    // Formatted unconditionally, which is what makes the overwrite
    // wholesale: the header's entry_count goes back to 0 and every stale
    // entry below it becomes unreachable in one step. A merge would have to
    // read the old entries first, and nothing here can tell one that still
    // qualifies from one that does not.
    FormatWaystonePage(bytes.value().bytes(), key, recorded_ts);

    for (std::size_t i = 0; i < touched.size(); ++i) {
        if (Status s = WriteWaystoneEntry(bytes.value().bytes(), i, EntryOf(touched[i])); !s.ok()) {
            return s;
        }
    }

    WaystoneHeader header = ReadWaystoneHeader(bytes.value().bytes());
    header.entry_count = static_cast<std::uint16_t>(touched.size());
    // Always terminal: this layer never continues a trail onto a second
    // page (trail_store.hpp). FormatWaystonePage already set it, and it is
    // restated here so the one place a continuation would be linked is
    // visible.
    header.next_page_id = kInvalidPageId;
    return WriteWaystoneHeader(bytes.value().bytes(), header);
}

StatusOr<std::vector<WaystoneEntry>> ReadTrail(storage::PageStore& store, PageId root, int depth,
                                               const InstanceKey& key) {
    std::vector<WaystoneEntry> out;

    auto page_id = LookupWaystonePage(store, root, depth, key);
    if (!page_id.ok()) return page_id.status();
    // An unpopulated range: normal, and the ordinary case for an instance
    // nobody has recorded.
    if (page_id.value() == kInvalidPageId) return out;

    // Read-only: a reader must not dirty a page it only looked at.
    auto bytes = store.GetForRead(page_id.value());
    if (!bytes.ok()) {
        // A directory child that does not resolve is damage to a headerless
        // interior page, which carries no checksum to catch it
        // (waystone_dir.hpp). For a *reader* that is a miss, not an error:
        // the whole point of the fall-through is that it costs a descent
        // and never an answer.
        if (bytes.status().code() == StatusCode::kNotFound) return out;
        return bytes.status();
    }

    // The identity check, and it is load-bearing: the directory is keyed by
    // a hash of `arg_hash`, so a collision leads here to a real, valid,
    // *wrong* trail. This is what makes that a miss instead of somebody
    // else's rows.
    if (!WaystonePageHolds(bytes.value().bytes(), key)) return out;

    const WaystoneHeader header = ReadWaystoneHeader(bytes.value().bytes());
    const std::size_t count = header.entry_count <= kEntriesPerWaystonePage
                                  ? header.entry_count
                                  : kEntriesPerWaystonePage;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto entry = ReadWaystoneEntry(bytes.value().bytes(), i);
        if (!entry.ok()) return entry.status();
        // A never-written entry reads all-zero; the valid flag is the only
        // safe test for one (waystone.hpp).
        if ((entry.value().flags & kWaystoneEntryValid) == 0) continue;
        out.push_back(entry.value());
    }
    return out;
}

}  // namespace kds::stats
