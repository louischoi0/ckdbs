#pragma once

#include <array>
#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

// The one PAGE_INIT emitter (the definition-rows review's finding: six
// hand-copies of the same twelve lines, two added by that change alone).
// Storage-free on purpose - a PAGE_INIT is deliberately unstamped at every
// call site, because the first record that lands in the page stamps it,
// and an empty formatted page is exactly what redo rebuilds. The one
// caller that needs the LSN anyway (the undo log's reclaim arm stamps
// *before* the wipe) takes it from the return.
//
// A null `wal` answers kNoLsn: the unlogged path every socket-free test
// runs, matching every sibling emitter.

namespace kds::wal {

inline StatusOr<Lsn> LogPageInit(WalManager* wal, std::uint64_t txn_id, PageId page_id,
                                 PageType type, std::uint64_t min_key,
                                 std::uint64_t owner_oid) {
    if (wal == nullptr) return kNoLsn;
    std::array<std::byte, kPageInitPayloadSize> buf{};
    const PageInitPayload fields{min_key, static_cast<std::uint8_t>(type), {0, 0, 0},
                                 /*reserved2=*/0, owner_oid};
    if (auto n = EncodePageInit(buf, fields); !n.ok()) return n.status();
    return wal->Append(RecordSpec{RecordType::kPageInit, txn_id, page_id}, buf);
}

}  // namespace kds::wal
