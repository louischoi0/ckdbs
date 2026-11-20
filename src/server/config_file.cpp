#include "kds/server/config_file.hpp"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace kds::server {

namespace {

std::string_view Trim(std::string_view s) {
    const char* ws = " \t\r\n";
    std::size_t b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    std::size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

}  // namespace

StatusOr<ConfigFile> ConfigFile::Load(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return Status::NotFound("cannot open config file '" + path +
                                "': " + std::strerror(errno));
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return Parse(buf.str(), path);
}

StatusOr<ConfigFile> ConfigFile::Parse(std::string_view text, const std::string& origin) {
    ConfigFile config;
    config.origin_ = origin;

    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view raw = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++line_no;

        const std::string where = origin + ":" + std::to_string(line_no);

        // A '#' outside quotes starts a comment. A '#' ahead of the line's
        // first '=' is always one - no key holds a '#' - so a full-line
        // comment and a commented-out `# key = value` are dropped here; a
        // '#' after the '=' is the value's to place (below).
        const std::size_t first_hash = raw.find('#');
        const std::size_t first_eq = raw.find('=');
        if (first_hash != std::string_view::npos &&
            (first_eq == std::string_view::npos || first_hash < first_eq)) {
            raw = raw.substr(0, first_hash);
        }

        std::string_view line = Trim(raw);
        if (line.empty()) continue;

        std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            return Status::InvalidArgument(where + ": expected 'key = value', got '" +
                                            std::string(line) + "'");
        }

        std::string key(Trim(line.substr(0, eq)));
        std::string_view rest = Trim(line.substr(eq + 1));
        std::string value;
        if (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) {
            // **A quoted value runs to its matching quote**, keeping any '#'
            // and whitespace inside it, and only whitespace or a '#' comment
            // may follow that quote. Anything else is refused rather than
            // folded into the value: guessing is how a config file lies.
            const std::size_t close = rest.find(rest.front(), 1);
            if (close == std::string_view::npos) {
                return Status::InvalidArgument(where + ": unterminated quoted value for key '" +
                                                key + "'");
            }
            const std::string_view after = Trim(rest.substr(close + 1));
            if (!after.empty() && after.front() != '#') {
                return Status::InvalidArgument(where + ": unexpected text '" +
                                                std::string(after) +
                                                "' after the quoted value of key '" + key + "'");
            }
            value.assign(rest.substr(1, close - 1));
        } else {
            // Unquoted: the first '#' starts the comment.
            const std::size_t hash = rest.find('#');
            value.assign(Trim(hash == std::string_view::npos ? rest : rest.substr(0, hash)));
        }

        if (key.empty()) {
            return Status::InvalidArgument(where + ": empty key");
        }
        if (auto it = config.values_.find(key); it != config.values_.end()) {
            // Not last-wins: which one the operator meant is exactly what
            // is unclear, and guessing is how a config file lies.
            return Status::InvalidArgument(where + ": key '" + key + "' already set on line " +
                                            std::to_string(it->second.line));
        }

        config.order_.push_back(key);
        config.values_.emplace(std::move(key), Entry{std::move(value), line_no});
    }

    return config;
}

bool ConfigFile::Has(std::string_view key) const {
    return values_.find(std::string(key)) != values_.end();
}

StatusOr<std::string> ConfigFile::GetString(std::string_view key) const {
    auto it = values_.find(std::string(key));
    if (it == values_.end()) {
        return Status::NotFound("config key '" + std::string(key) + "' is not set");
    }
    return it->second.value;
}

StatusOr<std::uint64_t> ConfigFile::GetUint(std::string_view key) const {
    auto it = values_.find(std::string(key));
    if (it == values_.end()) {
        return Status::NotFound("config key '" + std::string(key) + "' is not set");
    }

    const std::string& text = it->second.value;
    std::uint64_t out = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
        return Status::InvalidArgument(origin_ + ":" + std::to_string(it->second.line) + ": key '" +
                                        std::string(key) +
                                        "' expects a non-negative integer, got '" + text + "'");
    }
    return out;
}

StatusOr<bool> ConfigFile::GetBool(std::string_view key) const {
    auto value = GetString(key);
    if (!value.ok()) return value.status();

    std::string lower;
    for (char c : value.value()) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower == "true" || lower == "yes" || lower == "on" || lower == "1") return true;
    if (lower == "false" || lower == "no" || lower == "off" || lower == "0") return false;

    auto it = values_.find(std::string(key));
    return Status::InvalidArgument(origin_ + ":" + std::to_string(it->second.line) + ": key '" +
                                    std::string(key) + "' expects a boolean, got '" +
                                    value.value() + "'");
}

std::vector<std::string> ConfigFile::UnknownKeys(const std::vector<std::string>& known) const {
    std::vector<std::string> unknown;
    for (const std::string& key : order_) {
        bool found = false;
        for (const std::string& k : known) {
            if (k == key) {
                found = true;
                break;
            }
        }
        if (!found) unknown.push_back(key);
    }
    return unknown;
}

}  // namespace kds::server
