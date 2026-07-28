#include "kds/server/command_dispatcher.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>

#include "kds/storage/heap/heap_page.hpp"

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

// Hex-encodes a tuple payload for SHOW PAGE ... VALUES. Hex rather than
// raw text: an arbitrary tuple payload can contain any byte value,
// including a literal '\n' - embedding that directly would desync a
// client's one-line-per-response framing (see the escaping note in
// HandleShowPage). Hex output is plain ASCII by construction, so it is
// always safe to splice into the single response line.
std::string HexEncode(std::span<const std::byte> bytes) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::byte b : bytes) {
        auto v = static_cast<unsigned char>(b);
        out.push_back(kHexDigits[v >> 4]);
        out.push_back(kHexDigits[v & 0x0F]);
    }
    return out;
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
        if (IEquals(sub, "TABLES")) return HandleListTables();
        if (IEquals(sub, "PAGE")) return HandleShowPage(sub_rest);
        return {"ERR unknown SHOW target", false};
    }
    if (IEquals(cmd, "FIND")) {
        auto [sub, sub_rest] = SplitFirstToken(rest);
        if (IEquals(sub, "TABLE")) return HandleFindTable(sub_rest);
        return {"ERR unknown FIND target", false};
    }
    if (IEquals(cmd, "CREATE")) {
        auto [sub, sub_rest] = SplitFirstToken(rest);
        if (IEquals(sub, "TABLE")) return HandleCreateTable(sub_rest);
        return {"ERR unknown CREATE target", false};
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

DispatchOutcome CommandDispatcher::HandleShowPage(std::string_view args) {
    if (args.empty()) {
        return {"ERR SHOW PAGE requires a page id", false};
    }

    auto [id_token, option] = SplitFirstToken(args);

    bool show_values = false;
    if (!option.empty()) {
        if (!IEquals(option, "VALUES")) {
            return {"ERR unknown SHOW PAGE option: " + std::string(option), false};
        }
        show_values = true;
    }

    PageId page_id;
    auto [ptr, ec] = std::from_chars(id_token.data(), id_token.data() + id_token.size(), page_id);
    if (ec != std::errc() || ptr != id_token.data() + id_token.size()) {
        return {"ERR invalid page id: " + std::string(id_token), false};
    }

    auto page = page_store_.Get(page_id);
    if (!page.ok()) {
        return {"ERR " + page.status().message(), false};
    }

    heap::PageView view(page.value());

    // The wire protocol allows exactly one response line per command (see
    // this class's header comment / docs/client-manual.md section 2), so a
    // raw newline byte can't appear here - it would desync any client that
    // reads "up to the next \n" as one reply. Instead, sections are joined
    // with the two-character escape "\n" (backslash + n); the bundled CLI
    // (tools/ckdbs_cli.py) unescapes it back into real newlines before
    // printing, giving a readable multi-line dump for developers without
    // breaking the one-line-per-response contract on the wire.
    std::ostringstream os;
    os << "page_id=" << page_id << "\\n"
       << "min_key=" << view.min_key() << "\\n"
       << "nr_slots=" << view.slot_count() << "\\n"
       << "lower=" << view.lower() << "\\n"
       << "upper=" << view.upper() << "\\n"
       << "free_space=" << view.free_space() << "\\n"
       << "next_page_id=" << view.next_page_id();

    for (std::uint16_t i = 0; i < view.slot_count(); ++i) {
        auto slot = view.DebugSlotInfo(i);
        if (!slot.ok()) continue;
        os << "\\n"
           << "slot[" << i << "] offset=" << slot.value().offset
           << " length=" << slot.value().length << " dead=" << (slot.value().dead ? 1 : 0);

        if (show_values && !slot.value().dead) {
            auto tuple = view.ReadTuple(i);
            if (tuple.ok()) {
                os << " value=" << HexEncode(tuple.value().payload);
            }
        }
    }

    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleCreateTable(std::string_view args) {
    if (args.empty()) {
        return {"ERR CREATE TABLE requires a name", false};
    }

    auto existing = catalog_.FindTableOidByName(args);
    if (existing.ok()) {
        return {"EXISTS oid=" + std::to_string(existing.value()), false};
    }
    if (existing.status().code() != StatusCode::kNotFound) {
        return {"ERR " + existing.status().message(), false};
    }

    catalog::Schema schema;
    auto oid = catalog_.CreateTable(catalog::kNamespacePublic, args, schema,
                                     catalog::ClusteredType::kHeap);
    if (!oid.ok()) {
        return {"ERR " + oid.status().message(), false};
    }
    return {"CREATED oid=" + std::to_string(oid.value()), false};
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
