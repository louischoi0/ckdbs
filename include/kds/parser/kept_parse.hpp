#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/parser/ast.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/parser/lexer.hpp"

// **A prepared statement's parse, kept and bound by value** (EA-S1,
// `instructions/v3.2.0/workorder-ea-prepared-statement-reuse.md` §2).
//
// `KeptParse::Of` parses a statement's text once with each `?` a typed hole
// (`Parser::AdmitHoles`): a `kParam` value carrying the marker's byte and its
// ordinal. `Bind` takes one value per hole, already in the form the
// substituted literal would have lexed to (`HoleValue`), and gives a
// `BoundStatement`, whose `Materialize()` is the statement's AST with every
// hole filled - **what `Parse()` of the substituted text answers, value for
// value and type for type** (EA-R1 `[quiet-wrong]`), with each bound value's
// byte the marker's, the byte of the text the client sent (EA-R3).
//
// **Kept only where the two can be shown to agree**, and otherwise not kept,
// so the statement runs its substituted text exactly as before:
//
// - a `SELECT`, `INSERT`, `UPDATE` or `DELETE` the native grammar reads with
//   its holes (the query grammar's forms and every other statement are not
//   kept);
// - every hole reached by the bind's walk (`ast.hpp`'s value positions, a
//   predicate subquery's included): a `?` in a `RETURNING` item, a SET value's
//   scalar subquery or a query-grammar node is not, and the parse is not
//   kept, since those are read again from text, whose `?` has no value;
// - every `?` delimited in its text, so its substituted literal could not
//   have fused with a neighbouring byte into another token: no `-`, `.`,
//   quote, digit, letter, `_`, `$` or `?` against it (`x-?` bound with 5 is
//   `x-5`, one signed literal, and a fingerprint of other tokens).
//
// A bind carrying a NULL is not served either (`kwp_session.cpp`): a `NULL`
// literal is shape where a value is argument (`fingerprint.hpp`), so its
// substituted text has another `pattern_id`, and the text path is the one
// that answers it.
//
// **The fingerprint is the substituted text's** (EA-R2): `pattern_id` and
// `fetch_id` are the held text's, since a `?` and a literal are one shape
// marker, and `arg_hash` is folded at the bind from the argument stream with
// each hole's literal in place (`ArgumentHashOf`); `param_count` is 0 and
// `literal_count` every value's, as the substituted text's.
//
// **Nothing here holds catalog state**: names are resolved at every compile
// under the relation `IS` the bind declares, so a kept parse cannot go stale
// (§2.4). No lock or atomic: a `KeptParse` is immutable once built and a
// `BoundStatement` once bound; both are shared by `shared_ptr`.

namespace kds::parser {

// One bound value, as the literal `C_BIND` would have substituted for it
// lexes: an integer (`kInt`, its digits as written in `raw_int_text`), a
// numeric with a point (`kStr` holding its spelling, `bare_numeric`), or a
// string (`kStr`, the value without quotes - a quote included, which no
// literal can spell).
struct HoleValue {
    AstValue value;
    bool bare_numeric = false;
};

// The value of one literal as spelled - `-5`, `12.50`, `'2026-01-27'` - read
// by the lexer and taken as `ParseValue` takes its token. InvalidArgument for
// anything that is not one integer, numeric or string literal (NULL is never
// a hole's value). A string value whose bytes hold a quote has no spelling:
// it is `HoleValueOfString`'s.
StatusOr<HoleValue> HoleValueOfLiteral(std::string_view literal);

// The value of a string literal holding `value`, quotes and all.
HoleValue HoleValueOfString(std::string value);

struct BoundStatement;

class KeptParse {
public:
    // A text longer than this keeps no parse (EA-R6): a kept parse's memory
    // goes with its text, so the session's 64 statements bound it.
    static constexpr std::size_t kMaxText = 64 * 1024;

    // The statement's kept parse, or nullopt where it keeps none (above).
    static std::optional<KeptParse> Of(std::string_view sql);

    std::uint32_t holes() const noexcept { return holes_; }

private:
    friend struct BoundStatement;
    friend StatusOr<std::shared_ptr<const BoundStatement>> Bind(
        std::shared_ptr<const KeptParse> kept, std::vector<HoleValue> values, std::string text);

    // The walk that fills the holes of a copy of `statement_`; with no
    // values it fills nothing. Whether every hole was reached.
    bool Fill(Statement& stmt, const std::vector<HoleValue>* values) const;

    Statement statement_;
    std::uint32_t holes_ = 0;
    Fingerprint fingerprint_;
    // `ArgumentsOf` of the text, owned: inline literals as written, holes
    // `kUnbound`, in token order.
    std::vector<std::pair<StatementArgument::Kind, std::string>> arguments_;
};

struct BoundStatement {
    // Made only by `Bind`, in place, and never copied or moved: `arguments`
    // views `argument_texts`.
    struct Key {};
    explicit BoundStatement(Key) {}
    BoundStatement(const BoundStatement&) = delete;
    BoundStatement& operator=(const BoundStatement&) = delete;

    // The text the dispatch is handed and the dispatcher matches against
    // (`CommandDispatcher::KeptFor`): the substituted text, a value with no
    // spelling left as its `?` - which a form that reads the text again then
    // refuses by name (`Parser::ParseValue`).
    std::string text;
    // The substituted text's fingerprint (above).
    Fingerprint fingerprint;
    // The substituted text's arguments, holes in place: what a managed plan's
    // `ARG(k)` reads (EA-S2). `arguments` views `argument_texts`.
    std::vector<std::string> argument_texts;
    std::vector<StatementArgument> arguments;

    // The AST, holes filled. A copy per call, as a parse is one per call: the
    // dispatcher owns and edits what it is given (`AbsorbViews`).
    Statement Materialize() const;

    std::shared_ptr<const KeptParse> kept;
    std::vector<HoleValue> values;
};

// `kept` bound with one value per hole, in marker order; `text` is what the
// statement's dispatch is handed (`BoundStatement::text`). InvalidArgument
// when the count disagrees.
StatusOr<std::shared_ptr<const BoundStatement>> Bind(std::shared_ptr<const KeptParse> kept,
                                                     std::vector<HoleValue> values,
                                                     std::string text);

}  // namespace kds::parser
