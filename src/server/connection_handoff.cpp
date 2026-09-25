#include "kds/server/connection_handoff.hpp"

#include <unistd.h>

namespace kds::server {

ConnectionHandoff::ConnectionHandoff(std::uint32_t cores, const sched::WakeRegistry* wakers)
    : wakers_(wakers) {
    inboxes_.reserve(cores);
    for (std::uint32_t i = 0; i < cores; ++i) inboxes_.push_back(std::make_unique<Inbox>());
}

ConnectionHandoff::~ConnectionHandoff() {
    for (auto& inbox : inboxes_) {
        for (int fd : inbox->fds) ::close(fd);
    }
}

std::uint32_t ConnectionHandoff::NextCore() noexcept {
    return next_.fetch_add(1, std::memory_order_relaxed) % cores();
}

void ConnectionHandoff::Offer(std::uint32_t core, int fd) {
    Inbox& inbox = *inboxes_[core];
    {
        LatchGuard hold(&inbox.latch);
        inbox.fds.push_back(fd);
        inbox.pending.store(true, std::memory_order_release);
    }
    // After the latch: the state the destination will look for is
    // published, and the kick only ends the idle block it may be in.
    if (wakers_ != nullptr) wakers_->Kick(core);
}

bool ConnectionHandoff::Pending(std::uint32_t core) const noexcept {
    return inboxes_[core]->pending.load(std::memory_order_acquire);
}

std::vector<int> ConnectionHandoff::Take(std::uint32_t core) {
    Inbox& inbox = *inboxes_[core];
    std::vector<int> taken;
    LatchGuard hold(&inbox.latch);
    taken.swap(inbox.fds);
    inbox.pending.store(false, std::memory_order_relaxed);
    return taken;
}

}  // namespace kds::server
