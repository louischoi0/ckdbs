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

// Strips one layer of matching quotes, so a value with meaningful
// surrounding whitespace (or a trailing '#') can be written explicitly.
std::string_view Unquote(std::string_view s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
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

        // A '#' outside quotes starts a comment. Checked before trimming so
        // a full-line comment costs nothing.
        std::size_t hash = raw.find('#');
        if (hash != std::string_view::npos) {
            std::string_view before = Trim(raw.substr(0, hash));
            // Only honour the '#' as a comment when it is not inside a
            // quoted value - otherwise a path containing '#' is unwritable.
            bool in_quotes = false;
            std::size_t eq_probe = before.find('=');
            if (eq_probe != std::string_view::npos) {
                std::string_view val = Trim(before.substr(eq_probe + 1));
                in_quotes = !val.empty() && (val.front() == '"' || val.front() == '\'') &&
                            val.back() != val.front();
            }
            if (!in_quotes) raw = raw.substr(0, hash);
        }

        std::string_view line = Trim(raw);
        if (line.empty()) continue;

        std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            return Status::InvalidArgument(origin + ":" + std::to_string(line_no) +
                                            ": expected 'key = value', got '" +
                                            std::string(line) + "'");
        }

        std::string key(Trim(line.substr(0, eq)));
        std::string value(Unquote(Trim(line.substr(eq + 1))));

        if (key.empty()) {
            return Status::InvalidArgument(origin + ":" + std::to_string(line_no) +
                                            ": empty key");
        }
        if (auto it = config.values_.find(key); it != config.values_.end()) {
            // Not last-wins: which one the operator meant is exactly what
            // is unclear, and guessing is how a config file lies.
            return Status::InvalidArgument(origin + ":" + std::to_string(line_no) + ": key '" +
                                            key + "' already set on line " +
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
