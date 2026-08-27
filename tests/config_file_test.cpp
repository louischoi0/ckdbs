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

// ---- TLS (docs/spec/protocol.md §1, direct TLS decided 2026-08-13) ---------

TEST(ExpeditorConfigTest, TlsKeysParseAndDefaultOff) {
    Expeditor::Config config;
    EXPECT_FALSE(config.tls) << "TLS is opt-in";
    ASSERT_TRUE(config
                    .ApplyFile(ParseOk("tls = on\n"
                                       "tls_cert_file = /etc/kds/server.crt\n"
                                       "tls_key_file = /etc/kds/server.key\n"))
                    .ok());
    EXPECT_TRUE(config.tls);
    EXPECT_EQ(config.tls_cert_file, "/etc/kds/server.crt");
    EXPECT_EQ(config.tls_key_file, "/etc/kds/server.key");
}

TEST(ExpeditorConfigTest, TlsOnWithoutBothFilesIsRejected) {
    // A tls = on that could never handshake refuses at load - the same
    // moment a typo'd key does - not at the first connection.
    Expeditor::Config config;
    Status s = config.ApplyFile(ParseOk("tls = on\ntls_cert_file = /x.crt\n"));
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("tls_key_file"), std::string::npos) << s.message();

    Expeditor::Config bare;
    EXPECT_FALSE(bare.ApplyFile(ParseOk("tls = on\n")).ok());
}

TEST(ExpeditorConfigTest, AuthKeysParseAndDefaultOff) {
    Expeditor::Config config;
    EXPECT_FALSE(config.auth_scram) << "auth is opt-in";
    ASSERT_TRUE(config
                    .ApplyFile(ParseOk("auth = scram\n"
                                       "users_file = /etc/kds/users\n"))
                    .ok());
    EXPECT_TRUE(config.auth_scram);
    EXPECT_EQ(config.users_file, "/etc/kds/users");

    Expeditor::Config off;
    ASSERT_TRUE(off.ApplyFile(ParseOk("auth = off\n")).ok());
    EXPECT_FALSE(off.auth_scram);
}

TEST(ExpeditorConfigTest, AuthRefusesBooleansAndMissingUsersFile) {
    // Named methods only: "on" would leave *which* method unstated.
    Expeditor::Config config;
    Status s = config.ApplyFile(ParseOk("auth = on\n"));
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("'off' or 'scram'"), std::string::npos) << s.message();

    Expeditor::Config bare;
    Status s2 = bare.ApplyFile(ParseOk("auth = scram\n"));
    EXPECT_FALSE(s2.ok());
    EXPECT_NE(s2.message().find("users_file"), std::string::npos) << s2.message();
}

TEST(ExpeditorConfigTest, AuthRefusesAnOpenKwpPort) {
    // The KWP load endpoint has no auth stage: with auth = scram it
    // would be an ungated door on a guarded server, so the combination
    // refuses at load until KWP P07 exists.
    Expeditor::Config config;
    Status s = config.ApplyFile(ParseOk("auth = scram\n"
                                        "users_file = /etc/kds/users\n"
                                        "kwp_port = 15433\n"));
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("kwp_port"), std::string::npos) << s.message();
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

// ---- Aggregation caps (docs/spec/aggregate.md §6, AG11) ---------------

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

// ---- Physical optimizer (docs/spec/physical-optimizer.md R1/R3) --------

TEST(ExpeditorConfigTest, PhysicalOptimizerParsesOffAndShadowDefaultingToShadow) {
    Expeditor::Config config;
    EXPECT_EQ(config.physical_optimizer, PhysicalOptimizerMode::kShadow);

    ASSERT_TRUE(config.ApplyFile(ParseOk("physical_optimizer = off\n")).ok());
    EXPECT_EQ(config.physical_optimizer, PhysicalOptimizerMode::kOff);
    ASSERT_TRUE(config.ApplyFile(ParseOk("physical_optimizer = SHADOW\n")).ok());
    EXPECT_EQ(config.physical_optimizer, PhysicalOptimizerMode::kShadow);
}

TEST(ExpeditorConfigTest, PhysicalOptimizerOnIsRefusedNamingAllThreeGates) {
    // R3: a config written for the future fails loudly today. The message
    // must name what is missing, not what word to try next - all three of
    // §6's gates.
    Expeditor::Config config;
    Status on = config.ApplyFile(ParseOk("physical_optimizer = on\n"));
    ASSERT_EQ(on.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(on.message().find("reader horizon"), std::string::npos) << on.message();
    EXPECT_NE(on.message().find("ordered-between"), std::string::npos) << on.message();
    EXPECT_NE(on.message().find("page reuse"), std::string::npos) << on.message();

    Status typo = config.ApplyFile(ParseOk("physical_optimizer = onn\n"));
    EXPECT_EQ(typo.code(), StatusCode::kInvalidArgument);
}

TEST(ExpeditorConfigTest, DecayHalfLifeParsesSecondsAndCarriesTheProposedDefault) {
    Expeditor::Config config;
    // The spec's `[PROPOSED]` 600 s, pinned where it lives and nowhere
    // else. Nothing may depend on the number, only on the rule.
    EXPECT_EQ(config.decay_half_life_ns, 600'000'000'000ULL);

    ASSERT_TRUE(config.ApplyFile(ParseOk("decay_half_life = 2\n")).ok());
    EXPECT_EQ(config.decay_half_life_ns, 2'000'000'000ULL);
}

TEST(ExpeditorConfigTest, DecayHalfLifeRefusesZeroAndTheSecondsToNsOverflow) {
    Expeditor::Config config;

    Status zero = config.ApplyFile(ParseOk("decay_half_life = 0\n"));
    EXPECT_EQ(zero.code(), StatusCode::kInvalidArgument) << zero.message();

    // One past UINT64_MAX / 1e9: accepting it would wrap the ns value.
    Status wide = config.ApplyFile(ParseOk("decay_half_life = 18446744074\n"));
    EXPECT_EQ(wide.code(), StatusCode::kInvalidArgument) << wide.message();
}

// ---- Cabin optimizer boot settings (spec §II.6, workplan PHY05) --------

TEST(ExpeditorConfigTest, CabinOptimizerParsesItsFamilyAndDefaultsOff) {
    Expeditor::Config config;
    EXPECT_FALSE(config.cabin_optimizer);  // experimental: off, §II.6

    // The shipped amortization window and the cooldown that is no longer
    // derived from it: 64 and 128 half-lives, the overnight sizing the
    // business-days scenario ratified (2026-08-10). 128 is what the old
    // `2 x T_amort` expression yielded, so decoupling moved nothing.
    EXPECT_EQ(config.cabin_optimizer_amort_windows, 64u);
    EXPECT_EQ(config.cabin_optimizer_cooldown_half_lives, 128u);

    ASSERT_TRUE(config
                    .ApplyFile(ParseOk("cabin_optimizer = on\n"
                                       "cabin_optimizer_page_budget = 128\n"
                                       "cabin_optimizer_theta_create_pct = 400\n"
                                       "cabin_optimizer_theta_drop_pct = 25\n"
                                       "cabin_optimizer_confirm_snapshots = 5\n"
                                       "cabin_optimizer_amort_windows = 12\n"
                                       "cabin_optimizer_cooldown_half_lives = 7\n"))
                    .ok());
    EXPECT_TRUE(config.cabin_optimizer);
    EXPECT_EQ(config.cabin_optimizer_page_budget, 128u);

    // The assembled decision-core settings: percent to 16.16, half-life
    // shared with R1's key, amort in whole half-lives to 16.16.
    const stats::CabinOptimizerConfig assembled = config.CabinOptimizerSettings();
    EXPECT_EQ(assembled.theta_create, 4 * stats::kFixOne);
    EXPECT_EQ(assembled.theta_drop, stats::kFixOne / 4);
    EXPECT_EQ(assembled.confirm_snapshots, 5u);
    EXPECT_EQ(assembled.page_budget, 128u);
    EXPECT_EQ(assembled.half_life_ns, config.decay_half_life_ns);
    EXPECT_EQ(assembled.amort_windows, 12 * stats::kFixOne);
    EXPECT_EQ(assembled.cooldown_half_lives, 7u);
}

TEST(ExpeditorConfigTest, TheAmortWindowAndTheCooldownAreIndependent) {
    // They were one number (`cooldown = 2 x T_amort`) and are two
    // questions: how long a build is believed to pay for itself, versus
    // how much silence proves death. Moving one must not move the other -
    // otherwise an operator lengthening the amortization to admit
    // marginal shapes also doubles how long a dead Cabin lingers.
    Expeditor::Config config;
    ASSERT_TRUE(config.ApplyFile(ParseOk("cabin_optimizer_amort_windows = 4\n")).ok());
    EXPECT_EQ(config.CabinOptimizerSettings().cooldown_half_lives, 128u)
        << "the cooldown followed the amortization window";

    // And the cooldown may sit below the window, which the old expression
    // made unrepresentable - a 24/7 workload with no quiet period is the
    // case that wants it.
    Expeditor::Config shorter;
    ASSERT_TRUE(shorter
                    .ApplyFile(ParseOk("cabin_optimizer_amort_windows = 64\n"
                                       "cabin_optimizer_cooldown_half_lives = 8\n"))
                    .ok());
    const stats::CabinOptimizerConfig assembled = shorter.CabinOptimizerSettings();
    EXPECT_EQ(assembled.amort_windows, 64 * stats::kFixOne);
    EXPECT_EQ(assembled.cooldown_half_lives, 8u);
}

TEST(ExpeditorConfigTest, CabinOptimizerRefusesAZeroAmortWindow) {
    // T_amort divides the build cost: over zero half-lives every Cabin is
    // free, which is create-everything wearing a configuration's clothes.
    Expeditor::Config config;
    Status zero = config.ApplyFile(ParseOk("cabin_optimizer_amort_windows = 0\n"));
    EXPECT_EQ(zero.code(), StatusCode::kInvalidArgument) << zero.message();

    // A zero *cooldown* is accepted, deliberately: it means no time
    // patience, leaving the score hysteresis as the only anti-thrash -
    // coherent, unlike a zero window, which prices every Cabin free.
    Expeditor::Config no_patience;
    EXPECT_TRUE(
        no_patience.ApplyFile(ParseOk("cabin_optimizer_cooldown_half_lives = 0\n")).ok());
}

TEST(ExpeditorConfigTest, CabinOptimizerRefusesABrokenHysteresisGap) {
    // The one cross-key rule: theta_drop < 100 < theta_create, or the
    // anti-thrash gap the whole rule table stands on does not exist.
    Expeditor::Config config;
    Status inverted = config.ApplyFile(ParseOk("cabin_optimizer_theta_drop_pct = 150\n"));
    EXPECT_EQ(inverted.code(), StatusCode::kInvalidArgument) << inverted.message();

    Expeditor::Config config2;
    Status low_create =
        config2.ApplyFile(ParseOk("cabin_optimizer_theta_create_pct = 90\n"));
    EXPECT_EQ(low_create.code(), StatusCode::kInvalidArgument) << low_create.message();

    Expeditor::Config config3;
    Status zero_budget = config3.ApplyFile(ParseOk("cabin_optimizer_page_budget = 0\n"));
    EXPECT_EQ(zero_budget.code(), StatusCode::kInvalidArgument) << zero_budget.message();

    Expeditor::Config config4;
    Status zero_confirm =
        config4.ApplyFile(ParseOk("cabin_optimizer_confirm_snapshots = 0\n"));
    EXPECT_EQ(zero_confirm.code(), StatusCode::kInvalidArgument) << zero_confirm.message();
}

TEST(ExpeditorConfigTest, ZeroGroupsIsAcceptedAndMeansRefuseEveryFold) {
    // The same shape `cabin_max_values = 0` has: a coherent way to switch
    // the behaviour off per instance while leaving the grammar in place.
    Expeditor::Config config;
    ASSERT_TRUE(config.ApplyFile(ParseOk("aggregate_max_groups = 0\n")).ok());
    EXPECT_EQ(config.aggregate_max_groups, 0u);
}

// ---- `cores` (docs/inflight/in-progress/workplan-crosscore.md M6) --------------------------

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

TEST(ExpeditorConfigTest, PeerListenersParseAndRefuseTlsOrAuth) {
    // PW5's stated restriction: the credential store and TLS context are
    // core 0's stack, so peer listeners with either is refused truthfully
    // (Unsupported - understood and declined) rather than served with
    // secrets shared by accident.
    Expeditor::Config config;
    EXPECT_FALSE(config.peer_listeners);
    ASSERT_TRUE(config.ApplyFile(ParseOk("peer_listeners = on\n")).ok());
    EXPECT_TRUE(config.peer_listeners);

    constexpr auto kRotate = catalog::PlacementPolicy::kRotate;
    constexpr auto kCreating = catalog::PlacementPolicy::kCreatingCore;
    EXPECT_TRUE(CheckPeerListenerConfig(false, true, true, 1, kCreating).ok());
    EXPECT_TRUE(CheckPeerListenerConfig(true, false, false, 2, kRotate).ok());
    EXPECT_EQ(CheckPeerListenerConfig(true, true, false, 2, kRotate).code(),
              StatusCode::kUnsupported);
    EXPECT_EQ(CheckPeerListenerConfig(true, false, true, 2, kRotate).code(),
              StatusCode::kUnsupported);

    // The two pairings that cannot work (the PW5 review's finding 6):
    // no peer to listen, and a placement under which a peer session could
    // serve nothing. Both are plain misconfigurations, so InvalidArgument.
    EXPECT_EQ(CheckPeerListenerConfig(true, false, false, 1, kRotate).code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(CheckPeerListenerConfig(true, false, false, 2, kCreating).code(),
              StatusCode::kInvalidArgument);
}

TEST(ExpeditorConfigTest, FrameBudgetSharesAreEqualNonzeroAndNeverExceedTheTotal) {
    // The operator invariant of 2026-08-24: every core's share is *equal*
    // (no remainder seat for core 0 - equality is the invariant), nonzero
    // for any total CheckFrameBudget admits, and cores * share never
    // exceeds the total, so the key keeps meaning the whole pool. The
    // undistributed remainder is bounded by the divisor. Swept over every
    // (total, cores) boundary shape up to 8 cores.
    for (std::uint32_t cores = 1; cores <= 8; ++cores) {
        for (std::size_t total : {std::size_t{cores}, std::size_t{cores} + 1,
                                  std::size_t{cores} * 7 - 1, std::size_t{cores} * 7,
                                  std::size_t{1024}}) {
            ASSERT_TRUE(CheckFrameBudget(total, cores).ok());
            const std::size_t share = FrameBudgetShare(total, cores);
            EXPECT_GT(share, 0u) << "total " << total << " cores " << cores;
            EXPECT_LE(share * cores, total) << "total " << total << " cores " << cores;
            EXPECT_LT(total - share * cores, static_cast<std::size_t>(cores))
                << "total " << total << " cores " << cores;
        }
    }

}

TEST(ExpeditorConfigTest, AFrameBudgetBelowTheCoreCountIsRefusedNamingBothNumbers) {
    // The failure this prevents is inverted, not just wrong: 3 frames over
    // 4 cores gives some core a share of 0, and 0 means *unbounded* - so a
    // tiny budget would arm no sweep at all on that core.
    Status s = CheckFrameBudget(3, 4);
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("3"), std::string::npos) << s.message();
    EXPECT_NE(s.message().find("4"), std::string::npos) << s.message();

    // Zero total stays legal and means unbounded by request; equality and
    // above divide into nonzero shares everywhere.
    EXPECT_TRUE(CheckFrameBudget(0, 4).ok());
    EXPECT_TRUE(CheckFrameBudget(4, 4).ok());
    EXPECT_TRUE(CheckFrameBudget(9, 4).ok());
}

TEST(ExpeditorConfigTest, ExactlyTheCeilingIsAccepted) {
    Expeditor::Config config;
    ASSERT_TRUE(config.ApplyFile(ParseOk("cores = " + std::to_string(kMaxWalCores) + "\n")).ok());
    EXPECT_EQ(config.cores, kMaxWalCores);
}

}  // namespace
}  // namespace kds::server
