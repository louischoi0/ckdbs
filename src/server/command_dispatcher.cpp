#include "kds/server/command_dispatcher.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
#include <variant>

#include "kds/exec/row_codec.hpp"
#include "kds/parser/parser.hpp"
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

// Scans the page for a live tuple already carrying `id`. Only the fixed
// Keystone word at the front of each payload is read - no schema walk, no
// row decode - which is the practical payoff of putting the pk there.
// Delete-marked tuples still count: their key is not free until the slot
// is physically retired.
Status CheckDuplicateKey(const heap::PageView& page, std::uint64_t id) {
    std::uint16_t n = page.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        auto tuple = page.ReadTuple(i);
        if (!tuple.ok()) continue;  // retired or out-of-range slot

        auto existing = exec::RowKeystoneId(tuple.value().payload);
        if (!existing.ok()) return existing.status();
        if (existing.value() == id) {
            return Status::AlreadyExists("duplicate primary key " + std::to_string(id) +
                                          " already present at slot " + std::to_string(i));
        }
    }
    return Status::OK();
}

}  // namespace

DispatchOutcome CommandDispatcher::Dispatch(std::string_view line) {
    // Read only when something might report it; a dispatcher with no
    // logger does no clock reads at all.
    const sched::MonoTimeNs started_ns = log_ == nullptr ? 0 : NowNs();

    DispatchOutcome outcome = DispatchInner(line);
    if (log_ == nullptr) return outcome;

    // A failed command reports at Warn and a successful one at Debug, so
    // the level has to be decided *before* the enabled() test - gating the
    // whole block on Debug would silently drop every error at any threshold
    // above it, which is exactly the threshold an operator runs at.
    const bool failed = outcome.response.rfind("ERR ", 0) == 0;
    const LogLevel level = failed ? LogLevel::kWarn : LogLevel::kDebug;
    if (!log_->enabled(level)) return outcome;

    // The reply is summarized, not echoed: a SELECT response carries every
    // matching row, and a log that reproduces result sets is a log that
    // cannot be kept. An error is the exception - its whole content is the
    // reason, which is the thing worth having.
    std::string msg =
        "\"" + std::string(line) + "\" -> " +
        (failed ? outcome.response : std::to_string(outcome.response.size()) + "B reply");
    if (clock_ != nullptr) {
        msg += " in " + std::to_string((NowNs() - started_ns) / 1000) + "us";
    }
    log_->Log(level, "query", msg);
    return outcome;
}

DispatchOutcome CommandDispatcher::DispatchInner(std::string_view line) {
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
    if (IEquals(cmd, "DESCRIBE") || IEquals(cmd, "DESC")) {
        return HandleDescribe(rest);
    }
    if (IEquals(cmd, "CREATE")) {
        auto [sub, sub_rest] = SplitFirstToken(rest);
        if (IEquals(sub, "TABLE")) {
            // Disambiguate the legacy bare-name form ("CREATE TABLE foo")
            // from the SQL form ("CREATE TABLE foo (col type, ...)"): the
            // bare form's argument is just a name, so it never contains
            // '(' - the SQL grammar always does (ast.hpp: a column list is
            // mandatory). Route on that rather than trying to parse both
            // ways and see which succeeds.
            if (sub_rest.find('(') != std::string_view::npos) {
                return HandleCreateTableSql(Trim(line));
            }
            return HandleCreateTable(sub_rest);
        }
        return {"ERR unknown CREATE target", false};
    }
    if (IEquals(cmd, "INSERT")) {
        return HandleInsert(Trim(line));
    }
    if (IEquals(cmd, "SELECT")) {
        return HandleSelect(Trim(line));
    }
    if (IEquals(cmd, "UPDATE")) {
        return HandleUpdate(Trim(line));
    }
    if (IEquals(cmd, "SYNC")) {
        return HandleSync();
    }

    return {"ERR unknown command", false};
}

DispatchOutcome CommandDispatcher::HandleSync() {
    if (Status s = page_store_.Sync(); !s.ok()) {
        if (logging(LogLevel::kError)) {
            log_->Error("storage", "client SYNC failed: " + s.message());
        }
        return {"ERR " + s.message(), false};
    }
    // Info, not Debug: a client-forced sync is rare and it is a durability
    // point, so it is one of the few things worth having in a default log.
    if (logging(LogLevel::kInfo)) log_->Info("storage", "client SYNC: store persisted");
    return {"OK synced", false};
}

DispatchOutcome CommandDispatcher::HandleShowMeta() {
    std::ostringstream os;
    os << "version=" << superblock_.version() << " create_time=" << superblock_.create_time()
       << " last_mount_time=" << superblock_.last_mount_time()
       << " wal_anchor_count=" << superblock_.wal_anchor_count();
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

DispatchOutcome CommandDispatcher::HandleDescribe(std::string_view args) {
    if (args.empty()) {
        return {"ERR DESCRIBE requires a table name", false};
    }

    auto oid = catalog_.FindTableOidByName(args);
    if (!oid.ok()) {
        return {"ERR " + oid.status().message(), false};
    }

    auto table_row = catalog_.GetSysTableRow(oid.value());
    if (!table_row.ok()) {
        return {"ERR " + table_row.status().message(), false};
    }

    // A relation with no registered columns is a fact, not a failure: the
    // bootstrap catalog tables encode their rows through their own
    // SysXxxRow::Encode() rather than the schema-driven row codec, so they
    // have no sys.columns entries. Reporting the header with columns=0
    // beats refusing to describe them at all.
    catalog::Schema schema;
    auto built = catalog_.BuildSchemaFromColumns(oid.value());
    if (built.ok()) {
        schema = std::move(built.value());
    } else if (built.status().code() != StatusCode::kNotFound) {
        return {"ERR " + built.status().message(), false};
    }

    const char* clustered =
        table_row.value().clustered_type == catalog::ClusteredType::kBtree ? "BTREE" : "HEAP";

    // Same one-line-per-response contract as SHOW PAGE and SELECT: a
    // summary line, then one "\n"-escaped section per column, never a raw
    // newline byte.
    std::ostringstream os;
    os << "oid=" << oid.value() << " root_page_id=" << table_row.value().desc_page_id
       << " clustered_type=" << clustered << " next_id=" << table_row.value().next_id
       << " columns=" << schema.columns.size();

    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
        const catalog::SysColumnRow& col = schema.columns[i];

        // A type_val with no sys.types row is a catalog inconsistency, not
        // a reason to fail the whole DESCRIBE - report it in place, since
        // seeing *which* column is broken is the point of the command.
        auto type_row = catalog_.ResolveTypeByVal(col.type_val);
        const std::string type_name = type_row.ok()
                                          ? std::string(catalog::NameView(type_row.value().name))
                                          : "?type_val=" + std::to_string(col.type_val);

        // Column 0 is the Keystone primary key by construction, not by a
        // stored flag - heap-and-tuple.md section 4 makes it positional.
        const bool is_pk = i == 0;
        os << "\\n"
           << "pos=" << col.pos << " name=" << catalog::NameView(col.name) << " type=" << type_name
           << " len=" << col.len << " notnull=" << (col.notnull ? "yes" : "no")
           << " pk=" << (is_pk ? "yes" : "no")
           << " autoincrement=" << (is_pk ? "yes" : "no");
    }

    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleCreateTableSql(std::string_view line) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
    }
    if (!std::holds_alternative<parser::CreateTableStmt>(parsed.value())) {
        return {"ERR expected a CREATE TABLE statement", false};
    }
    auto& stmt = std::get<parser::CreateTableStmt>(parsed.value());

    auto existing = catalog_.FindTableOidByName(stmt.table_name);
    if (existing.ok()) {
        return {"EXISTS oid=" + std::to_string(existing.value()), false};
    }
    if (existing.status().code() != StatusCode::kNotFound) {
        return {"ERR " + existing.status().message(), false};
    }

    // Resolve each column's parsed type_name against sys.types - the
    // stand-in type registry (Catalog::ResolveTypeByName(), see its
    // comment) - before touching storage, so a bad type name fails clean
    // with nothing created.
    catalog::Schema schema;
    std::uint32_t pos = 0;
    for (const auto& col : stmt.columns) {
        auto type_row = catalog_.ResolveTypeByName(col.type_name);
        if (!type_row.ok()) {
            return {"ERR " + type_row.status().message(), false};
        }

        catalog::SysColumnRow row{};
        row.pos = pos++;
        catalog::SetName(row.name, col.name);
        row.type_val = type_row.value().type_val;
        row.len = type_row.value().len;
        row.notnull = true;  // no NULL support yet - see row_codec.hpp
        schema.columns.push_back(row);
    }

    auto oid =
        catalog_.CreateTable(catalog::kNamespacePublic, stmt.table_name, schema, stmt.clustered);
    if (!oid.ok()) {
        return {"ERR " + oid.status().message(), false};
    }
    // Info: DDL is rare and changes the shape of everything after it, so
    // it belongs in a default-level log even though ordinary writes do not.
    if (logging(LogLevel::kInfo)) {
        log_->Info("ddl", "created table '" + std::string(stmt.table_name) +
                              "' oid=" + std::to_string(oid.value()) +
                              " columns=" + std::to_string(schema.columns.size()));
    }
    return {"CREATED oid=" + std::to_string(oid.value()), false};
}

DispatchOutcome CommandDispatcher::HandleInsert(std::string_view line) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
    }
    if (!std::holds_alternative<parser::InsertStmt>(parsed.value())) {
        return {"ERR expected an INSERT statement", false};
    }
    auto& stmt = std::get<parser::InsertStmt>(parsed.value());

    auto oid = catalog_.FindTableOidByName(stmt.table_name);
    if (!oid.ok()) {
        return {"ERR " + oid.status().message(), false};
    }

    auto access = catalog_.InitTableAccess(catalog::kNamespacePublic, oid.value());
    if (!access.ok()) {
        return {"ERR " + access.status().message(), false};
    }

    // The primary key is the engine's to issue, never the caller's
    // (CLAUDE.md invariant 10), so VALUES supplies the columns *after* it.
    // Catching the old arity here gives a usable message instead of the
    // codec's "expected N value(s)".
    const std::size_t ncols = access.value().schema.columns.size();
    if (ncols > 0 && stmt.values.size() == ncols) {
        return {"ERR do not supply a value for primary-key column '" +
                    std::string(catalog::NameView(access.value().schema.columns.front().name)) +
                    "' - it is autoincrement and engine-assigned",
                false};
    }

    auto id = catalog_.AllocateRowId(oid.value());
    if (!id.ok()) {
        return {"ERR " + id.status().message(), false};
    }

    auto encoded = exec::EncodeRow(access.value().schema, id.value(), stmt.values);
    if (!encoded.ok()) {
        return {"ERR " + encoded.status().message(), false};
    }

    // Single-page heap only: writes go straight to the table's root/desc
    // page, no page-full overflow to a new page - heap page split policy
    // is an open decision (CLAUDE.md) this doesn't attempt to resolve.
    auto bytes = page_store_.Get(access.value().desc_page_id);
    if (!bytes.ok()) {
        return {"ERR " + bytes.status().message(), false};
    }

    heap::PageView page(bytes.value());

    // Belt to the sequence's braces. An issued id is unique by
    // construction, so this can only fire if the sequence and the heap
    // have drifted - a corrupted or rolled-back sys.tables row. That is
    // exactly when silently writing a second tuple under one key would do
    // the most damage, since Waystone addresses tuples by id directly.
    if (Status s = CheckDuplicateKey(page, id.value()); !s.ok()) {
        return {"ERR " + s.message(), false};
    }

    auto slot = page.InsertTuple(encoded.value(), /*trx_id=*/catalog::kBootstrapXid);
    if (!slot.ok()) {
        if (logging(LogLevel::kWarn)) {
            log_->Warn("heap", "insert into page " + std::to_string(access.value().desc_page_id) +
                                   " failed: " + slot.status().message());
        }
        return {"ERR " + slot.status().message(), false};
    }

    // Trace: one line per inserted tuple. Logged from here rather than from
    // PageView, which is a bare view over page bytes with no business
    // owning a logger - and which the catalog also writes through, where a
    // "heap insert" line would describe a catalog row, not a user tuple.
    if (logging(LogLevel::kTrace)) {
        log_->Trace("heap", "insert page=" + std::to_string(access.value().desc_page_id) +
                                " slot=" + std::to_string(slot.value()) +
                                " id=" + std::to_string(id.value()) +
                                " bytes=" + std::to_string(encoded.value().size()));
    }

    return {"INSERTED oid=" + std::to_string(oid.value()) + " id=" + std::to_string(id.value()) +
                " slot=" + std::to_string(slot.value()),
            false};
}

DispatchOutcome CommandDispatcher::HandleSelect(std::string_view line) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
    }
    if (!std::holds_alternative<parser::SelectStmt>(parsed.value())) {
        return {"ERR expected a SELECT statement", false};
    }
    auto& stmt = std::get<parser::SelectStmt>(parsed.value());

    auto oid = catalog_.FindTableOidByName(stmt.table_name);
    if (!oid.ok()) {
        return {"ERR " + oid.status().message(), false};
    }

    auto access = catalog_.InitTableAccess(catalog::kNamespacePublic, oid.value());
    if (!access.ok()) {
        return {"ERR " + access.status().message(), false};
    }

    auto bytes = page_store_.Get(access.value().desc_page_id);
    if (!bytes.ok()) {
        return {"ERR " + bytes.status().message(), false};
    }
    heap::PageView page(bytes.value());

    // Same one-line-per-response contract as SHOW PAGE: a header line of
    // column names, then one "\n"-escaped section per matching row
    // (comma-joined values), never a raw newline byte.
    std::ostringstream os;
    bool first_col = true;
    for (const auto& col : access.value().schema.columns) {
        if (!first_col) os << ',';
        os << catalog::NameView(col.name);
        first_col = false;
    }

    std::uint16_t n = page.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        auto tuple = page.ReadTuple(i);
        if (!tuple.ok()) continue;  // dead or out-of-range slot - skip

        auto row = exec::DecodeRow(access.value().schema, tuple.value().payload);
        if (!row.ok()) {
            return {"ERR " + row.status().message(), false};
        }

        if (!exec::MatchesWhere(access.value().schema, row.value(), stmt.where)) continue;

        os << "\\n";
        bool first_val = true;
        for (const auto& v : row.value()) {
            if (!first_val) os << ',';
            os << exec::FormatValue(v);
            first_val = false;
        }
    }

    return {os.str(), false};
}

DispatchOutcome CommandDispatcher::HandleUpdate(std::string_view line) {
    auto parsed = parser::Parse(line);
    if (!parsed.ok()) {
        return {"ERR " + parsed.status().message(), false};
    }
    if (!std::holds_alternative<parser::UpdateStmt>(parsed.value())) {
        return {"ERR expected an UPDATE statement", false};
    }
    auto& stmt = std::get<parser::UpdateStmt>(parsed.value());

    auto oid = catalog_.FindTableOidByName(stmt.table_name);
    if (!oid.ok()) {
        return {"ERR " + oid.status().message(), false};
    }

    auto access = catalog_.InitTableAccess(catalog::kNamespacePublic, oid.value());
    if (!access.ok()) {
        return {"ERR " + access.status().message(), false};
    }

    // Validate every SET target names a real column before touching
    // storage, so a bad column name fails clean with no partial update.
    const std::string pk_name =
        access.value().schema.columns.empty()
            ? std::string()
            : std::string(catalog::NameView(access.value().schema.columns.front().name));
    for (const auto& assignment : stmt.assignments) {
        if (access.value().schema.FindColumn(assignment.col_name) == nullptr) {
            return {"ERR unknown column '" + assignment.col_name + "'", false};
        }
        // The pk is the tuple's identity, not a field of it: Waystone and
        // any future hint index address a tuple by this id, so changing it
        // in place would silently retarget every reference to the row.
        if (assignment.col_name == pk_name) {
            return {"ERR primary-key column '" + pk_name + "' cannot be updated", false};
        }
    }

    auto bytes = page_store_.Get(access.value().desc_page_id);
    if (!bytes.ok()) {
        return {"ERR " + bytes.status().message(), false};
    }
    heap::PageView page(bytes.value());

    std::uint32_t updated = 0;
    std::uint16_t n = page.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        auto tuple = page.ReadTuple(i);
        if (!tuple.ok()) continue;  // dead or out-of-range slot - skip

        auto row = exec::DecodeRow(access.value().schema, tuple.value().payload);
        if (!row.ok()) {
            return {"ERR " + row.status().message(), false};
        }

        if (!exec::MatchesWhere(access.value().schema, row.value(), stmt.where)) continue;

        for (const auto& assignment : stmt.assignments) {
            for (std::size_t c = 0; c < access.value().schema.columns.size(); ++c) {
                if (catalog::NameView(access.value().schema.columns[c].name) ==
                    assignment.col_name) {
                    row.value()[c] = assignment.val;
                    break;
                }
            }
        }

        // The pk is unchanged by construction (rejected above), so it is
        // carried straight from the tuple's own Keystone word rather than
        // round-tripped through the decoded row.
        auto id = exec::RowKeystoneId(tuple.value().payload);
        if (!id.ok()) {
            return {"ERR " + id.status().message(), false};
        }
        const std::vector<parser::AstValue> body(row.value().begin() + 1, row.value().end());

        auto encoded = exec::EncodeRow(access.value().schema, id.value(), body);
        if (!encoded.ok()) {
            return {"ERR " + encoded.status().message(), false};
        }

        // HOT-style in-place overwrite - see PageView::OverwriteTuple's
        // comment. No fallback to retire+reinsert if the new payload no
        // longer fits the slot's original capacity (e.g. a grown
        // varchar); that fails with OutOfSpace here, surfaced as ERR.
        Status s = page.OverwriteTuple(i, encoded.value(), tuple.value().trx_id,
                                        tuple.value().undo_ptr);
        if (!s.ok()) {
            return {"ERR " + s.message(), false};
        }
        ++updated;
    }

    if (updated > 0 && logging(LogLevel::kTrace)) {
        log_->Trace("heap", "overwrite page=" + std::to_string(access.value().desc_page_id) +
                                " rows=" + std::to_string(updated));
    }
    return {"UPDATED " + std::to_string(updated), false};
}

}  // namespace kds::server
