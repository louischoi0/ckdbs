#include <algorithm>
#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/admin_plans.hpp"
#include "kds/exec/declaration_relation.hpp"
#include "kds/exec/plan_catalog.hpp"
#include "kds/exec/plan_ddl.hpp"
#include "kds/exec/plan_rule.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/parser/parser.hpp"
#include "kds/parser/lexer.hpp"
#include "kds/sched/coro.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/stats/plan_stats.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "reclaim_census.hpp"

// The administrator's plan (CX-S1b, instructions/v3.2.0/
// workorder-cx-managed-plans.md CX-R1, CX-R4, CX-R9; exec/admin_plans.hpp,
// exec/plan_rule.hpp, exec/plan_ddl.hpp). What this file pins: a `WHEN`
// rule is total and reads arguments alone; the DDL's grammar and its
// refusals; an enabled plan is the first source of a join order under every
// `join_order`, its variant chosen from the statement's literals at every
// compile; a variant is declined, never forced; the plan outlives a clean
// stop and a crash; and an enabled plan RESTRICTs its relations' drop.

namespace kds::server {
namespace {

using exec::PlanRule;
using parser::StatementArgument;

// ---- The arguments ------------------------------------------------------------

TEST(StatementArgumentsTest, EveryValuePositionInTokenOrderAndNullIsNotOne) {
    const std::vector<StatementArgument> args = parser::ArgumentsOf(
        "SELECT a FROM t WHERE x = 5 AND y = 'a b' AND z = 1.5 AND w = ? AND v = NULL AND "
        "u = -7 LIMIT 3;");
    ASSERT_EQ(args.size(), 6u);
    EXPECT_EQ(args[0].kind, StatementArgument::Kind::kInt);
    EXPECT_EQ(args[0].text, "5");
    EXPECT_EQ(args[1].kind, StatementArgument::Kind::kStr);
    EXPECT_EQ(args[1].text, "a b");
    // A numeric with a point is a string, as it is to every statement.
    EXPECT_EQ(args[2].kind, StatementArgument::Kind::kStr);
    EXPECT_EQ(args[2].text, "1.5");
    EXPECT_EQ(args[3].kind, StatementArgument::Kind::kUnbound);
    EXPECT_EQ(args[4].text, "-7");
    EXPECT_EQ(args[5].text, "3");
}

// ---- The rule -----------------------------------------------------------------

PlanRule Rule(const std::string& text) {
    parser::Lexer lexer(text);
    auto rule = exec::ParsePlanRule(lexer);
    EXPECT_TRUE(rule.ok()) << text << ": " << rule.status().message();
    EXPECT_EQ(lexer.Next().type, parser::TokenType::kEof) << text;
    return rule.ok() ? rule.value() : PlanRule{};
}

bool Holds(const std::string& rule, const std::string& statement) {
    const std::vector<StatementArgument> args = parser::ArgumentsOf(statement);
    return exec::PlanRuleHolds(Rule(rule), args);
}

Status RuleRefusal(const std::string& text) {
    parser::Lexer lexer(text);
    auto rule = exec::ParsePlanRule(lexer);
    EXPECT_FALSE(rule.ok()) << text;
    return rule.ok() ? Status::OK() : rule.status();
}

TEST(PlanRuleTest, ARulesTextReadsBackAsTheRuleItWas) {
    for (const std::string text :
         {"ARG(2) - ARG(1) <= 64", "ARG(1) = 'gold' AND ARG(2) + 1 > ARG(3) - -5",
          "ARG(1) != 7 AND 0 < ARG(1)", "ARG(1) = ARG(2)"}) {
        const PlanRule rule = Rule(text);
        EXPECT_EQ(exec::PlanRuleText(rule), text);
        EXPECT_EQ(Rule(exec::PlanRuleText(rule)), rule) << text;
    }
    // A numeric with a point is the string of its spelling, and `-` fused
    // to a digit is the subtraction it reads as.
    EXPECT_EQ(exec::PlanRuleText(Rule("ARG(1) = 12.50")), "ARG(1) = '12.50'");
    EXPECT_EQ(exec::PlanRuleText(Rule("ARG(1) -5 < 0")), "ARG(1) + -5 < 0");
    EXPECT_EQ(Rule("ARG(2) - ARG(1) <= 64 AND ARG(7) = 1").max_arg(), 7u);
    EXPECT_TRUE(PlanRule{}.is_else());
}

TEST(PlanRuleTest, IntegersAddSubtractAndCompare) {
    const std::string range = "SELECT x FROM t WHERE k BETWEEN 10 AND ";
    EXPECT_TRUE(Holds("ARG(2) - ARG(1) <= 64", range + "50"));
    EXPECT_TRUE(Holds("ARG(2) - ARG(1) <= 64", range + "74"));
    EXPECT_FALSE(Holds("ARG(2) - ARG(1) <= 64", range + "75"));
    EXPECT_TRUE(Holds("ARG(1) < ARG(2) AND ARG(2) >= 50", range + "50"));
    EXPECT_FALSE(Holds("ARG(1) < ARG(2) AND ARG(2) > 50", range + "50"));
    EXPECT_TRUE(Holds("ARG(1) -5 = 5", range + "50"));
    EXPECT_TRUE(Holds("ARG(1) != 11", range + "50"));
}

TEST(PlanRuleTest, AStringEqualsAStringByItsBytes) {
    const std::string by_tier = "SELECT x FROM t WHERE tier = 'gold' AND n = 3";
    EXPECT_TRUE(Holds("ARG(1) = 'gold'", by_tier));
    EXPECT_FALSE(Holds("ARG(1) = 'Gold'", by_tier));
    EXPECT_TRUE(Holds("ARG(1) != 'silver'", by_tier));
    EXPECT_TRUE(Holds("'gold' = ARG(1)", by_tier));
    EXPECT_TRUE(Holds("ARG(1) = ARG(1)", by_tier));
    EXPECT_TRUE(Holds("ARG(1) = 1.50", "SELECT x FROM t WHERE p = 1.50"));
    EXPECT_FALSE(Holds("ARG(1) = 1.5", "SELECT x FROM t WHERE p = 1.50")) << "spellings";
}

TEST(PlanRuleTest, AComparisonThatCannotBeDecidedIsFalseAndNeverAnError) {
    // CX-R4: total. Each of these has no value, so neither it nor its
    // negation holds and the statement goes on to the next variant.
    const std::string by_tier = "SELECT x FROM t WHERE tier = 'gold' AND n = 3";
    EXPECT_FALSE(Holds("ARG(1) > 0", by_tier)) << "a string in arithmetic";
    EXPECT_FALSE(Holds("ARG(1) <= 0", by_tier));
    EXPECT_FALSE(Holds("ARG(2) = 'x'", by_tier)) << "an integer against a string";
    EXPECT_FALSE(Holds("ARG(2) != 'x'", by_tier));
    EXPECT_FALSE(Holds("ARG(3) = 1", by_tier)) << "past the statement's arguments";
    const std::string huge = "SELECT x FROM t WHERE n = 9223372036854775807";
    EXPECT_TRUE(Holds("ARG(1) > 0", huge));
    EXPECT_FALSE(Holds("ARG(1) + 1 > 0", huge)) << "an overflow";
    EXPECT_FALSE(Holds("ARG(1) + 1 <= 0", huge));
    EXPECT_FALSE(Holds("ARG(1) > 0", "SELECT x FROM t WHERE n = 99999999999999999999"))
        << "an integer past 64 bits";
    EXPECT_FALSE(Holds("ARG(1) = 0", "SELECT x FROM t WHERE n = ?")) << "an unbound placeholder";
    EXPECT_FALSE(Holds("ARG(1) != 0", "SELECT x FROM t WHERE n = ?"));
}

TEST(PlanRuleTest, WhatTheLanguageDoesNotHoldIsRefusedAtItsByte) {
    // No IS NULL: a NULL in a value position is another shape.
    Status s = RuleRefusal("ARG(1) IS NULL");
    EXPECT_EQ(s.code(), StatusCode::kUnsupported) << s.message();
    EXPECT_NE(s.message().find("(byte 7)"), std::string::npos) << s.message();
    EXPECT_EQ(RuleRefusal("ARG(1) = NULL").code(), StatusCode::kUnsupported);
    EXPECT_EQ(RuleRefusal("ARG(1) = 1 OR ARG(1) = 2").code(), StatusCode::kNotImplemented);
    EXPECT_EQ(RuleRefusal("ARG(1) < 'm'").code(), StatusCode::kNotImplemented);
    // Simply wrong.
    s = RuleRefusal("1 < 2");
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument) << s.message();
    EXPECT_NE(s.message().find("(byte 0)"), std::string::npos) << s.message();
    for (const std::string text :
         {"ARG(0) = 1", "ARG(x) = 1", "ARG 1 = 1", "ARG(1) + 'a' = 1", "'a' = 'b'",
          "ARG(1) + 1 = 'a'", "ARG(1) = ?", "ARG(1) 5", "ARG(1) = col",
          "ARG(1) = 99999999999999999999"}) {
        EXPECT_EQ(RuleRefusal(text).code(), StatusCode::kInvalidArgument) << text;
    }
}

// ---- The DDL's grammar --------------------------------------------------------

TEST(PlanDdlTest, CreatePlanReadsTheStatementAndItsVariantsInRuleOrder) {
    const std::string select =
        "SELECT u.name FROM users u JOIN loans l ON l.user_id = u.id WHERE u.id BETWEEN 1 AND 9";
    const std::string line = "CREATE PLAN p_loans FOR " + select +
                             " VARIANT narrow WHEN ARG(2) - ARG(1) <= 64 ORDER (u, l)"
                             " VARIANT wide ELSE ORDER (l, u);";
    auto parsed = exec::ParseCreatePlan(line);
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    const exec::CreatePlanStmt& stmt = parsed.value();
    EXPECT_EQ(stmt.name, "p_loans");
    EXPECT_EQ(line.substr(stmt.statement_at, stmt.statement_size), select);
    EXPECT_FALSE(stmt.searched);
    ASSERT_EQ(stmt.variants.size(), 2u);
    EXPECT_EQ(stmt.variants[0].name, "narrow");
    EXPECT_EQ(exec::PlanRuleText(stmt.variants[0].rule), "ARG(2) - ARG(1) <= 64");
    EXPECT_EQ(stmt.variants[0].order, (std::vector<std::string>{"u", "l"}));
    EXPECT_EQ(stmt.variants[1].name, "wide");
    EXPECT_TRUE(stmt.variants[1].rule.is_else());
    EXPECT_EQ(stmt.variants[1].order, (std::vector<std::string>{"l", "u"}));
    EXPECT_EQ(line.substr(stmt.variants[1].name_at, 4), "wide");

    auto searched = exec::ParseCreatePlan("CREATE PLAN q FOR " + select + " AS SEARCHED");
    ASSERT_TRUE(searched.ok()) << searched.status().message();
    EXPECT_TRUE(searched.value().searched);
    EXPECT_EQ(("CREATE PLAN q FOR " + select)
                  .substr(searched.value().statement_at, searched.value().statement_size),
              select);
}

TEST(PlanDdlTest, AColumnNamedVariantDoesNotEndTheStatement) {
    // No word is reserved: the statement ends at `VARIANT <name> WHEN|ELSE`
    // outside parentheses, which no statement holds.
    const std::string select =
        "SELECT a.variant FROM a JOIN b ON a.id = b.id WHERE a.variant = 1 AND b.id IN (2, 3)";
    auto parsed =
        exec::ParseCreatePlan("CREATE PLAN p FOR " + select + " VARIANT variant ELSE ORDER (b, a)");
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    EXPECT_EQ(("CREATE PLAN p FOR " + select)
                  .substr(parsed.value().statement_at, parsed.value().statement_size),
              select);
    EXPECT_EQ(parsed.value().variants.front().name, "variant");
}

TEST(PlanDdlTest, ElseIsMandatoryAndLastAndEveryRefusalNamesItsByte) {
    const std::string head = "CREATE PLAN p FOR SELECT a.x FROM a JOIN b ON a.id = b.id ";
    const auto refusal = [&](const std::string& tail) {
        auto parsed = exec::ParseCreatePlan(head + tail);
        EXPECT_FALSE(parsed.ok()) << tail;
        return parsed.ok() ? Status::OK() : parsed.status();
    };
    // CX-R4: no ELSE, no plan.
    Status s = refusal("VARIANT v WHEN ARG(1) = 1 ORDER (a, b)");
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument) << s.message();
    EXPECT_NE(s.message().find("ELSE"), std::string::npos) << s.message();
    // ELSE ends the list.
    s = refusal("VARIANT v ELSE ORDER (a, b) VARIANT w WHEN ARG(1) = 1 ORDER (b, a)");
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument) << s.message();
    EXPECT_NE(s.message().find("(byte " + std::to_string(head.size() + 28) + ")"),
              std::string::npos)
        << s.message();
    EXPECT_EQ(refusal("VARIANT v WHEN ARG(1) = 1 ORDER (a, b) VARIANT V ELSE ORDER (b, a)").code(),
              StatusCode::kInvalidArgument)
        << "one name twice";
    // The clause of a later stage, at its word.
    s = refusal("VARIANT v ELSE ORDER (a, b) BUILD a OFF");
    EXPECT_EQ(s.code(), StatusCode::kNotImplemented) << s.message();
    EXPECT_EQ(refusal("VARIANT v ELSE ORDER (a, b) ACCESS a SCAN BUILD a OFF").code(),
              StatusCode::kNotImplemented);
    for (const std::string tail :
         {"", "VARIANT v ELSE", "VARIANT v ELSE ORDER a, b", "VARIANT v ELSE ORDER (a b)",
          "VARIANT v ELSE ORDER ()", "VARIANT v ELSE ORDER (a, b) extra"}) {
        EXPECT_EQ(refusal(tail).code(), StatusCode::kInvalidArgument) << tail;
    }
    EXPECT_EQ(exec::ParseCreatePlan("CREATE PLAN p SELECT 1").status().code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(exec::ParseCreatePlan("CREATE PLAN p FOR AS SEARCHED").status().code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(exec::ParseCreatePlan("CREATE PLAN 7 FOR SELECT 1 AS SEARCHED").status().code(),
              StatusCode::kInvalidArgument);
}

TEST(PlanDdlTest, AnAccessClausePinsOrForbidsOneArmARelation) {
    const std::string head = "CREATE PLAN p FOR SELECT a.x FROM a JOIN b ON a.id = b.id ";
    auto parsed = exec::ParseCreatePlan(
        head + "VARIANT v WHEN ARG(1) = 1 ORDER (a, b) ACCESS a TUPLE_KEY, b INDEX b_ix "
               "VARIANT w ELSE ORDER (b, a) ACCESS b NOT CABIN, a INDEX;");
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    const auto& first = parsed.value().variants[0].access;
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[0].binding, "a");
    EXPECT_EQ(first[0].pin, (exec::ArmPin{exec::ArmPin::Arm::kTupleKey, false, ""}));
    EXPECT_EQ(first[1].pin, (exec::ArmPin{exec::ArmPin::Arm::kIndex, false, "b_ix"}));
    const auto& second = parsed.value().variants[1].access;
    ASSERT_EQ(second.size(), 2u);
    EXPECT_EQ(second[0].pin, (exec::ArmPin{exec::ArmPin::Arm::kCabin, true, ""}));
    EXPECT_EQ(second[1].pin, (exec::ArmPin{exec::ArmPin::Arm::kIndex, false, ""}));

    // By written position, whatever order the clause names them in.
    auto variant = exec::ResolvePlanVariant({"a", "b"}, 1, parsed.value().variants[1]);
    ASSERT_TRUE(variant.ok()) << variant.status().message();
    ASSERT_EQ(variant.value().access.size(), 2u);
    EXPECT_EQ(variant.value().access[0].arm, exec::ArmPin::Arm::kIndex);
    EXPECT_TRUE(variant.value().access[1].forbid);
    EXPECT_EQ(exec::PlanAccessText(variant.value().access, {"a", "b"}),
              " ACCESS a INDEX, b NOT CABIN");
    // A variant that pins nothing carries nothing.
    auto bare = exec::ParseCreatePlan(head + "VARIANT v ELSE ORDER (a, b)");
    ASSERT_TRUE(bare.ok());
    auto unpinned = exec::ResolvePlanVariant({"a", "b"}, 0, bare.value().variants[0]);
    ASSERT_TRUE(unpinned.ok());
    EXPECT_TRUE(unpinned.value().access.empty());

    const auto refusal = [&](const std::string& access) {
        auto bad = exec::ParseCreatePlan(head + "VARIANT v ELSE ORDER (a, b) ACCESS " + access);
        if (!bad.ok()) return bad.status();
        auto resolved = exec::ResolvePlanVariant({"a", "b"}, 0, bad.value().variants[0]);
        EXPECT_FALSE(resolved.ok()) << access;
        return resolved.ok() ? Status::OK() : resolved.status();
    };
    for (const std::string access :
         {"a NOT SCAN", "a HASH", "a", "TUPLE_KEY", "a TUPLE_KEY b SCAN", "a NOT INDEX b_ix",
          "c SCAN", "a SCAN, A TUPLE_KEY", "a TUPLE_KEY,", "a NOT CABIN (x)", "a SCAN (x)"}) {
        EXPECT_EQ(refusal(access).code(), StatusCode::kInvalidArgument) << access;
    }
}

// CX-S3b: `CABIN (<column>)` names the Cabin on one column.
TEST(PlanDdlTest, ACabinPinMayNameItsColumn) {
    const std::string head = "CREATE PLAN p FOR SELECT a.x FROM a JOIN b ON a.id = b.id ";
    auto parsed = exec::ParseCreatePlan(
        head + "VARIANT v ELSE ORDER (a, b) ACCESS a CABIN (sym), b CABIN");
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    const auto& access = parsed.value().variants[0].access;
    ASSERT_EQ(access.size(), 2u);
    EXPECT_EQ(access[0].pin.arm, exec::ArmPin::Arm::kCabin);
    EXPECT_EQ(access[0].pin.cabin, "sym");
    EXPECT_EQ(parsed.value().variants[0].access[0].name_at,
              (head + "VARIANT v ELSE ORDER (a, b) ACCESS a CABIN (").size());
    EXPECT_TRUE(access[1].pin.cabin.empty());
    auto variant = exec::ResolvePlanVariant({"a", "b"}, 0, parsed.value().variants[0]);
    ASSERT_TRUE(variant.ok()) << variant.status().message();
    EXPECT_EQ(exec::PlanAccessText(variant.value().access, {"a", "b"}),
              " ACCESS a CABIN (sym), b CABIN");

    // A named Cabin is a column's: an expression or a predicate is not read.
    for (const std::string key : {"(DATE(ts))", "(x = 1)", "()", "(x, y)"}) {
        auto bad = exec::ParseCreatePlan(head + "VARIANT v ELSE ORDER (a, b) ACCESS a CABIN " +
                                         key);
        EXPECT_EQ(bad.status().code(), StatusCode::kNotImplemented) << key;
    }
}

TEST(PlanDdlTest, AlterPlanReadsItsThreeActions) {
    auto enable = exec::ParseAlterPlan("ALTER PLAN p ENABLE;");
    ASSERT_TRUE(enable.ok()) << enable.status().message();
    EXPECT_EQ(enable.value().action, exec::AlterPlanStmt::Action::kEnable);
    auto disable = exec::ParseAlterPlan("alter plan P disable");
    ASSERT_TRUE(disable.ok());
    EXPECT_EQ(disable.value().action, exec::AlterPlanStmt::Action::kDisable);
    EXPECT_EQ(disable.value().name, "P");
    auto replace =
        exec::ParseAlterPlan("ALTER PLAN p REPLACE VARIANT narrow WHEN ARG(1) < 9 ORDER (l, u)");
    ASSERT_TRUE(replace.ok()) << replace.status().message();
    EXPECT_EQ(replace.value().action, exec::AlterPlanStmt::Action::kReplaceVariant);
    EXPECT_EQ(replace.value().variant.name, "narrow");
    EXPECT_EQ(exec::PlanRuleText(replace.value().variant.rule), "ARG(1) < 9");
    for (const std::string line :
         {"ALTER PLAN p", "ALTER PLAN p ENABLE now", "ALTER PLAN p RENAME TO q",
          "ALTER PLAN p REPLACE narrow ELSE ORDER (a, b)",
          "ALTER PLAN p REPLACE VARIANT v ELSE ORDER (a, b) VARIANT w ELSE ORDER (a, b)"}) {
        EXPECT_EQ(exec::ParseAlterPlan(line).status().code(), StatusCode::kInvalidArgument)
            << line;
    }
}

TEST(PlanDdlTest, AnOrderNamesEachBindingOnce) {
    const std::vector<std::string> bindings = {"u", "Loans", "x"};
    exec::PlanVariantSpec spec;
    spec.name = "v";
    spec.order = {"loans", "X", "u"};
    auto order = exec::ResolvePlanOrder(bindings, spec);
    ASSERT_TRUE(order.ok()) << order.status().message();
    EXPECT_EQ(order.value()[0], 1);
    EXPECT_EQ(order.value()[1], 2);
    EXPECT_EQ(order.value()[2], 0);
    for (const std::vector<std::string>& bad :
         {std::vector<std::string>{"u", "loans"}, std::vector<std::string>{"u", "loans", "u"},
          std::vector<std::string>{"u", "loans", "y"},
          std::vector<std::string>{"u", "loans", "x", "u"}}) {
        spec.order = bad;
        EXPECT_EQ(exec::ResolvePlanOrder(bindings, spec).status().code(),
                  StatusCode::kInvalidArgument);
    }
}

// ---- The row's text -----------------------------------------------------------

exec::PlanRow SampleRow() {
    exec::PlanRow row;
    row.name = "p";
    row.key.fetch_id = 77;
    row.key.n = 2;
    row.key.oids = {4001, 4002};
    row.bindings = {"u", "l"};
    row.arguments = 2;
    row.variants.push_back(exec::PlanVariant{"narrow", Rule("ARG(2) - ARG(1) <= 64"), {0, 1}});
    row.variants.push_back(exec::PlanVariant{"wide", {}, {1, 0}});
    row.statement = "SELECT u.name FROM users u JOIN loans l ON l.user_id = u.id WHERE u.id "
                    "BETWEEN ? AND ?";
    return row;
}

TEST(AdminPlanTextTest, ARowsTextReadsBackAsWhatWasWritten) {
    const exec::PlanRow row = SampleRow();
    const std::string text = exec::EncodeAdminPlanText(row, 1234);
    exec::PlanRow back;
    ASSERT_TRUE(exec::DecodeAdminPlanText(text, back).ok()) << text;
    EXPECT_EQ(back.fingerprint_version, parser::kFingerprintVersion);
    EXPECT_EQ(back.key.n, 2);
    EXPECT_EQ(back.key.oids, row.key.oids);
    EXPECT_EQ(back.bindings, row.bindings);
    EXPECT_EQ(back.arguments, 2u);
    EXPECT_EQ(back.written_at_us, 1234);
    EXPECT_EQ(back.statement, row.statement);
    ASSERT_EQ(back.variants.size(), 2u);
    EXPECT_EQ(back.variants[0].name, "narrow");
    EXPECT_EQ(back.variants[0].rule, row.variants[0].rule);
    EXPECT_EQ(back.variants[0].order, row.variants[0].order);
    EXPECT_TRUE(back.variants[1].rule.is_else());
    EXPECT_EQ(back.variants[1].order, row.variants[1].order);
}

TEST(AdminPlanTextTest, AVariantsPinsReadBackAndARowWithNoneReadsAsBefore) {
    exec::PlanRow row = SampleRow();
    row.variants[0].access = {exec::ArmPin{exec::ArmPin::Arm::kTupleKey, false, ""},
                              exec::ArmPin{exec::ArmPin::Arm::kIndex, false, "loans_user_idx"}};
    row.variants[1].access = {exec::ArmPin{}, exec::ArmPin{exec::ArmPin::Arm::kCabin, true, ""}};
    const std::string text = exec::EncodeAdminPlanText(row, 7);
    EXPECT_NE(text.find("narrow 0,1 access=0:TUPLE_KEY;1:INDEX:loans_user_idx WHEN "),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("wide 1,0 access=1:!CABIN ELSE\n"), std::string::npos) << text;
    exec::PlanRow back;
    ASSERT_TRUE(exec::DecodeAdminPlanText(text, back).ok()) << text;
    EXPECT_EQ(back.variants[0].access, row.variants[0].access);
    EXPECT_EQ(back.variants[1].access, row.variants[1].access);
    // No pin, no field: the text S1b wrote.
    EXPECT_EQ(exec::EncodeAdminPlanText(SampleRow(), 7).find("access="), std::string::npos);
    // A named Cabin's column travels as a pinned index's name does (CX-S3b).
    exec::PlanRow named = SampleRow();
    named.variants[0].access = {exec::ArmPin{},
                                exec::ArmPin{exec::ArmPin::Arm::kCabin, false, "", "sym"}};
    const std::string named_text = exec::EncodeAdminPlanText(named, 7);
    EXPECT_NE(named_text.find("narrow 0,1 access=1:CABIN:sym WHEN "), std::string::npos)
        << named_text;
    exec::PlanRow named_back;
    ASSERT_TRUE(exec::DecodeAdminPlanText(named_text, named_back).ok()) << named_text;
    EXPECT_EQ(named_back.variants[0].access, named.variants[0].access);

    const std::string head = "v1 fp=2 oids=4001,4002 bindings=u,l args=2 at=5 variants=1\na 0,1 ";
    for (const std::string access :
         {"access=2:SCAN", "access=0:SCAN;0:CABIN", "access=0:HASH", "access=0:!SCAN",
          "access=0:SCAN:x", "access=0:TUPLE_KEY:x", "access=0:!CABIN:x", "access=0:CABIN:",
          "access=0:!INDEX:x", "access=0:INDEX:", "access=x:SCAN",
          "access=", "access=0:SCAN;", "access=0:scan"}) {
        exec::PlanRow kept;
        EXPECT_EQ(exec::DecodeAdminPlanText(head + access + " ELSE\nS", kept).code(),
                  StatusCode::kCorruption)
            << access;
    }
}

TEST(AdminPlanTextTest, ATextThatBreaksARuleOfThePlanIsCorruptionAndLeavesTheRow) {
    const std::string head = "v1 fp=2 oids=4001,4002 bindings=u,l args=2 at=5 variants=";
    for (const std::string text : {
             head + "2\na 0,1 ELSE\nb 1,0 ELSE\nS",                      // ELSE not last alone
             head + "1\na 0,1 WHEN ARG(1) = 1\nS",                        // no ELSE
             head + "2\na 0,1 WHEN ARG(3) = 1\nb 1,0 ELSE\nS",            // past the arguments
             head + "2\na 0,1 WHEN ARG(1) IS NULL\nb 1,0 ELSE\nS",        // a rule that does not read
             head + "2\na 0,1 WHEN ARG(1) = 1 junk\nb 1,0 ELSE\nS",
             head + "2\na 0,0 WHEN ARG(1) = 1\nb 1,0 ELSE\nS",            // not a permutation
             head + "2\na 0,1 WHEN ARG(1) = 1\nA 1,0 ELSE\nS",            // one name twice
             head + "2\na 0,1 WHEN ARG(1) = 1\nS",                        // ends inside
             head + "0\nS",
             head + "17\nS",
             std::string("v1 fp=2 oids=4001,4002 bindings=u args=2 at=5 variants=1\na 0,1 "
                         "ELSE\nS"),
             std::string("v1 fp=2 oids=4001,4002 bindings=u,U args=2 at=5 variants=1\na 0,1 "
                         "ELSE\nS"),
             std::string("v1 fp=2 oids= bindings= args=0 at=5 variants=1\na  ELSE\nS"),
             std::string("v2 fp=2 oids=4001,4002 bindings=u,l args=2 at=5 variants=1\na 0,1 "
                         "ELSE\nS"),
             std::string("v1 fp=2 oids=4001,4002 order=0,1 at=5\nS"),  // a captured row's text
         }) {
        exec::PlanRow row;
        row.name = "kept";
        EXPECT_EQ(exec::DecodeAdminPlanText(text, row).code(), StatusCode::kCorruption) << text;
        EXPECT_EQ(row.name, "kept");
        EXPECT_TRUE(row.variants.empty()) << text;
    }
}

// ---- The table ----------------------------------------------------------------

TEST(AdminPlansTableTest, OnlyAnEnabledPlanIsServedAndAnyIsTheGate) {
    exec::AdminPlans table;
    EXPECT_FALSE(table.any());
    auto plan = std::make_shared<exec::AdminPlan>();
    plan->name = "p";
    plan->key.fetch_id = 7;
    plan->key.n = 2;
    plan->key.oids = {200, 201};
    plan->variants.push_back(exec::PlanVariant{"v", {}, {1, 0}});
    table.Install(plan, /*enabled=*/false);
    EXPECT_FALSE(table.any());
    EXPECT_EQ(table.Find(plan->key), nullptr);
    EXPECT_EQ(table.Holder(plan->key), std::optional<std::string>("p"));
    EXPECT_FALSE(table.EnabledNaming(200).has_value());
    table.Install(plan, /*enabled=*/true);
    EXPECT_TRUE(table.any());
    EXPECT_EQ(table.Find(plan->key), plan);
    EXPECT_EQ(table.EnabledNaming(201), std::optional<std::string>("p"));
    EXPECT_EQ(table.plans(), 1u);
    // Exact matching (CX-R3): another oid in one position misses.
    exec::CapturedKey other = plan->key;
    other.oids[1] = 202;
    EXPECT_EQ(table.Find(other), nullptr);
    // A dropped relation's plans are forgotten.
    table.ForgetRelation(999);
    EXPECT_EQ(table.plans(), 1u);
    table.ForgetRelation(201);
    EXPECT_EQ(table.plans(), 0u);
    EXPECT_FALSE(table.any());
    // The DDL claim is one, and never waited for.
    {
        const exec::PlanDdlClaim first(table);
        EXPECT_TRUE(first.held());
        const exec::PlanDdlClaim second(table);
        EXPECT_FALSE(second.held());
    }
    EXPECT_TRUE(exec::PlanDdlClaim(table).held());
}

TEST(AdminPlansTableTest, TheFirstVariantWhoseRuleHoldsIsTheStatements) {
    exec::AdminPlan plan;
    plan.variants.push_back(exec::PlanVariant{"tiny", Rule("ARG(1) < 10"), {0, 1}});
    plan.variants.push_back(exec::PlanVariant{"small", Rule("ARG(1) < 100"), {0, 1}});
    plan.variants.push_back(exec::PlanVariant{"rest", {}, {1, 0}});
    const std::string head = "SELECT a.x FROM a JOIN b ON a.id = b.id WHERE a.n > ";
    EXPECT_EQ(plan.Choose(head + "5").name, "tiny");
    EXPECT_EQ(plan.Choose(head + "50").name, "small");
    EXPECT_EQ(plan.Choose(head + "500").name, "rest");
    EXPECT_EQ(plan.Choose(head + "'x'").name, "rest") << "undecided reaches ELSE";
}

// ---- The plan, through a dispatcher --------------------------------------------

class AdminPlanTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay=*/false,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);
        dispatcher_->set_plan_stats(&stats_);
        dispatcher_->set_join_order(exec::JoinOrder::kFixed);
        Ok("CREATE TABLE models (id int64, name int64) BTREE");
        Ok("CREATE TABLE results (id int64, model_id int64, pnl int64) BTREE");
        for (int m = 1; m <= 8; ++m) Ok("INSERT INTO models VALUES (" + std::to_string(m) + ")");
        for (int r = 0; r < 64; ++r) {
            Ok("INSERT INTO results VALUES (" + std::to_string(r + 1) + ", " +
               std::to_string(1 + r % 8) + ", " + std::to_string(r) + ")");
        }
        models_ = OidOf("models");
        results_ = OidOf("results");
        PublishModelsSmall();
    }
    void TearDown() override { dispatcher_->set_plan_stats(nullptr); }

    DispatchOutcome Do(const std::string& sql) { return dispatcher_->Dispatch(sql); }
    std::string Run(const std::string& sql) { return Do(sql).response; }
    void Ok(const std::string& sql) {
        const std::string out = Run(sql);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }
    StatusCode CodeOf(const std::string& sql) {
        const DispatchOutcome out = Do(sql);
        EXPECT_EQ(out.response.rfind("ERR", 0), 0u) << sql << " -> " << out.response;
        return out.status.code();
    }
    std::string Plan(const std::string& sql) { return Run("ANALYZE " + sql); }
    // A reply's rows, sorted: emission order is no contract (I11).
    std::vector<std::string> Rows(const std::string& sql) {
        const std::string out = Run(sql);
        EXPECT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
        std::vector<std::string> rows;
        std::size_t at = out.find("\\n");
        while (at != std::string::npos) {
            const std::size_t next = out.find("\\n", at + 2);
            rows.push_back(out.substr(at + 2, next == std::string::npos ? next : next - at - 2));
            at = next;
        }
        std::sort(rows.begin(), rows.end());
        return rows;
    }
    catalog::Oid OidOf(const std::string& name) {
        auto oid = boot_->catalog.ResolveRelation("", name, catalog::kNamespacePublic, 0, nullptr);
        EXPECT_TRUE(oid.ok()) << name;
        return oid.ok() ? oid.value() : 0;
    }
    // Priors under which the search puts models first, and their reverse.
    void PublishModelsSmall() {
        stats_.PublishForTest({{models_, stats::BandOf(8)}, {results_, stats::BandOf(2872)}}, {});
    }
    void PublishModelsLarge() {
        stats_.PublishForTest({{models_, stats::BandOf(2872)}, {results_, stats::BandOf(8)}}, {});
    }
    exec::AdminPlans& table() { return dispatcher_->admin_plans(); }
    std::vector<exec::PlanRow> PlanRows() {
        auto rows = exec::ListPlans(boot_->catalog, store_);
        EXPECT_TRUE(rows.ok()) << rows.status().message();
        return rows.ok() ? rows.value() : std::vector<exec::PlanRow>{};
    }

    // One value position, `results` written first.
    static std::string Where(const std::string& pnl) {
        return "SELECT results.pnl, models.name FROM results JOIN models ON results.model_id = "
               "models.id WHERE results.pnl > " +
               pnl;
    }
    static constexpr const char* kModelsFirst = " VARIANT models_first ELSE ORDER (models, results)";
    static constexpr const char* kAsWritten = " VARIANT as_written ELSE ORDER (results, models)";

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    stats::PlanStats stats_{1};
    std::optional<CommandDispatcher> dispatcher_;
    catalog::Oid models_ = 0;
    catalog::Oid results_ = 0;
};

TEST_F(AdminPlanTest, AnEnabledPlanIsTheOrderUnderEveryJoinOrderAndTheRowsAreTheSame) {
    const std::vector<std::string> before = Rows(Where("5"));
    ASSERT_EQ(before.size(), 58u);
    const std::string created = Run("CREATE PLAN p FOR " + Where("5") + kModelsFirst);
    ASSERT_EQ(created.rfind("CREATED PLAN name=p plan_id=", 0), 0u) << created;
    EXPECT_NE(created.find(" variants=1"), std::string::npos) << created;
    for (const char* value : {"fixed", "planned", "written"}) {
        Ok(std::string("SET JOIN_ORDER ") + value);
        const std::string plan = Plan(Where("7"));
        EXPECT_NE(plan.find("reordered=1"), std::string::npos) << value << ": " << plan;
        EXPECT_NE(plan.find("planned order=plan plan=p/models_first\\n"), std::string::npos)
            << value << ": " << plan;
        // CX-R2: an order chooses where to look, never what comes back.
        EXPECT_EQ(Rows(Where("5")), before) << value;
    }
    EXPECT_EQ(table().plans(), 1u);
    EXPECT_EQ(table().enabled(), 1u);
    EXPECT_EQ(table().declined(), 0u);
    const std::string meta = Run("SHOW META");
    EXPECT_NE(meta.find("join_order,admin_plans,1\\njoin_order,admin_plans_enabled,1\\n"),
              std::string::npos)
        << meta;
}

TEST_F(AdminPlanTest, ThePlanOutranksTheCapturedOrderWhichStaysBeneathIt) {
    // The shape's order is captured first: models first, under these priors.
    ASSERT_NE(Plan(Where("5")).find(" capture=first"), std::string::npos);
    ASSERT_NE(Plan(Where("5")).find("planned order=captured\\n"), std::string::npos);
    ASSERT_NE(Plan(Where("5")).find("reordered=1"), std::string::npos);
    // CX-R9: the administrator's plan runs instead - here the written order.
    Ok("CREATE PLAN p FOR " + Where("5") + kAsWritten);
    const std::string managed = Plan(Where("5"));
    EXPECT_NE(managed.find("planned order=plan plan=p/as_written\\n"), std::string::npos)
        << managed;
    EXPECT_EQ(managed.find("reordered=1"), std::string::npos) << managed;
    EXPECT_EQ(dispatcher_->captured_plans().shapes(), 1u) << "the captured order is kept";
    // CX-Q17: dropping the plan falls back to the captured order, not to a
    // search - whatever the statistics say now.
    PublishModelsLarge();
    const std::string dropped = Run("DROP PLAN p");
    ASSERT_EQ(dropped.rfind("DROPPED PLAN name=p", 0), 0u) << dropped;
    const std::string after = Plan(Where("5"));
    EXPECT_NE(after.find("planned order=captured\\n"), std::string::npos) << after;
    EXPECT_NE(after.find("reordered=1"), std::string::npos) << after;
    EXPECT_EQ(table().plans(), 0u);
}

TEST_F(AdminPlanTest, AVariantIsChosenFromTheStatementsLiteralsAtEveryCompile) {
    Ok("CREATE PLAN p FOR " + Where("5") +
       " VARIANT low WHEN ARG(1) < 10 ORDER (models, results)"
       " VARIANT high ELSE ORDER (results, models)");
    const std::string low = Plan(Where("9"));
    EXPECT_NE(low.find("planned order=plan plan=p/low\\n"), std::string::npos) << low;
    EXPECT_NE(low.find("reordered=1"), std::string::npos) << low;
    const std::string high = Plan(Where("10"));
    EXPECT_NE(high.find("planned order=plan plan=p/high\\n"), std::string::npos) << high;
    EXPECT_EQ(high.find("reordered=1"), std::string::npos) << high;
    // And back: nothing of a choice is remembered.
    EXPECT_NE(Plan(Where("-3")).find("plan=p/low\\n"), std::string::npos);
    EXPECT_EQ(Rows(Where("9")).size(), 54u);
    EXPECT_EQ(Rows(Where("10")).size(), 53u);
}

TEST_F(AdminPlanTest, AsSearchedFixesTheSearchsOrderOfThatMomentUnderEveryValue) {
    // CX-R11: a shape that is not captured - here because `join_order` is
    // `planned` - is fixed by hand.
    Ok("SET JOIN_ORDER planned");
    const std::string created = Run("CREATE PLAN p FOR " + Where("5") + " AS SEARCHED");
    ASSERT_EQ(created.rfind("CREATED PLAN name=p ", 0), 0u) << created;
    EXPECT_NE(created.find(" order=(models, results)"), std::string::npos) << created;
    // The statistics move; under `planned` the search would follow them.
    PublishModelsLarge();
    const std::string plan = Plan(Where("5"));
    EXPECT_NE(plan.find("planned order=plan plan=p/searched\\n"), std::string::npos) << plan;
    EXPECT_NE(plan.find("reordered=1"), std::string::npos) << plan;
    Ok("DROP PLAN p");
    const std::string searched = Plan(Where("5"));
    EXPECT_EQ(searched.find("reordered=1"), std::string::npos) << searched;
    EXPECT_NE(searched.find("planned order=written"), std::string::npos) << searched;
}

TEST_F(AdminPlanTest, TheRowHoldsTheShapeAndNoLiteralOfTheExample) {
    Ok("CREATE PLAN p FOR " + Where("123456789") +
       " VARIANT low WHEN ARG(1) < 10 ORDER (models, results)"
       " VARIANT high ELSE ORDER (results, models)");
    const std::vector<exec::PlanRow> rows = PlanRows();
    ASSERT_EQ(rows.size(), 1u);
    const exec::PlanRow& row = rows.front();
    EXPECT_FALSE(row.captured);
    EXPECT_TRUE(row.readable);
    EXPECT_TRUE(row.enabled);
    EXPECT_EQ(row.name, "p");
    EXPECT_EQ(row.arguments, 1u);
    EXPECT_EQ(row.bindings, (std::vector<std::string>{"results", "models"}));
    EXPECT_EQ(row.key.oids[0], results_);
    EXPECT_EQ(row.key.oids[1], models_);
    EXPECT_EQ(row.statement, Where("?"));
    auto raw = exec::ScanDeclarations(boot_->catalog, store_, catalog::kSysPlansTable);
    ASSERT_TRUE(raw.ok());
    ASSERT_EQ(raw.value().size(), 1u);
    EXPECT_EQ(raw.value().front().source_text.find("123456789"), std::string::npos);
    EXPECT_EQ(raw.value().front().flags, 0u);

    const std::string view = Run(
        "SELECT name, origin, state, relations, join_order, variants FROM sys.plans");
    EXPECT_NE(view.find("p,admin,enabled,public.results, public.models,public.results, "
                        "public.models,low WHEN ARG(1) < 10 ORDER (models, results); high ELSE "
                        "ORDER (results, models)"),
              std::string::npos)
        << view;
    EXPECT_EQ(Run("SHOW PLANS"), Run("SELECT * FROM sys.plans"));
}

TEST_F(AdminPlanTest, WhatCreatePlanCannotMakeAPlanOfIsRefusedByName) {
    const std::string join = Where("5");
    // A statement over one relation has no order to search (CX-S3b), and one
    // with a subquery predicate takes no plan.
    EXPECT_EQ(CodeOf("CREATE PLAN p FOR SELECT pnl FROM results WHERE pnl > 5 AS SEARCHED"),
              StatusCode::kUnsupported);
    EXPECT_EQ(CodeOf("CREATE PLAN p FOR SELECT pnl FROM results WHERE pnl IN (SELECT id FROM "
                     "models) VARIANT v ELSE ORDER (results)"),
              StatusCode::kNotImplemented);
    EXPECT_EQ(CodeOf("CREATE PLAN p FOR SELECT results.pnl FROM results JOIN models ON "
                     "results.model_id = models.id WHERE results.pnl IN (SELECT id FROM models) "
                     "AS SEARCHED"),
              StatusCode::kNotImplemented);
    EXPECT_EQ(CodeOf("CREATE PLAN p FOR " + Where("?") + kModelsFirst),
              StatusCode::kNotImplemented);
    EXPECT_EQ(CodeOf("CREATE PLAN captured_mine FOR " + join + kModelsFirst),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(CodeOf("CREATE PLAN p FOR INSERT INTO models VALUES (99) AS SEARCHED"),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(CodeOf("CREATE PLAN p FOR " + join + " VARIANT v ELSE ORDER (models, loans)"),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(CodeOf("CREATE PLAN p FOR " + join + " VARIANT v ELSE ORDER (models)"),
              StatusCode::kInvalidArgument);
    // A binding two relations share is the statement's refusal, given first.
    const DispatchOutcome twice =
        Do("CREATE PLAN p FOR SELECT models.name FROM models JOIN models ON models.id = "
           "models.id VARIANT v ELSE ORDER (models, models)");
    EXPECT_EQ(twice.response.rfind("ERR", 0), 0u) << twice.response;
    EXPECT_EQ(twice.response.find("ORDER names"), std::string::npos) << twice.response;
    // ARG(2), and the statement has one value position.
    const DispatchOutcome past =
        Do("CREATE PLAN p FOR " + join +
           " VARIANT a WHEN ARG(2) > 1 ORDER (models, results) VARIANT b ELSE ORDER (results, "
           "models)");
    EXPECT_EQ(past.status.code(), StatusCode::kInvalidArgument) << past.response;
    EXPECT_NE(past.response.find("1 value position"), std::string::npos) << past.response;
    // The statement's own refusal is the statement's.
    EXPECT_EQ(CodeOf("CREATE PLAN p FOR SELECT results.pnl FROM results JOIN nowhere ON "
                     "results.model_id = nowhere.id AS SEARCHED"),
              StatusCode::kNotFound);
    EXPECT_TRUE(PlanRows().empty());
    EXPECT_EQ(table().plans(), 0u);

    // Autocommit only.
    Session txn;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &txn).response.rfind("ERR", 0), std::string::npos);
    EXPECT_EQ(dispatcher_->Dispatch("CREATE PLAN p FOR " + join + kModelsFirst, &txn)
                  .status.code(),
              StatusCode::kNotImplemented);
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &txn).response.rfind("ERR", 0),
              std::string::npos);

    // One plan a shape, and one plan a name.
    Ok("CREATE PLAN p FOR " + join + kModelsFirst);
    const DispatchOutcome again = Do("CREATE PLAN q FOR " + Where("77") + kAsWritten);
    EXPECT_EQ(again.status.code(), StatusCode::kAlreadyExists) << again.response;
    EXPECT_NE(again.response.find("plan 'p' already holds"), std::string::npos) << again.response;
    EXPECT_EQ(CodeOf("CREATE PLAN P FOR SELECT models.name FROM models JOIN results ON "
                     "results.model_id = models.id VARIANT v ELSE ORDER (results, models)"),
              StatusCode::kAlreadyExists);
    EXPECT_EQ(PlanRows().size(), 1u);

    // Another plan DDL holds the claim: retryable, nothing written.
    {
        const exec::PlanDdlClaim busy(table());
        EXPECT_EQ(CodeOf("DROP PLAN p"), StatusCode::kTxnConflict);
        EXPECT_EQ(CodeOf("ALTER PLAN p DISABLE"), StatusCode::kTxnConflict);
        EXPECT_EQ(CodeOf("CREATE PLAN r FOR SELECT models.name FROM models JOIN results ON "
                         "results.model_id = models.id AS SEARCHED"),
                  StatusCode::kTxnConflict);
    }
    EXPECT_EQ(PlanRows().size(), 1u);
}

TEST_F(AdminPlanTest, DisableStopsServingThePlanAndEnableServesItAgain) {
    Ok("CREATE PLAN p FOR " + Where("5") + kAsWritten);
    const std::uint64_t first_id = PlanRows().front().id;
    const std::string disabled = Run("ALTER PLAN p DISABLE");
    ASSERT_EQ(disabled.rfind("ALTERED PLAN name=p plan_id=", 0), 0u) << disabled;
    EXPECT_NE(disabled.find(" state=disabled"), std::string::npos) << disabled;
    // One row, a new one.
    std::vector<exec::PlanRow> rows = PlanRows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_GT(rows.front().id, first_id);
    EXPECT_FALSE(rows.front().enabled);
    EXPECT_EQ(table().plans(), 1u);
    EXPECT_EQ(table().enabled(), 0u);
    // The shape goes to the next source: under `fixed`, the search, captured.
    const std::string searched = Plan(Where("5"));
    EXPECT_EQ(searched.find("plan=p"), std::string::npos) << searched;
    EXPECT_NE(searched.find(" capture=first"), std::string::npos) << searched;
    EXPECT_NE(Run("SELECT name, state FROM sys.plans").find("p,disabled"), std::string::npos);
    // A disabled plan still holds its shape.
    EXPECT_EQ(CodeOf("CREATE PLAN q FOR " + Where("5") + kModelsFirst),
              StatusCode::kAlreadyExists);
    // Disabling again writes nothing.
    const std::uint64_t disabled_id = PlanRows().back().id;
    Ok("ALTER PLAN p DISABLE");
    EXPECT_EQ(PlanRows().back().id, disabled_id);

    Ok("ALTER PLAN p ENABLE");
    EXPECT_NE(Plan(Where("5")).find("planned order=plan plan=p/as_written\\n"),
              std::string::npos);
    EXPECT_EQ(table().enabled(), 1u);

    EXPECT_EQ(CodeOf("ALTER PLAN nobody ENABLE"), StatusCode::kNotFound);
    // A captured plan has no state to alter.
    ASSERT_TRUE(exec::PersistCapturedPlans(boot_->catalog, store_, nullptr,
                                           dispatcher_->captured_plans(), 42)
                    .ok());
    std::string captured_name;
    for (const exec::PlanRow& row : PlanRows()) {
        if (row.captured) captured_name = row.name;
    }
    ASSERT_FALSE(captured_name.empty());
    EXPECT_EQ(CodeOf("ALTER PLAN " + captured_name + " DISABLE"), StatusCode::kNotImplemented);
}

TEST_F(AdminPlanTest, ReplaceVariantChangesARuleOrAnOrderAndKeepsTheListsShape) {
    Ok("CREATE PLAN p FOR " + Where("5") +
       " VARIANT low WHEN ARG(1) < 10 ORDER (models, results)"
       " VARIANT high ELSE ORDER (results, models)");
    ASSERT_NE(Plan(Where("50")).find("plan=p/high\\n"), std::string::npos);
    Ok("ALTER PLAN p REPLACE VARIANT low WHEN ARG(1) < 100 ORDER (models, results)");
    EXPECT_NE(Plan(Where("50")).find("plan=p/low\\n"), std::string::npos);
    // The ELSE variant's order.
    ASSERT_EQ(Plan(Where("500")).find("reordered=1"), std::string::npos);
    Ok("ALTER PLAN p REPLACE VARIANT HIGH ELSE ORDER (models, results)");
    EXPECT_NE(Plan(Where("500")).find("reordered=1"), std::string::npos);
    EXPECT_EQ(PlanRows().size(), 1u);

    // CX-R4: the list keeps its ELSE, last.
    EXPECT_EQ(CodeOf("ALTER PLAN p REPLACE VARIANT high WHEN ARG(1) < 5 ORDER (models, results)"),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(CodeOf("ALTER PLAN p REPLACE VARIANT low ELSE ORDER (models, results)"),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(CodeOf("ALTER PLAN p REPLACE VARIANT mid WHEN ARG(1) < 5 ORDER (models, results)"),
              StatusCode::kNotFound);
    EXPECT_EQ(CodeOf("ALTER PLAN p REPLACE VARIANT low WHEN ARG(2) < 5 ORDER (models, results)"),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(CodeOf("ALTER PLAN p REPLACE VARIANT low WHEN ARG(1) < 5 ORDER (models, models)"),
              StatusCode::kInvalidArgument);
    EXPECT_NE(Plan(Where("50")).find("plan=p/low\\n"), std::string::npos) << "unchanged";
}

TEST_F(AdminPlanTest, AVariantIsDeclinedNeverForcedAndTheWrittenOrderRuns) {
    // CC-Q10 per binding, as for a captured order: LIMIT is a literal, so
    // an order whose added sort is a top-N heap under one LIMIT is an
    // unbounded sort under a larger one. That statement runs as written.
    Ok("CREATE TABLE a (id int64, v int64) BTREE");
    Ok("CREATE TABLE b (id int64, a_id int64) BTREE");
    for (int i = 1; i <= 64; ++i) {
        Ok("INSERT INTO a VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
    }
    for (int i = 1; i <= 16; ++i) {
        Ok("INSERT INTO b VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
    }
    dispatcher_->set_sort_max_rows(10);
    dispatcher_->set_join_build_max_rows(0);
    const std::string head = "SELECT a.v, b.id FROM a JOIN b ON a.id = b.a_id ORDER BY a.id LIMIT ";
    // CX-R2 at CREATE: a variant that cannot run for the example is no plan.
    EXPECT_EQ(CodeOf("CREATE PLAN p FOR " + head + "100 VARIANT v ELSE ORDER (b, a)"),
              StatusCode::kInvalidArgument);
    EXPECT_TRUE(PlanRows().empty());
    Ok("CREATE PLAN p FOR " + head + "5 VARIANT v ELSE ORDER (b, a)");
    const std::string bounded = Plan(head + "6");
    EXPECT_NE(bounded.find("planned order=plan plan=p/v\\n"), std::string::npos) << bounded;
    EXPECT_NE(bounded.find("reordered=1"), std::string::npos) << bounded;
    const std::string wide = Plan(head + "100");
    EXPECT_NE(wide.find("planned order=written plan=p/v plan_declined=1\\n"), std::string::npos)
        << wide;
    EXPECT_EQ(wide.find("reordered=1"), std::string::npos) << wide;
    EXPECT_EQ(Rows(head + "100").size(), 16u);
    EXPECT_EQ(table().declined(), 2u);
    EXPECT_NE(Run("SHOW META").find("join_order,plan_declined,2\\n"), std::string::npos);
}

TEST_F(AdminPlanTest, ATupleKeyChainIsOrderedByItsPlanAndAnUnmanagedCompileAsksNothing) {
    // A lookup, then a probe: the search never orders it and the captured
    // table is never asked of it (CX-R6).
    const std::string point =
        "SELECT results.pnl, models.name FROM results JOIN models ON results.model_id = "
        "models.id WHERE results.id = 5";
    ASSERT_EQ(Plan(point).find("planned order="), std::string::npos);
    const std::uint64_t served = table().served();
    (void)Run(point);
    (void)Run("SELECT pnl FROM results WHERE id = 5");
    EXPECT_EQ(table().served(), served);
    // W4: an administrator's plan is always possible - and then it runs.
    Ok("CREATE PLAN p FOR " + point + kModelsFirst);
    const std::string managed = Plan(point);
    EXPECT_NE(managed.find("planned order=plan plan=p/models_first\\n"), std::string::npos)
        << managed;
    EXPECT_EQ(Rows(point).size(), 1u);
    // Another shape is not this plan's, and a single relation asks nothing.
    const std::uint64_t now = table().served();
    (void)Run("SELECT pnl FROM results WHERE id = 5");
    EXPECT_EQ(table().served(), now);

    // `written` with no plan for the shape compiles as it always did.
    Ok("SET JOIN_ORDER written");
    const std::string written = Plan(Where("5"));
    EXPECT_EQ(written.find("planned order="), std::string::npos) << written;
}

TEST_F(AdminPlanTest, APlanServedRunStillFoldsItsCountsForTheStatistics) {
    // CX-Q2: the statistics are gathered whatever chose the order, so a
    // `DROP PLAN` or an `AS SEARCHED` after it prices from what the shape's
    // runs measured - except under `written`, which gathers nothing.
    Ok("CREATE PLAN p FOR " + Where("5") + kModelsFirst);
    const std::string sql = Where("5");
    parser::Parser parser(sql);
    auto parsed = parser.Parse();
    ASSERT_TRUE(parsed.ok());
    ASSERT_TRUE(parser.fingerprint().has_value());
    exec::PlanOptions options;
    options.admin = &table();
    options.fetch_id = parser.fingerprint()->fetch_id;
    options.statement = sql;
    const auto& stmt = std::get<parser::SelectStmt>(parsed.value());
    auto searched = exec::Compile(boot_->catalog, stmt, nullptr, nullptr, nullptr, &options);
    ASSERT_TRUE(searched.ok()) << searched.status().message();
    ASSERT_TRUE(searched.value().plan_note.has_value());
    EXPECT_EQ(searched.value().plan_note->source, exec::PlanSource::kAdmin);
    EXPECT_EQ(searched.value().planned_relations.size(), 2u);
    ASSERT_TRUE(searched.value().planned.has_value());
    EXPECT_EQ(searched.value().planned->fetch_id, options.fetch_id);
    options.search = false;
    auto written = exec::Compile(boot_->catalog, stmt, nullptr, nullptr, nullptr, &options);
    ASSERT_TRUE(written.ok());
    EXPECT_EQ(written.value().plan_note->source, exec::PlanSource::kAdmin);
    EXPECT_TRUE(written.value().planned_relations.empty());
    EXPECT_FALSE(written.value().planned.has_value());
}

TEST_F(AdminPlanTest, ForOneOrderTheChainIsTheSameWhoeverChoseTheOrder) {
    // CX-S2 (parser-v2.md I12, the rewrite inventory): every rewrite is
    // decided by the text, the catalog and the order - never by `join_order`
    // or by what chose the order. So ANALYZE's plan, less its `planned`
    // line, is one text per order.
    const std::string sql =
        "SELECT results.pnl, models.name FROM results JOIN models ON results.model_id = "
        "models.id WHERE models.id BETWEEN 2 AND 3";
    const auto chain_of = [&](const std::string& statement) {
        std::string plan = Plan(statement);
        EXPECT_NE(plan.rfind("ERR", 0), 0u) << plan;
        // Out: the `planned ...` line, which says who chose and is not the
        // chain. The header's `reordered=1` stays: it is read off the
        // chain's step ids, so it is the same for one order.
        if (const std::size_t at = plan.find("planned order="); at != std::string::npos) {
            plan.erase(at, plan.find("\\n", at) + 2 - at);
        }
        return plan;
    };
    const auto derived_in = [](const std::string& plan) {
        std::size_t n = 0;
        for (std::size_t at = plan.find(" derived"); at != std::string::npos;
             at = plan.find(" derived", at + 1)) {
            ++n;
        }
        return n;
    };
    // The chain under each value of `join_order`, a plan in force.
    const auto under_each_value = [&](const std::string& statement, const std::string& want) {
        for (const char* value : {"written", "fixed", "planned"}) {
            Ok(std::string("SET JOIN_ORDER ") + value);
            EXPECT_EQ(chain_of(statement), want) << value << ": " << statement;
        }
    };
    // The written order: the option's own, then an administrator's plan
    // saying the same under each value.
    Ok("SET JOIN_ORDER written");
    const std::string written = chain_of(sql);
    ASSERT_EQ(written.find("reordered=1"), std::string::npos) << written;
    // Class (a): both bounds cross to `results.model_id`, under `written` too.
    EXPECT_EQ(derived_in(written), 2u) << written;
    Ok("CREATE PLAN keep FOR " + sql + kAsWritten);
    under_each_value(sql, written);
    Ok("DROP PLAN keep");

    // The other order: the search's, the captured one, and a plan's.
    Ok("SET JOIN_ORDER planned");
    const std::string searched = chain_of(sql);
    ASSERT_NE(searched.find("reordered=1"), std::string::npos) << searched;
    // The derived set does not depend on the order.
    EXPECT_EQ(derived_in(searched), 2u) << searched;
    Ok("SET JOIN_ORDER fixed");
    ASSERT_NE(Plan(sql).find(" capture=first"), std::string::npos);
    EXPECT_EQ(chain_of(sql), searched) << "captured";
    Ok("CREATE PLAN turn FOR " + sql + kModelsFirst);
    under_each_value(sql, searched);
    Ok("DROP PLAN turn");

    // With a sort in play: in the written order `ORDER BY results.id` is
    // the driving step's tuple key order and sorts nothing; in the other it
    // is a top-N heap. Each is one text whatever the value.
    const std::string sorted = sql + " ORDER BY results.id LIMIT 3";
    Ok("SET JOIN_ORDER written");
    const std::string sorted_written = chain_of(sorted);
    Ok("CREATE PLAN keep_sorted FOR " + sorted + kAsWritten);
    under_each_value(sorted, sorted_written);
    Ok("DROP PLAN keep_sorted");
    Ok("CREATE PLAN turn_sorted FOR " + sorted + kModelsFirst);
    Ok("SET JOIN_ORDER written");
    const std::string sorted_turned = chain_of(sorted);
    EXPECT_NE(sorted_turned.find("reordered=1"), std::string::npos) << sorted_turned;
    EXPECT_NE(sorted_turned, sorted_written);
    under_each_value(sorted, sorted_turned);
    EXPECT_EQ(Rows(sorted).size(), 3u);
}

// The step line ANALYZE prints for the relation at written position `id`.
std::string StepLine(const std::string& plan, int id) {
    const std::size_t at = plan.find("step " + std::to_string(id) + " ");
    if (at == std::string::npos) return {};
    return plan.substr(at, plan.find("\\n", at) - at);
}

TEST_F(AdminPlanTest, APinRestrictsTheLadderAndTheRowsAreTheSame) {
    // CX-S3a: results is written first (step 0), models second (step 1),
    // and results.model_id carries an index.
    Ok("CREATE INDEX results_model ON results (model_id)");
    const std::string sql = Where("5");
    const std::vector<std::string> rows = Rows(sql);
    ASSERT_EQ(rows.size(), 58u);
    // Models first, unpinned: the ladder enters results by its index, and
    // says nothing of whose choice that was.
    Ok("CREATE PLAN p FOR " + sql + kModelsFirst);
    std::string plan = Plan(sql);
    EXPECT_EQ(StepLine(plan, 0).rfind("step 0 IndexProbe ", 0), 0u) << plan;
    EXPECT_EQ(plan.find("arm=plan"), std::string::npos) << plan;
    Ok("DROP PLAN p");

    const auto pinned = [&](const std::string& access) {
        const std::string made = Run("CREATE PLAN p FOR " + sql + kModelsFirst + access);
        EXPECT_EQ(made.rfind("CREATED PLAN", 0), 0u) << access << " -> " << made;
        const std::string out = Plan(sql);
        EXPECT_NE(out.find("planned order=plan plan=p/models_first\\n"), std::string::npos)
            << access << ": " << out;
        EXPECT_EQ(Rows(sql), rows) << access;  // CX-R2
        Ok("DROP PLAN p");
        return out;
    };
    // The walk, where the ladder would have taken the index.
    plan = pinned(" ACCESS results SCAN");
    EXPECT_EQ(StepLine(plan, 0).find("Index"), std::string::npos) << plan;
    EXPECT_NE(StepLine(plan, 0).find(" arm=plan"), std::string::npos) << plan;
    EXPECT_EQ(StepLine(plan, 1).find(" arm=plan"), std::string::npos) << plan;
    // The same by forbidding the index.
    plan = pinned(" ACCESS results NOT INDEX");
    EXPECT_EQ(StepLine(plan, 0).find("Index"), std::string::npos) << plan;
    EXPECT_NE(StepLine(plan, 0).find(" arm=plan"), std::string::npos) << plan;
    // The index by name: what the ladder took anyway, now held to it.
    plan = pinned(" ACCESS results INDEX results_model, models SCAN");
    EXPECT_EQ(StepLine(plan, 0).rfind("step 0 IndexProbe ", 0), 0u) << plan;
    EXPECT_NE(StepLine(plan, 0).find(" arm=plan"), std::string::npos) << plan;
    EXPECT_NE(StepLine(plan, 1).find(" arm=plan"), std::string::npos) << plan;

    // In the written order models is a tuple key probe; kept from it, it is
    // walked, and the join's rows are the same.
    Ok("CREATE PLAN p FOR " + sql + std::string(kAsWritten) + " ACCESS models NOT TUPLE_KEY");
    plan = Plan(sql);
    EXPECT_EQ(StepLine(plan, 1).find("Probe"), std::string::npos) << plan;
    EXPECT_NE(StepLine(plan, 1).find(" arm=plan"), std::string::npos) << plan;
    EXPECT_EQ(Rows(sql), rows);
    EXPECT_NE(Run("SELECT variants FROM sys.plans")
                  .find("as_written ELSE ORDER (results, models) ACCESS models NOT TUPLE_KEY"),
              std::string::npos);
    Ok("DROP PLAN p");
    Ok("CREATE PLAN p FOR " + sql + std::string(kAsWritten) + " ACCESS models TUPLE_KEY");
    plan = Plan(sql);
    EXPECT_EQ(StepLine(plan, 1).rfind("step 1 Probe ", 0), 0u) << plan;
    EXPECT_NE(StepLine(plan, 1).find(" arm=plan"), std::string::npos) << plan;
    EXPECT_EQ(Rows(sql), rows);
}

TEST_F(AdminPlanTest, AKeyedPinIsMetByTheStrictRangeAWalkRankWouldOtherwiseTake) {
    // results' step carries a strict tuple key range and an unindexed
    // equality: unpinned, the filter scan outranks the strict range (ruling
    // (a) at TupleKeyRangeOf). The range is a tuple key arm the ladder can
    // take for the step, so a TUPLE_KEY pin is met by it - the walk ranks are
    // not the pin's class and do not stand in its way.
    const std::string sql =
        "SELECT results.pnl, models.name FROM results JOIN models ON results.model_id = "
        "models.id WHERE results.id > 3 AND results.id < 40 AND results.pnl = 10";
    const std::vector<std::string> rows = Rows(sql);
    ASSERT_EQ(rows.size(), 1u);
    Ok("CREATE PLAN p FOR " + sql + std::string(kAsWritten));
    std::string plan = Plan(sql);
    EXPECT_EQ(StepLine(plan, 0).rfind("step 0 FilterScan ", 0), 0u) << plan;
    Ok("DROP PLAN p");
    const std::string made =
        Run("CREATE PLAN p FOR " + sql + std::string(kAsWritten) + " ACCESS results TUPLE_KEY");
    ASSERT_EQ(made.rfind("CREATED PLAN", 0), 0u) << made;
    plan = Plan(sql);
    EXPECT_NE(plan.find("planned order=plan plan=p/as_written\\n"), std::string::npos) << plan;
    EXPECT_EQ(StepLine(plan, 0).rfind("step 0 Range ", 0), 0u) << plan;
    EXPECT_EQ(Rows(sql), rows);  // CX-R2
}

TEST_F(AdminPlanTest, APinNoArmMeetsIsRefusedAtCreateAndDeclinedAtACompile) {
    Ok("CREATE INDEX results_model ON results (model_id)");
    const std::string sql = Where("5");
    const std::vector<std::string> rows = Rows(sql);
    // CX-R2 at CREATE: a pin is never forced, so one nothing meets is no plan.
    for (const std::string access :
         {" ACCESS models CABIN", " ACCESS results INDEX no_such_index",
          " ACCESS models INDEX results_model", " ACCESS results TUPLE_KEY"}) {
        const DispatchOutcome out = Do("CREATE PLAN p FOR " + sql + kModelsFirst + access);
        EXPECT_EQ(out.status.code(), StatusCode::kInvalidArgument) << access << out.response;
        EXPECT_NE(out.response.find("cannot take the pinned arm"), std::string::npos)
            << out.response;
    }
    EXPECT_TRUE(PlanRows().empty());

    Ok("CREATE PLAN p FOR " + sql + kModelsFirst + " ACCESS results INDEX results_model");
    // CX-Q4 (a): the pinned index cannot be dropped under an enabled plan.
    const std::string refused = Run("DROP INDEX results_model");
    EXPECT_EQ(refused.rfind("ERR plan 'p' pins this index", 0), 0u) << refused;
    EXPECT_NE(Plan(sql).find("step 0 IndexProbe "), std::string::npos);
    // Disabled, the plan declares nothing; enabled again over the missing
    // index, its variant is declined statement by statement, never forced.
    Ok("ALTER PLAN p DISABLE");
    ASSERT_EQ(Run("DROP INDEX results_model").rfind("ERR", 0), std::string::npos);
    Ok("ALTER PLAN p ENABLE");
    const std::string declined = Plan(sql);
    EXPECT_NE(declined.find("planned order=written plan=p/models_first plan_declined=1\\n"),
              std::string::npos)
        << declined;
    EXPECT_EQ(declined.find("arm=plan"), std::string::npos) << declined;
    EXPECT_EQ(Rows(sql), rows);
    EXPECT_GE(table().declined(), 2u);

    // `REPLACE VARIANT` compiles nothing, and still knows an index by name -
    // refused at the name's own byte.
    const std::string replace =
        "ALTER PLAN p REPLACE VARIANT models_first ELSE ORDER (models, results) "
        "ACCESS results INDEX results_model";
    EXPECT_EQ(CodeOf(replace), StatusCode::kNotFound);
    EXPECT_NE(Run(replace).find("(byte " + std::to_string(replace.rfind("results_model")) + ")"),
              std::string::npos)
        << Run(replace);
    Ok("CREATE INDEX results_model ON results (model_id)");
    Ok("ALTER PLAN p REPLACE VARIANT models_first ELSE ORDER (models, results) ACCESS results "
       "INDEX results_model");
    const std::string served = Plan(sql);
    EXPECT_NE(served.find("planned order=plan plan=p/models_first\\n"), std::string::npos)
        << served;
    EXPECT_NE(StepLine(served, 0).find(" arm=plan"), std::string::npos) << served;

    // The pins are the row's, and a mount's.
    exec::CapturedPlans captured;
    exec::AdminPlans next_run;
    ASSERT_TRUE(exec::LoadPlans(boot_->catalog, store_, nullptr, captured, next_run).ok());
    const std::vector<exec::PlanRow> stored = PlanRows();
    ASSERT_EQ(stored.size(), 1u);
    const auto loaded = next_run.Find(stored.front().key);
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->variants.front().access.size(), 2u);
    EXPECT_EQ(loaded->variants.front().access[0],
              (exec::ArmPin{exec::ArmPin::Arm::kIndex, false, "results_model"}));
    EXPECT_EQ(next_run.EnabledPinningIndex(results_, "RESULTS_MODEL"),
              std::optional<std::string>("p"));
    EXPECT_FALSE(next_run.EnabledPinningIndex(models_, "results_model").has_value());
}

// CX-S3b: a statement over one relation has no order and an arm to pin.
TEST_F(AdminPlanTest, AOneRelationStatementTakesAPlanForItsArm) {
    Ok("CREATE INDEX results_model ON results (model_id)");
    const std::string sql = "SELECT pnl FROM results WHERE model_id = 3";
    const std::string other = "SELECT pnl FROM results WHERE model_id = 4";
    const std::vector<std::string> rows = Rows(sql);
    ASSERT_EQ(rows.size(), 8u);
    EXPECT_EQ(StepLine(Plan(sql), 0).rfind("step 0 IndexProbe ", 0), 0u) << Plan(sql);

    // With no one-relation plan enabled its compile asks the table nothing,
    // whatever join plans there are (CX-R6).
    Ok("CREATE PLAN j FOR " + Where("5") + kModelsFirst);
    EXPECT_TRUE(table().any());
    EXPECT_FALSE(table().any_single());
    const std::uint64_t served = table().served();
    (void)Run(sql);
    (void)Run("SELECT pnl FROM results WHERE id = 5");
    EXPECT_EQ(table().served(), served);

    // The plan: the ladder less its index arm, for every binding of the
    // shape, and the same rows.
    Ok("CREATE PLAN p FOR " + sql + " VARIANT v ELSE ORDER (results) ACCESS results NOT INDEX");
    EXPECT_TRUE(table().any_single());
    for (const std::string& statement : {sql, other}) {
        const std::string plan = Plan(statement);
        EXPECT_NE(plan.find("planned order=plan plan=p/v\\n"), std::string::npos) << plan;
        // An indexed column's equality is no filter scan: with the index
        // ruled out the step is the walk.
        EXPECT_EQ(StepLine(plan, 0).rfind("step 0 Scan ", 0), 0u) << plan;
        EXPECT_NE(StepLine(plan, 0).find("arm=plan"), std::string::npos) << plan;
    }
    EXPECT_EQ(Rows(sql), rows);
    // Another shape over the relation is not the plan's.
    EXPECT_EQ(Plan("SELECT pnl FROM results WHERE model_id = 3 AND pnl > 1").find("plan=p/"),
              std::string::npos);

    // A pin no arm meets: refused at CREATE, declined at a compile - the
    // statement runs unpinned and is counted.
    EXPECT_EQ(CodeOf("CREATE PLAN q FOR SELECT pnl FROM results WHERE model_id = 3 AND pnl > 1 "
                     "VARIANT v ELSE ORDER (results) ACCESS results TUPLE_KEY"),
              StatusCode::kInvalidArgument);
    Ok("ALTER PLAN p REPLACE VARIANT v ELSE ORDER (results) ACCESS results INDEX results_model");
    EXPECT_EQ(StepLine(Plan(sql), 0).rfind("step 0 IndexProbe ", 0), 0u) << Plan(sql);
    Ok("ALTER PLAN p DISABLE");
    EXPECT_FALSE(table().any_single());
    Ok("DROP INDEX results_model");
    Ok("ALTER PLAN p ENABLE");
    const std::uint64_t declined = table().declined();
    const std::string walked = Plan(sql);
    EXPECT_NE(walked.find("planned order=written plan=p/v plan_declined=1\\n"),
              std::string::npos)
        << walked;
    EXPECT_EQ(walked.find("arm=plan"), std::string::npos) << walked;
    EXPECT_EQ(table().declined(), declined + 1);
    EXPECT_EQ(Rows(sql), rows);

    // The relation's drop is RESTRICTed by it, as by a join's plan.
    Ok("DROP PLAN j");
    const std::string restricted = Run("DROP TABLE results");
    EXPECT_EQ(restricted.rfind("ERR", 0), 0u) << restricted;
    EXPECT_NE(restricted.find("'p'"), std::string::npos) << restricted;
    Ok("DROP PLAN p");
    EXPECT_FALSE(table().any_single());
    EXPECT_EQ(Plan(sql).find("planned order="), std::string::npos);
}

TEST_F(AdminPlanTest, StatementStatsNameThePlanAndTheVariantThatRan) {
    Ok("CREATE PLAN stats_plan FOR " + Where("5") +
       " VARIANT low WHEN ARG(1) < 10 ORDER (models, results)"
       " VARIANT high ELSE ORDER (results, models)");
    (void)Run(Where("3"));
    std::string view = Run("SELECT plan_source, plan_name, variant, kind FROM sys.statement_stats");
    EXPECT_EQ(view.rfind("plan_source,plan_name,variant,kind\\n", 0), 0u) << view;
    EXPECT_NE(view.find("admin,stats_plan,low,SELECT"), std::string::npos) << view;
    (void)Run(Where("30"));
    view = Run("SELECT plan_source, plan_name, variant, kind FROM sys.statement_stats");
    EXPECT_NE(view.find("admin,stats_plan,high,SELECT"), std::string::npos) << view;
    EXPECT_EQ(view.find("admin,stats_plan,low,SELECT"), std::string::npos) << view;
    // With the plan gone the shape reads its next source.
    Ok("DROP PLAN stats_plan");
    (void)Run(Where("30"));
    view = Run("SELECT plan_source, plan_name, variant, kind FROM sys.statement_stats");
    EXPECT_EQ(view.find("stats_plan"), std::string::npos) << view;
    EXPECT_NE(view.find("searched,NULL,NULL,SELECT"), std::string::npos) << view;
}

TEST_F(AdminPlanTest, AStreamedSelectsStatementStatsNameThePlanThatRan) {
    // The server's path: `DispatchAsync` hands a walking SELECT's walk on
    // (BA-S15), and the walk's end rebuilds the outcome (`SelectRun::
    // Execute`), which must keep the plan the compile recorded, or the
    // shape's row reads NULL, or what an earlier statement left.
    Ok("CREATE PLAN streamed_plan FOR " + Where("5") + kModelsFirst);
    const std::string sql = Where("4");
    DispatchOutcome out;
    auto task = sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                    dispatcher_->DispatchAsync(sql, nullptr, &out));
    bool done = false;
    for (int i = 0; i < 10000 && !done; ++i) done = task->Poll() == sched::PollResult::kDone;
    ASSERT_TRUE(done);
    ASSERT_NE(out.response.rfind("ERR", 0), 0u) << out.response;
    const std::string view =
        Run("SELECT plan_source, plan_name, variant, kind FROM sys.statement_stats");
    EXPECT_NE(view.find("admin,streamed_plan,models_first,SELECT"), std::string::npos) << view;
}

TEST_F(AdminPlanTest, AnEnabledPlanRestrictsItsRelationsDropAndADisabledOneReadsInvalid) {
    Ok("CREATE PLAN p FOR " + Where("5") + kModelsFirst);
    // CX-Q24 (a).
    const std::string refused = Run("DROP TABLE results");
    EXPECT_EQ(refused.rfind("ERR plan 'p' names this relation", 0), 0u) << refused;
    EXPECT_EQ(Rows(Where("5")).size(), 58u);
    Ok("ALTER PLAN p DISABLE");
    ASSERT_EQ(Run("DROP TABLE results").rfind("DROPPED", 0), 0u);
    // The administrator's object is not deleted with the relation. (This
    // rig has no log, so its drop's commit names no LSN and forgets nothing
    // in the table; `AdminPlanMountTest` pins the forget.)
    EXPECT_EQ(PlanRows().size(), 1u);
    EXPECT_NE(Run("SELECT name, origin, state FROM sys.plans").find("p,admin,invalid"),
              std::string::npos);
    // It can serve nothing again, and says so.
    EXPECT_EQ(CodeOf("ALTER PLAN p ENABLE"), StatusCode::kUnsupported);
    EXPECT_EQ(CodeOf("ALTER PLAN p REPLACE VARIANT models_first ELSE ORDER (results, models)"),
              StatusCode::kUnsupported);
    // The persisting task removes captured rows of a dropped relation, never
    // an administrator's.
    dispatcher_->captured_plans().ForgetRelation(results_);
    ASSERT_TRUE(exec::PersistCapturedPlans(boot_->catalog, store_, nullptr,
                                           dispatcher_->captured_plans(), 42)
                    .ok());
    EXPECT_EQ(PlanRows().size(), 1u);
    ASSERT_EQ(Run("DROP PLAN p").rfind("DROPPED PLAN name=p", 0), 0u);
    EXPECT_TRUE(PlanRows().empty());
}

TEST_F(AdminPlanTest, AMountServesAnEnabledPlanHoldsADisabledOneAndKeepsTheNewestRowOfAName) {
    Ok("CREATE PLAN p FOR " + Where("5") + kModelsFirst);
    Ok("CREATE PLAN q FOR SELECT models.name FROM models JOIN results ON results.model_id = "
       "models.id VARIANT v ELSE ORDER (results, models)");
    Ok("ALTER PLAN q DISABLE");
    exec::PlanRow p;
    for (const exec::PlanRow& row : PlanRows()) {
        if (row.name == "p") p = row;
    }
    ASSERT_EQ(p.name, "p");

    // An `ALTER PLAN` a crash cut short: the new row is written and the old
    // one still stands.
    exec::PlanRow newer = p;
    newer.variants.front().order = {0, 1};
    auto id = boot_->catalog.AllocateRowId(catalog::kSysPlansTable);
    ASSERT_TRUE(id.ok());
    exec::DeclarationRow raw;
    raw.id = id.value();
    raw.target_oid = p.key.oids[0];
    raw.aux = static_cast<std::int64_t>(p.key.fetch_id);
    raw.name = "p";
    raw.source_text = exec::EncodeAdminPlanText(newer, 99);
    ASSERT_TRUE(exec::InsertDeclaration(boot_->catalog, store_, nullptr, catalog::kSysPlansTable,
                                        "plan", raw, exec::DeclarationName::kUniqueByConstruction)
                    .ok());
    ASSERT_EQ(PlanRows().size(), 3u);

    exec::CapturedPlans captured;
    exec::AdminPlans next_run;
    auto unreadable = exec::LoadPlans(boot_->catalog, store_, nullptr, captured, next_run);
    ASSERT_TRUE(unreadable.ok()) << unreadable.status().message();
    EXPECT_EQ(unreadable.value(), 0u);
    EXPECT_EQ(next_run.plans(), 2u);
    EXPECT_EQ(next_run.enabled(), 1u);
    const auto served = next_run.Find(p.key);
    ASSERT_NE(served, nullptr);
    EXPECT_EQ(served->variants.front().order, newer.variants.front().order)
        << "the newer row is the plan";
    // The older row of the name is gone; q is held and not served.
    const std::vector<exec::PlanRow> rows = PlanRows();
    ASSERT_EQ(rows.size(), 2u);
    for (const exec::PlanRow& row : rows) {
        EXPECT_NE(row.id, p.id);
        if (row.name != "q") continue;
        EXPECT_EQ(next_run.Find(row.key), nullptr);
        EXPECT_EQ(next_run.Holder(row.key), std::optional<std::string>("q"));
    }
    // And `DROP PLAN` and `ALTER PLAN` go by the newest row of a name too.
    auto found = exec::FindPlan(boot_->catalog, store_, "P");
    ASSERT_TRUE(found.ok());
    EXPECT_EQ(found.value().id, raw.id);
}

// ---- A whole instance: a clean stop and a crash --------------------------------

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / ("kds_admin_plans_" + std::to_string(::getpid()) +
                                            "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::string Reply(Expeditor& db, const std::string& sql) {
    return db.dispatcher().Dispatch(sql).response;
}

TEST(AdminPlanMountTest, APlanOutlivesACleanStopAndACrashAndAServersDefaultRunsIt) {
    const std::string join =
        "SELECT freights.id FROM freights JOIN cargos ON freights.cargo_id = cargos.id WHERE "
        "freights.id > 0";
    TempDir live;
    TempDir crashed;
    {
        auto opened = reclaim_census::Mount(live.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        for (const std::string line :
             {"CREATE TABLE cargos (id int64, org_id int64) BTREE",
              "CREATE TABLE freights (id int64, cargo_id int64) BTREE",
              "INSERT INTO cargos VALUES (1, 1)", "INSERT INTO freights VALUES (1, 1)"}) {
            ASSERT_EQ(Reply(db, line).rfind("ERR", 0), std::string::npos) << line;
        }
        const std::string created =
            Reply(db, "CREATE PLAN p FOR " + join + " VARIANT v ELSE ORDER (cargos, freights)");
        ASSERT_EQ(created.rfind("CREATED PLAN name=p", 0), 0u) << created;
        ASSERT_EQ(Reply(db, "CREATE PLAN off FOR SELECT cargos.id FROM cargos JOIN freights ON "
                            "freights.cargo_id = cargos.id AS SEARCHED")
                      .rfind("CREATED PLAN name=off", 0),
                  0u);
        ASSERT_EQ(Reply(db, "ALTER PLAN off DISABLE").rfind("ALTERED PLAN", 0), 0u);
        // Answered once durable: the crash image holds what the replies said.
        reclaim_census::CrashImage(live.path, crashed.path);
        reclaim_census::CleanShutdownCheckpoint(db);
    }
    for (const fs::path& dir : {live.path, crashed.path}) {
        auto mounted = reclaim_census::Mount(dir);
        ASSERT_TRUE(mounted.ok()) << dir << ": " << mounted.status().message();
        Expeditor& db = *mounted.value();
        CurrentCoreGuard as(0);
        EXPECT_EQ(db.dispatcher().admin_plans().plans(), 2u) << dir;
        EXPECT_EQ(db.dispatcher().admin_plans().enabled(), 1u) << dir;
        const std::string plan = Reply(db, "ANALYZE " + join);
        EXPECT_NE(plan.find("planned order=plan plan=p/v\\n"), std::string::npos)
            << dir << ": " << plan;
        const std::string rows = Reply(db, "SELECT name, origin, state FROM sys.plans");
        EXPECT_NE(rows.find("p,admin,enabled"), std::string::npos) << rows;
        EXPECT_NE(rows.find("off,admin,disabled"), std::string::npos) << rows;
        // The DROP TABLE RESTRICT is in force from the mount.
        EXPECT_EQ(Reply(db, "DROP TABLE cargos").rfind("ERR plan 'p' names this relation", 0), 0u);
        reclaim_census::CleanShutdownCheckpoint(db);
    }
}

TEST(AdminPlanMountTest, ADisabledPlansRelationCanBeDroppedAndThePlanReadsInvalidFromThenOn) {
    TempDir live;
    {
        auto opened = reclaim_census::Mount(live.path);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        CurrentCoreGuard as(0);
        for (const std::string line :
             {"CREATE TABLE cargos (id int64, org_id int64) BTREE",
              "CREATE TABLE freights (id int64, cargo_id int64) BTREE",
              "CREATE PLAN p FOR SELECT freights.id FROM freights JOIN cargos ON "
              "freights.cargo_id = cargos.id VARIANT v ELSE ORDER (cargos, freights)",
              "ALTER PLAN p DISABLE"}) {
            ASSERT_EQ(Reply(db, line).rfind("ERR", 0), std::string::npos) << line;
        }
        EXPECT_EQ(db.dispatcher().admin_plans().plans(), 1u);
        // A drop that rolls back forgets nothing.
        Session txn;
        ASSERT_EQ(db.dispatcher().Dispatch("BEGIN", &txn).response.rfind("ERR", 0),
                  std::string::npos);
        ASSERT_EQ(db.dispatcher().Dispatch("DROP TABLE cargos", &txn).response.rfind("DROPPED", 0),
                  0u);
        ASSERT_EQ(db.dispatcher().Dispatch("ROLLBACK", &txn).response.rfind("ERR", 0),
                  std::string::npos);
        EXPECT_EQ(db.dispatcher().admin_plans().plans(), 1u);
        EXPECT_NE(Reply(db, "SELECT name, state FROM sys.plans").find("p,disabled"),
                  std::string::npos);
        // One that commits does: the table forgets the plan and the row stays.
        ASSERT_EQ(Reply(db, "DROP TABLE cargos").rfind("DROPPED", 0), 0u);
        EXPECT_EQ(db.dispatcher().admin_plans().plans(), 0u);
        EXPECT_NE(Reply(db, "SELECT name, state FROM sys.plans").find("p,invalid"),
                  std::string::npos);
        EXPECT_EQ(db.dispatcher().Dispatch("ALTER PLAN p ENABLE").status.code(),
                  StatusCode::kUnsupported);
        reclaim_census::CleanShutdownCheckpoint(db);
    }
    auto mounted = reclaim_census::Mount(live.path);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    Expeditor& db = *mounted.value();
    CurrentCoreGuard as(0);
    // A mount neither loads nor removes it: it is the administrator's to drop.
    EXPECT_EQ(db.dispatcher().admin_plans().plans(), 0u);
    EXPECT_NE(Reply(db, "SELECT name, state FROM sys.plans").find("p,invalid"), std::string::npos);
    EXPECT_EQ(Reply(db, "DROP PLAN p").rfind("DROPPED PLAN name=p", 0), 0u);
    EXPECT_EQ(Reply(db, "SELECT name FROM sys.plans").find("p"), std::string::npos);
    reclaim_census::CleanShutdownCheckpoint(db);
}

}  // namespace
}  // namespace kds::server
