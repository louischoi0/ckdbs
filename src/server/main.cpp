#include <chrono>
#include <cstdlib>
#include <iostream>

#include "kds/server/expeditor.hpp"

// Entrypoint of the DB master server process. Platform layer only: argv,
// the wall clock, and stdout. Everything else belongs to the Expeditor,
// which owns the subsystems (expeditor.hpp).
//
// Durability: writes reach the data file on SYNC and at clean shutdown
// (STOP). A crash or kill loses everything since the last of those - the
// WAL (docs/wal.md) is what closes that gap.

namespace {

constexpr const char* kDefaultDataFile = "kds.db";

std::uint64_t NowUnixSeconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace

int main(int argc, char** argv) {
    kds::server::Expeditor::Config config;
    config.data_file = argc > 1 ? argv[1] : kDefaultDataFile;

    auto expeditor = kds::server::Expeditor::Open(config, NowUnixSeconds());
    if (!expeditor.ok()) {
        std::cerr << "startup failed: " << expeditor.status().message() << "\n";
        return EXIT_FAILURE;
    }
    auto& db = *expeditor.value();

    std::cout << "ckdbs on " << db.config().data_file << ": "
              << db.store().allocated_pages() << " pages, superblock version "
              << db.superblock().version() << "\n"
              << "listening on 127.0.0.1:" << db.config().port << "\n";

    if (kds::Status s = db.Serve(); !s.ok()) {
        std::cerr << "server stopped: " << s.message() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "stopped; " << db.store().allocated_pages() << " pages persisted\n";
    return EXIT_SUCCESS;
}
