#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/captured_plans.hpp"
#include "kds/parser/ast.hpp"
#include "kds/txn/read_view.hpp"

// **Absorption** (instructions/v3.2.0/workorder-dp-view.md §2, DP-S2): a
// view the block's `FROM` names is replaced by its body - its relations at
// the view's position, its conjuncts appended to the block's, its columns
// read through the view's binding - so the block that leaves here is the
// statement's expansion written out by hand, and everything after it (the
// bind, equality propagation, the ladder, the inner build, every plan
// source) runs on it as written.
//
// **A class (a) compile-time item** (DP-R2, `parser-v2.md` I12's CX-S2
// inventory): it reads the statement's text and the catalog's DDL state -
// the view rows a reader sees - and nothing else. It takes no
// `PlanOptions`, no statistic and no memo, and it absorbs a body whole or
// not at all: a view whose body is not absorbable (DP-Q1) is refused
// `NotImplemented`, which a `SELECT` answers through the query layer as a
// derived table with nothing pushed in (DP-Q2, DP-R6).
//
// **A body's names are its own** (DP-Q23): every name and relation of a
// spliced body carries a fresh `lens`, and the compiler resolves a name only
// against relations and views of its own lens. The reader sees a view's
// columns, by the view's binding or bare, and never its base relations; the
// body sees its own relations and nothing of the reader's.
//
// Each block absorbs its own `FROM`: a body's subquery blocks are absorbed
// when the compiler reaches them, under the lens counter the statement
// shares (`next_lens`, the highest lens issued so far).

namespace kds::exec {

// DP-Q8: how deep views may nest, a view's body reading a view counting
// one. Measured at DP-S4 (DP-R11, `bench/v3.5.0/results-dp-close-v3.5.0-84-g78a32c4f.md`):
// about 3 us a level, a point read through eight 2.1x its hand-written cost.
inline constexpr std::uint32_t kMaxViewDepth = 8;

// The block with every view its `FROM` names absorbed, or nullopt when it
// names none - always nullopt while no view has existed (DP-Q18). `view` is
// the reader's (DT3): a view whose create it cannot see is not there.
// `default_namespace` is what an unqualified name of the statement's own
// text means (NS5). The block returned is marked `views_settled`.
StatusOr<std::optional<parser::SelectStmt>> AbsorbViews(catalog::Catalog& catalog,
                                                        const parser::SelectStmt& stmt,
                                                        const txn::ReadView* view,
                                                        catalog::Oid default_namespace,
                                                        std::uint32_t& next_lens);

// What a view was created over, as a body's relations carry it
// (`parser::ViewReads`): each relation and view it reads, once.
std::shared_ptr<const parser::ViewReads> ReadsOf(const catalog::ViewDef& def);

// A plan key's views (DP-R4, DP-Q17): every view the block absorbed, in
// declaration order. False - and the key unchanged - past what a key holds.
bool FillKeyViews(const parser::SelectStmt& block, CapturedKey& key);

// DP-Q22 (b): `oid`, what `rel`'s name resolved to, against what its view
// was created over (`RelationRef::reads`) - InvalidArgument at the name's
// byte for any other object, OK for a name of the statement's own text.
Status CheckViewReads(const parser::RelationRef& rel, catalog::Oid oid);

// A one-relation block a view absorbed to (DP-Q10): its WHERE with every
// conjunct's column read through the views to the relation's own column, so
// a reader of names alone (`FoldTupleKeyWindow`) reads the relation's
// columns and never a view's column of the same name. A name no view maps
// is emptied, which names no column.
std::vector<parser::Condition> WhereOnRelation(const parser::SelectStmt& absorbed);

}  // namespace kds::exec
