#pragma once

#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/manager.hpp"

// **The purge of one key** (BH-R6 phase 2, BH-R7;
// `instructions/v3.0.0/workorder-bh-purge-key.md`).
//
// Protocol, in order, and the order is the correctness:
//
//   1. `btree::BtreeHoldTombstone` holds the key's leaf exclusive and hands
//      its slot back if it is still the tombstone `deleter_trx_id` stamped -
//      otherwise the key is skipped (PU12). The version's spills are read
//      from its bytes before anything is written.
//   2. The slot is retired, and `SLOT_RETIRE` at `wal::kNoTxnId` appended,
//      **under that hold**
//      (AT-S21), so a named `INSERT` that places the key after the retire
//      logs after it too.
//   3. The hold is released, and each spill the retired version pointed at
//      is released with `VARHEAP_RELEASE` at `wal::kNoTxnId`.
//
// **Retire first, release second, and never the reverse** (`[quiet-wrong]`):
// a crash after a release and before the retire would leave the tombstone
// standing, and a re-run would release its spills a second time - by then
// possibly another value's. A crash between the two leaks that row's
// spills, which is the stated gap.
//
// Both records carry `kNoTxnId`, so analysis never counts them as a
// loser's and undo never reverses them (`analysis.cpp`); redo applies each
// with today's applier. No record kind is added.
//
// The caller has judged the key (BH-R3: committed deleter, resolved for
// every reader) and holds the relation's `IX`; this function judges
// nothing but the slot's identity.

namespace kds::exec {

// **How long a `PURGE` waits for a key it may not free yet** (PU4, PU5,
// BH-Q3 (a)): an undecided writer, or a reader whose snapshot predates the
// key's delete. A constant with no configuration key, and deliberately not
// `lock_wait_fault_net_ms`: that bounds a lost wake and logs its firing as
// a fault, while an open older snapshot is the ordinary case this waits
// out. 1 s, after which the statement is refused `TxnConflict` naming the
// key.
inline constexpr std::uint64_t kPurgeHorizonWaitNs = 1'000'000'000;

enum class PurgeOutcome : std::uint8_t {
    kPurged = 0,
    kSkipped = 1,  // absent, or no longer the judged tombstone
};

StatusOr<PurgeOutcome> PurgeKey(storage::PageStore& store, wal::WalManager* wal,
                                const catalog::TableAccess& access, std::uint64_t id,
                                std::uint64_t deleter_trx_id);

}  // namespace kds::exec
