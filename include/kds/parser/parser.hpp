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

private:
    StatusOr<CreateTableStmt> ParseCreateTable();
    StatusOr<InsertStmt> ParseInsert();
    StatusOr<UpdateStmt> ParseUpdate();
    StatusOr<DeleteStmt> ParseDelete();

    // `CREATE PATTERN <name> (<params>) [WITH (...)] OF <select>`. The
    // leading `CREATE PATTERN` is already consumed.
    StatusOr<CreatePatternStmt> ParseCreatePattern();
    StatusOr<DropPatternStmt> ParseDropPattern();

    // `{CREATE | DROP} CABIN ON <table>(<column>)`, with the leading two
    // words already consumed. One production for both, since they differ
    // only in `drop` - see CabinStmt (ast.hpp).
    StatusOr<CabinStmt> ParseCabin(bool drop);
    StatusOr<IndexStmt> ParseIndex(bool drop);

    // `ALTER TABLE <t> RENAME TO <new> | RENAME COLUMN <old> TO <new>`
    // (docs/spec/alter.md AL7), with `ALTER` already consumed. Every other
    // form under ALTER is refused here, by name and position (AL1).
    StatusOr<AlterStmt> ParseAlter();

    // `{CREATE | DROP} ASSERTION ...` (docs/spec/assertion.md §3), with the
    // leading two words already consumed. One production for both, for
    // ParseCabin's reason - see AssertionStmt (ast.hpp).
    StatusOr<AssertionStmt> ParseAssertion(bool drop);

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

    // The two bracketed lists of a declaration, split out only because
    // ParseCreatePattern is otherwise three loops in a row.
    Status ParsePatternParams(CreatePatternStmt& stmt);
    Status ParsePatternOptions(CreatePatternStmt& stmt);

    // `depth` is how many query blocks enclose this one: 0 at the top
    // level, +1 per predicate-position subquery. Carried as a parameter
    // rather than as parser state because it must unwind exactly with the
    // recursion, and a member would have to be restored by hand on every
    // error path (V07).
    StatusOr<SelectStmt> ParseSelect(std::uint32_t depth);

    // `( SELECT ... )` in predicate position. Consumes both parens and
    // enforces kMaxSubqueryDepth.
    StatusOr<std::shared_ptr<SelectStmt>> ParseSubquery(std::uint32_t depth);

    // FROM-list productions (V05). ParseRelationRef takes `<name> [AS
    // <alias>]`; ParseJoins takes zero or more `JOIN <rel> ON <q> = <q>`
    // and appends them in written order.
    StatusOr<RelationRef> ParseRelationRef();
    Status ParseJoins(SelectStmt& stmt);
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
    StatusOr<Condition> ParseOneCondition(std::uint32_t depth);
    StatusOr<std::vector<Condition>> ParseOptionalWhere(std::uint32_t depth);

    StatusOr<std::string> ParseIdent();

    // One argument of a parameterized type - `decimal(10, 2)`'s 10 and 2.
    // `what` names it in the error, so a client is told which of the two
    // was wrong rather than that "a type argument" was.
    StatusOr<std::uint32_t> ParseTypeArgument(std::string_view what);
    Status ExpectKeyword(std::string_view keyword);
    Status ExpectToken(TokenType type, std::string_view desc);
    void ConsumeOptionalSemicolon();

    // The statement text as handed in, kept so a declaration's body can be
    // sliced out of it verbatim (ast.hpp explains why verbatim matters).
    // Non-owning: a Parser does not outlive the string it was given.
    std::string_view sql_;

    Lexer lexer_;

    // Where a `$param` is legal, and where its occurrences are recorded.
    //
    // Non-null for exactly the span of a CREATE PATTERN body and null
    // everywhere else, which is how spec section 3.1 is enforced: the token
    // exists and fingerprints, but no other production accepts it. A
    // pointer rather than a bool because the two facts are one - a `$x` is
    // legal precisely where there is a declaration to record it against -
    // and splitting them is how they come to disagree.
    std::vector<ParamUse>* param_uses_ = nullptr;
};

// Convenience free function: Parser(sql).Parse().
StatusOr<Statement> Parse(std::string_view sql);

}  // namespace kds::parser
