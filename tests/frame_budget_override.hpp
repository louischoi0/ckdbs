#pragma once

#include <cstdlib>
#include <optional>
#include <string>

// `KDS_TEST_FRAME_BUDGET` lowers every debug store's capacity to the floor
// (`DevicePageStore::Open`, BE-R3), so the whole suite runs under eviction
// pressure. A cell that dirties more pages than the floor in one burst, with
// no checkpoint between - a race over page creation, a fill that builds a
// tree - is refused `ResourceExhausted` there, which is BE-R4 working as
// ruled, not a defect the cell is about. Such a cell holds one of these for
// the stores it opens, saying why beside it. Sound because ctest runs each
// cell in a process of its own, so the environment it restores is its own.
class WithoutFrameBudgetOverride {
public:
    WithoutFrameBudgetOverride() {
        if (const char* value = std::getenv(kName); value != nullptr) saved_ = value;
        ::unsetenv(kName);
    }
    ~WithoutFrameBudgetOverride() {
        if (saved_.has_value()) ::setenv(kName, saved_->c_str(), /*overwrite=*/1);
    }
    WithoutFrameBudgetOverride(const WithoutFrameBudgetOverride&) = delete;
    WithoutFrameBudgetOverride& operator=(const WithoutFrameBudgetOverride&) = delete;

private:
    static constexpr const char* kName = "KDS_TEST_FRAME_BUDGET";
    std::optional<std::string> saved_;
};
