#pragma once

#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/page_header.hpp"

// A Bound Cabin's entry page (`docs/spec/assertion.md` §5.1, workplan AST04).
//
// ---- What a Bound Cabin is, in one line -----------------------------------
//
// A Cabin that is required to have observed everything, forever
// (`docs/spec/cabin.md` §12). The observational class is lazy, evictable and
// advisory; this one is eager, **pinned** and authoritative, because an
// assertion's admission check reads its group aggregate and a missing entry
// would be a wrong answer rather than a slow one.
//
// ---- The entry: 32 bytes, and what each half is for ------------------------
//
// | offset | width | field                                                  |
// |--------|-------|--------------------------------------------------------|
// |    0   |   8   | tuple key (48) | flags (8) | reserved (8): Keystone   |
// |    8   |   8   | location hint: page_id (32) | epoch (16) | slot (16)    |
// |   16   |   8   | aggregate value, int64                                  |
// |   24   |   4   | group_id (AS6a)                                         |
// |   28   |   4   | padding to 32                                           |
//
// **The tuple key is authoritative and the hint is advisory**, which is C2 carried
// over from the observational class unchanged. Under K1 a stored Keystone id
// is a name: it can dangle - the row may be gone - and it comes to mean a
// different row only after a `PURGE` freed it (`heap-and-tuple.md` §4.1c).
// An entry is a contribution, summed and settled, and its tuple key is never
// resolved to read a row, so a re-placed key's old entries stay the old
// row's and cost nothing. A wrong hint costs a descent and never a wrong
// answer. The hint is verified through the one verifier
// (`exec/tuple_verify.hpp`) that Waystone replay and the observational Cabin
// already share.
//
// **The aggregate value is inline** so a re-summation - the integrity check
// §7 asks for, and the repair path if a header is ever doubted - reads the
// entries and nothing else. For a `COUNT(*)` assertion it is written 1, which
// makes re-summation one code path rather than two.
//
// The last 4 bytes are padding: §5.1 fixes the entry at 32 bytes so
// `entry_index × 32` is a shift, and `group_id` took the first half of what
// was 8 bytes of padding without a format change.
//
// ---- Page layout -----------------------------------------------------------
//
// The common 32-byte page header (S1), then a small page header, then the
// entry array. Headered and checksummed like any authoritative class: this is
// committed data, so `kVarHeap`'s argument applies and the advisory rules that
// govern a waystone page do not.
//
// Concurrency: none of its own. The pages sit under the page latch like any
// page, and the structure they hold is guarded by the instance's assertion
// registry under its directory latch since AT-S5d (`bound_cabin.hpp`,
// `docs/spec/assertion.md` §6.1). No atomics here.

namespace kds::storage::cabin {

// `flags` bit 0: the entry was written by a statement that has not committed
// (`docs/spec/assertion.md` §6.2 step 3). Cleared at commit, and the entry
// plus its delta are removed at abort.
//
// **A reservation counts in the aggregate from the moment of admission**,
// which is what makes false *admissions* impossible - the property §6.2 calls
// out - at the price of bounded false rejections, which are accepted and
// documented there.
inline constexpr std::uint8_t kEntryReserved = 0x1;

// `flags` bit 1: the location hint is worth trying. Cleared - never the entry
// dropped - when a hint has been proven wrong and there was nothing better to
// heal it with, exactly as `stats::kCabinHintValid` is for the observational
// class. The tuple key is the authority, so an entry with no usable hint is still a
// complete, correct entry that costs a descent.
inline constexpr std::uint8_t kEntryHintValid = 0x2;

// `flags` bit 2: the entry records a row *leaving* its group - an UPDATE's
// departure half (§4.2's group-move row) or a DELETE - so its contribution
// to re-summation is (-1, -value) where an ordinary entry's is (+1, +value).
// One flag rather than a signed value, because a SUM column may itself be
// negative and overloading the value's sign would make the two unreadable
// apart. Amends AST04's "cardinality moves by one per entry" to "by *plus or
// minus* one per entry, by this flag, never by the value" - the rule's point,
// value-independence, survives intact.
inline constexpr std::uint8_t kEntryDeparture = 0x4;

// `flags` bit 3: the reservation that wrote this entry **aborted**. The bytes
// stay - abort has never rewritten a page and the slot is the recorded leak
// that rides on purge - but they are no longer any group's, and this is what
// says so on the page itself.
//
// **Why the page has to carry it** (`docs/spec/assertion.md` §7, AS6b,
// 2026-08-12). A live abort removes the entry from the group's list in
// memory, but a *recovered* directory rebuilds that linkage by scanning these
// pages (`exec/assertion_recover.cpp`), and a scan cannot otherwise tell an
// aborted entry from a live one: it would re-attach the orphan and
// `VerifyAgainstEntries` (§5.2) would report `Corruption` for a correct
// directory.
//
// **This flag meets that same ordering note from the other side**, and §7 says
// where it is answered: once a mark is durable the walk skips an entry whose
// `ASSERT_ROLLBACK` the fold still has to compensate, so `ReplayRollback`
// restores the linkage the walk declined before un-applying it.
//
// Bit 3 was free, so the 32-byte width (§5.1) did not move.
inline constexpr std::uint8_t kEntryOrphaned = 0x8;

// One entry, exactly 32 bytes on disk (§5.1's normative width).
//
// A plain struct with explicit encode/decode rather than a memcpy'd overlay:
// `docs/rules/rules.md` forbids compiler bitfields in a persisted format, and the
// tuple key/flags/reserved word is packed by shift and mask for the same reason the
// Keystone word is (invariant 6 - bitfield layout is implementation-defined
// and this format must be architecture-portable).
struct BoundCabinEntry {
    std::uint64_t tuple_key = 0;  // kKeystoneIdBits used; the bits above are always 0
    std::uint8_t flags = 0;

    // The advisory location. Every Bound Cabin writer stores `page_epoch` 0
    // (`exec/assertion_build.cpp`, `exec/assertion_check.cpp`) although pages
    // do carry a `page_epoch` (page_header.hpp) that a btree leaf division
    // bumps; `exec/tuple_verify.hpp` compares the two, so a hint into a page
    // whose epoch moved fails verification and costs a descent - never a
    // wrong answer, the tuple key being the authority.
    PageId page_id = kInvalidPageId;
    std::uint16_t page_epoch = 0;
    std::uint16_t slot = 0;

    // The row's SUM column value, or 1 for a COUNT assertion.
    std::int64_t value = 0;

    // **AS6a.** Which group of this cabin the entry belongs to - authoritative,
    // not advisory: it is what lets recovery rebuild the header->entry linkage
    // by scanning the cabin's own pages instead of persisting O(all entries) at
    // every checkpoint (`assertion.md` §5.1, §7).
    //
    // An id and **not a group-key hash**, and the difference is correctness:
    // `HashGroupKey`'s collisions are expected and are resolved by confirming
    // the stored key, which an entry does not carry - so an entry holding a
    // hash could not be attributed between two colliding groups.
    //
    // It occupies the first 4 bytes of the former padding, so the 32-byte
    // width is unchanged.
    std::uint32_t group_id = 0;

    bool reserved() const noexcept { return (flags & kEntryReserved) != 0; }
    bool departure() const noexcept { return (flags & kEntryDeparture) != 0; }
    bool orphaned() const noexcept { return (flags & kEntryOrphaned) != 0; }
    bool hint_valid() const noexcept {
        return (flags & kEntryHintValid) != 0 && page_id != kInvalidPageId;
    }
};

inline constexpr std::size_t kEntryBytes = 32;

// Field-wise codec. The entry's first word is packed exactly as the Keystone
// word is (BK-R2: one layout, keystone.hpp's constants), and `EncodeEntry`
// fails with InvalidArgument on a tuple key above `kMaxKeystoneId` - refused rather
// than truncated, because a truncated tuple key is a name that means a different
// row, which is precisely what K1 exists to prevent.
Status EncodeEntry(const BoundCabinEntry& entry, std::span<std::byte, kEntryBytes> out);
BoundCabinEntry DecodeEntry(std::span<const std::byte, kEntryBytes> in);

// ---- The page ------------------------------------------------------------

// Bytes of the page header that follows the common one.
inline constexpr std::size_t kCabinPageHeaderBytes = 8;

// Where the entry array starts, and how many fit.
inline constexpr std::size_t kEntriesOffset = kPageBodyOffset + kCabinPageHeaderBytes;
inline constexpr std::uint16_t kMaxEntriesPerPage =
    static_cast<std::uint16_t>((kPageSize - kEntriesOffset) / kEntryBytes);

static_assert(kEntryBytes == 32, "assertion.md §5.1 fixes the entry at 32 bytes");
static_assert(kEntriesOffset % 8 == 0, "entries stay 8-byte aligned within the page");
static_assert(kMaxEntriesPerPage == 254,
              "8192 - 32 (common header) - 8 (page header) = 8152; 8152 / 32 = 254");

// A read/write view over one `kCabinBound` page. Non-owning: it borrows the
// caller's span and does not outlive it.
class BoundCabinPage {
public:
    // Formats a freshly created page: common header stamped kCabinBound,
    // entry count zero, next page invalid.
    static Status Format(std::span<std::byte, kPageSize> page);

    // Fails with Corruption if the page is not a formatted kCabinBound page -
    // so a caller that reached the wrong page finds out here rather than by
    // decoding whatever bytes were there.
    static StatusOr<BoundCabinPage> Open(std::span<std::byte, kPageSize> page);

    std::uint16_t entry_count() const noexcept;
    PageId next_page_id() const noexcept;
    void SetNextPageId(PageId next) noexcept;

    bool full() const noexcept { return entry_count() >= kMaxEntriesPerPage; }

    // Appends one entry. Fails with OutOfSpace when the page is full - the
    // caller grows the chain, exactly as a heap chain's tail does.
    StatusOr<std::uint16_t> Append(const BoundCabinEntry& entry);

    StatusOr<BoundCabinEntry> Read(std::uint16_t index) const;

    // Rewrites an entry in place. The only mutations the format needs are flag
    // moves - commit clears `kEntryReserved`, abort sets `kEntryOrphaned` - so
    // nothing here ever shrinks and an entry's tuple key never changes. Abort still
    // removes the entry from the *directory*; the flag is what lets a rebuild
    // that reads only these pages agree with it.
    Status Write(std::uint16_t index, const BoundCabinEntry& entry);

    // The two flag moves the format allows, named. Commit clears
    // `kEntryReserved` (§6.2 step 4); abort sets `kEntryOrphaned` (AS6b).
    //
    // **Here rather than at the call sites**, because "a live page and a
    // replayed page hold the same bytes after the same flag move" is a property
    // of one function, not of four copies of read-tweak-write staying in step by
    // eye. Both the live path (`exec/assertion_check.cpp`) and replay
    // (`exec/assertion_replay.cpp`) go through these, which is what makes
    // `AssertionReplayTest.TheFoldRestoresThePageAndTheDirectoryExactly`'s
    // byte-for-byte comparison a check on the callers rather than on the
    // mutation.
    //
    // Both are idempotent - one clears a bit, one sets it - which is what
    // repeated replay of the same record needs.
    Status ClearReserved(std::uint16_t index);
    Status MarkOrphaned(std::uint16_t index);

private:
    explicit BoundCabinPage(std::span<std::byte, kPageSize> page) noexcept : page_(page) {}

    void SetEntryCount(std::uint16_t count) noexcept;

    // The shared half of the two flag moves above.
    Status SetFlags(std::uint16_t index, std::uint8_t set, std::uint8_t clear);

    std::span<std::byte, kPageSize> page_;
};

}  // namespace kds::storage::cabin
