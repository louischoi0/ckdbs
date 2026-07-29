#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "kds/server/config_file.hpp"
#include "kds/server/expeditor.hpp"

// Entrypoint of the DB master server process. Platform layer only: argv,
// the wall clock, and stdout. Everything else belongs to the Expeditor,
// which owns the subsystems (expeditor.hpp).
//
// Durability: dirty pages reach the data file on the checkpoint cadence
// (`checkpoint_interval_ms`), on SYNC, and at clean shutdown. A crash loses
// at most what changed since the last of those - the WAL (docs/wal.md)
// closes the remainder of the gap once mutations are logged.
//
// Configuration precedence, applied in this order: built-in defaults, then
// the config file, then the command line. Later wins, which is the only
// ordering that lets an operator override a deployed file for one run.

namespace {

constexpr const char* kUsage =
    "usage: kds_server [<data_file>] [--config <path>] [--port <n>]\n"
    "                  [--log-file <name>] [--log-dir <dir>] [--log-level <level>]\n"
    "\n"
    "  --config <path>   key = value settings file; see docs/client-manual.md\n"
    "  <data_file>       positional, overrides data_file from the config\n"
    "\n"
    "Config keys: data_file, port, wal_dir, checkpoint_interval_ms,\n"
    "             log_dir, log_file, log_level\n";

std::uint64_t NowUnixSeconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// Returns false and prints why on a bad argument list.
bool ParseArgs(int argc, char** argv, kds::server::Expeditor::Config& config) {
    // The file is applied before the flags, so a flag always wins over it
    // regardless of where --config appears in argv.
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        }
    }
    if (!config_path.empty()) {
        auto file = kds::server::ConfigFile::Load(config_path);
        if (!file.ok()) {
            std::cerr << "config: " << file.status().message() << "\n";
            return false;
        }
        if (kds::Status s = config.ApplyFile(file.value()); !s.ok()) {
            std::cerr << "config: " << s.message() << "\n";
            return false;
        }
    }

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << what << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--config") {
            ++i;  // already consumed above
        } else if (arg == "--help" || arg == "-h") {
            std::cout << kUsage;
            std::exit(EXIT_SUCCESS);
        } else if (arg == "--port") {
            const char* v = next("--port");
            if (v == nullptr) return false;
            int port = std::atoi(v);
            if (port <= 0 || port > 65535) {
                std::cerr << "--port must be 1..65535, got '" << v << "'\n";
                return false;
            }
            config.port = static_cast<std::uint16_t>(port);
        } else if (arg == "--log-file") {
            const char* v = next("--log-file");
            if (v == nullptr) return false;
            config.log_file = v;
        } else if (arg == "--log-dir") {
            const char* v = next("--log-dir");
            if (v == nullptr) return false;
            config.log_dir = v;
        } else if (arg == "--log-level") {
            const char* v = next("--log-level");
            if (v == nullptr) return false;
            auto level = kds::ParseLogLevel(v);
            if (!level.ok()) {
                std::cerr << level.status().message() << "\n";
                return false;
            }
            config.log_level = level.value();
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "unknown option '" << arg << "'\n" << kUsage;
            return false;
        } else {
            config.data_file = std::string(arg);
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    kds::server::Expeditor::Config config;
    if (!ParseArgs(argc, argv, config)) {
        return EXIT_FAILURE;
    }

    auto expeditor = kds::server::Expeditor::Open(config, NowUnixSeconds());
    if (!expeditor.ok()) {
        std::cerr << "startup failed: " << expeditor.status().message() << "\n";
        return EXIT_FAILURE;
    }
    auto& db = *expeditor.value();

    std::cout << "ckdbs on " << db.config().data_file << ": "
              << db.store().allocated_pages() << " pages, superblock version "
              << db.superblock().version() << "\n"
              << "logging to " << (db.config().LogPath().empty() ? "(disabled)"
                                                                 : db.config().LogPath())
              << " at level " << kds::LogLevelName(db.config().log_level) << "\n"
              // Flushed rather than left buffered: Serve() blocks for the
              // life of the process, so a buffered banner would not appear
              // until shutdown - exactly when it is no longer useful.
              << "listening on 127.0.0.1:" << db.config().port << std::endl;

    if (kds::Status s = db.Serve(); !s.ok()) {
        std::cerr << "server stopped: " << s.message() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "stopped; " << db.store().allocated_pages() << " pages persisted\n";
    return EXIT_SUCCESS;
}
