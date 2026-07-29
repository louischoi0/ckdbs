#pragma once

#include "kds/base/status.hpp"
#include "kds/wal/record.hpp"

// The WAL-before-data seam (wal.md section 8-1, storage-layout section 8).
//
// The rule it exists to enforce: a dirty page may be written to the data
// file only once the log records describing its modifications are durable.
// wal.md moved that from caller discipline into code - the per-core
// BufferPool holds one of these and refuses to flush a frame until the
// frame's page_lsn is durable - so the interface lives in its own header,
// small enough that the storage layer can depend on it without depending
// on the WAL implementation.
//
// ---- LSN comparison is strict, and that is not an off-by-one ------------
//
// A record's LSN is the offset of its *first* byte, while durable_lsn() is
// a watermark: every byte *below* it is durable. So the record placed at
// `lsn` is durable once `durable_lsn() > lsn`, never at equality - at
// equality the record's own bytes are exactly what has not been synced.
// Every comparison against a record LSN in this engine is strict for that
// reason, including the page_lsn check in the pool.

namespace kds::wal {

class WalDurability {
public:
    virtual ~WalDurability() = default;

    // Watermark: every byte below this is on stable storage. A page whose
    // page_lsn is `l` is safe to write back iff durable_lsn() > l.
    virtual Lsn durable_lsn() const noexcept = 0;

    // Makes the record placed at `lsn` durable, syncing if it is not
    // already. A no-op when the watermark has passed it, so callers on the
    // flush path pay nothing in the common case where the log ran ahead.
    virtual Status EnsureDurable(Lsn lsn) = 0;

    // durable_lsn() > lsn, spelled out so callers never write the
    // comparison themselves and get the strictness wrong.
    bool IsDurable(Lsn lsn) const noexcept { return durable_lsn() > lsn; }
};

}  // namespace kds::wal
