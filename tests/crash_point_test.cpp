#include "kds/base/crash_point.hpp"

#include <gtest/gtest.h>

// RP7's crash-point facility. What is tested here is the *spec parser* and
// the unarmed path, which are the two things that can be wrong without
// anything dying; the kill itself is proved where it has to be, in
// `bench/txn_2pc_kill_matrix_probe.py`, by a process that does not come
// back.
//
// The parser matters because a spec that silently loses its tail arms
// nothing while reading as armed - the harness would then report "the
// process survived the point" for a point that was never set.

namespace kds::base {
namespace {

TEST(CrashPointArm, ABareNameArmsItsFirstHit) {
    const CrashPointArm arm = ParseCrashPointArm("coordinator.before_prepare");
    EXPECT_EQ(arm.name, "coordinator.before_prepare");
    EXPECT_EQ(arm.ordinal, 1u);
}

TEST(CrashPointArm, AnOrdinalSuffixSelectsTheNthHit) {
    const CrashPointArm arm = ParseCrashPointArm("participant.prepare_logged_predurable:2");
    EXPECT_EQ(arm.name, "participant.prepare_logged_predurable");
    EXPECT_EQ(arm.ordinal, 2u);
}

TEST(CrashPointArm, AnEmptySpecArmsNothing) {
    const CrashPointArm arm = ParseCrashPointArm("");
    EXPECT_TRUE(arm.name.empty());
}

TEST(CrashPointArm, ANonNumericSuffixStaysPartOfTheNameRatherThanBeingDropped) {
    // The failure this forbids: `a:b` parsed as name `a` would arm a point
    // that does not exist, and the harness would read the survival as a
    // property of the protocol.
    const CrashPointArm arm = ParseCrashPointArm("a:b");
    EXPECT_EQ(arm.name, "a:b");
    EXPECT_EQ(arm.ordinal, 1u);
}

TEST(CrashPointArm, ATrailingColonIsPartOfTheNameAndAZeroOrdinalIsNotAnOrdinal) {
    EXPECT_EQ(ParseCrashPointArm("a:").name, "a:");
    // Ordinals are 1-based, so `:0` names no hit; keeping it in the name
    // means the arm matches nothing, which is the safe reading.
    EXPECT_EQ(ParseCrashPointArm("a:0").name, "a:0");
}

TEST(CrashPointArm, OnlyTheLastColonIsTheSeparator) {
    const CrashPointArm arm = ParseCrashPointArm("a:b:3");
    EXPECT_EQ(arm.name, "a:b");
    EXPECT_EQ(arm.ordinal, 3u);
}

TEST(CrashPointHitTest, TheTestBinaryIsUnarmedAndEveryHitReturns) {
    // The proof that an un-armed process cannot be killed by this facility,
    // stated where it is cheapest: this very binary calls every point the
    // engine has and runs on. `KDS_CRASH_POINT` is not set for `ctest`, and
    // the arm is read once, so nothing a later test does can arm it.
    ASSERT_TRUE(ArmedCrashPoint().empty());
    CrashPointHit("coordinator.before_prepare");
    CrashPointHit("participant.prepare_logged_predurable");
    CrashPointHit("participant.prepare_durable_prereply");
    CrashPointHit("coordinator.prepared_predecide");
    CrashPointHit("coordinator.decided_presend");
    SUCCEED();
}

}  // namespace
}  // namespace kds::base
