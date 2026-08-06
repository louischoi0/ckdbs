#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "kds/base/status.hpp"

// The server's configuration file: `key = value` lines, `#` comments.
//
//     # kds.conf
//     data_file = kds.db
//     port      = 15432
//     log_dir   = /var/log/kds
//     log_file  = kdb.log
//     log_level = info
//     checkpoint_interval_ms = 5000
//
// ---- Why this format ----------------------------------------------------
//
// No dependencies. The engine has none and a config file is a poor reason
// to acquire the first: TOML/YAML/JSON all mean vendoring a parser, and
// every one of them buys expressiveness this file does not need. Flat
// key/value with comments is what the settings actually are.
//
// ---- Why unknown keys are an error --------------------------------------
//
// A typo in a config file is otherwise the quietest failure a server has -
// `chekpoint_interval_ms = 100` looks like it worked, and the operator
// finds out when the loss window turns out to be 5 seconds. Rejecting the
// key at startup costs a restart; accepting it costs a misunderstanding
// that can last months. The same reasoning applies to a value that does not
// parse, and to a duplicate key: silently keeping the last one hides the
// question of which the operator meant.
//
// ---- Precedence ---------------------------------------------------------
//
// Command line beats file, file beats built-in default. main() applies it
// in that order, and nothing here knows about argv.

namespace kds::server {

// One parsed file: the keys in the order they appeared, with their values.
// Order is kept so an error can name a line number and so re-emitting a
// config does not scramble it.
class ConfigFile {
public:
    // Reads and parses `path`. Fails with NotFound if the file does not
    // exist - callers wanting "use defaults when absent" should check for
    // that code rather than have this invent an empty config, since a
    // mistyped --config path must not silently start a default server.
    static StatusOr<ConfigFile> Load(const std::string& path);

    // Parses already-read text. `origin` only names the source in error
    // messages.
    static StatusOr<ConfigFile> Parse(std::string_view text, const std::string& origin);

    bool Has(std::string_view key) const;

    // Value lookups. Each fails with NotFound if the key is absent and
    // InvalidArgument if the value does not parse as the requested type,
    // naming the key and the offending text.
    StatusOr<std::string> GetString(std::string_view key) const;
    StatusOr<std::uint64_t> GetUint(std::string_view key) const;
    StatusOr<bool> GetBool(std::string_view key) const;

    // Keys present in the file that are not in `known`. Callers report
    // these and refuse to start - see the header comment.
    std::vector<std::string> UnknownKeys(const std::vector<std::string>& known) const;

    const std::string& origin() const noexcept { return origin_; }
    std::size_t size() const noexcept { return values_.size(); }

private:
    struct Entry {
        std::string value;
        std::size_t line;
    };

    std::unordered_map<std::string, Entry> values_;
    std::vector<std::string> order_;
    std::string origin_;
};

}  // namespace kds::server
