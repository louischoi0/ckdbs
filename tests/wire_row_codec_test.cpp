#include "kds/wire/row_codec.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/well_known.hpp"

// The KWP/1 row encoding (docs/protocol.md D5 and §6).
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
    // float and decimal are refused at CREATE TABLE under the fixed-length
    // rule, and DECIMAL's wire format is [OPEN] - so encoding one here
    // would settle a decision that belongs to the type system.
    const auto schema = SchemaOf({Column(0, "d", catalog::kTypeValDecimal)});
    RowBatchWriter writer;
    EXPECT_EQ(writer.AppendRow(schema, std::vector<parser::AstValue>{Int(1)}).code(),
              StatusCode::kUnsupported);
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

}  // namespace
}  // namespace kds::wire
