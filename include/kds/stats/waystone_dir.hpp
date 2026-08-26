#pragma once

#include <cstddef>
#include <cstdint>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/stats/instance_key.hpp"
#include "kds/storage/page_store.hpp"

// The per-pattern waystone directory (docs/spec/waystone-concpets.md §5): the
// second level of addressing, turning an `arg_hash` into the waystone page
// holding that pattern instance's trail.
//
//   pattern_id --> sys.patterns row           (catalog lookup, cached)
//   arg_hash   --> waystone for that instance (this file)
//
// The shape is the inode block map: interior pages of 2048 child PageIds,
// walked by the base-2048 digits of the `arg_hash`, most significant digit
// first, lazily allocated. The root and its depth are stored on the
// pattern's catalog row (`waystone_root`, `dir_depth`) and are handed in
// and out here as plain values - this layer never touches the catalog.
//
// ---- A hash directory, not a radix index --------------------------------
//
// The structure this replaces walked digits of a *pk*: a dense key issued
// in order, where a directory of depth d covered exactly the ids below
// 2048^d * 256 and anything above that was a caller bug to be refused.
// Nothing of that survives the rekey. An `arg_hash` is a 64-bit hash with
// no order, no density and no ceiling, which changes two things:
//
//   1. **No key is ever out of range.** A depth-d walk consumes the low
//      11*d bits and ignores the rest, so every hash addresses something.
//      Two hashes agreeing on those bits land on the same slot - a
//      collision, resolved by the waystone's own header (WaystonePageHolds
//      in waystone.hpp), which is what turns it into a miss instead of
//      somebody else's trail. What to do about repeated collisions - chain,
//      displace, or drop - is `[OPEN]` (spec §9) and is deliberately not
//      decided here: this layer resolves an address and nothing else.
//
//   2. **Growth cools the directory; it does not preserve it.** See below.
//
// ---- What GrowPatternDirectory really costs ------------------------------
//
// Growth relinks the root, per spec §5: a new root whose slot 0 points at
// the old one, O(1), nothing rewritten. On the dense pk key that preserved
// every prior mapping, because a key below the old coverage had a zero in
// the new top digit by construction. **A hash does not**, and no O(1)
// growth can make it: after growing to depth d+1 an instance is still
// reachable only if bits [11d, 11d+11) of its `arg_hash` are zero, which
// is 1 in 2048 of them.
//
// So a growth is, in practice, a cache flush. That is *safe* and not a
// correctness question - invariant 8, deleting waystones wholesale may
// cost performance and must never change a result - and it is
// self-healing: the next execution of an instance re-records its trail at
// the new address. The old pages are not leaked either; they stay
// reachable under slot 0, and a colliding lookup that reaches one gets a
// header mismatch and a miss, after which recording overwrites it.
//
// The consequence for the caller is that growth is a *rare* operation to
// be paid for by capacity, not a routine one: each level multiplies the
// addressable instances by 2048, so a directory should be created at the
// depth its pattern needs and grown only when it is genuinely full.
//
// ---- Interior pages are headerless ---------------------------------------
//
// 2048 x 4 bytes tiles 8 KiB exactly, which is the whole reason the fanout
// is 2048: a common page header would cost a child slot and, worse,
// DevicePageStore stamps a checksum at byte offset 4 of every headered
// frame it writes - which on one of these pages is child 1. They are
// therefore allocated through PageStore::CreateNewHeaderless(), which
// records the fact durably. This is the caller spec §10 says the mechanism
// no longer had; waystone *pages* stayed headered, their directory did not.
//
// The cost is that interior pages carry no checksum. A damaged one leads a
// walk to a page that is not the instance's waystone, which the header
// check turns into a miss - the same outcome as a cold directory.
//
// Concurrency: none of its own. Core-local, owned by the pattern's owning
// core (rules.md §3); the caller holds whatever pin/latch discipline
// applies, exactly as with PageView.

namespace kds::stats {

// Child ids per interior page: 8192 / 4 = 2048, tiling the page exactly.
inline constexpr std::size_t kDirFanout = kPageSize / sizeof(PageId);
static_assert(kDirFanout == 2048);
static_assert(kDirFanout * sizeof(PageId) == kPageSize, "child ids must tile the page exactly");

// log2(2048) = 11, so a walk is shifts and masks.
inline constexpr int kDirFanoutBits = 11;
static_assert((std::size_t{1} << kDirFanoutBits) == kDirFanout);
inline constexpr std::uint64_t kDirIndexMask = (std::uint64_t{1} << kDirFanoutBits) - 1;

// Deepest directory the key can justify: ceil(64 / 11) = 6 levels address
// the whole 64-bit `arg_hash`, and a seventh would consume digits that are
// always 0. Derived, not chosen (spec §5).
inline constexpr int kMaxPatternDirDepth = 6;
static_assert(kMaxPatternDirDepth * kDirFanoutBits >= 64);
static_assert((kMaxPatternDirDepth - 1) * kDirFanoutBits < 64,
              "kMaxPatternDirDepth must be the smallest depth covering a 64-bit key");

// Unpopulated ranges hold this at every level; a walk that meets it stops
// and reports a miss rather than allocating. It is kInvalidPageId rather
// than 0 because 0 is a real page id - the superblock's.
inline constexpr PageId kEmptyDirSlot = kInvalidPageId;

// Child slot a walk at `level` (0 = root) uses for `arg_hash` in a
// directory of `depth` levels: the base-2048 digits of the key, most
// significant first. Exposed for tests and for callers reasoning about the
// walk without performing it.
//
// Bits at or above 11*depth are simply not consumed - that is the folding
// described above, not an error. At depth 6 the top digit's shift is 55,
// so its two high bits are always 0; harmless, and the reason a seventh
// level would buy nothing.
constexpr std::size_t DirIndexAt(std::uint64_t arg_hash, int depth, int level) noexcept {
    const int shift = kDirFanoutBits * (depth - 1 - level);
    return static_cast<std::size_t>((arg_hash >> shift) & kDirIndexMask);
}

// Allocates an empty interior page - every slot kEmptyDirSlot - and
// returns its id. One of these is the root of a new pattern's directory;
// the same shape serves at every level. Headerless, for the reason above.
StatusOr<PageId> CreateDirPage(storage::PageStore& store);

// Resolves `key` to the page that would hold its trail, walking `depth`
// levels from `root`.
//
// **Only `key.arg_hash` steers the walk** - the directory is a pattern's
// own, so its `pattern_id` is already implied by `root`. The whole instance
// travels anyway, because the returned page id is not usable without it:
// every caller owes a WaystonePageHolds() check against the same pair, and
// taking the pair here is what stops a caller from resolving with one
// instance and validating with another (instance_key.hpp).
//
// Returns kInvalidPageId when any level holds kEmptyDirSlot: that range
// was never populated, which is a normal answer and not an error - on the
// replay path it is the ordinary case for an instance nobody has recorded.
//
// **A returned page id is an address, not an answer.** It may hold a
// foreign instance's trail (a collision, or the cold half of a growth) or
// nothing at all, and every caller must run WaystonePageHolds() on it
// before reading a single entry.
//
// Fails with InvalidArgument for a depth outside [1, kMaxPatternDirDepth],
// and with whatever the store reports for a child id that does not
// resolve.
StatusOr<PageId> LookupWaystonePage(storage::PageStore& store, PageId root, int depth,
                                    const InstanceKey& key);

// The same walk, allocating the interior pages the path needs and linking
// each into its parent, and allocating the target page if the leaf slot is
// empty. Returns its id.
//
// A freshly allocated target is **zeroed, not formatted**: it reads back
// as PageType::kInvalid, so WaystonePageHolds() reports false for it and a
// reader falls through exactly as it would for an unpopulated slot. That is
// deliberate - formatting it belongs to whatever writes the trail (P08),
// and so does the decision of what to do when the slot already holds
// another instance's page, which is the `[OPEN]` collision policy. This
// function reports where; it never displaces.
StatusOr<PageId> LookupOrCreateWaystonePage(storage::PageStore& store, PageId root, int depth,
                                            const InstanceKey& key);

// Deepens a directory by one level: allocates a new root whose slot 0
// points at `root`, and returns it. The caller raises its stored depth by
// one at the same time - the two are one fact, which is why
// Catalog::SetPatternWaystoneRoot() writes them together.
//
// Read the growth note in this file's header before calling: the prior
// contents survive only for the 1-in-2048 of keys whose new top digit is
// zero, and everything else is cooled, not corrupted.
//
// Fails with OutOfRange at kMaxPatternDirDepth, which already addresses
// every bit of the key.
StatusOr<PageId> GrowPatternDirectory(storage::PageStore& store, PageId root, int depth);

}  // namespace kds::stats
