#include "kds/exec/tuple_verify.hpp"

#include "kds/exec/row_codec.hpp"

namespace kds::exec {

VerifiedTuple VerifyTupleAt(storage::PageStore& store, PageId page_id, std::uint16_t slot,
                            std::uint64_t expected_pk, std::uint32_t recorded_epoch) {
    VerifiedTuple out;

    auto bytes = store.GetForRead(page_id);
    if (!bytes.ok()) {
        out.outcome = VerifyOutcome::kPageGone;
        return out;
    }

    heap::PageView page(bytes.value().bytes());

    // Rule 2, before the slot is read: a bumped epoch means tuples on this
    // page have moved, so nothing recorded against it may be trusted -
    // including the slot directory the read below would consult. Compared
    // at the entry formats' stored width (u32); see the header comment for
    // why the truncation is harmless.
    if (static_cast<std::uint32_t>(page.RelayoutEpoch()) != recorded_epoch) {
        out.outcome = VerifyOutcome::kEpochMismatch;
        return out;
    }

    auto tuple = page.ReadTuple(slot);
    if (!tuple.ok()) {
        out.outcome = VerifyOutcome::kSlotGone;
        return out;
    }

    // The tuple *actually there* must be the one the entry named. A decode
    // failure counts as a mismatch rather than as corruption: the caller is
    // about to read this row through the authoritative path anyway, and if
    // the bytes really are damaged that path reports it - with the relation
    // and the schema in hand, which this function does not have.
    auto found_pk = RowKeystoneId(tuple.value().payload);
    if (!found_pk.ok() || found_pk.value() != expected_pk) {
        out.outcome = VerifyOutcome::kPkMismatch;
        return out;
    }

    out.outcome = VerifyOutcome::kOk;
    out.page = page;
    return out;
}

std::uint32_t CurrentRelayoutEpoch(storage::PageStore& store, PageId page_id) {
    auto bytes = store.GetForRead(page_id);
    if (!bytes.ok()) return 0;
    return static_cast<std::uint32_t>(storage::GetRelayoutEpoch(bytes.value().bytes()));
}

}  // namespace kds::exec
