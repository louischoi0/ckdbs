#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "kds/base/status.hpp"
#include "kds/parser/ast.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/parser/lexer.hpp"

// Recursive-descent parser for the KDS SQL subset; ast.hpp documents the
// grammar. Every Parse*() method follows one contract: return or propagate
// a non-ok Status on the first syntax error and stop. There is no error
// recovery and no backtracking - once a production fails, the parse fails.
//
// rules.md #1: `throw` is forbidden, so every fallible step here returns
// Status/StatusOr rather than throwing - this is pure, syscall-free
// engine logic (no clock reads, no I/O), so unlike main.cpp/tcp_server.cpp
// there is nothing here that needs an injectable platform interface.

namespace kds::parser {

// ---- SUS-1's test seam, and it is only that ------------------------------
//
// Heap relations are **suspended**, not removed
// (`instructions/v3.0.0/workorder-as-sus1-heap-suspended.md`): no new one is
// created, so `CREATE TABLE ... HEAP` is refused `Unsupported` and `BTREE` is
// what an unqualified statement means. Everything below the parser is
// untouched - an existing heap relation mounts, serves and walks its chain.
//
// The refusal lives in the parser, so the bypass has to as well: the cells
// whose subject *is* heap storage keep exercising those paths through this
// switch (AS-R5). Three things it deliberately is not - a config key,
// anything reachable from KWP or the debug text port, and not `NDEBUG`-gated,
// because a release test build needs it and a debug-only seam would make the
// gate and the nightly run different engines.
//
// The raw setter is used exactly once, by `tests/heap_suspension_env.cpp`,
// which needs a permanent arm rather than a scope. Everything else uses the
// guard below.
void SetHeapStorageAllowedForTest(bool allowed) noexcept;
bool HeapStorageAllowedForTest() noexcept;

// The test binary runs with the suspension **lifted**, so a cell that means
// to observe the refusal has to put it back. Restores what it found, so
// nothing leaks into the next cell.
//
// There is deliberately no `AllowHeapStorageForTest` counterpart; if both
// directions are ever wanted, one `HeapStorageForTest(bool)` replaces this.
class SuspendHeapStorageForTest {
public:
    SuspendHeapStorageForTest() noexcept : saved_(HeapStorageAllowedForTest()) {
        SetHeapStorageAllowedForTest(false);
    }
    ~SuspendHeapStorageForTest() { SetHeapStorageAllowedForTest(saved_); }
    SuspendHeapStorageForTest(const SuspendHeapStorageForTest&) = delete;
    SuspendHeapStorageForTest& operator=(const SuspendHeapStorageForTest&) = delete;

private:
    bool saved_;
};

class Parser {
public:
    explicit Parser(std::string_view sql) noexcept : sql_(sql), lexer_(sql) {}

    // Parses exactly one statement from the input given at construction.
    // Fails with InvalidArgument (message describes the syntax error) if
    // the input is empty, uses an unsupported keyword, or is otherwise
    // malformed - including trailing garbage after an otherwise-valid
    // statement: only EOF may follow one.
    StatusOr<Statement> Parse();

    // The statement's fingerprint, computed **during** the parse rather than
    // by a second pass over the text (lexer.hpp).
    //
    // **Valid exactly when the lexer reached end of input**, which a
    // successful `Parse()` always does - it checks that only EOF follows a
    // complete statement - and a failed one sometimes does, when it failed
    // *because* the input ran out. Both of those are honest: the accumulator
    // saw the whole token stream, so its hash is the whole statement's.
    //
    // A parse that stopped in the middle - trailing garbage, an unexpected
    // token with more text behind it - saw a prefix, and a hash of a prefix
    // is not a prefix of a hash. That answers nullopt rather than something
    // plausible. A caller that needs a fingerprint for text like that wants
    // `FingerprintOf()`, which lexes it independently to the end.
    //
    // nullopt is also the ordinary answer for a statement that simply has
    // no pattern - `CREATE TABLE`, `SHOW`, anything whose leading word is
    // not SELECT/INSERT/UPDATE - which is not an error and never was.
    std::optional<Fingerprint> fingerprint() const noexcept { return lexer_.fingerprint(); }

    // **The query grammar** (BU-S4, `workorder-bu-basic-query-surface.md`
    // BU-Q8, BU-Q9): one SELECT, or set operations over several, with value
    // expressions wherever a value stands, `DISTINCT`, every join kind,
    // derived tables, `GROUP BY`/`HAVING` over expressions and `ORDER BY` an
    // expression, alias or ordinal. What the native grammar refuses and this
    // one reads runs on the query layer (`exec/query_layer.hpp`); the native
    // `Parse()` is always tried first and is unchanged by this entry.
    StatusOr<Query> ParseQuery();

    // A WHERE clause's body alone, to the end of the text, as the native
    // grammar reads it: the query layer asks this of one top-level term so a
    // relation's walk can carry it (BU-Q8). The text is the statement's,
    // blanked outside the term, so every byte reported is the statement's.
    StatusOr<std::vector<Condition>> ParseConditions();

private:
    StatusOr<CreateTableStmt> ParseCreateTable();
    StatusOr<ColumnDef> ParseColumnDef();
    StatusOr<InsertStmt> ParseInsert();
    StatusOr<UpdateStmt> ParseUpdate();
    StatusOr<DeleteStmt> ParseDelete();
    StatusOr<PurgeStmt> ParsePurge();

    // `{CREATE | DROP} CABIN ON <table>(<column>)`, with the leading two
    // words already consumed. One production for both, since they differ
    // only in `drop` - see CabinStmt (ast.hpp).
    StatusOr<CabinStmt> ParseCabin(bool drop);
    StatusOr<IndexStmt> ParseIndex(bool drop);

    // `{CREATE | DROP} NAMESPACE <name>` (AF-T3, AF-6's shape (a);
    // `docs/spec/namespace.md` NS7), with the leading two words already
    // consumed. One production for both, for ParseCabin's reason.
    StatusOr<NamespaceStmt> ParseNamespace(bool drop);

    // `ALTER TABLE <t> RENAME TO <new> | RENAME COLUMN <old> TO <new>`
    // (docs/spec/alter.md AL7), with `ALTER` already consumed. Every other
    // form under ALTER is refused here, by name and position (AL1).
    StatusOr<AlterStmt> ParseAlter();

    // `{CREATE | DROP} ASSERTION ...` (docs/spec/assertion.md §3), with the
    // leading two words already consumed. One production for both, for
    // ParseCabin's reason - see AssertionStmt (ast.hpp).
    StatusOr<AssertionStmt> ParseAssertion(bool drop);
    // `CREATE`/`DROP TRANSITION` (BV-S2): TransitionStmt in ast.hpp.
    StatusOr<TransitionStmt> ParseTransition(bool drop);
    // One `(<state>, ...)` list of a Transition clause; `what` names the
    // clause in a refusal.
    Status ParseTransitionStates(std::vector<AstValue>& out, std::string_view what);

    // A parenthesised comma-separated column list, as both halves of a
    // CREATE INDEX declaration and an assertion's GROUP BY list are. `what`
    // names the list in every error.
    //
    // `cap == 0` means **no cap**, which is what an assertion's GROUP BY list
    // takes: `assertion.md` §3 declares no ceiling on it, and inventing
    // one here would settle a number nothing has measured. An index's lists
    // pass their own `[PROPOSED]` caps, which are refusals and never
    // truncations (docs/spec/index.md §13).
    Status ParseDeclaredColumnList(std::vector<IndexColumnRef>& out, const char* what,
                                   std::size_t cap);

    // `depth` is how many query blocks enclose this one: 0 at the top
    // level, +1 per predicate-position subquery. Carried as a parameter
    // rather than as parser state because it must unwind exactly with the
    // recursion, and a member would have to be restored by hand on every
    // error path (V07).
    StatusOr<SelectStmt> ParseSelect(std::uint32_t depth);

    // `( SELECT ... )` in predicate position. Consumes both parens and
    // enforces kMaxSubqueryDepth.
    StatusOr<std::shared_ptr<SelectStmt>> ParseSubquery(std::uint32_t depth);
    // The rest of a subquery whose `(` is already consumed and whose `SELECT` is
    // next, parsed as a block at `select_depth`. `close_at`, when given, gets the
    // byte of the `)` that ended it.
    StatusOr<std::shared_ptr<SelectStmt>> ParseSubqueryAfterOpen(std::uint32_t select_depth,
                                                                 std::uint32_t* close_at = nullptr);

    // FROM-list productions (V05). ParseRelationRef takes `<name> [AS
    // <alias>]`; ParseJoins takes zero or more `JOIN <rel> ON <q> = <q>`
    // and appends them in written order.
    StatusOr<RelationRef> ParseRelationRef();
    Status ParseJoins(SelectStmt& stmt);

    // `<name>` or `<ns>.<name>` - the one production every relation name in
    // the grammar goes through since AF-T3 (ast.hpp's namespace-qualifier
    // rule). `schema` is left empty when the name was written unqualified.
    //
    // `offset`, when given, receives the byte of what was written **first**
    // - the qualifier if there is one - so a refusal about a namespace
    // points at the namespace rather than at the name after the dot.
    //
    // Which namespaces exist is the catalog's question and not this
    // production's, exactly as `RelationRef::schema` has always said: an
    // unknown one fails at resolution, where the message can name what does
    // exist.
    Status ParseQualifiedName(std::string& schema, std::string& name,
                              std::uint32_t* offset = nullptr);
    Status CheckDistinctBindings(const SelectStmt& stmt) const;

    // `x` or `a.x`. ParseQualifiedColumn is the ON-clause form: the same
    // production, refusing the unqualified spelling.
    StatusOr<ColumnName> ParseColumnName();
    StatusOr<ColumnName> ParseQualifiedColumn();

    // `*`, or a comma-separated list of items. Sets `star` for `*` and
    // otherwise appends one `SelectItem` per entry, aggregated or not
    // (V06, AG01).
    //
    // **Items land in a caller-owned vector, not in the statement.** Where
    // they finally go depends on whether a GROUP BY follows and whether any
    // item is an aggregate - neither of which is known here - so the parse
    // stages them and `ParseSelect` decides between `projection` and
    // `agg_items` once both are in hand.
    Status ParseSelectList(std::vector<SelectItem>& items, bool& star);

    // One entry of a select list: a column, or `<agg>( [DISTINCT] col | * )`.
    StatusOr<SelectItem> ParseSelectItem();

    // `GROUP BY <col> [, ...]`, with the two words already consumed.
    Status ParseGroupBy(SelectStmt& stmt);

    // `HAVING <agg> <op> <val> [AND ...]`, with the word already consumed
    // (docs/inflight/in-progress/workplan-having.md HV-1). Shape only: that the left side is an
    // aggregate the fold can answer, and that the right side is a literal,
    // are this production's; whether the aggregate typechecks and whether a
    // plain column is a grouping key are the compiler's.
    Status ParseHaving(SelectStmt& stmt);

    // The pagination tail of a query block: `[ORDER BY <key> [ASC]]
    // [LIMIT <n>] [OFFSET <m>]` (spec I11, workplan V09, amended by
    // docs/inflight/in-progress/workplan-having.md HV4). Every refusal lives here with the
    // production - subquery position, an ordinal, an expression, an
    // aggregate key on a statement with no fold to answer it, `LIMIT` over
    // a fold - so there is one answer to what the tail admits.
    Status ParsePaginationTail(SelectStmt& stmt, bool aggregated, std::uint32_t depth);

    // One count clause of the tail - `LIMIT <n>` or `OFFSET <m>` -
    // absent-is-ok, refused over aggregated output and inside a subquery
    // with the clause's own byte. One production for both, since they
    // differ only in the word and where the count lands.
    Status ParseCountClause(std::string_view word, bool aggregated, std::uint32_t depth,
                            std::optional<std::uint64_t>& out);

    // The count in a LIMIT or OFFSET clause: a non-negative integer
    // literal, decoded from its digits so a value past int64 refuses
    // rather than wraps (token.hpp's digits() note). `clause` names the
    // clause in every error.
    StatusOr<std::uint64_t> ParsePaginationCount(std::string_view clause);

    StatusOr<AstValue> ParseValue();
    StatusOr<CompareOp> ParseCompareOp();
    // One condition as a leaf - or, for a value list or a negated form
    // (BU-S2), the subtree it means.
    StatusOr<BoolExpr> ParseOneCondition(std::uint32_t depth);

    // AP-S4's call, its head already read as `head`: `(`, column
    // arguments, `)`. Whether `head` names a function at all is the
    // compiler's to answer (exec/functions.hpp).
    StatusOr<FunctionCall> ParseFunctionCall(ColumnName head);
    // Whether a name just read, with `(` next, is a call this grammar
    // admits in a predicate: unqualified, and not an aggregate's name -
    // which keeps the refusal it has always had there.
    bool StartsFunctionCall(const ColumnName& head);
    // The rest of a comparison whose left side is a call.
    StatusOr<Condition> ParseFunctionComparison(Condition cond);
    // `IS [NOT] NULL`, if it is next: true and `op` set, false if not.
    StatusOr<bool> ParseIsNull(CompareOp& op);
    // A comparison's right side, after its operator: a column, a call (which
    // makes the condition a kCompareFunction), or a value.
    Status ParseComparisonRhs(Condition& cond);
    StatusOr<std::vector<Condition>> ParseOptionalWhere(std::uint32_t depth);

    // A WHERE's boolean structure (BU-S1): `OR` binds loosest, then `AND`
    // (one chain production, `is_or` saying which), then `NOT`; a
    // parenthesized group or one condition is the primary.
    StatusOr<BoolExpr> ParseBoolChain(std::uint32_t depth, bool is_or);
    StatusOr<BoolExpr> ParseBoolNot(std::uint32_t depth);
    // `IN (v, ...)` after its `(` (BU-S2): a disjunction of equalities, or
    // for `NOT IN` a conjunction of inequalities.
    StatusOr<BoolExpr> ParseValueList(ColumnName col, bool negated, std::uint32_t at);

    StatusOr<std::string> ParseIdent();

    // ---- The query grammar (BU-S4) ----------------------------------------
    // `UNION`/`EXCEPT` over `INTERSECT` terms, then the shared tail.
    StatusOr<std::shared_ptr<const Query>> ParseQueryExpr();
    StatusOr<std::shared_ptr<const Query>> ParseQueryTerm();
    StatusOr<std::shared_ptr<const Query>> ParseQueryPrimary();
    StatusOr<std::shared_ptr<const QueryBlock>> ParseQueryBlock();
    Status ParseQueryTail(Query& q);
    StatusOr<FromItem> ParseFromItem();
    Status ParseQueryJoins(QueryBlock& block);
    // A nested query in parentheses whose `(` at `open` is consumed: the
    // query, and its `)`.
    StatusOr<std::shared_ptr<const Query>> ParseNestedQuery(const Token& open);
    // The rest of a primary that began with the identifier `tok`, already
    // consumed: a column, a call, a CASE.
    StatusOr<std::shared_ptr<const Expr>> ParseExprAfterIdent(const Token& tok);
    // `(SELECT ...)` as a value, its `(` at `open` and already consumed; `op`
    // is the form, `lhs` an `IN`'s operand.
    StatusOr<std::shared_ptr<const Expr>> ParseExprQuerySubquery(const Token& open, ExprOp op,
                                                                 std::shared_ptr<const Expr> lhs);
    // `COUNT(*)`, `SUM([DISTINCT] expr)`, ... with the name and `(` consumed.
    StatusOr<std::shared_ptr<const Expr>> ParseExprAggregate(const Token& name, AggFunc func);
    // What may follow a value at comparison level in the query grammar:
    // `[NOT] IN`, `[NOT] BETWEEN`, `[NOT] LIKE`, or nothing.
    StatusOr<std::shared_ptr<const Expr>> ParseExprQueryComparison(std::shared_ptr<const Expr> lhs);

    // A SET value (BJ-S2, BJ-S5): one bare literal into `a.val`, anything
    // else into `a.expr`. The levels below are precedence climbing in
    // PostgreSQL's order, loosest first: OR, AND, NOT, IS, the comparisons,
    // `||`, `+ -`, `* / %`, a unary sign, then `::`.
    Status ParseSetValue(Assignment& a);
    Status ParseInsertTail(InsertStmt& stmt);
    Status ParseOutputItems(std::vector<OutputItem>& items);
    Status ParseReturning(Returning& out);
    StatusOr<RelationRef> ParseOtherRelation(const std::string& target);
    Status ParseRowValueAssignments(std::vector<Assignment>& out);
    StatusOr<std::shared_ptr<const Expr>> ParseExprOr();
    StatusOr<std::shared_ptr<const Expr>> ParseExprAnd();
    StatusOr<std::shared_ptr<const Expr>> ParseExprNot();
    StatusOr<std::shared_ptr<const Expr>> ParseExprIs();
    StatusOr<std::shared_ptr<const Expr>> ParseExprComparison();
    StatusOr<std::shared_ptr<const Expr>> ParseExprConcat();
    StatusOr<std::shared_ptr<const Expr>> ParseExprAdditive();
    StatusOr<std::shared_ptr<const Expr>> ParseExprMultiplicative(
        std::shared_ptr<const Expr> first);
    StatusOr<std::shared_ptr<const Expr>> ParseExprUnary();
    StatusOr<std::shared_ptr<const Expr>> ParseExprPostfix();
    StatusOr<std::shared_ptr<const Expr>> ParseExprPrimary();
    StatusOr<std::shared_ptr<const Expr>> ParseExprCall(const Token& name);
    StatusOr<std::shared_ptr<const Expr>> ParseExprCase(const Token& kw);
    // `name ( [expr, ...] )` for a scalar function (BJ-S6); `POSITION(a IN b)`
    // and `MOD(a, b)` are the two spellings that are not that shape.
    StatusOr<std::shared_ptr<const Expr>> ParseExprFunction(const Token& name);
    // A type name with its optional `(width)` or `(precision, scale)`.
    Status ParseTypeSpec(TypeSpec& out);
    // Any `::type` after `operand`.
    StatusOr<std::shared_ptr<const Expr>> ParseCastSuffix(std::shared_ptr<const Expr> operand);

    // One argument of a parameterized type - `decimal(10, 2)`'s 10 and 2.
    // `what` names it in the error, so a client is told which of the two
    // was wrong rather than that "a type argument" was.
    StatusOr<std::uint32_t> ParseTypeArgument(std::string_view what);
    Status ExpectKeyword(std::string_view keyword);
    Status ExpectToken(TokenType type, std::string_view desc);
    void ConsumeOptionalSemicolon();

    // The statement text as handed in, kept so text can be sliced out of it
    // verbatim: an assertion's `source_text` and a SET value subquery's
    // `subquery_sql`. Non-owning: a Parser does not outlive its string.
    std::string_view sql_;

    Lexer lexer_;

    // Nesting of parentheses and unary signs in a SET value, bounded so a
    // hostile statement cannot recurse the stack away.
    std::uint32_t expr_depth_ = 0;
    // The parentheses and `NOT`s around the WHERE node being parsed, across
    // the whole statement - every subquery's WHERE included (BU-Q2).
    std::uint32_t bool_nest_ = 0;
    // Set while a SELECT that is a value inside another statement is parsed:
    // a `;` is that statement's end, not a way to end the subquery early.
    bool inside_value_subquery_ = false;
    // A literal a SET value began with, already read, for the levels above the
    // primary to carry on from (`ParseSetValue`): when an operator follows it,
    // the expression chain is entered with the literal as its first primary.
    std::shared_ptr<const Expr> pending_primary_;
    // Set by `ParseQuery`: the expression levels read the query grammar's
    // forms (qualified columns, aggregates, `IN`, `LIKE`, subqueries).
    bool query_mode_ = false;
    // The query blocks open around the one being parsed, bounded by
    // `kMaxSubqueryDepth`.
    std::uint32_t query_depth_ = 0;

};

// Convenience free function: Parser(sql).Parse().
StatusOr<Statement> Parse(std::string_view sql);

}  // namespace kds::parser
