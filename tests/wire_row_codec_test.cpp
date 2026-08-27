#include "kds/wire/row_codec.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/well_known.hpp"

// The KWP/1 row encoding (docs/spec/protocol.md D5 and §6).
//
// The format's only real guarantee is that the encoder and the decoder
// agree, so most of these are round trips. What they are pinning beyond
// that: the byte layout itself (a client is written against it, not against
// this code), the NULL convention, and that a value the engine cannot store
// is refused rather than guessed.

namespace kds::wire {
namespace {

catalog::SysColumnRow Column(std::uint32_t pos, std::string_view name, std::uint32_t type_val) {
    catalog::SysColumnRow c{};
    c.pos = pos;
    catalog::SetName(c.name, name);
    c.type_val = type_val;
    return c;
}

catalog::Schema SchemaOf(std::initializer_list<catalog::SysColumnRow> cols) {
    catalog::Schema s;
    s.columns = cols;
    return s;
}

parser::AstValue Int(std::int64_t v) {
    parser::AstValue a;
    a.type = parser::ValueType::kInt;
    a.int_val = v;
    return a;
}

parser::AstValue Str(std::string v) {
    parser::AstValue a;
    a.type = parser::ValueType::kStr;
    a.str_val = std::move(v);
    return a;
}

// The decoded form a decimal column produces: unscaled int64 plus the
// column's scale (types.md TY5).
parser::AstValue Decimal(std::int64_t unscaled, std::uint8_t scale) {
    parser::AstValue a;
    a.type = parser::ValueType::kDecimal;
    a.int_val = unscaled;
    a.scale = scale;
    return a;
}

catalog::SysColumnRow DecimalColumn(std::uint32_t pos, std::string_view name, std::uint8_t p,
                                    std::uint8_t s) {
    catalog::SysColumnRow c = Column(pos, name, catalog::kTypeValDecimal);
    c.len = catalog::PackDecimalLen(p, s);
    return c;
}

// ---- Row description ---------------------------------------------------

TEST(WireRowDescriptionTest, RoundTripsEveryField) {
    const auto schema = SchemaOf({Column(0, "id", catalog::kTypeValInt64),
                                  Column(1, "name", catalog::kTypeValVarchar),
                                  Column(2, "flag", catalog::kTypeValBool)});

    std::vector<std::byte> out;
    EncodeRowDescription(DescribeSchema(schema), out);

    auto decoded = DecodeRowDescription(out);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    ASSERT_EQ(decoded.value().size(), 3u);

    EXPECT_EQ(decoded.value()[0].name, "id");
    EXPECT_EQ(decoded.value()[0].type_oid, catalog::kTypeValInt64);
    EXPECT_EQ(decoded.value()[0].type_len, 8);
    // Field 0 is the Keystone id on every user relation, and it is the one
    // field a client can rely on without reading the schema.
    EXPECT_TRUE(decoded.value()[0].flags & kFieldFlagKeystone);

    EXPECT_EQ(decoded.value()[1].name, "name");
    EXPECT_EQ(decoded.value()[1].type_len, -1) << "a varchar is variable-width on the wire";
    EXPECT_FALSE(decoded.value()[1].flags & kFieldFlagKeystone);

    EXPECT_EQ(decoded.value()[2].type_len, 1);
}

TEST(WireRowDescriptionTest, ADecimalFieldCarriesItsScaleInTheDescription) {
    // The wire value is only the unscaled int64; without (p, s) beside it
    // in the description the number means nothing. type_mod is literally
    // the catalog's packed len word - one packing, two readers - and it is
    // zero for every other type, so a client can key on it without a type
    // table.
    const auto schema = SchemaOf({Column(0, "id", catalog::kTypeValInt64),
                                  DecimalColumn(1, "amt", 12, 2)});

    std::vector<std::byte> out;
    EncodeRowDescription(DescribeSchema(schema), out);
    auto decoded = DecodeRowDescription(out);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();

    const FieldDescription& amt = decoded.value()[1];
    EXPECT_EQ(amt.type_oid, catalog::kTypeValDecimal);
    EXPECT_EQ(amt.type_len, 8);
    EXPECT_EQ(catalog::DecimalPrecisionOf(amt.type_mod), 12);
    EXPECT_EQ(catalog::DecimalScaleOf(amt.type_mod), 2);

    EXPECT_EQ(decoded.value()[0].type_mod, 0u) << "type_mod leaks nothing for other types";
}

TEST(WireRowDescriptionTest, DateAndTimestampAreFixedWidthOnTheWire) {
    // Their storage widths (well_known.hpp's TY1/TY9 table): an epoch-day
    // int32 and an epoch-microsecond int64.
    EXPECT_EQ(WireTypeLen(catalog::kTypeValDate), 4);
    EXPECT_EQ(WireTypeLen(catalog::kTypeValTimestamp), 8);
    EXPECT_EQ(WireTypeLen(catalog::kTypeValDecimal), 8);
}

TEST(WireRowDescriptionTest, ACharColumnIsVariableWidthOnTheWire) {
    // Its *storage* width is a schema fact; the length of a value in it is
    // not, and conflating the two is how a client ends up padding.
    EXPECT_EQ(WireTypeLen(catalog::kTypeValChar), -1);
    EXPECT_EQ(WireTypeLen(catalog::kTypeValVarchar), -1);
}

TEST(WireRowDescriptionTest, ATruncatedDescriptionIsCorruption) {
    const auto schema = SchemaOf({Column(0, "id", catalog::kTypeValInt64)});
    std::vector<std::byte> out;
    EncodeRowDescription(DescribeSchema(schema), out);

    for (std::size_t cut = 1; cut < out.size(); ++cut) {
        std::vector<std::byte> shortened(out.begin(), out.begin() + cut);
        EXPECT_FALSE(DecodeRowDescription(shortened).ok()) << "accepted a " << cut << "-byte prefix";
    }
}

// ---- Row batches -------------------------------------------------------

TEST(WireRowBatchTest, RoundTripsRowsOfEveryStorableType) {
    const auto schema = SchemaOf({Column(0, "id", catalog::kTypeValInt64),
                                  Column(1, "small", catalog::kTypeValInt32),
                                  Column(2, "name", catalog::kTypeValVarchar),
                                  Column(3, "flag", catalog::kTypeValBool)});

    RowBatchWriter writer;
    ASSERT_TRUE(writer.AppendRow(schema, std::vector<parser::AstValue>{Int(1), Int(-7),
                                                                       Str("alpha"), Int(1)})
                    .ok());
    ASSERT_TRUE(writer.AppendRow(schema, std::vector<parser::AstValue>{Int(2), Int(2'000'000'000),
                                                                       Str(""), Int(0)})
                    .ok());
    EXPECT_EQ(writer.row_count(), 2);

    const auto payload = writer.Finish();
    auto rows = DecodeRowBatch(payload, schema.columns.size());
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    ASSERT_EQ(rows.value().size(), 2u);

    EXPECT_EQ(DecodeInt(rows.value()[0][0].bytes).value(), 1);
    // Sign-extended from the field's own width - a negative int32 must not
    // read back as four billion.
    EXPECT_EQ(DecodeInt(rows.value()[0][1].bytes).value(), -7);
    EXPECT_EQ(DecodeText(rows.value()[0][2].bytes), "alpha");
    EXPECT_EQ(DecodeInt(rows.value()[0][3].bytes).value(), 1);

    EXPECT_EQ(DecodeInt(rows.value()[1][1].bytes).value(), 2'000'000'000);
    // An empty string is a value, not a NULL - the distinction the length
    // prefix exists to carry.
    EXPECT_EQ(DecodeText(rows.value()[1][2].bytes), "");
    EXPECT_FALSE(rows.value()[1][2].is_null);
}

TEST(WireRowBatchTest, AUint64SurvivesTheUpperHalfOfItsRange) {
    // int_val is signed and cannot represent it, which is why the encoder
    // reads the preserved digit text - the same reason CompareValues does.
    const auto schema = SchemaOf({Column(0, "big", catalog::kTypeValUint64)});
    parser::AstValue v;
    v.type = parser::ValueType::kInt;
    v.raw_int_text = "18446744073709551615";

    RowBatchWriter writer;
    ASSERT_TRUE(writer.AppendRow(schema, std::vector<parser::AstValue>{v}).ok());
    // Named, not a temporary: DecodedField holds views into this buffer.
    const auto payload = writer.Finish();
    auto rows = DecodeRowBatch(payload, 1);
    ASSERT_TRUE(rows.ok());
    EXPECT_EQ(DecodeUint64(rows.value()[0][0].bytes).value(), 18446744073709551615ULL);
}

TEST(WireRowBatchTest, ANullIsMinusOneAndCarriesNoBytes) {
    // protocol.md §6's one NULL convention. Nothing produces a NULL today -
    // the engine cannot store one - and the format has to have decided,
    // because deciding later would be a wire break.
    const auto schema = SchemaOf({Column(0, "id", catalog::kTypeValInt64),
                                  Column(1, "name", catalog::kTypeValVarchar)});
    parser::AstValue null_value;  // defaults to kNull

    RowBatchWriter writer;
    ASSERT_TRUE(
        writer.AppendRow(schema, std::vector<parser::AstValue>{Int(5), null_value}).ok());
    const auto payload = writer.Finish();
    auto rows = DecodeRowBatch(payload, 2);
    ASSERT_TRUE(rows.ok());

    EXPECT_FALSE(rows.value()[0][0].is_null);
    EXPECT_TRUE(rows.value()[0][1].is_null);
    EXPECT_TRUE(rows.value()[0][1].bytes.empty());
}

TEST(WireRowBatchTest, AnEmptyBatchIsWellFormed) {
    // A step that produced no rows still sends a batch, so zero rows has to
    // decode rather than fail.
    RowBatchWriter writer;
    const auto payload = writer.Finish();
    auto rows = DecodeRowBatch(payload, 3);
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    EXPECT_TRUE(rows.value().empty());
}

TEST(WireRowBatchTest, AWriterIsReusableAfterFinish) {
    // What keeps a streaming caller from allocating a buffer per batch.
    const auto schema = SchemaOf({Column(0, "id", catalog::kTypeValInt64)});
    RowBatchWriter writer;

    ASSERT_TRUE(writer.AppendRow(schema, std::vector<parser::AstValue>{Int(1)}).ok());
    const auto first = writer.Finish();
    ASSERT_EQ(DecodeRowBatch(first, 1).value().size(), 1u);

    EXPECT_EQ(writer.row_count(), 0);
    ASSERT_TRUE(writer.AppendRow(schema, std::vector<parser::AstValue>{Int(2)}).ok());
    const auto payload = writer.Finish();
    auto second = DecodeRowBatch(payload, 1);
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second.value().size(), 1u);
    EXPECT_EQ(DecodeInt(second.value()[0][0].bytes).value(), 2);
}

TEST(WireRowBatchTest, AFailedRowLeavesTheBatchParseable) {
    // Rolled back rather than left half-encoded: the caller's natural
    // response to an error is to report it and keep the rows it had, and a
    // partial row would make those unreadable too.
    const auto schema = SchemaOf({Column(0, "id", catalog::kTypeValInt64),
                                  Column(1, "name", catalog::kTypeValVarchar)});
    RowBatchWriter writer;
    ASSERT_TRUE(
        writer.AppendRow(schema, std::vector<parser::AstValue>{Int(1), Str("ok")}).ok());

    // An integer where the schema says text.
    EXPECT_FALSE(writer.AppendRow(schema, std::vector<parser::AstValue>{Int(2), Int(3)}).ok());
    EXPECT_EQ(writer.row_count(), 1);

    const auto payload = writer.Finish();
    auto rows = DecodeRowBatch(payload, 2);
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    ASSERT_EQ(rows.value().size(), 1u);
    EXPECT_EQ(DecodeText(rows.value()[0][1].bytes), "ok");
}

TEST(WireRowBatchTest, AWrongWidthRowIsRefused) {
    const auto schema = SchemaOf({Column(0, "id", catalog::kTypeValInt64),
                                  Column(1, "name", catalog::kTypeValVarchar)});
    RowBatchWriter writer;
    EXPECT_EQ(writer.AppendRow(schema, std::vector<parser::AstValue>{Int(1)}).code(),
              StatusCode::kInvalidArgument);
}

TEST(WireRowBatchTest, ATruncatedBatchIsCorruptionAndNotAPartialAnswer) {
    // A caller cannot tell a short batch from a corrupt one, so neither may
    // this - half a result set is worse than none.
    const auto schema = SchemaOf({Column(0, "id", catalog::kTypeValInt64),
                                  Column(1, "name", catalog::kTypeValVarchar)});
    RowBatchWriter writer;
    ASSERT_TRUE(
        writer.AppendRow(schema, std::vector<parser::AstValue>{Int(1), Str("alpha")}).ok());
    const auto payload = writer.Finish();

    for (std::size_t cut = 2; cut < payload.size(); ++cut) {
        std::vector<std::byte> shortened(payload.begin(), payload.begin() + cut);
        auto rows = DecodeRowBatch(shortened, 2);
        EXPECT_FALSE(rows.ok()) << "accepted a " << cut << "-byte prefix";
        EXPECT_EQ(rows.status().code(), StatusCode::kCorruption);
    }
}

TEST(WireRowBatchTest, ATypeTheEngineCannotStoreIsRefusedRatherThanGuessed) {
    // This pinned decimal until 2026-08-07, when its wire encoding was
    // settled (protocol.md §6) - float is what remains unstorable and
    // unencodable, and the flip from decimal to float here is that
    // decision landing, not the guard weakening.
    const auto schema = SchemaOf({Column(0, "f", catalog::kTypeValFloat)});
    RowBatchWriter writer;
    EXPECT_EQ(writer.AppendRow(schema, std::vector<parser::AstValue>{Int(1)}).code(),
              StatusCode::kUnsupported);
}

// ---- The new types (protocol.md §6, settled 2026-08-07) ------------------

TEST(WireRowBatchTest, ADecimalRoundTripsAsItsUnscaledInteger) {
    const auto schema = SchemaOf({Column(0, "id", catalog::kTypeValInt64),
                                  DecimalColumn(1, "amt", 12, 2)});

    RowBatchWriter writer;
    // 100.25 at scale 2, and a negative value - the sign lives in the
    // int64, not in any text.
    ASSERT_TRUE(writer.AppendRow(schema,
                                 std::vector<parser::AstValue>{Int(1), Decimal(10025, 2)})
                    .ok());
    ASSERT_TRUE(writer.AppendRow(schema,
                                 std::vector<parser::AstValue>{Int(2), Decimal(-999, 2)})
                    .ok());

    const auto payload = writer.Finish();
    auto rows = DecodeRowBatch(payload, 2);
    ASSERT_TRUE(rows.ok()) << rows.status().message();

    EXPECT_EQ(rows.value()[0][1].bytes.size(), 8u);
    EXPECT_EQ(DecodeInt(rows.value()[0][1].bytes).value(), 10025);
    EXPECT_EQ(DecodeInt(rows.value()[1][1].bytes).value(), -999);
}

TEST(WireRowBatchTest, ADecimalWhoseScaleDisagreesWithItsColumnIsRefused) {
    // An unscaled integer under the wrong scale is a different number
    // wearing the right width - refused, never rescaled, the same rule the
    // storage codec applies (TY04). The description already told the
    // client scale 2; sending 12.340's unscaled form would quietly mean
    // 123.40.
    const auto schema = SchemaOf({DecimalColumn(0, "amt", 12, 2)});
    RowBatchWriter writer;
    EXPECT_EQ(writer.AppendRow(schema, std::vector<parser::AstValue>{Decimal(12340, 3)}).code(),
              StatusCode::kInvalidArgument);
    // And a non-decimal value against the column is the usual kind error.
    EXPECT_EQ(writer.AppendRow(schema, std::vector<parser::AstValue>{Int(1)}).code(),
              StatusCode::kInvalidArgument);
}

TEST(WireRowBatchTest, AWideDecimalRoundTripsAsSixteenBytes) {
    // TY2's separate 16-byte type on the wire: its own type_oid, its own
    // width, the same type_mod convention - what §6's DECIMAL decision
    // reserved for it, now real.
    catalog::SysColumnRow col = Column(0, "nav", catalog::kTypeValDecimalWide);
    col.len = catalog::PackDecimalLen(24, 2);
    const auto schema = SchemaOf({col});

    // 2^64 + 1 unscaled at scale 2 - a value with both halves non-zero -
    // and a negative one.
    parser::AstValue wide;
    wide.type = parser::ValueType::kDecimalWide;
    wide.dec_hi = 1;
    wide.int_val = 1;
    wide.scale = 2;
    parser::AstValue negative;
    negative.type = parser::ValueType::kDecimalWide;
    negative.dec_hi = -1;
    negative.int_val = -42;
    negative.scale = 2;

    RowBatchWriter writer;
    ASSERT_TRUE(writer.AppendRow(schema, std::vector<parser::AstValue>{wide}).ok());
    ASSERT_TRUE(writer.AppendRow(schema, std::vector<parser::AstValue>{negative}).ok());

    const auto payload = writer.Finish();
    auto rows = DecodeRowBatch(payload, 1);
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    ASSERT_EQ(rows.value()[0][0].bytes.size(), 16u);

    const Int128 a = DecodeDecimalWide(rows.value()[0][0].bytes).value();
    EXPECT_EQ(Int128High(a), 1);
    EXPECT_EQ(Int128Low(a), 1);
    const Int128 b = DecodeDecimalWide(rows.value()[1][0].bytes).value();
    EXPECT_EQ(b, static_cast<Int128>(-42));

    // The description carries the wide type's (p, s) exactly as the
    // narrow one's, and its 16-byte fixed width.
    std::vector<std::byte> desc;
    EncodeRowDescription(DescribeSchema(schema), desc);
    auto fields = DecodeRowDescription(desc);
    ASSERT_TRUE(fields.ok());
    EXPECT_EQ(fields.value()[0].type_len, 16);
    EXPECT_EQ(catalog::DecimalPrecisionOf(fields.value()[0].type_mod), 24);
    EXPECT_EQ(catalog::DecimalScaleOf(fields.value()[0].type_mod), 2);
}

TEST(WireRowBatchTest, ADateAndATimestampRoundTripAsEpochIntegers) {
    // Both decode as kInt (types.md TY5) and ride the int arm at
    // their storage widths. Rendering into a calendar is the client's act,
    // exactly as it is at the text protocol's emission boundary.
    const auto schema = SchemaOf({Column(0, "d", catalog::kTypeValDate),
                                  Column(1, "at", catalog::kTypeValTimestamp)});

    RowBatchWriter writer;
    // 2026-08-07 is epoch day 20672; a negative day is pre-1970, which the
    // DATE range (1900-01-01..) includes and the sign extension must keep.
    ASSERT_TRUE(writer.AppendRow(schema,
                                 std::vector<parser::AstValue>{Int(20672), Int(1786195200000000)})
                    .ok());
    ASSERT_TRUE(writer.AppendRow(schema,
                                 std::vector<parser::AstValue>{Int(-25567), Int(-1)})
                    .ok());

    const auto payload = writer.Finish();
    auto rows = DecodeRowBatch(payload, 2);
    ASSERT_TRUE(rows.ok()) << rows.status().message();

    EXPECT_EQ(rows.value()[0][0].bytes.size(), 4u);
    EXPECT_EQ(rows.value()[0][1].bytes.size(), 8u);
    EXPECT_EQ(DecodeInt(rows.value()[0][0].bytes).value(), 20672);
    EXPECT_EQ(DecodeInt(rows.value()[0][1].bytes).value(), 1786195200000000);
    EXPECT_EQ(DecodeInt(rows.value()[1][0].bytes).value(), -25567);
    EXPECT_EQ(DecodeInt(rows.value()[1][1].bytes).value(), -1);
}

TEST(WireRowBatchTest, AParameterIsRefusedBecauseADeclarationIsNotAnExecution) {
    const auto schema = SchemaOf({Column(0, "id", catalog::kTypeValInt64)});
    parser::AstValue param;
    param.type = parser::ValueType::kParam;
    param.str_val = "x";

    RowBatchWriter writer;
    EXPECT_EQ(writer.AppendRow(schema, std::vector<parser::AstValue>{param}).code(),
              StatusCode::kUnsupported);
}

// ---- The byte layout itself --------------------------------------------

TEST(WireRowDescriptionTest, TheFieldTrailerLayoutIsWhatTheSpecSays) {
    // The trailer is {type_oid u32, type_len i16, flags u16, type_mod u32}
    // - 12 bytes since type_mod landed with the DECIMAL decision. Pinned
    // byte for byte, because a client reads this layout, not this code.
    const auto schema = SchemaOf({DecimalColumn(0, "a", 12, 2)});
    std::vector<std::byte> out;
    EncodeRowDescription(DescribeSchema(schema), out);

    const std::vector<std::uint8_t> expected = {
        0x01, 0x00,              // field_count u16 = 1
        0x01, 0x00,              // name_len u16 = 1
        'a',                     // name
        0x07, 0x00, 0x00, 0x00,  // type_oid u32 = kTypeValDecimal
        0x08, 0x00,              // type_len i16 = 8
        0x01, 0x00,              // flags u16 = keystone (pos 0)
        0x02, 0x0C, 0x00, 0x00,  // type_mod u32 = (12 << 8) | 2, LE
    };
    ASSERT_EQ(out.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(std::to_integer<std::uint8_t>(out[i]), expected[i]) << "at byte " << i;
    }
}

TEST(WireRowBatchTest, TheByteLayoutIsWhatTheSpecSays) {
    // Pinned against the encoder rather than only round-tripped: a client is
    // written against protocol.md §6, not against this code, so a layout
    // change has to be a deliberate act.
    const auto schema = SchemaOf({Column(0, "id", catalog::kTypeValInt32)});
    RowBatchWriter writer;
    ASSERT_TRUE(writer.AppendRow(schema, std::vector<parser::AstValue>{Int(1)}).ok());
    const auto payload = writer.Finish();

    const std::vector<std::uint8_t> expected = {
        0x01, 0x00,              // row_count u16 = 1
        0x04, 0x00, 0x00, 0x00,  // field len i32 = 4
        0x01, 0x00, 0x00, 0x00,  // value int32 = 1, little-endian
    };
    ASSERT_EQ(payload.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(std::to_integer<std::uint8_t>(payload[i]), expected[i]) << "at byte " << i;
    }
}

TEST(WireFieldToValueTest, TheCheckedFormRefusesAWidthTheColumnDisagreesWith) {
    // Invariant 13's rule one level up (workplan P4d-4b-3): the bare form
    // zero-extends a short field into a smaller number that joins
    // plausibly; the checked form calls it Corruption.
    const auto col = Column(0, "id", catalog::kTypeValInt64);

    const std::vector<std::byte> full(8, std::byte{0x01});
    DecodedField good{std::span<const std::byte>(full), false};
    auto value = FieldToValueChecked(col, good);
    ASSERT_TRUE(value.ok()) << value.status().message();
    EXPECT_EQ(value.value().int_val, 0x0101010101010101);

    const std::vector<std::byte> short_bytes(4, std::byte{0x01});
    DecodedField truncated{std::span<const std::byte>(short_bytes), false};
    auto refused = FieldToValueChecked(col, truncated);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kCorruption);

    // NULL carries no bytes and no width to disagree with.
    DecodedField null_field{std::span<const std::byte>(), true};
    EXPECT_TRUE(FieldToValueChecked(col, null_field).ok());
}

}  // namespace
}  // namespace kds::wire
