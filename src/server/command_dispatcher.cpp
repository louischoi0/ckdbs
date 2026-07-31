#include "kds/server/command_dispatcher.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <charconv>
#include <sstream>
#include <variant>

#include <vector>

#include "kds/exec/row_codec.hpp"
#include "kds/parser/parser.hpp"
#include "kds/stats/waystone_dir.hpp"
#include "kds/stats/waystone_probe.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/wal/payload.hpp"

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
    if (IEquals(cmd, "WAYSTONE")) {
        return HandleWaystone(Trim(rest));
    }

    return {"ERR unknown command", false};
}

DispatchOutcome CommandDispatcher::HandleSync() {
    // The log first, and unconditionally. The store's gate syncs it only
    // as far as the pages it is about to write need, so a relaxed commit
    // whose pages are already clean would survive a SYNC unsynced - and
    // SYNC's whole promise is that it does not.
    if (wal_ != nullptr) {
        if (Status s = wal_->SyncAll(); !s.ok()) {
            if (logging(LogLevel::kError)) {
                log_->Error("wal", "client SYNC failed to sync the log: " + s.message());
            }
            return {"ERR " + s.message(), false};
        }
    }
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

DispatchOutcome CommandDispatcher::HandleWaystone(std::string_view args) {
    auto [verb, rest] = SplitFirstToken(args);
    const std::string_view name = Trim(rest);

    if (IEquals(verb, "STATUS")) {
        std::ostringstream os;
        os << "probe_hits=" << waystone_probe_hits_
           << " probe_misses=" << waystone_probe_misses_;
        if (!name.empty()) {
            auto oid = catalog_.FindTableOidByName(name);
            if (!oid.ok()) return {"ERR " + oid.status().message(), false};
            auto access = catalog_.InitTableAccess(oid.value());
            if (!access.ok()) return {"ERR " + access.status().message(), false};
            os << " state=" << static_cast<int>(access.value()->waystone_state)
               << " dir_root=" << access.value()->waystone_dir_root
               << " dir_depth=" << static_cast<int>(access.value()->waystone_dir_depth);
        }
        return {os.str(), false};
    }

    if (name.empty()) {
        return {"ERR WAYSTONE " + std::string(verb) + " requires a table name", false};
    }
    auto oid = catalog_.FindTableOidByName(name);
    if (!oid.ok()) return {"ERR " + oid.status().message(), false};

    if (IEquals(verb, "ENABLE")) {
        auto access = catalog_.InitTableAccess(oid.value());
        if (!access.ok()) return {"ERR " + access.status().message(), false};
        if (catalog::WaystoneActive(access.value()->waystone_state)) {
            return {"OK already enabled", false};
        }

        // Depth from the relation's *next* id, not its current one: the
        // directory has to cover the ids this relation is about to issue,
        // and provisioning one level short would force a growth on the
        // very first insert.
        auto row = catalog_.GetSysTableRow(oid.value());
        if (!row.ok()) return {"ERR " + row.status().message(), false};
        auto depth = stats::DirDepthFor(row.value().next_id);
        if (!depth.ok()) return {"ERR " + depth.status().message(), false};

        auto root = stats::CreateDirPage(page_store_);
        if (!root.ok()) return {"ERR " + root.status().message(), false};

        // A relation with rows already in it needs a backfill before
        // coverage can be claimed (spec section 7); an empty one is
        // trivially covered. Backfill (T17) does not exist, so a
        // non-empty relation is refused rather than silently left with
        // partial coverage that a later reader might trust.
        if (row.value().next_id != catalog::kFirstRowId) {
            return {"ERR cannot enable Waystone on a relation that already holds rows; "
                    "backfill is not implemented",
                    false};
        }

        if (Status s = catalog_.SetWaystoneDirectory(oid.value(), catalog::WaystoneState::kCovered,
                                                     root.value(),
                                                     static_cast<std::uint8_t>(depth.value()));
            !s.ok()) {
            return {"ERR " + s.message(), false};
        }
        return {"OK waystone enabled root=" + std::to_string(root.value()) +
                    " depth=" + std::to_string(depth.value()),
                false};
    }

    if (IEquals(verb, "DISABLE")) {
        // The directory and entry pages are deliberately leaked rather
        // than freed: the store has no free-page path yet (page.md's
        // SpaceManager), and orphaning pages is the safe half of that
        // trade. Dropping the root reference is what makes them
        // unreachable, and re-enabling allocates a fresh directory.
        if (Status s = catalog_.SetWaystoneDirectory(
                oid.value(), catalog::WaystoneState::kDisabled, kInvalidPageId, 0);
            !s.ok()) {
            return {"ERR " + s.message(), false};
        }
        return {"OK waystone disabled", false};
    }

    return {"ERR unknown WAYSTONE verb: " + std::string(verb) + " (ENABLE|DISABLE|STATUS)",
            false};
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

    // A B+ tree internal node has no slot directory and no tuples, so it
    // gets its own render rather than a heap dump of nonsense - SHOW PAGE
    // is the tool someone reaches for when a descent went somewhere
    // unexpected, and it is useless if it cannot show the separators that
    // sent it there.
    if (storage::RawPageType(page.value()) ==
        static_cast<std::uint8_t>(PageType::kBtreeInternal)) {
        btree::InternalView node(page.value());
        std::ostringstream os;
        os << "page_id=" << page_id << "\\n"
           << "page_type=BTREE_INTERNAL\\n"
           << "level=" << node.level() << "\\n"
           << "nr_entries=" << node.entry_count() << "\\n"
           << "max_entries=" << btree::kInternalMaxEntries << "\\n"
           << "leftmost_child=" << node.leftmost_child();
        for (std::uint16_t i = 0; i < node.entry_count(); ++i) {
            auto entry = node.Entry(i);
            if (!entry.ok()) continue;
            os << "\\n"
               << "entry[" << i << "] sep_key=" << entry.value().sep_key
               << " child=" << entry.value().child;
        }
        return {os.str(), false};
    }

    heap::PageView view(page.value());
    const bool is_leaf = storage::RawPageType(page.value()) ==
                         static_cast<std::uint8_t>(PageType::kBtreeLeaf);

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
       << "page_type=" << (is_leaf ? "BTREE_LEAF" : "HEAP") << "\\n"
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

    // The tree's shape, which is the thing you actually want to know about
    // a btree relation and cannot get from anywhere else. Reported
    // best-effort: a relation whose index pages are unreadable still has a
    // describable schema, and refusing the whole command over it would
    // remove the tool at the moment it is needed.
    if (table_row.value().clustered_type == catalog::ClusteredType::kBtree) {
        auto height = btree::BtreeHeight(page_store_, table_row.value().desc_page_id);
        auto leaves = btree::BtreeLeafCount(page_store_, table_row.value().desc_page_id);
        os << " height=" << (height.ok() ? std::to_string(height.value()) : std::string("?"))
           << " leaves=" << (leaves.ok() ? std::to_string(leaves.value()) : std::string("?"));
    }

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

Status CommandDispatcher::LogInsert(const storage::InsertPlacement& placed, PageType leaf_type,
                                    std::span<const std::byte> tuple, std::uint64_t trx_id) {
    if (wal_ == nullptr) return Status::OK();

    const std::uint64_t txn_id = next_txn_id_++;
    if (auto begun = wal_->Append(wal::RecordSpec{wal::RecordType::kTxnBegin, txn_id});
        !begun.ok()) {
        return begun.status();
    }

    // Every page the insert restructured, in the order the storage layer
    // says redo has to apply it, and all of it before the tuple's own
    // record. A brand-new tuple page is a PAGE_INIT that the HEAP_INSERT
    // below then fills; everything else - a link edit, a B+ tree internal
    // node created or amended, a new root - is a full page image, because
    // no record type describes those (insert_placement.hpp).
    for (const storage::StructuralChange& change : placed.changes()) {
        if (change.is_new_page) {
            std::array<std::byte, wal::kPageInitPayloadSize> init{};
            const wal::PageInitPayload init_fields{change.min_key,
                                                   static_cast<std::uint8_t>(leaf_type),
                                                   {0, 0, 0}};
            if (auto n = wal::EncodePageInit(init, init_fields); !n.ok()) return n.status();
            if (auto rec = wal_->Append(
                    wal::RecordSpec{wal::RecordType::kPageInit, txn_id, change.page_id}, init);
                !rec.ok()) {
                return rec.status();
            }
            // Deliberately unstamped: a new tuple page is always the page
            // the HEAP_INSERT below lands in, and that stamps it. A new
            // *internal* node is never reported as is_new_page, so it takes
            // the image arm and is stamped there.
            continue;
        }

        auto bytes = page_store_.Get(change.page_id);
        if (!bytes.ok()) return bytes.status();

        std::vector<std::byte> image(wal::kFullPageImagePayloadSize);
        if (auto n = wal::EncodeFullPageImage(
                image, std::span<const std::byte, kPageSize>(bytes.value()));
            !n.ok()) {
            return n.status();
        }
        auto fpi = wal_->Append(
            wal::RecordSpec{wal::RecordType::kFullPageImage, txn_id, change.page_id}, image);
        if (!fpi.ok()) return fpi.status();
        if (Status s = page_store_.StampPageLsn(change.page_id, fpi.value()); !s.ok()) return s;
    }

    // undo_ptr 0: an insert supersedes no version, so its undo chain ends
    // at itself (wal.md section 5.1).
    std::vector<std::byte> payload(wal::kHeapWriteFixedSize + tuple.size());
    const wal::HeapWritePayload fields{trx_id, /*undo_ptr=*/0, placed.slot,
                                       static_cast<std::uint16_t>(tuple.size())};
    if (auto n = wal::EncodeHeapWrite(payload, fields, tuple); !n.ok()) return n.status();

    auto rec = wal_->Append(
        wal::RecordSpec{wal::RecordType::kHeapInsert, txn_id, placed.page_id}, payload);
    if (!rec.ok()) return rec.status();
    if (Status s = page_store_.StampPageLsn(placed.page_id, rec.value()); !s.ok()) return s;

    auto commit = wal_->Commit(txn_id, durability_);
    if (!commit.ok()) return commit.status();

    // kStrict already synced inside Commit(). kGroup did not: it staged
    // the commit for the next drain, and the acknowledgement owed to the
    // client is "durable", so the wait happens here. With one connection
    // the batch is always this one commit and the drain is one sync - the
    // batching only pays off once concurrent committers exist to fill it
    // (manager.hpp). kRelaxed waits for nothing by definition.
    if (durability_ == wal::DurabilityClass::kGroup && !wal_->IsDurable(commit.value())) {
        if (Status s = wal_->DrainOnce(); !s.ok()) return s;
        if (Status s = wal_->EnsureDurable(commit.value()); !s.ok()) return s;
    }
    return Status::OK();
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

    auto access = catalog_.InitTableAccess(oid.value());
    if (!access.ok()) {
        return {"ERR " + access.status().message(), false};
    }
    // Borrowed from the catalog's cache, not owned: valid for this
    // statement, including across AllocateRowId() (catalog.hpp).
    const catalog::TableAccess& ta = *access.value();

    // The primary key is the engine's to issue, never the caller's
    // (CLAUDE.md invariant 10), so VALUES supplies the columns *after* it.
    // Catching the old arity here gives a usable message instead of the
    // codec's "expected N value(s)".
    const std::size_t ncols = ta.schema.columns.size();
    if (ncols > 0 && stmt.values.size() == ncols) {
        return {"ERR do not supply a value for primary-key column '" +
                    std::string(catalog::NameView(ta.schema.columns.front().name)) +
                    "' - it is autoincrement and engine-assigned",
                false};
    }

    auto id = catalog_.AllocateRowId(oid.value());
    if (!id.ok()) {
        return {"ERR " + id.status().message(), false};
    }

    auto encoded = exec::EncodeRow(ta.schema, id.value(), stmt.values);
    if (!encoded.ok()) {
        return {"ERR " + encoded.status().message(), false};
    }

    // Into whichever storage the relation uses - a chain of heap pages or
    // a clustered B+ tree. Duplicate-key and min_key enforcement live in
    // there, not here: they are storage invariants, not dispatcher policy.
    const bool is_btree = ta.clustered_type == catalog::ClusteredType::kBtree;
    auto placed = InsertIntoRelation(ta, id.value(), encoded.value(),
                                     /*trx_id=*/catalog::kBootstrapXid);
    if (!placed.ok()) {
        if (logging(LogLevel::kWarn)) {
            log_->Warn(is_btree ? "btree" : "heap",
                       "insert into the relation rooted at page " +
                           std::to_string(ta.desc_page_id) +
                           " failed: " + placed.status().message());
        }
        return {"ERR " + placed.status().message(), false};
    }

    // Logged after the page is mutated and before the client is answered -
    // see the ordering note in this class's header for why that is safe
    // here and what would break it.
    if (Status s = LogInsert(placed.value(),
                             is_btree ? PageType::kBtreeLeaf : PageType::kHeap, encoded.value(),
                             /*trx_id=*/catalog::kBootstrapXid);
        !s.ok()) {
        if (logging(LogLevel::kError)) {
            log_->Error("wal", "logging the insert of id " + std::to_string(id.value()) +
                                   " failed: " + s.message());
        }
        return {"ERR " + s.message(), false};
    }

    // The tree grew a level, so the relation's root moved. Persisted only
    // now: the new root's contents are logged above, and a root published
    // before the pages under it are described is a root recovery cannot
    // follow. This invalidates the catalog cache, so `ta` is dangling from
    // here on - the rest of this function uses only `oid`, `id`, `placed`,
    // and the copy taken below.
    const catalog::TableAccess ta_after_relink = ta;
    if (placed.value().new_root != kInvalidPageId) {
        if (Status s = catalog_.UpdateRelationDescPage(oid.value(), placed.value().new_root);
            !s.ok()) {
            if (logging(LogLevel::kError)) {
                log_->Error("btree", "table oid " + std::to_string(oid.value()) +
                                         " grew a level but its root could not be repointed at "
                                         "page " +
                                         std::to_string(placed.value().new_root) + ": " +
                                         s.message());
            }
            return {"ERR " + s.message(), false};
        }
        if (logging(LogLevel::kInfo)) {
            log_->Info("btree", "table oid " + std::to_string(oid.value()) +
                                    " grew a level; root is now page " +
                                    std::to_string(placed.value().new_root));
        }
    }

    // Coverage: the tuple exists, so its entry must too (spec section 3).
    // Deliberately after LogInsert and deliberately not fatal - Waystone is
    // advisory, and an INSERT that landed and was logged must not be
    // reported as failed because a hint could not be recorded. The cost of
    // swallowing it is a probe miss and a fallback scan for this one id.
    //
    // Nothing below this point may touch `ta`: on a directory growth
    // RecordWaystoneInsert() writes the catalog, which clears the cache
    // and leaves the reference dangling. The rest of this function uses
    // only oid, id and placed.
    if (catalog::WaystoneActive(ta_after_relink.waystone_state)) {
        auto updated = RecordWaystoneInsert(ta_after_relink, id.value(), placed.value());
        if (!updated.ok() && logging(LogLevel::kWarn)) {
            log_->Warn("waystone", "could not record id " + std::to_string(id.value()) +
                                       " for table oid " + std::to_string(oid.value()) + ": " +
                                       updated.status().message());
        }
    }

    // A relation growing a page is rare and structural - the closest thing
    // this engine has to a file extending - so it is Debug, above the
    // per-tuple Trace line below.
    if (placed.value().restructured() && logging(LogLevel::kDebug)) {
        log_->Debug(is_btree ? "btree" : "heap",
                    "relation of table oid " + std::to_string(oid.value()) +
                        " grew: new tuple page " + std::to_string(placed.value().page_id) +
                        " min_key=" + std::to_string(id.value()) + " pages_logged=" +
                        std::to_string(placed.value().changes().size()));
    }

    // Trace: one line per inserted tuple. Logged from here rather than from
    // PageView, which is a bare view over page bytes with no business
    // owning a logger - and which the catalog also writes through, where a
    // "heap insert" line would describe a catalog row, not a user tuple.
    if (logging(LogLevel::kTrace)) {
        log_->Trace(is_btree ? "btree" : "heap", "insert page=" + std::to_string(placed.value().page_id) +
                                " slot=" + std::to_string(placed.value().slot) +
                                " id=" + std::to_string(id.value()) +
                                " bytes=" + std::to_string(encoded.value().size()));
    }

    // The page id is part of the reply because it is no longer implied by
    // the table: a client that wants to `SHOW PAGE` the row it just wrote
    // would otherwise have to walk the chain to guess where it went.
    return {"INSERTED oid=" + std::to_string(oid.value()) + " id=" + std::to_string(id.value()) +
                " page=" + std::to_string(placed.value().page_id) +
                " slot=" + std::to_string(placed.value().slot),
            false};
}

std::optional<std::uint64_t> CommandDispatcher::KeystoneIdAtSlot(storage::PageStore& store,
                                                                 PageId page_id,
                                                                 std::uint16_t slot, void*) {
    auto bytes = store.Get(page_id);
    if (!bytes.ok()) return std::nullopt;

    heap::PageView page(bytes.value());
    auto tuple = page.ReadTuple(slot);
    if (!tuple.ok()) return std::nullopt;  // retired, out of range, or dead
    if (tuple.value().payload.size() < kKeystoneWordSize) return std::nullopt;

    std::uint64_t word = 0;
    std::memcpy(&word, tuple.value().payload.data(), sizeof(word));
    return Keystone::Decode(word).id;
}

StatusOr<storage::InsertPlacement> CommandDispatcher::InsertIntoRelation(
    const catalog::TableAccess& access, std::uint64_t id, std::span<const std::byte> payload,
    std::uint64_t trx_id) {
    storage::InsertPlacement out;

    switch (access.clustered_type) {
        case catalog::ClusteredType::kHeap: {
            // The relation is a chain of heap pages (heap_chain.hpp): the
            // tuple goes into the tail, and a full tail grows the chain by
            // one page rather than failing. Duplicate-key and min_key
            // enforcement live in there - they are heap invariants, not
            // dispatcher policy.
            auto placed = heap::ChainInsert(page_store_, access.desc_page_id, id, payload, trx_id);
            if (!placed.ok()) return placed.status();

            out.page_id = placed.value().page_id;
            out.slot = placed.value().slot;
            if (placed.value().grew_chain) {
                // Chain growth in the shared vocabulary, in the order redo
                // applies it: the old tail's image (which already carries
                // the new link - ChainInsert sets it before returning),
                // then the page it points at, which the HEAP_INSERT fills.
                out.Record(placed.value().linked_from, /*is_new_page=*/false, 0);
                out.Record(placed.value().page_id, /*is_new_page=*/true, id);
            }
            return out;
        }
        case catalog::ClusteredType::kBtree: {
            // The relation is a clustered B+ tree (btree.hpp) rooted at the
            // same desc page: the descent picks the leaf, and a full leaf
            // splits right without moving a key. Reports its own structural
            // set, and a new root when the tree gained a level.
            auto placed = btree::BtreeInsert(page_store_, access.desc_page_id, id, payload, trx_id);
            if (!placed.ok()) return placed.status();

            out.page_id = placed.value().page_id;
            out.slot = placed.value().slot;
            return placed.value();
        }
    }
    return Status::Corruption("relation oid " + std::to_string(access.oid) +
                              " has an unknown clustered_type");
}

Status CommandDispatcher::VisitRelation(
    const catalog::TableAccess& access,
    const std::function<Status(PageId, heap::PageView&, std::uint16_t)>& fn) {
    switch (access.clustered_type) {
        case catalog::ClusteredType::kHeap:
            return heap::ChainVisit(page_store_, access.desc_page_id, fn);
        case catalog::ClusteredType::kBtree:
            return btree::BtreeVisit(page_store_, access.desc_page_id, fn);
    }
    return Status::Corruption("relation oid " + std::to_string(access.oid) +
                              " has an unknown clustered_type");
}

StatusOr<const catalog::TableAccess*> CommandDispatcher::RecordWaystoneInsert(
    const catalog::TableAccess& access, std::uint64_t id,
    const storage::InsertPlacement& placed) {
    const catalog::TableAccess* current = &access;
    stats::WaystoneRef ws = WaystoneRefOf(*current);

    Status s = stats::OnInsert(page_store_, ws, id, placed.page_id, placed.slot,
                               epochs_.EpochOf(placed.page_id));

    // OutOfRange is the directory saying "I am too shallow for this id" -
    // an expected event once per coverage boundary, not a failure. Grow,
    // persist the new root and depth, re-acquire, retry once.
    if (s.code() == StatusCode::kOutOfRange) {
        auto grown = stats::GrowDirectory(page_store_, ws.dir_root, ws.depth);
        if (!grown.ok()) return grown.status();

        const auto depth = static_cast<std::uint8_t>(ws.depth + 1);
        if (Status set = catalog_.SetWaystoneDirectory(current->oid, current->waystone_state,
                                                       grown.value(), depth);
            !set.ok()) {
            return set;
        }
        if (logging(LogLevel::kInfo)) {
            log_->Info("waystone", "table oid " + std::to_string(current->oid) +
                                       " directory deepened to " + std::to_string(depth) +
                                       " for id " + std::to_string(id));
        }

        // SetWaystoneDirectory bumped the catalog version, which cleared
        // the cache: `access` is dangling from here on. Re-acquired rather
        // than reused, and returned so the caller cannot keep the old one.
        auto refreshed = catalog_.InitTableAccess(current->oid);
        if (!refreshed.ok()) return refreshed.status();
        current = refreshed.value();

        s = stats::OnInsert(page_store_, WaystoneRefOf(*current), id, placed.page_id,
                            placed.slot, epochs_.EpochOf(placed.page_id));
    }
    if (!s.ok()) return s;
    return current;
}

std::optional<std::uint64_t> CommandDispatcher::PkEqualityTarget(
    const catalog::TableAccess& access, const std::vector<parser::Condition>& where) const {
    // Deliberately storage-agnostic: this answers "is this statement a bare
    // pk point lookup", which is a property of the WHERE clause alone.
    // Whether *anything* can shortcut it - a tree descent, a Waystone probe,
    // or neither - is LocateByPk's question.
    if (access.schema.columns.empty()) return std::nullopt;
    // Exactly one condition: an extra AND could exclude the row the probe
    // would find, and the probe cannot evaluate the second predicate.
    // Falling through costs a scan; getting this wrong costs a wrong answer.
    if (where.size() != 1) return std::nullopt;

    const parser::Condition& cond = where.front();
    if (cond.op != parser::CompareOp::kEq) return std::nullopt;
    if (cond.val.type != parser::ValueType::kInt) return std::nullopt;
    // Negative ids do not exist (invariant 6 zero-extends the 40-bit id),
    // so a negative literal is a guaranteed miss - and casting it to
    // uint64 would probe an enormous pk instead.
    if (cond.val.int_val < 0) return std::nullopt;
    if (!IEquals(cond.col_name, catalog::NameView(access.schema.columns.front().name))) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(cond.val.int_val);
}

CommandDispatcher::PkLookup CommandDispatcher::LocateByPk(const catalog::TableAccess& access,
                                                          std::uint64_t pk) {
    if (access.clustered_type == catalog::ClusteredType::kBtree) {
        // The tree *is* the relation's storage, so its answer is
        // authoritative in both directions: a hit is where the row lives,
        // and a NotFound means no such row - the scan it replaces would
        // visit the same leaf and find the same nothing. This is the one
        // place a point lookup may skip the scan on a miss, and it is
        // allowed precisely because it is not a hint (contrast invariant 8,
        // which governs Waystone and is untouched by this).
        auto found = btree::BtreeLookup(page_store_, access.desc_page_id, pk);
        if (found.ok()) {
            return PkLookup{PkLookup::Kind::kAt,
                            TupleLocation{found.value().page_id, found.value().slot}};
        }
        if (found.status().code() == StatusCode::kNotFound) {
            return PkLookup{PkLookup::Kind::kAbsent, {}};
        }
        // A corrupt descent or an unreadable page: fall back to the scan
        // rather than fail the query. The scan reaches the leaves through
        // the sibling links, so it can still answer when the index above
        // them cannot.
        if (logging(LogLevel::kWarn)) {
            log_->Warn("btree", "descent for pk " + std::to_string(pk) + " in table oid " +
                                    std::to_string(access.oid) +
                                    " failed, falling back to a scan: " +
                                    found.status().message());
        }
        return PkLookup{PkLookup::Kind::kScan, {}};
    }

    if (!catalog::WaystoneActive(access.waystone_state)) {
        return PkLookup{PkLookup::Kind::kScan, {}};
    }
    if (std::optional<TupleLocation> at = ProbeForPk(access, pk); at.has_value()) {
        return PkLookup{PkLookup::Kind::kAt, *at};
    }
    // A Waystone miss is never kAbsent: spec 3.1 rule 1 says a miss is not
    // an answer.
    return PkLookup{PkLookup::Kind::kScan, {}};
}

std::optional<TupleLocation> CommandDispatcher::ProbeForPk(const catalog::TableAccess& access,
                                                           std::uint64_t pk) {
    auto probed = stats::ProbeAndVerify(page_store_, WaystoneRefOf(access), epochs_, pk,
                                        &CommandDispatcher::KeystoneIdAtSlot, nullptr);
    // A Status here is a caller error (unusable directory, pk past the
    // depth) and is still only a reason to scan: the query has an answer
    // either way, and refusing to produce it because a hint structure
    // complained would be the advisory contract broken outright.
    if (!probed.ok() || !probed.value().trusted) {
        ++waystone_probe_misses_;
        return std::nullopt;
    }
    ++waystone_probe_hits_;
    return TupleLocation{probed.value().page_id, probed.value().slot};
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

    auto access = catalog_.InitTableAccess(oid.value());
    if (!access.ok()) {
        return {"ERR " + access.status().message(), false};
    }
    // Borrowed from the catalog's cache, not owned: valid for this
    // statement, including across AllocateRowId() (catalog.hpp).
    const catalog::TableAccess& ta = *access.value();

    // Same one-line-per-response contract as SHOW PAGE: a header line of
    // column names, then one "\n"-escaped section per matching row
    // (comma-joined values), never a raw newline byte.
    std::ostringstream os;
    bool first_col = true;
    for (const auto& col : ta.schema.columns) {
        if (!first_col) os << ',';
        os << catalog::NameView(col.name);
        first_col = false;
    }

    // Emits one row if it matches the WHERE. Shared by the probe path and
    // the scan so there is exactly one formatter and the two cannot drift
    // into producing different bytes for the same tuple - which is the
    // whole equivalence property the advisory contract rests on.
    auto emit = [&](std::span<const std::byte> payload) -> Status {
        auto row = exec::DecodeRow(ta.schema, payload);
        if (!row.ok()) return row.status();
        if (!exec::MatchesWhere(ta.schema, row.value(), stmt.where)) return Status::OK();

        os << "\\n";
        bool first_val = true;
        for (const auto& v : row.value()) {
            if (!first_val) os << ',';
            os << exec::FormatValue(v);
            first_val = false;
        }
        return Status::OK();
    };

    // The point-lookup fast path, taken only for a WHERE that is exactly
    // one equality against the pk column - anything else, including an
    // extra AND, falls straight through to the scan below. Which shortcut
    // is available, and whether its "no" is trustworthy, is LocateByPk's
    // call; see PkLookup for why the two differ.
    if (std::optional<std::uint64_t> pk = PkEqualityTarget(ta, stmt.where); pk.has_value()) {
        const PkLookup found = LocateByPk(ta, *pk);
        if (found.kind == PkLookup::Kind::kAbsent) {
            return {os.str(), false};  // header line only: no such row
        }
        if (found.kind == PkLookup::Kind::kAt) {
            auto bytes = page_store_.Get(found.at.page_id);
            if (bytes.ok()) {
                heap::PageView page(bytes.value());
                auto tuple = page.ReadTuple(found.at.slot);
                // The locator already confirmed the Keystone id at this
                // slot, so a read failure here means the page changed
                // underneath us. Fall through rather than fail: the scan is
                // still correct.
                if (tuple.ok()) {
                    if (Status s = emit(tuple.value().payload); !s.ok()) {
                        return {"ERR " + s.message(), false};
                    }
                    if (logging(LogLevel::kTrace)) {
                        log_->Trace("query", "pk " + std::to_string(*pk) + " served from " +
                                                 std::to_string(found.at.page_id) + ":" +
                                                 std::to_string(found.at.slot));
                    }
                    return {os.str(), false};
                }
            }
        }
    }

    // Full scan, left to right - which is id order page by page for either
    // storage, so rows come back roughly sorted by pk without anything
    // sorting them. Still a scan: no min_key-based pruning of pages the
    // WHERE cannot match, and no use of the tree's separators to start
    // partway in. Both are range-scan work the point path did not need.
    Status scan = VisitRelation(
        ta, [&](PageId, heap::PageView& page, std::uint16_t slot) -> Status {
            auto tuple = page.ReadTuple(slot);
            if (!tuple.ok()) return Status::OK();  // dead or out-of-range slot - skip
            return emit(tuple.value().payload);
        });
    if (!scan.ok()) {
        return {"ERR " + scan.message(), false};
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

    auto access = catalog_.InitTableAccess(oid.value());
    if (!access.ok()) {
        return {"ERR " + access.status().message(), false};
    }
    // Borrowed from the catalog's cache, not owned: valid for this
    // statement, including across AllocateRowId() (catalog.hpp).
    const catalog::TableAccess& ta = *access.value();

    // Validate every SET target names a real column before touching
    // storage, so a bad column name fails clean with no partial update.
    const std::string pk_name =
        ta.schema.columns.empty()
            ? std::string()
            : std::string(catalog::NameView(ta.schema.columns.front().name));
    for (const auto& assignment : stmt.assignments) {
        if (ta.schema.FindColumn(assignment.col_name) == nullptr) {
            return {"ERR unknown column '" + assignment.col_name + "'", false};
        }
        // The pk is the tuple's identity, not a field of it: Waystone and
        // any future hint index address a tuple by this id, so changing it
        // in place would silently retarget every reference to the row.
        if (assignment.col_name == pk_name) {
            return {"ERR primary-key column '" + pk_name + "' cannot be updated", false};
        }
    }

    std::uint32_t updated = 0;
    std::uint32_t pages_touched = 0;
    PageId last_page = kInvalidPageId;

    // Applies the SET list to one slot if it matches the WHERE. Shared by
    // the probe path and the scan, for the same reason SELECT shares its
    // formatter: two copies of "how a row is updated" is two chances for
    // the probed path and the scanned path to do different things to the
    // same tuple.
    auto apply = [&](PageId page_id, heap::PageView& page, std::uint16_t slot) -> Status {
        auto tuple = page.ReadTuple(slot);
        if (!tuple.ok()) return Status::OK();  // dead or out-of-range slot - skip

        auto row = exec::DecodeRow(ta.schema, tuple.value().payload);
        if (!row.ok()) return row.status();

        if (!exec::MatchesWhere(ta.schema, row.value(), stmt.where)) {
            return Status::OK();
        }

        for (const auto& assignment : stmt.assignments) {
            for (std::size_t c = 0; c < ta.schema.columns.size(); ++c) {
                if (catalog::NameView(ta.schema.columns[c].name) == assignment.col_name) {
                    row.value()[c] = assignment.val;
                    break;
                }
            }
        }

        // The pk is unchanged by construction (rejected above), so it is
        // carried straight from the tuple's own Keystone word rather than
        // round-tripped through the decoded row.
        auto id = exec::RowKeystoneId(tuple.value().payload);
        if (!id.ok()) return id.status();
        const std::vector<parser::AstValue> body(row.value().begin() + 1, row.value().end());

        auto encoded = exec::EncodeRow(ta.schema, id.value(), body);
        if (!encoded.ok()) return encoded.status();

        // HOT-style in-place overwrite - see PageView::OverwriteTuple's
        // comment. No fallback to retire+reinsert if the new payload no
        // longer fits the slot's original capacity (e.g. a grown varchar);
        // that fails with OutOfSpace here, surfaced as ERR.
        //
        // Note this keeps the row on its own page even across a chain: an
        // update never moves a tuple, so no page's min_key can be
        // invalidated by one - and no Waystone entry needs re-observing,
        // which is why the probe path below records nothing.
        if (Status s = page.OverwriteTuple(slot, encoded.value(), tuple.value().trx_id,
                                            tuple.value().undo_ptr);
            !s.ok()) {
            return s;
        }
        ++updated;
        if (page_id != last_page) {
            last_page = page_id;
            ++pages_touched;
        }
        return Status::OK();
    };

    // Same fast path as SELECT, and the same contract: a point UPDATE by pk
    // is the other statement shape whose cost is otherwise linear in the
    // relation. Any reason to decline falls through to the scan, which
    // produces the identical result - the locator picks the slot to look
    // at, never which rows match.
    if (std::optional<std::uint64_t> pk = PkEqualityTarget(ta, stmt.where); pk.has_value()) {
        const PkLookup found = LocateByPk(ta, *pk);
        if (found.kind == PkLookup::Kind::kAbsent) {
            return {"UPDATED 0", false};  // no such row, on the tree's authority
        }
        if (found.kind == PkLookup::Kind::kAt) {
            auto bytes = page_store_.Get(found.at.page_id);
            if (bytes.ok()) {
                heap::PageView page(bytes.value());
                if (Status s = apply(found.at.page_id, page, found.at.slot); !s.ok()) {
                    return {"ERR " + s.message(), false};
                }
                if (logging(LogLevel::kTrace)) {
                    log_->Trace("query", "pk " + std::to_string(*pk) + " updated at " +
                                             std::to_string(found.at.page_id) + ":" +
                                             std::to_string(found.at.slot));
                }
                return {"UPDATED " + std::to_string(updated), false};
            }
        }
    }

    Status scan = VisitRelation(
        ta, [&](PageId page_id, heap::PageView& page, std::uint16_t slot) -> Status {
            return apply(page_id, page, slot);
        });
    if (!scan.ok()) {
        // Partial by design: rows updated before the failure stay updated.
        // There is no transaction to roll back into yet - the same
        // exposure the single-page version had, now spread over a chain.
        return {"ERR " + scan.message(), false};
    }

    if (updated > 0 && logging(LogLevel::kTrace)) {
        log_->Trace("heap", "overwrite rows=" + std::to_string(updated) + " across " +
                                std::to_string(pages_touched) + " page(s) of table oid " +
                                std::to_string(oid.value()));
    }
    return {"UPDATED " + std::to_string(updated), false};
}

}  // namespace kds::server
