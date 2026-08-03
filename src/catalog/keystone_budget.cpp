#include "kds/catalog/keystone_budget.hpp"

namespace kds::catalog {

KeystoneBudget BudgetOf(std::uint64_t next_id) noexcept {
    KeystoneBudget budget;

    // Below the first issuable id: an uninitialized or corrupt row. Nothing
    // has been issued, and reporting that beats reporting a huge negative
    // wrapped into a uint64.
    if (next_id < kFirstRowId) {
        budget.remaining = kKeystoneBudgetCapacity;
        return budget;
    }

    // Past the ceiling: AllocateRowId has already started refusing. The
    // issued count is capped at the capacity rather than left to run past
    // it, so `issued <= capacity` holds for every input.
    if (next_id > kMaxKeystoneId) {
        budget.issued = kKeystoneBudgetCapacity;
        budget.used_fraction = 1.0;
        budget.warn = true;
        budget.exhausted = true;
        return budget;
    }

    budget.issued = next_id - kFirstRowId;
    // Inclusive of `next_id` itself, which is still issuable - the ceiling
    // check in AllocateRowId is `id > kMaxKeystoneId`, so kMaxKeystoneId is
    // handed out and remaining is 1 at that point, not 0.
    budget.remaining = kMaxKeystoneId - next_id + 1;
    budget.used_fraction =
        static_cast<double>(budget.issued) / static_cast<double>(kKeystoneBudgetCapacity);
    budget.warn = budget.used_fraction >= kKeystoneBudgetWarnFraction;
    budget.exhausted = false;
    return budget;
}

}  // namespace kds::catalog
