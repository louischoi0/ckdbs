#pragma once

#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/storage/visit.hpp"

namespace kds::exec {

// **A relation's leaf chain, walked a slice at a time** (A1 stage 2,
// `sched.md` §3): the cursor `IndexBuild`, `AssertionBuild` and
// `TransitionBuild` resume their walk from between two slices. It holds the
// next leaf's id and the walk's cycle guard - no page and no span - so a
// caller may yield its core between two `Step`s. Resuming at a leaf id is
// safe under the relation's `X`, which every caller but an online index
// build holds, and under that build's `IS`: a live relation's leaf is never
// freed, and keeps its id and lower half (`index.md` §10.3).
class LeafCursor {
public:
    // At the relation's leftmost leaf.
    Status Start(storage::PageStore& store, PageId desc_page_id) {
        auto first = btree::BtreeLeftmostLeaf(store, desc_page_id);
        if (!first.ok()) return first.status();
        leaf_ = first.value();
        guard_ = storage::LeafChainGuard("relation leaf chain");
        return Status::OK();
    }

    // Whether the walk has reached the end of the chain.
    bool walked() const noexcept { return leaf_ == kInvalidPageId; }

    // Walks up to `leaves` more leaves through `per_leaf(leaf)`, which
    // returns the leaf after it. The chain's cycle guard is the whole
    // walk's, not a slice's (`storage::LeafChainGuard`). A refusal ends the
    // walk where it stands.
    template <typename PerLeaf>
    Status Step(std::uint32_t leaves, PerLeaf&& per_leaf) {
        for (std::uint32_t n = 0; n < leaves && leaf_ != kInvalidPageId; ++n) {
            if (Status s = guard_.Visit(leaf_); !s.ok()) return s;
            StatusOr<PageId> next = per_leaf(leaf_);
            if (!next.ok()) return next.status();
            leaf_ = next.value();
        }
        return Status::OK();
    }

private:
    PageId leaf_ = kInvalidPageId;
    storage::LeafChainGuard guard_;
};

}  // namespace kds::exec
