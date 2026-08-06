#include "kds/server/config_file.hpp"

#include <string>

#include <gtest/gtest.h>

#include "kds/server/expeditor.hpp"

// A config file's job is to be unsurprising. Most of these assert that a
// mistake is *reported* rather than absorbed - a typo'd key that silently
// does nothing is the failure mode this parser exists to prevent.

namespace kds::server {
namespace {

ConfigFile ParseOk(std::string_view text) {
    auto config = ConfigFile::Parse(text, "test.conf");
    EXPECT_TRUE(config.ok()) << config.status().message();
    return config.ok() ? std::move(config.value()) : ConfigFile{};
}

TEST(ConfigFileTest, ParsesKeyValueLines) {
    ConfigFile config = ParseOk("data_file = kds.db\nport = 15432\n");

    EXPECT_EQ(config.size(), 2u);
    EXPECT_EQ(config.GetString("data_file").value(), "kds.db");
    EXPECT_EQ(config.GetUint("port").value(), 15432u);
}

TEST(ConfigFileTest, WhitespaceAroundKeysAndValuesIsIgnored) {
    ConfigFile config = ParseOk("   port   =    15432   \n\t log_file\t=\tkdb.log\t\n");
    EXPECT_EQ(config.GetUint("port").value(), 15432u);
    EXPECT_EQ(config.GetString("log_file").value(), "kdb.log");
}

TEST(ConfigFileTest, CommentsAndBlankLinesAreSkipped) {
    ConfigFile config = ParseOk(
        "# the data file\n"
        "\n"
        "data_file = kds.db   # trailing comment\n"
        "   \n"
        "# port = 9999\n"
        "port = 15432\n");

    EXPECT_EQ(config.size(), 2u);
    EXPECT_EQ(config.GetString("data_file").value(), "kds.db");
    EXPECT_EQ(config.GetUint("port").value(), 15432u);
}

TEST(ConfigFileTest, QuotesPreserveWhitespaceAndHashes) {
    ConfigFile config = ParseOk("log_dir = \"/var/log/my db\"\n");
    EXPECT_EQ(config.GetString("log_dir").value(), "/var/log/my db");
}

TEST(ConfigFileTest, MissingFileIsNotFoundNotAnEmptyConfig) {
    auto config = ConfigFile::Load("/nonexistent/kds/does-not-exist.conf");
    EXPECT_FALSE(config.ok());
    EXPECT_EQ(config.status().code(), StatusCode::kNotFound)
        << "a mistyped --config path must not silently start a default server";
}

TEST(ConfigFileTest, ALineWithoutAnEqualsIsRejected) {
    auto config = ConfigFile::Parse("port 15432\n", "test.conf");
    EXPECT_FALSE(config.ok());
    EXPECT_EQ(config.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(config.status().message().find("test.conf:1"), std::string::npos)
        << config.status().message();
}

TEST(ConfigFileTest, ADuplicateKeyIsRejectedRatherThanLastWins) {
    auto config = ConfigFile::Parse("port = 1\nport = 2\n", "test.conf");
    EXPECT_FALSE(config.ok());
    // Which one the operator meant is exactly what is unclear.
    EXPECT_NE(config.status().message().find("already set on line 1"), std::string::npos)
        << config.status().message();
}

TEST(ConfigFileTest, AnEmptyKeyIsRejected) {
    auto config = ConfigFile::Parse("= 5\n", "test.conf");
    EXPECT_FALSE(config.ok());
    EXPECT_EQ(config.status().code(), StatusCode::kInvalidArgument);
}

TEST(ConfigFileTest, AnAbsentKeyIsNotFound) {
    ConfigFile config = ParseOk("port = 1\n");
    EXPECT_FALSE(config.Has("log_file"));
    EXPECT_EQ(config.GetString("log_file").status().code(), StatusCode::kNotFound);
}

TEST(ConfigFileTest, ANonNumericValueForAnUintKeyNamesTheLine) {
    ConfigFile config = ParseOk("port = fifteen\n");
    auto port = config.GetUint("port");
    EXPECT_FALSE(port.ok());
    EXPECT_EQ(port.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(port.status().message().find("test.conf:1"), std::string::npos)
        << port.status().message();
}

TEST(ConfigFileTest, PartiallyNumericValuesAreRejected) {
    ConfigFile config = ParseOk("port = 15432abc\n");
    EXPECT_FALSE(config.GetUint("port").ok()) << "a trailing suffix must not be silently dropped";
}

TEST(ConfigFileTest, BooleansAcceptTheUsualSpellings) {
    ConfigFile config = ParseOk("a = true\nb = no\nc = ON\nd = 0\n");
    EXPECT_TRUE(config.GetBool("a").value());
    EXPECT_FALSE(config.GetBool("b").value());
    EXPECT_TRUE(config.GetBool("c").value());
    EXPECT_FALSE(config.GetBool("d").value());
    EXPECT_FALSE(ParseOk("a = maybe\n").GetBool("a").ok());
}

TEST(ConfigFileTest, UnknownKeysAreReportedInFileOrder) {
    ConfigFile config = ParseOk("port = 1\ntypo_one = x\ndata_file = d\ntypo_two = y\n");
    auto unknown = config.UnknownKeys({"port", "data_file"});
    EXPECT_EQ(unknown, (std::vector<std::string>{"typo_one", "typo_two"}));
}

// ---- Expeditor::Config overlay ------------------------------------------

TEST(ExpeditorConfigTest, DefaultsAreUsedForKeysTheFileOmits) {
    Expeditor::Config config;
    const std::string default_data_file = config.data_file;

    ASSERT_TRUE(config.ApplyFile(ParseOk("port = 6000\n")).ok());
    EXPECT_EQ(config.port, 6000);
    EXPECT_EQ(config.data_file, default_data_file) << "an omitted key must leave the default";
    EXPECT_EQ(config.log_file, "kdb.log");
}

TEST(ExpeditorConfigTest, EveryKnownKeyIsApplied) {
    Expeditor::Config config;
    ASSERT_TRUE(config
                    .ApplyFile(ParseOk("data_file = /srv/kds/main.db\n"
                                       "port = 6543\n"
                                       "wal_dir = /srv/kds/wal\n"
                                       "checkpoint_interval_ms = 250\n"
                                       "log_dir = /var/log/kds\n"
                                       "log_file = server.log\n"
                                       "log_level = debug\n"))
                    .ok());

    EXPECT_EQ(config.data_file, "/srv/kds/main.db");
    EXPECT_EQ(config.port, 6543);
    EXPECT_EQ(config.wal_dir, "/srv/kds/wal");
    EXPECT_EQ(config.checkpoint_interval_ns, 250'000'000u) << "ms in the file, ns internally";
    EXPECT_EQ(config.log_level, LogLevel::kDebug);
    EXPECT_EQ(config.LogPath(), "/var/log/kds/server.log");
}

TEST(ExpeditorConfigTest, AnUnknownKeyRefusesTheWholeFile) {
    Expeditor::Config config;
    Status s = config.ApplyFile(ParseOk("port = 6000\nchekpoint_interval_ms = 100\n"));

    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("chekpoint_interval_ms"), std::string::npos) << s.message();
    // A typo'd cadence key would otherwise leave the operator believing the
    // loss window is 100ms when it is still the 5s default.
}

TEST(ExpeditorConfigTest, AnOutOfRangePortIsRejected) {
    Expeditor::Config config;
    EXPECT_FALSE(config.ApplyFile(ParseOk("port = 70000\n")).ok());
    EXPECT_FALSE(config.ApplyFile(ParseOk("port = 0\n")).ok());
}

TEST(ExpeditorConfigTest, AnUnknownLogLevelIsRejected) {
    Expeditor::Config config;
    Status s = config.ApplyFile(ParseOk("log_level = chatty\n"));
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("chatty"), std::string::npos) << s.message();
}

TEST(ExpeditorConfigTest, LogPathJoinsDirAndFile) {
    Expeditor::Config config;

    config.log_dir = "";
    config.log_file = "kdb.log";
    EXPECT_EQ(config.LogPath(), "kdb.log");

    config.log_dir = "/var/log/kds";
    EXPECT_EQ(config.LogPath(), "/var/log/kds/kdb.log");

    config.log_dir = "/var/log/kds/";  // trailing slash must not double up
    EXPECT_EQ(config.LogPath(), "/var/log/kds/kdb.log");

    // An absolute log_file wins outright - the dir is not prepended.
    config.log_file = "/tmp/elsewhere.log";
    EXPECT_EQ(config.LogPath(), "/tmp/elsewhere.log");

    // No file means no file logging.
    config.log_file = "";
    EXPECT_TRUE(config.LogPath().empty());
}

TEST(ExpeditorConfigTest, ZeroCheckpointIntervalKeepsItsDisabledMeaning) {
    Expeditor::Config config;
    ASSERT_TRUE(config.ApplyFile(ParseOk("checkpoint_interval_ms = 0\n")).ok());
    EXPECT_EQ(config.checkpoint_interval_ns, 0u);
}

TEST(ExpeditorConfigTest, KnownKeysCoverEveryKeyTheOverlayReads) {
    // Guards the pairing the header comment promises: a new field with a
    // new key must appear in KnownConfigKeys() or it is rejected as
    // unknown the moment anyone sets it.
    Expeditor::Config config;
    for (const std::string& key : Expeditor::Config::KnownConfigKeys()) {
        std::string text = key + " = 1\n";
        auto file = ConfigFile::Parse(text, "probe.conf");
        ASSERT_TRUE(file.ok());
        EXPECT_TRUE(file.value().UnknownKeys(Expeditor::Config::KnownConfigKeys()).empty())
            << key << " is not in KnownConfigKeys()";
    }
}

// ---- Aggregation caps (docs/feat-aggregate.md §6, AG11) ---------------

TEST(ExpeditorConfigTest, AggregateCapsParseAndCarryTheProposedDefaults) {
    Expeditor::Config config;
    // The spec's `[PROPOSED]` numbers, in one place each. Nothing may
    // depend on the values - this pins where they live, not what they are.
    EXPECT_EQ(config.aggregate_max_groups, 65536u);
    EXPECT_EQ(config.aggregate_max_distinct, 1048576u);

    ASSERT_TRUE(config
                    .ApplyFile(ParseOk("aggregate_max_groups = 128\n"
                                       "aggregate_max_distinct = 256\n"))
                    .ok());
    EXPECT_EQ(config.aggregate_max_groups, 128u);
    EXPECT_EQ(config.aggregate_max_distinct, 256u);
}

TEST(ExpeditorConfigTest, ZeroGroupsIsAcceptedAndMeansRefuseEveryFold) {
    // The same shape `cabin_max_values = 0` has: a coherent way to switch
    // the behaviour off per instance while leaving the grammar in place.
    Expeditor::Config config;
    ASSERT_TRUE(config.ApplyFile(ParseOk("aggregate_max_groups = 0\n")).ok());
    EXPECT_EQ(config.aggregate_max_groups, 0u);
}

// ---- `cores` (docs/workplan-crosscore.md M6) --------------------------

TEST(ExpeditorConfigTest, CoresParsesAndDefaultsToOne) {
    Expeditor::Config config;
    EXPECT_EQ(config.cores, 1u);

    ASSERT_TRUE(config.ApplyFile(ParseOk("cores = 4\n")).ok());
    EXPECT_EQ(config.cores, 4u);
}

TEST(ExpeditorConfigTest, ZeroCoresIsRefused) {
    // A database with no reactor is not a configuration, it is a typo.
    Expeditor::Config config;
    Status s = config.ApplyFile(ParseOk("cores = 0\n"));
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(config.cores, 1u) << "a refused value must not be half-applied";
}

TEST(ExpeditorConfigTest, MoreCoresThanWalAnchorSlotsIsRefusedNamingTheCeiling) {
    // kMaxWalCores is a hard ceiling: the anchor table is indexed by
    // core_id, so a core above it has nowhere to publish a checkpoint from.
    Expeditor::Config config;
    Status s = config.ApplyFile(
        ParseOk("cores = " + std::to_string(kMaxWalCores + 1) + "\n"));
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find(std::to_string(kMaxWalCores)), std::string::npos) << s.message();
}

TEST(ExpeditorConfigTest, ExactlyTheCeilingIsAccepted) {
    Expeditor::Config config;
    ASSERT_TRUE(config.ApplyFile(ParseOk("cores = " + std::to_string(kMaxWalCores) + "\n")).ok());
    EXPECT_EQ(config.cores, kMaxWalCores);
}

}  // namespace
}  // namespace kds::server
