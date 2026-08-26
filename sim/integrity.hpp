#pragma once

// sim/integrity.hpp — the instance-wide integrity sweep (bench/workplan-
// teststrategy SIM02): CheckInstance() walks a **quiesced** instance and
// reports every violated structural invariant it can see, each finding
// tagged with the check that produced it so a corrupted byte names its
// category.
//
// What it checks, against docs/spec/heap-and-tuple.md's numbered invariants:
//
//   kPageHeader     page header validates; on the device-backed overload,
//                   every allocated non-headerless page is readable (which
//                   re-verifies its checksum on a cold read) and validates.
//   kCatalog        ListTables walks, oids and names are unique, every
//                   user relation resolves to a TableAccess.
//   kChainOrder     invariants 2 and 3: every tuple's id >= its page's
//                   min_key, and each page's ids sit entirely below the
//                   successor page's min_key (heap chains and btree leaf
//                   walks alike).
//   kBtreeStructure separators: an internal entry's sep_key equals its
//                   child page's min_key, entries ascend, levels descend.
//   kKeystone       invariants 5 and 7: id <= 2^40-1, reserved bits zero.
//   kRowSize        invariant 13: payload length equals the relation's
//                   RowLayout row_size, exactly.
//   kTrxId          a stored trx_id at or above the superblock's persisted
//                   next_trx_id names a transaction that was never issued.
//   kUndoPtr        every nonzero undo_ptr decodes plausibly and points at
//                   a kUndo page. The chain walk it once included was
//                   retired by the undo purge - a settled page recycles,
//                   so a committed tuple's pointer may legally name bytes
//                   that belong to newer records now (integrity.cpp says
//                   why the two surviving halves stay true under reuse).
//   kVarHeap        the per-relation var-heap chain is walkable and typed
//                   kVarHeap; every kSpilled cell resolves to bytes of the
//                   recorded length on a page of this relation's own chain.
//
// Deliberately out of scope for v1, stated rather than implied: catalog
// relations (`sys.*`) are checked at the catalog level only — their rows
// are typed catalog codecs, not user tuples, so the per-tuple checks do
// not apply to them (sys.pattern_defs, the one exception, is skipped with
// them). The documented-gap ghost detector (docs/spec/txn.md section 8) lives
// in the loop's oracle reconciliation, not here: an uncommitted-but-
// surviving row is structurally indistinguishable from a committed one,
// which is precisely the gap.
//
// The sweep never mutates and holds no page span across a cross-page
// fetch: per-tuple facts that need another page (undo pointers, spilled
// cells) are collected during the walk and resolved after it — the same
// span discipline docs/spec/parser-v2.md I15 R1 imposes on the executor.

#include <cstdint>
#include <string>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/page_device.hpp"
#include "kds/storage/page_store.hpp"

namespace kds::sim {

enum class CheckKind : std::uint8_t {
    kPageHeader = 0,
    kCatalog = 1,
    kChainOrder = 2,
    kBtreeStructure = 3,
    kKeystone = 4,
    kRowSize = 5,
    kTrxId = 6,
    kUndoPtr = 7,
    kVarHeap = 8,
};

const char* CheckKindName(CheckKind kind);

struct Finding {
    CheckKind kind;
    PageId page_id;  // kInvalidPageId when the finding is not page-scoped
    std::string detail;
};

struct IntegrityReport {
    std::vector<Finding> findings;

    // Coverage counters, so "clean" is distinguishable from "looked at
    // nothing".
    std::size_t relations_swept = 0;
    std::size_t pages_swept = 0;
    std::size_t tuples_swept = 0;

    bool ok() const { return findings.empty(); }

    // Findings of one category — the corruption tests key on this.
    std::size_t CountOf(CheckKind kind) const;

    // One line per finding, or "integrity clean" with the coverage counts.
    std::string Summary() const;
};

// The catalog-reachable sweep: everything the catalog can name, walked
// through the same PageStore interface the engine reads through.
IntegrityReport CheckInstance(storage::PageStore& store, catalog::Catalog& catalog);

// The device-backed sweep adds what only a DevicePageStore can offer: every
// *allocated* page is fetched (verifying its checksum if it is not already
// resident) and its header validated, headerless pages excluded by the
// store's own map.
IntegrityReport CheckInstance(storage::DevicePageStore& store,
                              storage::PageDevice& device,
                              catalog::Catalog& catalog);

}  // namespace kds::sim
