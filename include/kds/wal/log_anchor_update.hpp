#pragma once

#include <array>
#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

// The one ANCHOR_UPDATE emitter (PW2; log_page_handoff.hpp's shape, for
// its reason). One anchor slot moved: index_oid 0 is the clustered root,
// anything else that index's root. The caller stamps the page through
// StampPageLsn with the returned LSN, like every logged mutation.
//
// A null `wal` answers kNoLsn, matching every sibling emitter.

namespace kds::wal {

inline StatusOr<Lsn> LogAnchorUpdate(WalManager* wal, std::uint64_t trx_id, PageId anchor_page,
                                     std::uint64_t index_oid, PageId root) {
    if (wal == nullptr) return kNoLsn;
    std::array<std::byte, kAnchorUpdatePayloadSize> buf{};
    if (auto n = EncodeAnchorUpdate(buf, AnchorUpdatePayload{index_oid, root}); !n.ok()) {
        return n.status();
    }
    return wal->Append(RecordSpec{RecordType::kAnchorUpdate, trx_id, anchor_page}, buf);
}

}  // namespace kds::wal
