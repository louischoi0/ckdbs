#pragma once

#include <array>
#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

// The one PAGE_HANDOFF emitter (PW1c-1; log_page_init.hpp's shape, for its
// reason - hand-copied appends of the same record are how six PAGE_INIT
// emitters happened).
//
// The PL §9 rule 1 ordering is the *caller's* to keep, and it is a
// correctness statement, not a preference: the page must be flushed
// durable **before** this append, and the returned LSN must be durable
// **before** the incoming core is granted write rights. This function only
// writes the fact; PW1c-4's publish path owns the sequence.
//
// Non-transactional (txn_id 0): a handoff is an ownership event, not part
// of any transaction's atom - it neither commits nor rolls back.
//
// A null `wal` answers kNoLsn, matching every sibling emitter - though
// unlike a PAGE_INIT an unlogged handoff has nothing to protect, since an
// unlogged store recovers nothing.

namespace kds::wal {

inline StatusOr<Lsn> LogPageHandoff(WalManager* wal, PageId page_id,
                                    std::uint32_t incoming_core) {
    if (wal == nullptr) return kNoLsn;
    std::array<std::byte, kPageHandoffPayloadSize> buf{};
    if (auto n = EncodePageHandoff(buf, PageHandoffPayload{incoming_core}); !n.ok()) {
        return n.status();
    }
    return wal->Append(RecordSpec{RecordType::kPageHandoff, /*txn_id=*/0, page_id}, buf);
}

}  // namespace kds::wal
