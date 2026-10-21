#include "kds/exec/transition.hpp"

#include <algorithm>
#include <utility>
#include <variant>
#include <vector>

#include "kds/exec/row_codec.hpp"
#include "kds/parser/parser.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/visit.hpp"
#include "kds/txn/visibility.hpp"

namespace kds::exec {

namespace {

bool IsIntegerType(std::uint32_t type_val) noexcept {
    return type_val == catalog::kTypeValInt8 || type_val == catalog::kTypeValInt16 ||
           type_val == catalog::kTypeValInt32 || type_val == catalog::kTypeValInt64;
}

// A state key as a reader would want to see it: an integer bare, a string
// quoted.
std::string Shown(const catalog::SysColumnRow& column, const std::string& key) {
    return IsIntegerType(column.type_val) ? key : "'" + key + "'";
}

bool Contains(const std::vector<std::string>& set, const std::string& key) {
    return std::find(set.begin(), set.end(), key) != set.end();
}

void AddUnique(std::vector<std::string>& set, std::string key) {
    if (!Contains(set, key)) set.push_back(std::move(key));
}

std::string ByteSuffix(const parser::AstValue& value) {
    return value.byte_offset != 0 ? " (byte " + std::to_string(value.byte_offset) + ")" : "";
}

// Every state the declaration names (CN-4 T4: the set is closed).
std::vector<std::string> StateSet(const catalog::TransitionRef& t) {
    std::vector<std::string> states = t.initial;
    for (const auto& [from, to] : t.edges) {
        AddUnique(states, from);
        AddUnique(states, to);
    }
    for (const std::string& s : t.deletable) AddUnique(states, s);
    return states;
}

Status Violation(const catalog::TableAccess& access, const catalog::TransitionRef& t,
                 std::uint64_t id, const std::string& what, const parser::AstValue* at) {
    const catalog::SysColumnRow& column = access.schema.columns[t.column_no];
    std::string msg = "transition '" + t.name + "' on column '" +
                      std::string(catalog::NameView(column.name)) + "': " + what;
    if (id != 0) msg += ", row id " + std::to_string(id);
    if (at != nullptr) msg += ByteSuffix(*at);
    return Status::TransitionViolation(std::move(msg));
}

}  // namespace

std::size_t StateCount(const catalog::TransitionRef& t) { return StateSet(t).size(); }

bool IsTransitionColumnType(std::uint32_t type_val) noexcept {
    return IsIntegerType(type_val) || type_val == catalog::kTypeValChar ||
           type_val == catalog::kTypeValVarchar;
}

std::optional<std::string> StateKey(const catalog::SysColumnRow& column,
                                    const parser::AstValue& value) {
    if (IsIntegerType(column.type_val)) {
        // An integer cell's encode takes an integer and nothing else
        // (`row_codec.cpp`), so a string is the encode's refusal.
        if (value.type == parser::ValueType::kInt) return std::to_string(value.int_val);
        return std::nullopt;
    }
    if (column.type_val == catalog::kTypeValChar || column.type_val == catalog::kTypeValVarchar) {
        // Byte-wise over the unpadded value (`types.md` §2b): a decoded
        // `char(N)` reads back to its first NUL, so the key is the value as
        // written.
        if (value.type == parser::ValueType::kStr) return value.str_val;
        return std::nullopt;
    }
    return std::nullopt;
}

StatusOr<catalog::TransitionRef> CompileTransition(const catalog::TableAccess& access,
                                                   const parser::TransitionStmt& stmt,
                                                   std::uint64_t id, std::uint16_t column_no) {
    if (column_no >= access.schema.columns.size()) {
        return Status::Corruption("transition '" + stmt.name + "' names column " +
                                  std::to_string(column_no) + " of a relation with " +
                                  std::to_string(access.schema.columns.size()));
    }
    const catalog::SysColumnRow& column = access.schema.columns[column_no];
    const std::string at = " (byte " + std::to_string(stmt.column_byte_offset) + ")";
    if (column_no == 0) {
        return Status::InvalidArgument("a Transition cannot govern the tuple key '" +
                                       stmt.column + "': it is never updated" + at);
    }
    if (!IsTransitionColumnType(column.type_val)) {
        return Status::InvalidArgument("column '" + stmt.column +
                                       "' cannot be a state column: a Transition governs an "
                                       "integer, char(N) or varchar(N) column" +
                                       at);
    }
    if (!column.notnull) {
        // CN-4 T3: NULL is not a state. `NOT NULL` is the default, so a
        // nullable column was declared one on purpose.
        return Status::InvalidArgument("column '" + stmt.column +
                                       "' is nullable; a Transition governs a NOT NULL "
                                       "column" +
                                       at);
    }

    const auto key_of = [&](const parser::AstValue& v) -> StatusOr<std::string> {
        auto key = StateKey(column, v);
        if (!key.has_value()) {
            return Status::InvalidArgument("a state of column '" + stmt.column + "' must be " +
                                           (IsIntegerType(column.type_val) ? "an integer"
                                                                           : "a string") +
                                           " literal" + ByteSuffix(v));
        }
        // A state the column cannot hold is one no row can be in: a literal
        // the lexer wrapped past 64 bits (its `int_val` is not its value),
        // or one outside a narrow integer's width.
        if (IsIntegerType(column.type_val) &&
            (parser::IntLiteralWrapped(v) || !FitsSigned(v.int_val, static_cast<int>(column.len)))) {
            // `OutOfRange`, the code an INSERT of the same literal gets.
            return Status::OutOfRange("state " + (v.raw_int_text.empty() ? *key : v.raw_int_text) +
                                           " does not fit column '" + stmt.column + "'" +
                                           ByteSuffix(v));
        }
        if (column.type_val == catalog::kTypeValChar && key->size() > column.len) {
            return Status::InvalidArgument("state '" + *key + "' is longer than char(" +
                                           std::to_string(column.len) + ")" + ByteSuffix(v));
        }
        return std::move(*key);
    };

    catalog::TransitionRef t;
    t.id = id;
    t.name = stmt.name;
    t.column_no = column_no;
    for (const parser::AstValue& v : stmt.initial) {
        auto key = key_of(v);
        if (!key.ok()) return key.status();
        AddUnique(t.initial, std::move(key.value()));
    }
    for (const parser::TransitionStmt::Edge& e : stmt.edges) {
        auto from = key_of(e.from);
        if (!from.ok()) return from.status();
        auto to = key_of(e.to);
        if (!to.ok()) return to.status();
        std::pair<std::string, std::string> edge{std::move(from.value()), std::move(to.value())};
        if (std::find(t.edges.begin(), t.edges.end(), edge) == t.edges.end()) {
            t.edges.push_back(std::move(edge));
        }
    }
    t.delete_declared = stmt.delete_declared;
    for (const parser::AstValue& v : stmt.deletable) {
        auto key = key_of(v);
        if (!key.ok()) return key.status();
        AddUnique(t.deletable, std::move(key.value()));
    }

    // Every state must be reachable from an initial one (CN-4 §4): a state
    // no walk reaches is one no row can ever hold, which is a declaration
    // that says something it does not mean.
    std::vector<std::string> reached = t.initial;
    for (std::size_t i = 0; i < reached.size(); ++i) {
        for (const auto& [from, to] : t.edges) {
            if (from == reached[i]) AddUnique(reached, to);
        }
    }
    const auto unreachable = [&](const parser::AstValue& v) -> std::optional<Status> {
        auto key = StateKey(column, v);
        if (key.has_value() && !Contains(reached, *key)) {
            return Status::InvalidArgument("state " + Shown(column, *key) +
                                           " is reachable from no ON INSERT state" +
                                           ByteSuffix(v));
        }
        return std::nullopt;
    };
    for (const parser::TransitionStmt::Edge& e : stmt.edges) {
        if (auto s = unreachable(e.from); s.has_value()) return *s;
    }
    for (const parser::AstValue& v : stmt.deletable) {
        if (auto s = unreachable(v); s.has_value()) return *s;
    }
    return t;
}

Status LoadTransitions(catalog::Catalog& catalog, storage::PageStore& store,
                       catalog::TableAccess& access) {
    auto defs = TransitionsOnRelation(catalog, store, access.oid);
    if (!defs.ok()) return defs.status();
    for (const TransitionDef& def : defs.value()) {
        auto parsed = parser::Parse(def.source_text);
        const auto* stmt =
            parsed.ok() ? std::get_if<parser::TransitionStmt>(&parsed.value()) : nullptr;
        if (stmt == nullptr) {
            return Status::Corruption("stored declaration of transition '" + def.name +
                                      "' no longer parses");
        }
        auto compiled = CompileTransition(access, *stmt, def.id, def.column_no);
        if (!compiled.ok()) {
            return Status::Corruption("stored declaration of transition '" + def.name +
                                      "' no longer compiles: " + compiled.status().message());
        }
        access.transitions.push_back(std::move(compiled.value()));
    }
    return Status::OK();
}

Status CheckTransitionInsert(const catalog::TableAccess& access,
                             std::span<const parser::AstValue> body, std::uint64_t id) {
    for (const catalog::TransitionRef& t : access.transitions) {
        const parser::AstValue& value = body[t.column_no - 1];
        const catalog::SysColumnRow& column = access.schema.columns[t.column_no];
        auto key = StateKey(column, value);
        if (!key.has_value()) continue;  // the encode refuses it (StateKey)
        if (!Contains(t.initial, *key)) {
            return Violation(access, t, id,
                             "a row may not be inserted in state " + Shown(column, *key), &value);
        }
    }
    return Status::OK();
}

Status CheckTransitionUpdate(const catalog::TableAccess& access,
                             const catalog::TransitionRef& transition,
                             const parser::AstValue& before, const parser::AstValue& after,
                             std::uint64_t id) {
    const catalog::SysColumnRow& column = access.schema.columns[transition.column_no];
    auto to = StateKey(column, after);
    if (!to.has_value()) return Status::OK();  // the encode refuses it (StateKey)
    auto from = StateKey(column, before);
    if (!from.has_value()) {
        // The committed value is NOT NULL and of the column's type, so this
        // is a decode the check cannot read - never a pass.
        return Status::Corruption("transition '" + transition.name +
                                  "': the row's current state could not be read, row id " +
                                  std::to_string(id));
    }
    if (*from == *to) return Status::OK();  // CN-4 T2: no transition
    for (const auto& edge : transition.edges) {
        if (edge.first == *from && edge.second == *to) return Status::OK();
    }
    return Violation(access, transition, id,
                     Shown(column, *from) + " => " + Shown(column, *to) +
                         " is not a declared edge",
                     &after);
}

Status CheckTransitionDelete(const catalog::TableAccess& access,
                             std::span<const parser::AstValue> row, std::uint64_t id) {
    for (const catalog::TransitionRef& t : access.transitions) {
        if (!t.delete_declared) continue;
        const catalog::SysColumnRow& column = access.schema.columns[t.column_no];
        auto key = StateKey(column, row[t.column_no]);
        if (!key.has_value()) {
            return Status::Corruption("transition '" + t.name +
                                      "': the row's state could not be read, row id " +
                                      std::to_string(id));
        }
        if (!Contains(t.deletable, *key)) {
            return Violation(access, t, id,
                             "a row may not be deleted from state " + Shown(column, *key),
                             nullptr);
        }
    }
    return Status::OK();
}

StatusOr<catalog::Oid> TransitionTargetOid(catalog::Catalog& catalog,
                                           const parser::TransitionStmt& stmt,
                                           catalog::Oid default_namespace,
                                           const txn::ReadView* view) {
    // A system relation never resolves (`Catalog::ResolveRelation` refuses
    // `sys`), so none can carry a Transition.
    auto oid = catalog.ResolveRelation(stmt.schema, stmt.table_name, default_namespace,
                                       stmt.table_byte_offset, view);
    if (!oid.ok()) return oid.status();
    return oid.value();
}

namespace {

// One leaf of the build scan: every live row's state must be in the state
// set (CN-4 §4). History is never verified - a row already in a late state
// is accepted; the graph speaks only for values committed while it stands.
// Returns the leaf after it.
StatusOr<PageId> CheckLeaf(storage::PageStore& store, const catalog::TableAccess& access,
                           const catalog::TransitionRef& t, const std::vector<std::string>& states,
                           const parser::TransitionStmt& stmt, const txn::ReadView& check_view,
                           txn::HolderProgress::Scope* progress, std::size_t& checked,
                           PageId leaf) {
    const catalog::SysColumnRow& column = access.schema.columns[t.column_no];
    // Per leaf, and per row below: what keeps this build's waiters inside
    // the fault net, however wide a leaf's rows make it.
    if (progress != nullptr) progress->Advance();
    // Copied out under the page's span, decoded after it (I15's R1): a
    // spilled state value fetches its var-heap page only once the leaf
    // is let go.
    std::vector<std::vector<std::byte>> staged;
    PageId next = kInvalidPageId;
    {
        auto bytes = store.GetForRead(leaf);
        if (!bytes.ok()) return bytes.status();
        heap::PageView page(bytes.value().bytes());
        for (std::uint16_t i = 0; i < page.slot_count(); ++i) {
            auto tuple = page.ReadTuple(i);
            if (tuple.status().code() == StatusCode::kNotFound) continue;  // retired
            if (!tuple.ok()) return tuple.status();
            switch (txn::CheckVisibility(check_view, tuple.value().trx_id,
                                         tuple.value().deleted)) {
                case txn::CheckVerdict::kBusy:
                    // A row an open transaction wrote: it may commit in
                    // a state the scan never saw. Refused, retryably,
                    // the assertion build's shape.
                    return Status::TxnConflict(
                        "relation '" + stmt.table_name +
                        "' has a row written by an in-flight transaction; CREATE "
                        "TRANSITION reads settled state - retry when it has ended");
                case txn::CheckVerdict::kAbsent:
                    continue;
                case txn::CheckVerdict::kLive:
                    break;
            }
            staged.emplace_back(tuple.value().payload.begin(), tuple.value().payload.end());
        }
        next = page.next_page_id();
    }
    for (const std::vector<std::byte>& payload : staged) {
        if (progress != nullptr) progress->Advance();
        std::vector<PendingSpill> spills;
        auto decoded = DecodeRow(access.schema, access.layout, payload, &spills);
        if (!decoded.ok()) return decoded.status();
        if (Status s = ResolveSpills(store, spills, decoded.value()); !s.ok()) return s;
        auto key = StateKey(column, decoded.value()[t.column_no]);
        if (!key.has_value() || !Contains(states, *key)) {
            return Violation(access, t, static_cast<std::uint64_t>(decoded.value()[0].int_val),
                             "an existing row is in state " +
                                 (key.has_value() ? Shown(column, *key)
                                                  : std::string("<unreadable>")) +
                                 ", which the declaration does not name",
                             nullptr);
        }
        ++checked;
    }
    return next;
}

}  // namespace

TransitionBuild::TransitionBuild(catalog::Catalog& catalog, storage::PageStore& store,
                                 parser::TransitionStmt stmt, catalog::Oid target_oid,
                                 wal::WalManager* wal, txn::HolderProgress::Scope* progress)
    : catalog_(catalog),
      store_(store),
      stmt_(std::move(stmt)),
      target_oid_(target_oid),
      wal_(wal),
      progress_(progress) {}

StatusOr<std::unique_ptr<TransitionBuild>> TransitionBuild::ForCreate(
    catalog::Catalog& catalog, storage::PageStore& store, const parser::TransitionStmt& stmt,
    catalog::Oid target_oid, wal::WalManager* wal, txn::HolderProgress::Scope* progress) {
    auto access = catalog.InitTableAccess(target_oid);
    if (!access.ok()) return access.status();
    const catalog::TableAccess& rel = *access.value();

    auto found = rel.schema.FindColumn(stmt.column, stmt.column_byte_offset);
    if (!found.ok()) return found.status();
    const catalog::SysColumnRow* column = found.value();
    if (column == nullptr) {
        return Status::NotFound("no column '" + stmt.column + "' in relation '" +
                                stmt.table_name + "' (byte " +
                                std::to_string(stmt.column_byte_offset) + ")");
    }
    const auto col = static_cast<std::uint16_t>(column->pos);
    // CN-4 T6: one Transition per column - several would be an
    // intersection nobody declared.
    if (const catalog::TransitionRef* held = rel.TransitionOn(col); held != nullptr) {
        return Status::AlreadyExists("column '" + stmt.column + "' already carries transition '" +
                                     held->name + "' (byte " +
                                     std::to_string(stmt.column_byte_offset) + ")");
    }
    std::unique_ptr<TransitionBuild> build(
        new TransitionBuild(catalog, store, stmt, target_oid, wal, progress));
    if (Status s = build->NameFree(); !s.ok()) return s;

    auto compiled = CompileTransition(rel, stmt, 0, col);
    if (!compiled.ok()) return compiled.status();
    build->access_ = &rel;
    build->column_no_ = col;
    build->compiled_ = std::move(compiled.value());
    build->states_ = StateSet(build->compiled_);
    if (Status s = build->cursor_.Start(store, rel.desc_page_id); !s.ok()) return s;
    return build;
}

Status TransitionBuild::NameFree() const {
    auto taken = FindTransitionByName(catalog_, store_, stmt_.name);
    if (!taken.ok()) return taken.status();
    if (taken.value().has_value()) {
        return Status::AlreadyExists("transition \"" + stmt_.name + "\" already exists (byte " +
                                     std::to_string(stmt_.byte_offset) + ")");
    }
    return Status::OK();
}

Status TransitionBuild::Step(std::uint32_t leaves, const txn::ReadView& check_view) {
    return cursor_.Step(leaves, [&](PageId leaf) {
        return CheckLeaf(store_, *access_, compiled_, states_, stmt_, check_view, progress_,
                         checked_, leaf);
    });
}

StatusOr<TransitionDdlResult> TransitionBuild::Finish(const Status& ran) {
    if (!ran.ok()) return ran;
    // **The name asked again**: one another relation's create took while the
    // build yielded is refused with the head's message and byte.
    if (Status s = NameFree(); !s.ok()) return s;
    auto id = catalog_.AllocateRowId(catalog::kSysTransitionsTable);
    if (!id.ok()) return id.status();
    TransitionDef def;
    def.id = id.value();
    def.target_oid = target_oid_;
    def.column_no = column_no_;
    def.name = stmt_.name;
    def.source_text = stmt_.source_text;
    if (Status s = InsertTransition(catalog_, store_, wal_, def); !s.ok()) return s;
    // `access_` is not read past this line: the bump drops the entry it
    // points into.
    catalog_.NoteDeclarationsChanged("sys.transitions create");
    return TransitionDdlResult{def.id, checked_};
}

StatusOr<TransitionDef> DropTransition(catalog::Catalog& catalog, storage::PageStore& store,
                                       const parser::TransitionStmt& stmt, wal::WalManager* wal) {
    auto dropped = DeleteTransition(catalog, store, wal, stmt.name);
    if (!dropped.ok()) {
        if (dropped.status().code() == StatusCode::kNotFound) {
            return Status::NotFound("no transition named \"" + stmt.name + "\" (byte " +
                                    std::to_string(stmt.byte_offset) + ")");
        }
        return dropped.status();
    }
    catalog.NoteDeclarationsChanged("sys.transitions drop");
    return dropped;
}

}  // namespace kds::exec
