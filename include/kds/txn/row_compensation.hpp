#pragma once

#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/manager.hpp"

// The three row compensations - an insert retired, an overwrite restored, a
// delete mark cleared - each a page mutation and its record, logged.
//
// **They live here rather than at their callers because there are two of
// them and they must not drift**: the live rollback
// (`TransactionManager::Compensate`) and recovery's undo phase
// (`txn::RecoveryUndo::Apply`) do the same steps on a page the caller has
// found and holds exclusive. Written out twice, the record type of one arm
// drifted once already (the unmark logged as a mark until 2026-08-11);
// `varheap_release.hpp` is the precedent for the fourth compensation.
//
// Each mutates `page` first and returns OK after it when `wal` is null;
// otherwise it appends its record under **the aborting transaction's id, not
// `kNoTxnId`** - `payload.hpp` says no transaction owns a SLOT_RETIRE, which is
// true of a purge pass and false of a rollback, and `kNoTxnId` would hide the
// rollback from the next analysis phase (`txn.md` §6's amendment) - and stamps
// `page_id` behind it. The caller keeps its own switch and its own refusal
// for a type it does not know.

namespace kds::txn {

// Retires `slot` (the loser's insert) and logs `SLOT_RETIRE`.
Status CompensateInsert(storage::PageStore& store, wal::WalManager* wal, std::uint64_t txn_id,
                        PageId page_id, heap::PageView& page, std::uint16_t slot);

// Writes `image` back over `slot` with the prior header and logs
// `HEAP_OVERWRITE`.
Status CompensateOverwrite(storage::PageStore& store, wal::WalManager* wal, std::uint64_t txn_id,
                           PageId page_id, heap::PageView& page, std::uint16_t slot,
                           std::span<const std::byte> image, std::uint64_t prior_trx_id,
                           std::uint64_t prior_undo_ptr);

// Clears `slot`'s delete mark with the prior header and logs
// `HEAP_DELETE_UNMARK`.
Status CompensateDeleteMark(storage::PageStore& store, wal::WalManager* wal, std::uint64_t txn_id,
                            PageId page_id, heap::PageView& page, std::uint16_t slot,
                            std::uint64_t prior_trx_id, std::uint64_t prior_undo_ptr);

}  // namespace kds::txn
