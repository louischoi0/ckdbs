#pragma once

// The instance's cancel registry (docs/spec/protocol.md §10, BA-S15).
//
// **Lock protocol.** One `Latch` over one map, taken by every core: a KWP
// session registers when its connection is accepted and unregisters when it
// closes, on its own core; a cancel connection looks a session up on
// whichever core accepted it, which is usually another. Nothing else is
// taken under the latch and it is held for one map operation, never across
// a statement. The flag a hit sets is an atomic the target session's core
// reads at its slice boundary and at its next frame; nothing kicks that
// core, because a walk that can see the flag is runnable already and an
// idle session reads it at its next frame.
//
// **A miss and a wrong key look the same**, and neither answers: §10's
// "a wrong key is silently ignored", which is what keeps the registry from
// being an oracle for which session ids are live.

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "kds/base/latch.hpp"

namespace kds::server {

class CancelRegistry {
public:
    using Flag = std::shared_ptr<std::atomic<bool>>;

    void Register(std::uint64_t session_id, std::uint64_t cancel_key, Flag flag) {
        LatchGuard guard(&latch_);
        sessions_[session_id] = Entry{cancel_key, std::move(flag)};
    }

    void Unregister(std::uint64_t session_id) {
        LatchGuard guard(&latch_);
        sessions_.erase(session_id);
    }

    // Sets the session's flag when the key is its key. True on a hit, for a
    // cell; the wire answers neither outcome.
    bool Cancel(std::uint64_t session_id, std::uint64_t cancel_key) {
        Flag flag;
        {
            LatchGuard guard(&latch_);
            const auto it = sessions_.find(session_id);
            if (it == sessions_.end() || it->second.key != cancel_key) return false;
            flag = it->second.flag;
        }
        flag->store(true, std::memory_order_release);
        return true;
    }

private:
    struct Entry {
        std::uint64_t key = 0;
        Flag flag;
    };
    Latch latch_;
    std::unordered_map<std::uint64_t, Entry> sessions_;
};

}  // namespace kds::server
