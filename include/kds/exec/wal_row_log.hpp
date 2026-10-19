#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/manager.hpp"

// The records a row-codec system relation's writes (`sys.assertions`) owe
// the log, so a declaration - an *enforcing* constraint - survives a crash
// by redo rather than only to the last checkpoint (RV3).
//
// The order is the DML path's, and it is load-bearing (wal.md §11a): the
// new page's PAGE_INIT and the spilled values precede the row record that
// points at them, and the row record's stamp is what the WAL-before-data
// gate holds the page on. Envelopes are `wal::kNoTxnId` throughout -
// these writes belong to no bracketed transaction, and naming the header
// writer is the B1 phantom-loser mistake (catalog.cpp's LogCatRow says
// it in full).

namespace kds::exec {

// The spilled values, each preceded by the page that had to exist for it:
// the extracted body of the dispatcher's LogSpills, which now delegates.
// One spill's records - `LogSpills`' body - for a caller that logs each
// spill as it is appended, under its page's hold (`VarHeapSink::on_append`).
Status LogSpill(wal::WalManager* wal, storage::PageStore& store, const AppendedSpill& spill,
                std::uint64_t env_txn, std::uint64_t owner_oid);

Status LogSpills(wal::WalManager* wal, storage::PageStore& store,
                 const std::vector<AppendedSpill>& spills, std::uint64_t env_txn,
                 std::uint64_t owner_oid);

// Everything one ChainInsert owes: the grown page's PAGE_INIT (min_key
// read back off the landed page - ChainInsertResult does not carry it),
// the repointed old tail as a full page image, the spills, then the
// HEAP_INSERT itself, stamped.
Status LogChainInsert(wal::WalManager* wal, storage::PageStore& store,
                      const heap::ChainInsertResult& placed, std::span<const std::byte> tuple,
                      std::uint64_t hdr_trx, std::uint64_t owner_oid,
                      const std::vector<AppendedSpill>& spills);

// One retired slot, stamped.
Status LogSlotRetire(wal::WalManager* wal, storage::PageStore& store, std::uint64_t env_txn,
                     PageId page_id, std::uint16_t slot);

// One slot's bytes as a `HEAP_INSERT` or `HEAP_OVERWRITE`, stamped: the
// header's writer and undo pointer in the payload, which is what redo
// applies, and `env_txn` in the envelope, which only analysis reads.
Status LogHeapWrite(wal::WalManager* wal, storage::PageStore& store, wal::RecordType type,
                    std::uint64_t env_txn, PageId page_id, std::uint16_t slot,
                    std::span<const std::byte> bytes, std::uint64_t hdr_trx,
                    std::uint64_t hdr_undo);

}  // namespace kds::exec
