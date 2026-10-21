#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include "kds/base/status.hpp"

namespace kds {

// A one-shot injected failure, for the test devices (`MemoryPageDevice`,
// `MemoryLogDevice`): armed with a status and a count of matching operations
// to let through first, it fires on the operation after them, once. A skip
// of 0 is "the next one"; a larger one is how BS-S3's sweep fails the Nth
// operation of a kind (`sim/faults.hpp`). Not synchronized: each device
// guards it with the latch its operations already take.
class InjectedFailure {
public:
    void Arm(Status status, std::uint64_t skip) {
        status_ = std::move(status);
        skip_ = skip;
    }
    void Clear() noexcept {
        status_.reset();
        skip_ = 0;
    }

    // One matching operation: the failure, if this is the one it fires on.
    std::optional<Status> Take() {
        if (!status_.has_value()) return std::nullopt;
        if (skip_ > 0) {
            --skip_;
            return std::nullopt;
        }
        std::optional<Status> taken = std::move(status_);
        status_.reset();
        return taken;
    }

private:
    std::optional<Status> status_;
    std::uint64_t skip_ = 0;
};

}  // namespace kds
