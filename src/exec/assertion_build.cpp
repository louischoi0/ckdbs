#include "kds/exec/assertion_build.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "kds/exec/assertion_violation.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/cabin_bound_page.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/txn/visibility.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/payload.hpp"

namespace kds::exec {
namespace {

using storage::cabin::BoundCabinEntry;
using storage::cabin::BoundCabinPage;
using storage::cabin::kEntryBytes;
using storage::cabin::kEntryHintValid;

// One live row of one leaf, copied out of its page - index_ddl.cpp's
// StagedRow and for its reason: appending an entry fetches pages, and
// parser-v2.md I15's R1 forbids a fetch while a span into another page is
// live. Two phases per leaf bounds the memory at one page.
struct StagedRow {
    std::uint64_t pk = 0;
    std::uint16_t slot = 0;
    std::vector<std::byte> payload;
};

}  // namespace

Status BoundCabinChainWriter::EnsureRoot(storage::PageStore& store, wal::WalManager* wal) {
    return tail_ == kInvalidPageId ? Grow(store, wal) : Status::OK();
}

Status BoundCabinChainWriter::AdoptChain(storage::PageStore& store, PageId root) {
    if (root == kInvalidPageId) {
        return Status::InvalidArgument(
            "bound cabin chain: cannot adopt an invalid root; an assertion whose row carries no "
            "root was never built");
    }

    PageId page_id = root;
    std::size_t pages = 0;
    while (true) {
        if (++pages > heap::kMaxChainPages) {
            return Status::Corruption("bound cabin chain from page " + std::to_string(root) +
                                      " exceeds the maximum length; the links may form a cycle");
        }
        auto page = store.Get(page_id);
        if (!page.ok()) return page.status();
        // Open() is what proves the page class: a root that is not a
        // kCabinBound page is not this cabin's chain, and appending into it
        // would put entries where nothing can relink them.
        auto view = BoundCabinPage::Open(page.value().bytes());
        if (!view.ok()) return view.status();
        const PageId next = view.value().next_page_id();
        if (next == kInvalidPageId) break;
        page_id = next;
    }

    root_ = root;
    tail_ = page_id;
    pages_ = pages;
    return Status::OK();
}

StatusOr<std::pair<PageId, std::uint16_t>> BoundCabinChainWriter::Append(
    storage::PageStore& store, wal::WalManager* wal, const BoundCabinEntry& entry,
    const std::string& key, wal::RecordType type, std::uint64_t txn_id) {
    if (tail_ == kInvalidPageId) {
        if (Status s = Grow(store, wal); !s.ok()) return s;
    }
    auto page = store.Get(tail_);
    if (!page.ok()) return page.status();
    auto opened = BoundCabinPage::Open(page.value().bytes());
    if (!opened.ok()) return opened.status();
    if (opened.value().full()) {
        if (Status s = Grow(store, wal); !s.ok()) return s;
        page = store.Get(tail_);
        if (!page.ok()) return page.status();
        opened = BoundCabinPage::Open(page.value().bytes());
        if (!opened.ok()) return opened.status();
    }

    auto index = opened.value().Append(entry);
    if (!index.ok()) return index.status();

    if (wal != nullptr) {
        std::array<std::byte, kEntryBytes> entry_bytes{};
        if (Status s = storage::cabin::EncodeEntry(entry, entry_bytes); !s.ok()) return s;
        std::vector<std::byte> payload(wal::kAssertEntryFixedSize + kEntryBytes + key.size());
        wal::AssertEntryPayload fields{};
        fields.assertion_id = assertion_id_;
        fields.index = index.value();
        // AS6a: the same id the entry bytes carry, so replay binds the group by
        // id instead of re-deriving one in its own allocation order.
        fields.group_id = entry.group_id;
        auto used = wal::EncodeAssertEntry(
            payload, fields, entry_bytes,
            std::as_bytes(std::span<const char>(key.data(), key.size())));
        if (!used.ok()) return used.status();
        auto rec = wal->Append(wal::RecordSpec{type, txn_id, tail_}, payload);
        if (!rec.ok()) return rec.status();
        if (Status s = store.StampPageLsn(tail_, rec.value()); !s.ok()) return s;
    }
    return std::make_pair(tail_, index.value());
}

Status BoundCabinChainWriter::Grow(storage::PageStore& store, wal::WalManager* wal) {
    auto created = store.CreateNew();
    if (!created.ok()) return created.status();
    auto& [pid, bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> bytes = bytes_ref.bytes();
    if (Status s = BoundCabinPage::Format(bytes); !s.ok()) return s;
    ++pages_;

    if (wal != nullptr) {
        // Deliberately unstamped, LogInsert's rule for a new tuple page:
        // the first entry record into it stamps it, and an empty root that
        // never receives one carries page_lsn 0, "never logged".
        std::array<std::byte, wal::kPageInitPayloadSize> init{};
        const wal::PageInitPayload fields{
            0, static_cast<std::uint8_t>(PageType::kCabinBound), {0, 0, 0}};
        if (auto n = wal::EncodePageInit(init, fields); !n.ok()) return n.status();
        if (auto rec = wal->Append(
                wal::RecordSpec{wal::RecordType::kPageInit, wal::kNoTxnId, pid}, init);
            !rec.ok()) {
            return rec.status();
        }
    }

    if (tail_ == kInvalidPageId) {
        root_ = pid;
    } else {
        // The link edit, then a full page image of the old tail: no record
        // type describes a link edit, which is the same reason heap chain
        // growth images its predecessor (docs/wal.md).
        auto old_tail = store.Get(tail_);
        if (!old_tail.ok()) return old_tail.status();
        auto opened = BoundCabinPage::Open(old_tail.value().bytes());
        if (!opened.ok()) return opened.status();
        opened.value().SetNextPageId(pid);
        if (wal != nullptr) {
            std::vector<std::byte> image(wal::kFullPageImagePayloadSize);
            if (auto n = wal::EncodeFullPageImage(
                    image, std::span<const std::byte, kPageSize>(old_tail.value().bytes()));
                !n.ok()) {
                return n.status();
            }
            auto rec = wal->Append(
                wal::RecordSpec{wal::RecordType::kFullPageImage, wal::kNoTxnId, tail_}, image);
            if (!rec.ok()) return rec.status();
            if (Status s = store.StampPageLsn(tail_, rec.value()); !s.ok()) return s;
        }
    }
    tail_ = pid;
    return Status::OK();
}

StatusOr<BoundCabinBuild> BuildBoundCabin(storage::PageStore& store,
                                          const catalog::TableAccess& access,
                                          const parser::AssertionStmt& stmt,
                                          std::uint64_t assertion_id,
                                          std::span<const std::uint16_t> group_cols,
                                          std::uint16_t sum_col,
                                          const txn::ReadView& check_view,
                                          wal::WalManager* wal) {
    const BoundAggregate aggregate =
        stmt.func == parser::AggFunc::kSum ? BoundAggregate::kSum : BoundAggregate::kCount;
    BoundCabinBuild build(aggregate, stmt.enforced_max(), assertion_id);
    if (Status s = build.chain.EnsureRoot(store, wal); !s.ok()) return s;

    // A btree leaf is a heap page, so the walk below is one loop for both
    // clustered forms - only the first leaf differs.
    PageId leaf = access.desc_page_id;
    if (access.clustered_type == catalog::ClusteredType::kBtree) {
        auto first = btree::BtreeSeekLeaf(store, access.desc_page_id, 0);
        if (!first.ok()) return first.status();
        leaf = first.value();
    }

    std::vector<parser::AstValue> group_values(group_cols.size());
    for (std::uint32_t leaves = 0; leaf != kInvalidPageId; ++leaves) {
        if (leaves >= heap::kMaxChainPages) {
            return Status::Corruption("relation leaf chain exceeds " +
                                      std::to_string(heap::kMaxChainPages) + " pages");
        }

        // ---- Phase 1: copy out, with no page fetch under the span --------
        std::vector<StagedRow> staged;
        PageId next = kInvalidPageId;
        {
            auto bytes = store.GetForRead(leaf);
            if (!bytes.ok()) return bytes.status();
            heap::PageView page(bytes.value().bytes());
            const std::uint16_t n = page.slot_count();
            staged.reserve(n);
            for (std::uint16_t i = 0; i < n; ++i) {
                auto tuple = page.ReadTuple(i);
                if (tuple.status().code() == StatusCode::kNotFound) continue;  // retired slot
                if (!tuple.ok()) return tuple.status();

                switch (txn::CheckVisibility(check_view, tuple.value().trx_id,
                                             tuple.value().deleted)) {
                    case txn::CheckVerdict::kBusy:
                        // See the header: counting it and losing the abort
                        // overstates the group forever, skipping it and
                        // seeing the commit understates it. Refuse,
                        // retryably - F3's shape.
                        return Status::TxnConflict(
                            "relation '" + stmt.table_name +
                            "' has a row written by an in-flight transaction; CREATE "
                            "ASSERTION reads settled state - retry when it has ended");
                    case txn::CheckVerdict::kAbsent:
                        continue;
                    case txn::CheckVerdict::kLive:
                        break;
                }

                auto pk = kds::KeystoneIdOfPayload(tuple.value().payload);
                if (!pk.ok()) return pk.status();

                StagedRow row;
                row.pk = pk.value();
                row.slot = i;
                row.payload.assign(tuple.value().payload.begin(), tuple.value().payload.end());
                staged.push_back(std::move(row));
            }
            next = page.next_page_id();
        }

        // ---- Phase 2: decode, append, accumulate, check ------------------
        for (const StagedRow& row : staged) {
            std::vector<PendingSpill> spills;
            auto decoded = DecodeRow(access.schema, access.layout, row.payload, &spills);
            if (!decoded.ok()) return decoded.status();
            // Safe here and only here: the leaf's span was dropped with
            // phase 1, so a spilled group value may fetch its var-heap page.
            if (Status s = ResolveSpills(store, spills, decoded.value()); !s.ok()) return s;

            for (std::size_t i = 0; i < group_cols.size(); ++i) {
                group_values[i] = decoded.value()[group_cols[i]];
            }
            const std::string key = EncodeGroupKey(group_values);
            const std::int64_t delta = aggregate == BoundAggregate::kSum
                                           ? decoded.value()[sum_col].int_val
                                           : std::int64_t{1};

            BoundCabinEntry entry;
            entry.pk = row.pk;
            entry.flags = kEntryHintValid;  // committed: no kEntryReserved
            entry.page_id = leaf;
            entry.page_epoch = 0;  // no page epoch exists; written 0 like every hint
            entry.slot = row.slot;
            entry.value = delta;
            // AS6a: stamped before the append, because the page write precedes
            // the Apply that would otherwise create the group. An entry written
            // with `group_id = 0` is an entry recovery cannot attribute.
            entry.group_id = build.cabin.EnsureGroupId(key);

            auto at = build.chain.Append(store, wal, entry, key,
                                         wal::RecordType::kAssertBuild, wal::kNoTxnId);
            if (!at.ok()) return at.status();
            if (Status s = build.cabin.Apply(key, delta, at.value().first, at.value().second);
                !s.ok()) {
                return s;  // checked-arithmetic overflow: the AG3 statement error
            }
            ++build.rows_incorporated;

            // The admission check the data itself has to pass, run as each
            // row lands so "the first violating group" is deterministic in
            // scan order rather than in hash-map order.
            const GroupHeader* header = build.cabin.Find(key);
            if (header != nullptr && header->aggregate(aggregate) > build.cabin.bound()) {
                std::vector<GroupKeyPart> parts(group_cols.size());
                for (std::size_t i = 0; i < group_cols.size(); ++i) {
                    parts[i].column = stmt.group_columns[i].name;
                    parts[i].type_val = access.schema.columns[group_cols[i]].type_val;
                    parts[i].value = group_values[i];
                }
                return Status::AssertionViolation(AssertionViolationMessage(
                    stmt.name, parts, aggregate, stmt.sum_column.name, build.cabin.bound()));
            }
        }

        leaf = next;
    }

    build.cabin_root = build.chain.root();
    build.pages_allocated = build.chain.pages();
    return build;
}

}  // namespace kds::exec
