#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/exec/chain_frame.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/parser/ast.hpp"
#include "kds/wire/row_codec.hpp"

// **Where a result row becomes bytes** - the one seam between the engine's
// values and whatever a client is going to read them as
// (`docs/spec/protocol.md` §6, `docs/inflight/in-progress/protocol-wp.md`
// P08).
//
// ---- Why this type exists, and what it is not ----------------------------
//
// Until KWP the dispatcher had exactly one output form - the newline
// protocol's comma-joined text - and it built it inline, with a comment at
// each site warning that a second formatter would drift ("a sorted reply
// rendering a DATE as an epoch day because one of two copies forgot
// `projection_types`", `RenderProjectedRow`). KWP is a second output form
// and would have been that second copy four times over: the local walk, the
// sorted drain, the aggregate fold and the cross-core fan-in.
//
// So it is not a copy - it is the same four call sites, calling a sink. The
// text form is `TextResultSink` below, is installed by default, and does
// **exactly** what the inline code did, byte for byte; that is the property
// the whole existing suite checks, since every one of its expected replies
// is one of these strings.
//
// ---- The shape, and why encode and emit are two calls --------------------
//
// A sorted statement encodes rows during the walk and emits them after
// (`exec::OutputSort` buffers an opaque payload per row, ordered by keys it
// normalised separately). So encoding and emitting cannot be one call, and
// splitting them is not an accommodation - it is what lets a top-N sort
// *skip* encoding for a row that is already beaten, which is worth 94-104
// ns a row (`bench/results-order-by.md`) and is the reason `OutputSort`
// has `Admit` and `Take` rather than one method.
//
// ---- Two encoders, because there are two row shapes ----------------------
//
// A projected row is read out of a `ChainFrame` through `ColumnRef`s; a
// fold's output row arrives as a span of values. Neither can be expressed
// as the other without copying every value of every row - an `AstValue`
// owns a `std::string` - so the seam carries both rather than paying that
// per row to have one method.

namespace kds::server {

class ResultSink {
public:
    virtual ~ResultSink() = default;

    // Declares the result's shape, once, before any row. A statement that
    // produces no result set never calls it, and **that is how a caller
    // tells a result set from a completion**: `KwpSession` frames typed
    // rows exactly when this was called, and reads the dispatcher's text
    // reply otherwise.
    virtual Status Describe(std::vector<wire::FieldDescription> fields) = 0;

    // Encodes one projected row into `out`, which is cleared. `types` is
    // positional with `projection`; a shorter `types` means the caller had
    // none for the tail, which the text form renders untyped and the wire
    // form refuses - a wire value with no type is not renderable at all.
    virtual Status EncodeProjectedRow(std::span<const exec::ColumnRef> projection,
                                      std::span<const std::uint32_t> types,
                                      const exec::ChainFrame& frame, std::string& out) = 0;

    // The same for a row that arrives as values - a fold's output, and a
    // row decoded off the wire by the fan-in.
    virtual Status EncodeValueRow(std::span<const std::uint32_t> types,
                                  std::span<const parser::AstValue> values,
                                  std::string& out) = 0;

    // Emits a row an `Encode*` call produced. Taken by value-ish view: the
    // caller's scratch buffer is reused for the next row, so a sink that
    // keeps the bytes must copy them.
    virtual Status Emit(std::string_view row) = 0;
};

// The newline protocol's form: a header line of comma-joined column names,
// then one `"\n"`-escaped section per row (`docs/spec/client-manual.md`
// §2). Accumulates the reply; the caller takes it at the end.
//
// **This is the pre-existing rendering, moved and not rewritten.** Every
// expected reply in the test suite is one of these strings, so any
// difference is a failure somewhere, which is the point of moving it whole.
class TextResultSink final : public ResultSink {
public:
    // **A `std::string`, not a `std::ostringstream`.** Every write this
    // sink makes is a plain append - a comma, a name, a row - and nothing
    // formats. A stream would cost a stringbuf and a locale per statement
    // for that, which is the same cost this file's own aggregate path
    // measured as most of a fold's per-statement overhead (AP03), and the
    // KWP path would pay it for a stream it never writes to.
    Status Describe(std::vector<wire::FieldDescription> fields) override {
        bool first = true;
        for (const wire::FieldDescription& f : fields) {
            if (!first) out_ += ',';
            out_ += f.name;
            first = false;
        }
        return Status::OK();
    }

    Status EncodeProjectedRow(std::span<const exec::ColumnRef> projection,
                              std::span<const std::uint32_t> types,
                              const exec::ChainFrame& frame, std::string& out) override {
        out.clear();
        for (std::size_t i = 0; i < projection.size(); ++i) {
            if (i != 0) out += ',';
            out += exec::FormatValue(i < types.size() ? types[i] : 0, frame.Get(projection[i]));
        }
        return Status::OK();
    }

    Status EncodeValueRow(std::span<const std::uint32_t> types,
                          std::span<const parser::AstValue> values, std::string& out) override {
        out.clear();
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) out += ',';
            out += exec::FormatValue(i < types.size() ? types[i] : 0, values[i]);
        }
        return Status::OK();
    }

    Status Emit(std::string_view row) override {
        out_ += "\\n";
        out_ += row;
        return Status::OK();
    }

    // The reply, moved out. Left empty, so a caller that takes it twice
    // gets an empty string rather than a duplicate.
    std::string Take() { return std::move(out_); }

private:
    std::string out_;
};

}  // namespace kds::server
