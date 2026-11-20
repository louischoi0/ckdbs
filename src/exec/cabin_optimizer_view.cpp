#include "kds/exec/cabin_optimizer_view.hpp"

#include <utility>

namespace kds::exec {

std::shared_ptr<const CabinOptimizerViewData> BuildCabinOptimizerView(
    const stats::CabinOptimizer* controller, const CabinOptimizerExecutor* executor,
    const stats::OptimizerSignals* signals) {
    auto view = std::make_shared<CabinOptimizerViewData>();
    view->has_controller = controller != nullptr;
    view->has_executor = executor != nullptr;
    if (controller != nullptr) {
        view->log = controller->DecisionLog();
        view->entries = controller->ManagedEntries();
        view->managed = controller->managed_count();
        view->pages_committed = controller->pages_committed();
        view->page_budget = controller->config().page_budget;
        view->quality.reserve(view->entries.size());
        for (const stats::ManagedEntryView& entry : view->entries) {
            view->quality.push_back(
                signals != nullptr && entry.cabin_id != 0
                    ? std::optional<stats::SnapshotCabin>(signals->QualityOf(entry.cabin_id))
                    : std::nullopt);
        }
    }
    if (executor != nullptr) view->counters = executor->counters();
    return view;
}

bool CabinOptimizerViewBoard::Publish(std::shared_ptr<const CabinOptimizerViewData> view) noexcept {
    {
        const LatchGuard hold(&latch_, std::try_to_lock);
        if (!hold.owns()) return false;
        view_.swap(view);
    }
    // `view` is the copy replaced: let go here, outside the latch.
    return true;
}

std::shared_ptr<const CabinOptimizerViewData> CabinOptimizerViewBoard::Current() const {
    const LatchGuard hold(&latch_);
    return view_;
}

}  // namespace kds::exec
