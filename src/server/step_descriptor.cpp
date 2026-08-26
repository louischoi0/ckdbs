#include "kds/server/step_descriptor.hpp"

#include <cstring>
#include <string>

#include "kds/exec/plan_printer.hpp"

namespace kds::server {

namespace {

// ---- Little-endian append/read helpers ----------------------------------

void PutU8(std::vector<std::byte>& out, std::uint8_t v) { out.push_back(std::byte{v}); }

template <typename T>
void PutInt(std::vector<std::byte>& out, T v) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(std::byte(static_cast<std::uint64_t>(v) >> (8 * i) & 0xFF));
    }
}

void PutBytes(std::vector<std::byte>& out, std::string_view s) {
    PutInt<std::uint32_t>(out, static_cast<std::uint32_t>(s.size()));
    for (char c : s) out.push_back(std::byte(static_cast<unsigned char>(c)));
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    StatusOr<std::uint8_t> U8() {
        if (pos_ + 1 > bytes_.size()) return Truncated();
        return static_cast<std::uint8_t>(bytes_[pos_++]);
    }

    template <typename T>
    StatusOr<T> Int() {
        if (pos_ + sizeof(T) > bytes_.size()) return Truncated();
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            v |= static_cast<std::uint64_t>(bytes_[pos_ + i]) << (8 * i);
        }
        pos_ += sizeof(T);
        return static_cast<T>(v);
    }

    StatusOr<std::string> Str() {
        auto len = Int<std::uint32_t>();
        if (!len.ok()) return len.status();
        if (pos_ + len.value() > bytes_.size()) return Truncated();
        std::string s(len.value(), '\0');
        std::memcpy(s.data(), bytes_.data() + pos_, len.value());
        pos_ += len.value();
        return s;
    }

    bool exhausted() const noexcept { return pos_ == bytes_.size(); }

private:
    Status Truncated() const {
        return Status::InvalidArgument("step descriptor truncated at byte " +
                                        std::to_string(pos_));
    }
    std::span<const std::byte> bytes_;
    std::size_t pos_ = 0;
};

// ---- AstValue ------------------------------------------------------------

Status EncodeValue(std::vector<std::byte>& out, const parser::AstValue& v) {
    if (v.type == parser::ValueType::kParam) {
        return Status::Unsupported(
            "a $param cannot ship in a step descriptor; a declared pattern's body never "
            "executes");
    }
    PutU8(out, static_cast<std::uint8_t>(v.type));
    PutInt<std::int64_t>(out, v.int_val);
    PutInt<std::int64_t>(out, v.dec_hi);
    PutU8(out, v.scale);
    PutInt<std::uint32_t>(out, v.byte_offset);
    PutBytes(out, v.str_val);
    PutBytes(out, v.raw_int_text);
    return Status::OK();
}

StatusOr<parser::AstValue> DecodeValue(Reader& r) {
    auto type = r.U8();
    if (!type.ok()) return type.status();
    // kParam is refused at encode, so its arrival here is corruption, not
    // a case. The range check covers both at once.
    if (type.value() > static_cast<std::uint8_t>(parser::ValueType::kDecimalWide) ||
        type.value() == static_cast<std::uint8_t>(parser::ValueType::kParam)) {
        return Status::InvalidArgument("step descriptor holds value type " +
                                        std::to_string(type.value()) +
                                        ", which no encoder writes");
    }
    parser::AstValue v;
    v.type = static_cast<parser::ValueType>(type.value());
    auto int_val = r.Int<std::int64_t>();
    if (!int_val.ok()) return int_val.status();
    v.int_val = int_val.value();
    auto dec_hi = r.Int<std::int64_t>();
    if (!dec_hi.ok()) return dec_hi.status();
    v.dec_hi = dec_hi.value();
    auto scale = r.U8();
    if (!scale.ok()) return scale.status();
    v.scale = scale.value();
    auto offset = r.Int<std::uint32_t>();
    if (!offset.ok()) return offset.status();
    v.byte_offset = offset.value();
    auto str = r.Str();
    if (!str.ok()) return str.status();
    v.str_val = std::move(str.value());
    auto raw = r.Str();
    if (!raw.ok()) return raw.status();
    v.raw_int_text = std::move(raw.value());
    return v;
}

// ---- ColumnRef / Operand / StepPredicate ---------------------------------

void EncodeColumnRef(std::vector<std::byte>& out, const exec::ColumnRef& c) {
    PutInt<std::uint16_t>(out, c.up);
    PutInt<std::uint16_t>(out, c.rel_slot);
    PutInt<std::uint16_t>(out, c.col_pos);
}

StatusOr<exec::ColumnRef> DecodeColumnRef(Reader& r) {
    exec::ColumnRef c;
    auto up = r.Int<std::uint16_t>();
    if (!up.ok()) return up.status();
    c.up = up.value();
    auto slot = r.Int<std::uint16_t>();
    if (!slot.ok()) return slot.status();
    c.rel_slot = slot.value();
    auto pos = r.Int<std::uint16_t>();
    if (!pos.ok()) return pos.status();
    c.col_pos = pos.value();
    return c;
}

Status EncodeOperand(std::vector<std::byte>& out, const exec::Operand& op) {
    PutU8(out, static_cast<std::uint8_t>(op.kind));
    if (op.kind == exec::OperandKind::kLiteral) {
        return EncodeValue(out, op.literal);
    }
    EncodeColumnRef(out, op.column);
    return Status::OK();
}

StatusOr<exec::Operand> DecodeOperand(Reader& r) {
    auto kind = r.U8();
    if (!kind.ok()) return kind.status();
    if (kind.value() > static_cast<std::uint8_t>(exec::OperandKind::kColumn)) {
        return Status::InvalidArgument("step descriptor holds operand kind " +
                                        std::to_string(kind.value()));
    }
    exec::Operand op;
    op.kind = static_cast<exec::OperandKind>(kind.value());
    if (op.kind == exec::OperandKind::kLiteral) {
        auto v = DecodeValue(r);
        if (!v.ok()) return v.status();
        op.literal = std::move(v.value());
    } else {
        auto c = DecodeColumnRef(r);
        if (!c.ok()) return c.status();
        op.column = c.value();
    }
    return op;
}

}  // namespace

bool ShipsAsWalk(exec::AccessKind kind) {
    return kind == exec::AccessKind::kIndexProbe || kind == exec::AccessKind::kIndexRange ||
           kind == exec::AccessKind::kCabinProbe;
}

exec::Step ShippedForm(exec::Step step) {
    // The build annotation is compiled, core-local state (workplan JB1):
    // cleared for every shipped step, so "the descriptor never carries it"
    // is the downgrade's property rather than the codec's silence - and no
    // wrong-frame `key_from` survives the session's residual rewrite.
    step.build.reset();
    if (!ShipsAsWalk(step.kind)) return step;
    step.kind = exec::AccessKind::kScan;
    step.index.reset();
    step.cabin.reset();
    // Cleared for self-consistency, not for a peer-side consumer (no
    // shipped stage records statistics today): a Step whose kind and
    // access shape disagree is a lie on the wire, and kScan's shape is
    // empty (AccessColumnsOf).
    step.access_columns.clear();
    return step;
}

StatusOr<std::vector<std::byte>> EncodeStepDescriptor(const exec::Step& step) {
    if (!step.sub_chains.empty()) {
        return Status::Unsupported(
            "a step carrying a predicate-position subquery cannot ship yet; nested chains "
            "are P4d's multi-step work (docs/inflight/in-progress/workplan-crosscore.md)");
    }
    switch (step.kind) {
        case exec::AccessKind::kLookup:
        case exec::AccessKind::kProbe:
        case exec::AccessKind::kRange:
        case exec::AccessKind::kFilterScan:
        case exec::AccessKind::kScan:
            break;
        case exec::AccessKind::kCabinProbe:
        case exec::AccessKind::kIndexProbe:
        case exec::AccessKind::kIndexRange:
            // A backstop, not the policy: the session ships these as
            // `ShippedForm`'s walk, so reaching this refusal means a
            // caller skipped the sanctioned route - refused rather than
            // silently transformed, because a codec that rewrites what it
            // is given hides the decision from the one place that owns it.
            return Status::Unsupported(
                "a " + std::string(exec::AccessKindName(step.kind)) +
                " step carries core-local structure state and cannot ship; the session "
                "ships its walk (ShippedForm) instead");
    }

    std::vector<std::byte> out;
    PutU8(out, kStepDescriptorVersion);
    PutInt<std::uint32_t>(out, step.step_id);
    PutInt<std::uint64_t>(out, step.rel_oid);
    PutBytes(out, step.rel_name);
    PutU8(out, static_cast<std::uint8_t>(step.kind));

    PutU8(out, step.key.has_value() ? 1 : 0);
    if (step.key.has_value()) {
        if (Status s = EncodeOperand(out, *step.key); !s.ok()) return s;
    }

    PutU8(out, step.range.has_value() ? 1 : 0);
    if (step.range.has_value()) {
        PutInt<std::uint64_t>(out, step.range->low);
        PutInt<std::uint64_t>(out, step.range->high);
    }

    PutInt<std::uint32_t>(out, static_cast<std::uint32_t>(step.residual.size()));
    for (const exec::StepPredicate& pred : step.residual) {
        EncodeColumnRef(out, pred.lhs);
        PutU8(out, static_cast<std::uint8_t>(pred.op));
        if (Status s = EncodeOperand(out, pred.rhs); !s.ok()) return s;
    }

    PutInt<std::uint32_t>(out, static_cast<std::uint32_t>(step.access_columns.size()));
    for (std::uint16_t col : step.access_columns) PutInt<std::uint16_t>(out, col);

    PutInt<std::uint64_t>(out, step.filter_columns);
    PutInt<std::uint64_t>(out, step.read_columns);
    return out;
}

StatusOr<exec::Step> DecodeStepDescriptor(std::span<const std::byte> bytes) {
    Reader r(bytes);

    auto version = r.U8();
    if (!version.ok()) return version.status();
    if (version.value() != kStepDescriptorVersion) {
        return Status::InvalidArgument("step descriptor version " +
                                        std::to_string(version.value()) + "; this build reads " +
                                        std::to_string(kStepDescriptorVersion));
    }

    exec::Step step;
    auto step_id = r.Int<std::uint32_t>();
    if (!step_id.ok()) return step_id.status();
    step.step_id = step_id.value();
    auto oid = r.Int<std::uint64_t>();
    if (!oid.ok()) return oid.status();
    step.rel_oid = oid.value();
    auto name = r.Str();
    if (!name.ok()) return name.status();
    step.rel_name = std::move(name.value());

    auto kind = r.U8();
    if (!kind.ok()) return kind.status();
    if (kind.value() > static_cast<std::uint8_t>(exec::AccessKind::kScan)) {
        return Status::InvalidArgument("step descriptor holds access kind " +
                                        std::to_string(kind.value()));
    }
    step.kind = static_cast<exec::AccessKind>(kind.value());

    auto has_key = r.U8();
    if (!has_key.ok()) return has_key.status();
    if (has_key.value() != 0) {
        auto op = DecodeOperand(r);
        if (!op.ok()) return op.status();
        step.key = std::move(op.value());
    }

    auto has_range = r.U8();
    if (!has_range.ok()) return has_range.status();
    if (has_range.value() != 0) {
        exec::RangeBounds range;
        auto low = r.Int<std::uint64_t>();
        if (!low.ok()) return low.status();
        range.low = low.value();
        auto high = r.Int<std::uint64_t>();
        if (!high.ok()) return high.status();
        range.high = high.value();
        step.range = range;
    }

    auto residuals = r.Int<std::uint32_t>();
    if (!residuals.ok()) return residuals.status();
    step.residual.reserve(residuals.value());
    for (std::uint32_t i = 0; i < residuals.value(); ++i) {
        exec::StepPredicate pred;
        auto lhs = DecodeColumnRef(r);
        if (!lhs.ok()) return lhs.status();
        pred.lhs = lhs.value();
        auto op = r.U8();
        if (!op.ok()) return op.status();
        // Bounded by the **last enumerator**, not by the last relational
        // operator: `kIsNull`/`kIsNotNull` encode and evaluate like every
        // other op (their rhs is the kNull literal `EncodeValue` already
        // ships), so refusing them here would fail `WHERE col IS NULL`
        // against a peer-owned relation for no reason the format has.
        if (op.value() > static_cast<std::uint8_t>(parser::CompareOp::kIsNotNull)) {
            return Status::InvalidArgument("step descriptor holds comparison op " +
                                            std::to_string(op.value()));
        }
        pred.op = static_cast<parser::CompareOp>(op.value());
        auto rhs = DecodeOperand(r);
        if (!rhs.ok()) return rhs.status();
        pred.rhs = std::move(rhs.value());
        step.residual.push_back(std::move(pred));
    }

    auto access = r.Int<std::uint32_t>();
    if (!access.ok()) return access.status();
    step.access_columns.reserve(access.value());
    for (std::uint32_t i = 0; i < access.value(); ++i) {
        auto col = r.Int<std::uint16_t>();
        if (!col.ok()) return col.status();
        step.access_columns.push_back(col.value());
    }

    auto filter = r.Int<std::uint64_t>();
    if (!filter.ok()) return filter.status();
    step.filter_columns = filter.value();
    auto read = r.Int<std::uint64_t>();
    if (!read.ok()) return read.status();
    step.read_columns = read.value();

    if (!r.exhausted()) {
        return Status::InvalidArgument("step descriptor has trailing bytes");
    }
    return step;
}

}  // namespace kds::server
