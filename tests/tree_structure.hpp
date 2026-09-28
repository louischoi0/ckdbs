#pragma once

// **A tree's structural invariant, asked of every node** (AT-S16): each
// separator sits inside the range its parent routes to the node holding it,
// every leaf's keys sit inside the range the path to the leaf names, and the
// descent reaches every leaf the sibling chain does. A separator promoted
// into the wrong half of a parent breaks the first; a node no parent routes
// to breaks the last. One question for both trees - the clustered tree's
// keys are Keystone ids, the index's are sort keys - asked by the
// single-threaded cells (`btree_test.cpp`, `index_tree_test.cpp`) and by the
// threaded ones (`btree_race_test.cpp`, `index_race_test.cpp`) alike.

#include <cstdint>
#include <cstring>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/btree/btree.hpp"
#include "kds/storage/btree/btree_page.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/index/index_tree.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/page_header.hpp"

namespace kds::testing_race {

namespace detail {

inline bool IsType(std::span<const std::byte, kPageSize> page, PageType type) {
    return storage::RawPageType(page) == static_cast<std::uint8_t>(type);
}

inline void CheckBtree(storage::PageStore& store, PageId node, std::uint64_t lo,
                       std::uint64_t hi, std::set<PageId>& leaves) {
    auto bytes = store.GetForRead(node);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    if (IsType(bytes.value().bytes(), PageType::kBtreeLeaf)) {
        leaves.insert(node);
        heap::PageView leaf(bytes.value().bytes());
        for (std::uint16_t i = 0; i < leaf.slot_count(); ++i) {
            auto tuple = leaf.ReadTuple(i);
            if (!tuple.ok()) continue;  // retired slot
            auto id = KeystoneIdOfPayload(tuple.value().payload);
            ASSERT_TRUE(id.ok()) << id.status().message();
            EXPECT_TRUE(id.value() >= lo && id.value() < hi)
                << "id " << id.value() << " in leaf " << node << " sits outside [" << lo << ", "
                << hi << "), the range the path to it routes there";
        }
        return;
    }
    btree::InternalView view(bytes.value().bytes());
    std::vector<std::pair<std::uint64_t, PageId>> children{{lo, view.leftmost_child()}};
    for (std::uint16_t i = 0; i < view.entry_count(); ++i) {
        auto e = view.Entry(i);
        ASSERT_TRUE(e.ok()) << e.status().message();
        EXPECT_TRUE(e.value().sep_key >= lo && e.value().sep_key < hi)
            << "separator " << e.value().sep_key << " in node " << node << " sits outside ["
            << lo << ", " << hi << "), the range its parent routes to the node";
        children.emplace_back(e.value().sep_key, e.value().child);
    }
    bytes.value().Release();
    for (std::size_t i = 0; i < children.size(); ++i) {
        const std::uint64_t child_hi = i + 1 < children.size() ? children[i + 1].first : hi;
        CheckBtree(store, children[i].second, children[i].first, child_hi, leaves);
    }
}

// An empty bound is unbounded.
inline void CheckIndex(storage::PageStore& store, const index::IndexLayout& layout, PageId node,
                       const std::vector<std::byte>& lo, const std::vector<std::byte>& hi,
                       std::set<PageId>& leaves) {
    const std::size_t width = layout.sort_key_width();
    auto inside = [&](std::span<const std::byte> key) {
        return (lo.empty() || std::memcmp(key.data(), lo.data(), width) >= 0) &&
               (hi.empty() || std::memcmp(key.data(), hi.data(), width) < 0);
    };
    auto bytes = store.GetForRead(node);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    if (IsType(bytes.value().bytes(), PageType::kIndexLeaf)) {
        leaves.insert(node);
        index::IndexLeafView leaf(bytes.value().bytes());
        for (std::uint16_t i = 0; i < leaf.entry_count(); ++i) {
            auto key = leaf.SortKey(i);
            ASSERT_TRUE(key.ok()) << key.status().message();
            EXPECT_TRUE(inside(key.value()))
                << "entry " << i << " of leaf " << node
                << " sits outside the range the path to it routes there";
        }
        return;
    }
    index::IndexInternalView view(bytes.value().bytes());
    std::vector<std::pair<std::vector<std::byte>, PageId>> children{{lo, view.leftmost_child()}};
    for (std::uint16_t i = 0; i < view.entry_count(); ++i) {
        auto sep = view.Separator(i);
        auto child = view.Child(i);
        ASSERT_TRUE(sep.ok() && child.ok());
        EXPECT_TRUE(inside(sep.value()))
            << "separator " << i << " of node " << node
            << " sits outside the range its parent routes to the node";
        children.emplace_back(std::vector<std::byte>(sep.value().begin(), sep.value().end()),
                              child.value());
    }
    bytes.value().Release();
    for (std::size_t i = 0; i < children.size(); ++i) {
        const std::vector<std::byte>& child_hi =
            i + 1 < children.size() ? children[i + 1].first : hi;
        CheckIndex(store, layout, children[i].second, children[i].first, child_hi, leaves);
    }
}

inline void ExpectChainReached(const std::set<PageId>& chained, const std::set<PageId>& reached) {
    for (PageId leaf : chained) {
        EXPECT_EQ(1u, reached.count(leaf))
            << "leaf " << leaf << " is on the sibling chain and no descent reaches it";
    }
}

}  // namespace detail

// ---- An index's shape, for the cells that build one to a plan ----------

// The node at `level` on the leftmost spine, or kInvalidPageId if the tree
// is not that tall.
inline PageId IndexLeftmostAtLevel(storage::PageStore& store, PageId root, std::uint16_t level) {
    PageId node = root;
    for (;;) {
        auto bytes = store.GetForRead(node);
        EXPECT_TRUE(bytes.ok()) << bytes.status().message();
        if (detail::IsType(bytes.value().bytes(), PageType::kIndexLeaf)) {
            return level == 0 ? node : kInvalidPageId;
        }
        index::IndexInternalView view(bytes.value().bytes());
        if (view.level() == level) return node;
        node = view.leftmost_child();
    }
}

// Every child an internal node routes to, leftmost first.
inline std::vector<PageId> IndexChildren(storage::PageStore& store, PageId node) {
    auto bytes = store.GetForRead(node);
    EXPECT_TRUE(bytes.ok()) << bytes.status().message();
    index::IndexInternalView view(bytes.value().bytes());
    std::vector<PageId> out{view.leftmost_child()};
    for (std::uint16_t i = 0; i < view.entry_count(); ++i) {
        auto child = view.Child(i);
        EXPECT_TRUE(child.ok()) << child.status().message();
        out.push_back(child.value());
    }
    return out;
}

inline bool IndexPageIsFull(storage::PageStore& store, PageId page,
                            const index::IndexLayout& layout) {
    auto bytes = store.GetForRead(page);
    EXPECT_TRUE(bytes.ok()) << bytes.status().message();
    if (detail::IsType(bytes.value().bytes(), PageType::kIndexLeaf)) {
        return index::IndexLeafView(bytes.value().bytes()).IsFull();
    }
    return index::IndexInternalView(bytes.value().bytes()).IsFull(layout);
}

inline void ExpectBtreeSeparatorsBoundTheirSubtrees(storage::PageStore& store, PageId root) {
    std::set<PageId> reached;
    detail::CheckBtree(store, root, 0, ~std::uint64_t{0}, reached);
    std::set<PageId> chained;
    Status s = btree::BtreeVisit(store, root, storage::PageAccess::kRead,
                                 [&](PageId page_id, heap::PageView&,
                                     std::uint16_t) -> StatusOr<storage::VisitControl> {
                                     chained.insert(page_id);
                                     return storage::VisitControl::kContinue;
                                 });
    ASSERT_TRUE(s.ok()) << s.message();
    detail::ExpectChainReached(chained, reached);
}

inline void ExpectIndexSeparatorsBoundTheirSubtrees(storage::PageStore& store, PageId root,
                                                    const index::IndexLayout& layout) {
    std::set<PageId> reached;
    detail::CheckIndex(store, layout, root, {}, {}, reached);
    std::set<PageId> chained;
    Status s = index::IndexVisit(store, root, layout, storage::PageAccess::kRead,
                                 [&](PageId page_id, index::IndexLeafView&,
                                     std::uint16_t) -> StatusOr<storage::VisitControl> {
                                     chained.insert(page_id);
                                     return storage::VisitControl::kContinue;
                                 });
    ASSERT_TRUE(s.ok()) << s.message();
    detail::ExpectChainReached(chained, reached);
}

}  // namespace kds::testing_race
