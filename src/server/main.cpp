#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

#include "kds/server/config_file.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/server/stop_signal.hpp"

// auth.hpp is OpenSSL-free and safe to include in every build; only the
// AddUser body below links against symbols KDS_WITH_TLS compiles.
#include "kds/server/auth.hpp"

#if KDS_WITH_TLS
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <sstream>
#endif

// Entrypoint of the DB master server process. Platform layer only: argv,
// the wall clock, and stdout. Everything else belongs to the Expeditor,
// which owns the subsystems (expeditor.hpp).
//
// Durability: dirty pages reach the data file on the checkpoint cadence
// (`checkpoint_interval_ms`), on SYNC, and at clean shutdown. A crash loses
// at most what changed since the last of those - the WAL (docs/spec/wal.md)
// closes the remainder of the gap once mutations are logged.
//
// Configuration precedence, applied in this order: built-in defaults, then
// the config file, then the command line. Later wins, which is the only
// ordering that lets an operator override a deployed file for one run.

namespace {

constexpr const char* kUsage =
    "usage: kds_server [<data_file>] [--config <path>] [--port <n>]\n"
    "                  [--log-file <name>] [--log-dir <dir>] [--log-level <level>]\n"
    "       kds_server --add-user <name> [--role readonly|readwrite|admin]\n"
    "                  [--users-file <path>] [--config <path>]\n"
    "\n"
    "  --config <path>   key = value settings file; see docs/spec/client-manual.md\n"
    "  <data_file>       positional, overrides data_file from the config\n"
    "  --add-user <name> provision a SCRAM-SHA-256 user into the users file\n"
    "                    (prompts for the password; the server does not run)\n"
    "\n"
    "Config keys: data_file, port, wal_dir, checkpoint_interval_ms,\n"
    "             tls, tls_cert_file, tls_key_file, auth, users_file,\n"
    "             log_dir, log_file, log_level, ... (see kds.conf.sample)\n";

std::uint64_t NowUnixSeconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

#if KDS_WITH_TLS

// Reads a password from stdin, echo off when stdin is a terminal (a
// pipe is read as-is, which is what scripted provisioning uses).
std::string PromptPassword(const char* prompt) {
    std::cout << prompt << std::flush;
    // `echo_off` tracks *what was changed*, not "is a terminal": on an fd
    // that isatty() accepts but tcgetattr() fails on, `saved` is still the
    // zero-initialized struct, and writing that back sets the terminal to
    // no echo, no canonical mode and a zero baud rate - wrecking the very
    // thing the restore exists to protect.
    termios saved{};
    bool echo_off = false;
    if (::isatty(STDIN_FILENO) == 1 && ::tcgetattr(STDIN_FILENO, &saved) == 0) {
        termios noecho = saved;
        noecho.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        echo_off = ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &noecho) == 0;
    }
    std::string password;
    std::getline(std::cin, password);
    if (echo_off) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        std::cout << "\n";
    }
    return password;
}

// The provisioning mode: derive a verifier and append it to the users
// file. Runs instead of the server and never opens the data file - a
// users file is deployment state, not database state.
//
// The iteration count is fixed at scram::kDefaultIterations, and the
// --iterations flag that briefly raised it was removed on review: the
// unknown-user mock always answers i=4096, so any verifier above it was
// identifiable in one round trip - the same enumeration oracle the mock
// exists to close, through a different attribute. Raising the count is
// future work tied to teaching the mock the deployment's own number
// (docs/spec/protocol.md §14).
int AddUser(const std::string& users_file, const std::string& username,
            kds::server::Role role) {
    const std::uint32_t iterations = kds::server::scram::kDefaultIterations;
    if (users_file.empty()) {
        std::cerr << "--add-user needs a users file: pass --users-file or set "
                     "users_file in the config\n";
        return EXIT_FAILURE;
    }
    if (username.empty() ||
        username.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   "abcdefghijklmnopqrstuvwxyz0123456789_.-") !=
            std::string::npos) {
        // Restricting the charset here is what lets the SCRAM layer skip
        // SASLprep and the file format keep its one-space grammar.
        std::cerr << "username must be [A-Za-z0-9_.-]+, got '" << username << "'\n";
        return EXIT_FAILURE;
    }

    // Refuse a duplicate before prompting, not after. `creating` is
    // decided here, on the open, not later off a stream that has since
    // been read to its end.
    std::ifstream existing(users_file);
    const bool creating = !existing;
    if (existing) {
        std::ostringstream buf;
        buf << existing.rdbuf();
        auto store = kds::server::FileCredentialStore::Parse(buf.str(), users_file);
        if (!store.ok()) {
            std::cerr << "users file: " << store.status().message() << "\n";
            return EXIT_FAILURE;
        }
        if (store.value().Has(username)) {
            std::cerr << "user '" << username << "' already exists in " << users_file
                      << " (changing a password or a role is delete-then-add for now)\n";
            return EXIT_FAILURE;
        }
    }

    std::string password = PromptPassword("password: ");
    if (password.empty()) {
        std::cerr << "empty password refused\n";
        return EXIT_FAILURE;
    }
    if (PromptPassword("repeat: ") != password) {
        std::cerr << "passwords do not match\n";
        return EXIT_FAILURE;
    }

    // scram::RandomSalt is the one producer of salts, and that is the
    // point: a real user's salt must be byte-for-byte indistinguishable
    // from the unknown-user mock's, or server-first's s= is a user list
    // readable without a password. On RNG failure it returns empty and
    // DeriveVerifier refuses the empty salt - fail closed, no check here.
    auto verifier = kds::server::scram::DeriveVerifier(
        password, kds::server::scram::RandomSalt(), iterations);
    if (!verifier.ok()) {
        std::cerr << "derivation failed: " << verifier.status().message() << "\n";
        return EXIT_FAILURE;
    }
    std::ofstream out(users_file, std::ios::app);
    // A file of credentials - verifiers, not passwords, but still the
    // material a thief would run PBKDF2 against - is nobody's business
    // but the server's. Only on creation: an operator who widened an
    // existing file's mode on purpose keeps their decision.
    if (creating) (void)::chmod(users_file.c_str(), 0600);
    out << username << " " << kds::server::RoleName(role) << " " << verifier.value().Serialize()
        << "\n";
    // Closed here rather than at destruction: a stream flushed by its
    // destructor reports nothing, so a users file that failed on the way
    // out (a full or read-only filesystem) would print as provisioned.
    out.close();
    if (!out) {
        std::cerr << "cannot write " << users_file << ": " << std::strerror(errno) << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "user '" << username << "' added to " << users_file << " as "
              << kds::server::RoleName(role) << " (" << iterations << " PBKDF2 iterations)\n";
    return EXIT_SUCCESS;
}

#endif  // KDS_WITH_TLS

// What the argv asked for besides serving: provisioning mode.
struct CliMode {
    std::string add_user;  // empty = run the server
    // Least privilege when absent: admin is a word someone must type.
    // Unset rather than defaulted in place, so `--role` *without*
    // `--add-user` can be refused instead of silently ignored - a flag
    // accepted and not acted on is a lie about what the process did.
    std::optional<kds::server::Role> role;
};

// Returns false and prints why on a bad argument list.
bool ParseArgs(int argc, char** argv, kds::server::Expeditor::Config& config, CliMode& mode) {
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
        } else if (arg == "--add-user") {
            const char* v = next("--add-user");
            if (v == nullptr) return false;
            mode.add_user = v;
        } else if (arg == "--role") {
            const char* v = next("--role");
            if (v == nullptr) return false;
            auto role = kds::server::ParseRole(v);
            if (!role.ok()) {
                std::cerr << role.status().message() << "\n";
                return false;
            }
            mode.role = role.value();
        } else if (arg == "--users-file") {
            const char* v = next("--users-file");
            if (v == nullptr) return false;
            config.users_file = v;
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
    CliMode mode;
    if (!ParseArgs(argc, argv, config, mode)) {
        return EXIT_FAILURE;
    }

    // `--role` provisions; it says nothing about a running server. Refused
    // rather than ignored, in the same posture as every other refusal here:
    // an operator who typed it meant something by it.
    if (mode.role.has_value() && mode.add_user.empty()) {
        std::cerr << "--role only applies to --add-user\n" << kUsage;
        return EXIT_FAILURE;
    }

    if (!mode.add_user.empty()) {
#if KDS_WITH_TLS
        return AddUser(config.users_file, mode.add_user,
                       mode.role.value_or(kds::server::Role::kReadOnly));
#else
        std::cerr << "--add-user requires a build with KDS_WITH_TLS (OpenSSL)\n";
        return EXIT_FAILURE;
#endif
    }

    // **Before Open(), and that is the whole of the ordering.** Open() starts the
    // WAL writer thread, and a signal is delivered to whichever thread does not
    // block it - so installing this afterwards would leave a thread that still
    // takes the default action and kills the process, intermittently. Blocking
    // here means every thread this process ever starts inherits the block
    // (`server/stop_signal.hpp`).
    //
    // Fatal if it fails: a server that cannot hear a stop is a server that has to
    // be killed, and being killed - no final sync, no shutdown checkpoint, a next
    // mount that recovers as if from a crash - is exactly what this prevents.
    auto stop_signal = kds::server::StopSignal::Install();
    if (!stop_signal.ok()) {
        std::cerr << "startup failed: " << stop_signal.status().message() << "\n";
        return EXIT_FAILURE;
    }

    auto expeditor = kds::server::Expeditor::Open(config, NowUnixSeconds());
    if (!expeditor.ok()) {
        std::cerr << "startup failed: " << expeditor.status().message() << "\n";
        return EXIT_FAILURE;
    }
    auto& db = *expeditor.value();
    db.set_stop_signal(&stop_signal.value());

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
