#include "kds/server/command_dispatcher.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace kds::server {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool IEquals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// Splits `line` into the first whitespace-delimited token and "the rest"
// (trimmed), so multi-word commands like "SHOW META"/"FIND TABLE x" can
// be matched one token at a time without pulling in a real tokenizer.
std::pair<std::string_view, std::string_view> SplitFirstToken(std::string_view line) {
    line = Trim(line);
    std::size_t sp = line.find_first_of(" \t");
    if (sp == std::string_view::npos) return {line, std::string_view{}};
    return {line.substr(0, sp), Trim(line.substr(sp + 1))};
}

}  // namespace

DispatchOutcome CommandDispatcher::Dispatch(std::string_view line) {
    auto [cmd, rest] = SplitFirstToken(line);

    if (cmd.empty()) {
        return {"ERR empty command", false};
    }
    if (IEquals(cmd, "PING")) {
        return {"PONG", false};
    }
    if (IEquals(cmd, "STOP")) {
        return {"OK bye", true};
    }
    if (IEquals(cmd, "SHOW")) {
        auto [sub, sub_rest] = SplitFirstToken(rest);
        if (IEquals(sub, "META")) return HandleShowMeta();
        return {"ERR unknown SHOW target", false};
    }
    if (IEquals(cmd, "LIST")) {
        auto [sub, sub_rest] = SplitFirstToken(rest);
        if (IEquals(sub, "TABLES")) return HandleListTables();
        return {"ERR unknown LIST target", false};
    }
    if (IEquals(cmd, "FIND")) {
        auto [sub, sub_rest] = SplitFirstToken(rest);
        if (IEquals(sub, "TABLE")) return HandleFindTable(sub_rest);
        return {"ERR unknown FIND target", false};
    }

    return {"ERR unknown command", false};
}

DispatchOutcome CommandDispatcher::HandleShowMeta() {
    std::ostringstream os;
    os << "version=" << superblock_.version() << " max_page_id=" << superblock_.max_page_id()
       << " create_time=" << superblock_.create_time()
       << " last_mount_time=" << superblock_.last_mount_time()
       << " last_fsync_time=" << superblock_.last_fsync_time()
       << " total_pages=" << superblock_.total_pages()
       << " free_pages=" << superblock_.free_pages();
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleListTables() {
    auto tables = catalog_.ListTables();
    if (!tables.ok()) {
        return {"ERR " + tables.status().message(), false};
    }

    std::ostringstream os;
    bool first = true;
    for (const auto& row : tables.value()) {
        if (!first) os << ' ';
        os << catalog::NameView(row.name);
        first = false;
    }
    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleFindTable(std::string_view args) {
    if (args.empty()) {
        return {"ERR FIND TABLE requires a name", false};
    }

    auto oid = catalog_.FindTableOidByName(args);
    if (!oid.ok()) {
        return {"ERR " + oid.status().message(), false};
    }
    return {"oid=" + std::to_string(oid.value()), false};
}

}  // namespace kds::server
