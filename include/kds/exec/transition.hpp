#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/transition_catalog.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/read_view.hpp"

namespace kds::wal {
class WalManager;
}

// **Transition**: a column's allowed state changes as a row-local,
// stateless constraint (`instructions/v3.2.0/workorder-bv-transition-
// constraints.md`, `docs/spec/transition.md`).
//
// A Transition is its `sys.transitions` row and nothing else (BV-R1): no
// registry, no derived structure, no WAL record kind. Every check reads the
// row being written and `TableAccess::transitions`, which the catalog's
// loader fills by re-parsing the stored declaration. The path property -
// "FINALIZED only after CHECKED" - follows by induction over committed
// versions from the INSERT and UPDATE checks together; it is never checked
// on its own and no history is stored.
//
// **Every check refuses before anything of its row is written** (BV-R3):
// INSERT before the tuple key is drawn, UPDATE and DELETE after the row's
// conflict check (the old value is the head the borrow holds) and before
// the assertion reservation, the undo record and the no-refuse window. A
// refusal is `kTransitionViolation`, and like every write failure it fails
// the transaction.

namespace kds::exec {

// A value's state key for `column` (`catalog::TransitionRef`), or nullopt
// when the value is not one the column can hold - NULL, or a kind the column
// does not take. Nullopt is never a pass: it is returned only where the
// write's own encode refuses the value (a NULL into the NOT NULL state
// column, a string into an integer column), so the check defers to that
// refusal rather than naming a second one for the same value.
std::optional<std::string> StateKey(const catalog::SysColumnRow& column,
                                    const parser::AstValue& value);

// The column types a Transition may govern (CN-4 T5): the signed integers,
// `char(N)` and `varchar(N)`.
bool IsTransitionColumnType(std::uint32_t type_val) noexcept;

// The parsed rule of `stmt` against `access`'s schema: every state checked
// to be one the column can hold. `InvalidArgument` naming the offending
// literal's byte otherwise.
StatusOr<catalog::TransitionRef> CompileTransition(const catalog::TableAccess& access,
                                                   const parser::TransitionStmt& stmt,
                                                   std::uint64_t id, std::uint16_t column_no);

// The declaration loader's Transition half (`Catalog::SetDeclarationLoader`): every
// Transition on `access`'s relation, re-parsed from its stored text against
// the governed column the row names by number, so `ALTER TABLE RENAME`
// moves nothing. A stored declaration that no longer parses or compiles is
// `Corruption` - the row was written only after both succeeded.
Status LoadTransitions(catalog::Catalog& catalog, storage::PageStore& store,
                       catalog::TableAccess& access);

// ---- The three checks ---------------------------------------------------

// INSERT: `body` is the row without its tuple key (schema position `c` at
// `body[c - 1]`); each governed column's value must be in the initial set.
// `id` names the row in the refusal, 0 when the key is not yet drawn.
Status CheckTransitionInsert(const catalog::TableAccess& access,
                             std::span<const parser::AstValue> body, std::uint64_t id);

// UPDATE of a governed column: `before -> after` must be a declared edge or
// a self-loop (CN-4 T2: an unchanged value is no transition).
Status CheckTransitionUpdate(const catalog::TableAccess& access,
                             const catalog::TransitionRef& transition,
                             const parser::AstValue& before, const parser::AstValue& after,
                             std::uint64_t id);

// DELETE: `row` by schema position; each governed column's value must be in
// its deletable set when `ON DELETE FROM` was declared.
Status CheckTransitionDelete(const catalog::TableAccess& access,
                             std::span<const parser::AstValue> row, std::uint64_t id);

// ---- DDL -----------------------------------------------------------------

// The statement's relation, or a refusal with its byte. `view` is the
// visibility the name resolves under (null: every relation).
StatusOr<catalog::Oid> TransitionTargetOid(catalog::Catalog& catalog,
                                           const parser::TransitionStmt& stmt,
                                           const txn::ReadView* view = nullptr);

struct TransitionDdlResult {
    std::uint64_t transition_id = 0;
    std::size_t rows_checked = 0;
};

// `CREATE TRANSITION`: CN-4 §4's create-time refusals, then the build scan
// under `check_view` (every live row's state must be in the state set; a
// row an in-flight transaction wrote refuses `TxnConflict`), then the row,
// then the schema word. The caller holds the relation `X` (BV-R4).
StatusOr<TransitionDdlResult> CreateTransition(catalog::Catalog& catalog,
                                               storage::PageStore& store,
                                               const parser::TransitionStmt& stmt,
                                               catalog::Oid target_oid,
                                               const txn::ReadView& check_view,
                                               wal::WalManager* wal);

// `DROP TRANSITION`: the row retired, then the schema word. Returns what
// was dropped. The caller holds the relation `X`.
StatusOr<TransitionDef> DropTransition(catalog::Catalog& catalog, storage::PageStore& store,
                                       const parser::TransitionStmt& stmt, wal::WalManager* wal);

}  // namespace kds::exec
