#pragma once

// **The volume census** (`instructions/v3.2.0/workorder-dv-unreachable-page-reclaim.md`,
// DV-S0): a mark from every root the catalog names over a quiescent volume,
// then every allocated page the mark did not reach, classed by the page's
// type and `owner_oid`.
//
// Protocol: read-only and single-threaded. The caller guarantees no writer
// (DV-R2): the census holds no lock, takes no page latch beyond each read's
// pin, and a page allocated while it runs may be reported as unmarked - which
// is why nothing here frees (DV-R3 is the sweep's rule, DV-S3). A page's
// allocation is read from the store (`IsAllocated`), never from a free-map
// page's bytes.
//
// **The mark is complete or it is not a mark** (DV-R1): any root that could
// not be walked - an unreadable page, a link to a page of the wrong class, a
// cycle, a page reached twice - sets `complete` false and names itself in
// `refusals`. A report with `complete == false` proves nothing about its
// `unmarked` list.
//
// Roots, each listed from the code at the commit named in the order:
//   fixed      every page below `kFirstUserPageId`, every map page;
//   catalog    each `catalog::kCatalogRoots` chain, by `next_page_id`, and
//              its `sys.tables` var-heap chain;
//   relations  each live relation's anchor and var-heap root, and each
//              tombstone's pending roots, through `PageReclaimer::ReachFrom`
//              (its trees, its indexes' trees, its heap pages, its chain);
//   creates    each create that did not stand and is still owed by the
//              reclaim (`PageReclaimer::UndoneCreates`), the same way;
//   assertions each `AssertionDef::cabin_root` chain (`kCabinBound`);
//   waystone   each opted row's directory: `dir_depth` interior levels, then
//              its trail pages.
// **Undo pages are a class apart** (`unprovable`): a tuple's `undo_ptr` may
// name a record on a previous run's page, so no root list proves such a page
// unreachable without reading every tuple; they are counted, never listed as
// unmarked.

#include <compare>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"

namespace kds::catalog {
class Catalog;
}
namespace kds::storage {
class DevicePageStore;
}

namespace kds::server {

class PageReclaimer;

struct VolumeCensus {
    // One row of the report: a page type and an owner.
    struct Key {
        std::string page_class;  // `PageType`'s name, or "waystone_dir" for a headerless page
        std::uint64_t owner = 0;
        auto operator<=>(const Key&) const = default;
    };

    std::uint64_t allocated = 0;
    std::uint64_t marked = 0;
    std::uint64_t unmarked = 0;
    std::uint64_t unprovable = 0;        // undo pages: allocated, not marked, not unmarked
    bool complete = true;                // DV-R1: every root walked with no refusal
    std::vector<std::string> refusals;   // why `complete` is false
    std::map<Key, std::uint64_t> unmarked_by_class;
    // Roots walked, by kind, so a report shows what it covered.
    std::map<std::string, std::uint64_t> roots_walked;
};

// Runs the census over ids [0, `page_limit`). `live_undo_pages` is this
// run's undo chain and recycle list (`txn::UndoLog::AppendOwnedPages`), every
// core's: marked, since this run's readers reach them. Errors only when a
// catalog read that names the roots fails outright; a root that cannot be
// walked is a refusal in the report.
StatusOr<VolumeCensus> RunVolumeCensus(storage::DevicePageStore& store, catalog::Catalog& catalog,
                                       PageReclaimer& reclaimer, std::uint32_t page_limit,
                                       std::span<const PageId> live_undo_pages);

// The report as one line, for the mount's log (DV-S1): the counts, the
// unmarked pages by class, and the first refusal of a mark that is not whole.
std::string DescribeVolumeCensus(const VolumeCensus& census);

}  // namespace kds::server
