#include <chrono>
#include <cstdlib>
#include <iostream>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/tcp_server.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// Entrypoint of the DB master server process (src/server). This is the
// platform layer, not engine logic: it is allowed to read the wall clock
// and do real socket/stdout I/O directly (rules.md #4 requires *engine*
// logic to go through injectable interfaces for time/IO/randomness so it
// can run under the deterministic simulator - main.cpp and tcp_server.cpp
// are the boundary places that provide the real versions of those to
// everything else).
//
// Current state: bootstraps a database (against an InMemoryPageStore -
// there is still no real disk backend, an open decision in CLAUDE.md, so
// nothing persists across a restart yet) and then serves client commands
// over a plain-text TCP protocol (CommandDispatcher's small command set -
// see its file comment for why there's no SQL yet). Still missing before
// this is a real server: a disk-backed PageStore, and the thread-per-core
// worker pool (rules.md #3) that would let it serve more than one client
// at a time.

namespace {

constexpr std::uint16_t kDefaultPort = 15432;

std::uint64_t NowUnixSeconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace

int main() {
    std::cout << "ckdbs server starting (InMemoryPageStore - nothing persists across a restart "
                 "yet)\n";

    kds::storage::InMemoryPageStore store(kds::server::kFirstUserPageId);

    auto boot = kds::bootstrap::BootstrapDatabase(store, NowUnixSeconds());
    if (!boot.ok()) {
        std::cerr << "bootstrap failed: " << boot.status().message() << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "bootstrap ok: superblock version=" << boot.value().superblock.version()
              << " max_page_id=" << boot.value().superblock.max_page_id() << "\n";

    kds::server::CommandDispatcher dispatcher(boot.value().superblock, boot.value().catalog);

    auto listener = kds::server::TcpServer::Listen(kDefaultPort);
    if (!listener.ok()) {
        std::cerr << "failed to listen on port " << kDefaultPort << ": "
                  << listener.status().message() << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "listening on 127.0.0.1:" << kDefaultPort
              << " (try: printf 'PING\\n' | nc 127.0.0.1 " << kDefaultPort << ")\n";

    listener.value().Serve(dispatcher);

    std::cout << "ckdbs server stopped (STOP command received)\n";
    return EXIT_SUCCESS;
}
