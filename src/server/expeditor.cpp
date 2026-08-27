#include "kds/server/expeditor.hpp"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>

#include "kds/exec/step_vm.hpp"
#include "kds/sched/epoll_io_backend.hpp"
#include "kds/sched/send_retry.hpp"
#include "kds/server/relation_grant_service.hpp"
#include "kds/server/remote_checkpoint_anchor.hpp"
#include "kds/storage/file_page_device.hpp"
#include "kds/wal/log_page_handoff.hpp"

#if KDS_WITH_TLS
#include "kds/server/auth.hpp"
#include "kds/server/tls_channel.hpp"
#endif

namespace kds::server {

namespace {

// PEM files are read whole at startup and never again: the TLS context
// keeps its own copy of what it parsed, and a cert rotated on disk takes
// effect at the next start, stated rather than discovered.
StatusOr<std::string> ReadWholeFile(const std::string& path, const char* what) {
    std::ifstream in(path);
    if (!in) {
        return Status::NotFound(std::string(what) + " '" + path +
                                "' cannot be opened: " + std::strerror(errno));
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

}  // namespace

std::string Expeditor::Config::LogPath() const {
    if (log_file.empty()) return {};                 // logging to a file disabled
    if (log_dir.empty() || log_file.front() == '/') return log_file;
    if (log_dir.back() == '/') return log_dir + log_file;
    return log_dir + "/" + log_file;
}

std::vector<std::string> Expeditor::Config::KnownConfigKeys() {
    return {"data_file",  "port",     "wal_dir",  "checkpoint_interval_ms", "durability",
            "tls",        "tls_cert_file",        "tls_key_file",
            "peer_listeners",
            "auth",       "users_file",
            "isolation",             "default_key_mode",
            "wal_drain_interval_us", "relaxed_flush_interval_us",
            "log_dir",  "log_file",               "log_level",
            "max_rows_touched",      "max_insert_rows",        "kwp_port",
            "buffer_pool_frames",
            "inline_cell_width",      "waystone_recording",
            "waystone_replay",
            "access_statistics",       "cabins",   "cabin_max_values",
            "indexes",
            "cabin_max_entries_per_value", "cores", "placement",
            "aggregate_max_groups",  "aggregate_max_distinct", "sort_max_rows",
            "join_build_max_rows",
            "decay_half_life",       "physical_optimizer",
            "cabin_optimizer",       "cabin_optimizer_page_budget",
            "cabin_optimizer_theta_create_pct", "cabin_optimizer_theta_drop_pct",
            "cabin_optimizer_theta_swap_pct",   "cabin_optimizer_theta_extend_pct",
            "cabin_optimizer_theta_heal_pct",   "cabin_optimizer_confirm_snapshots",
            "cabin_optimizer_amort_windows", "cabin_optimizer_cooldown_half_lives",
            "cabin_optimizer_snapshot_interval_ms"};
}

stats::CabinOptimizerConfig Expeditor::Config::CabinOptimizerSettings() const {
    stats::CabinOptimizerConfig config;
    const auto pct = [](std::uint32_t p) {
        return (static_cast<stats::Fix16>(p) * stats::kFixOne) / 100;
    };
    config.theta_create = pct(cabin_optimizer_theta_create_pct);
    config.theta_drop = pct(cabin_optimizer_theta_drop_pct);
    config.theta_swap = pct(cabin_optimizer_theta_swap_pct);
    config.theta_extend = pct(cabin_optimizer_theta_extend_pct);
    config.theta_heal = pct(cabin_optimizer_theta_heal_pct);
    config.confirm_snapshots = cabin_optimizer_confirm_snapshots;
    config.page_budget = cabin_optimizer_page_budget;
    config.half_life_ns = decay_half_life_ns;
    config.amort_windows = static_cast<stats::Fix16>(cabin_optimizer_amort_windows) *
                           stats::kFixOne;
    config.cooldown_half_lives = cabin_optimizer_cooldown_half_lives;
    return config;
}

Status CheckFrameBudget(std::size_t frames, std::uint32_t cores) {
    if (frames != 0 && frames < cores) {
        return Status::InvalidArgument(
            "buffer_pool_frames " + std::to_string(frames) + " is below cores " +
            std::to_string(cores) +
            "; the budget is an instance total divided per core, and a share of zero means "
            "unbounded, not tiny - raise the budget or drop the key");
    }
    return Status::OK();
}

std::size_t FrameBudgetShare(std::size_t frames, std::uint32_t cores) noexcept {
    return frames / cores;
}

Status CheckPeerListenerConfig(bool peer_listeners, bool tls, bool auth_scram,
                               std::uint32_t cores, catalog::PlacementPolicy placement) {
    if (!peer_listeners) return Status::OK();
    if (tls || auth_scram) {
        return Status::Unsupported(
            "peer_listeners = on cannot yet be combined with tls or auth: the credential "
            "store and TLS context live on core 0's stack, and sharing them across per-core "
            "listeners is PW5's open half (workplan-peer-writer.md) - run peer listeners on "
            "the loopback plaintext port, or keep one listener");
    }
    // Two pairings that cannot work, refused rather than served (the PW5
    // review's finding 6). One core has no peer to listen - the sole
    // effect would be SO_REUSEPORT on the only socket, which lets a
    // second process bind the same port and silently take half the
    // connections into a different database. And creating-core placement
    // puts every relation on core 0, so every peer-accepted session would
    // refuse every statement while answering OK to PING.
    if (cores == 1) {
        return Status::InvalidArgument(
            "peer_listeners = on with cores = 1 has no peer to listen; the only effect "
            "would be losing the exclusive bind on the one socket");
    }
    if (placement != catalog::PlacementPolicy::kRotate) {
        return Status::InvalidArgument(
            "peer_listeners = on needs placement = rotate: with creating-core placement "
            "every relation is core 0's, so a peer-accepted session could serve nothing");
    }
    return Status::OK();
}

Status Expeditor::Config::ApplyFile(const ConfigFile& file) {
    std::vector<std::string> unknown = file.UnknownKeys(KnownConfigKeys());
    if (!unknown.empty()) {
        std::string msg = file.origin() + ": unknown config key(s):";
        for (const std::string& key : unknown) msg += " '" + key + "'";
        return Status::InvalidArgument(std::move(msg));
    }

    if (file.Has("data_file")) {
        auto v = file.GetString("data_file");
        if (!v.ok()) return v.status();
        data_file = std::move(v.value());
    }
    if (file.Has("port")) {
        auto v = file.GetUint("port");
        if (!v.ok()) return v.status();
        if (v.value() == 0 || v.value() > std::numeric_limits<std::uint16_t>::max()) {
            return Status::InvalidArgument(file.origin() + ": port " + std::to_string(v.value()) +
                                            " is outside 1..65535");
        }
        port = static_cast<std::uint16_t>(v.value());
    }
    if (file.Has("wal_dir")) {
        auto v = file.GetString("wal_dir");
        if (!v.ok()) return v.status();
        wal_dir = std::move(v.value());
    }
    if (file.Has("buffer_pool_frames")) {
        // MG06: how many frames may stay resident across the **whole
        // instance** - divided evenly per core, remainder to core 0
        // (eviction.md §6 EV4; Open() refuses a nonzero total below
        // `cores`, CheckFrameBudget). 0 (the default) is unbounded, the
        // exact pre-eviction behaviour. Nonzero arms the CLOCK sweep on
        // the fault path (spec-eviction EV5's on-demand trigger).
        auto v = file.GetUint("buffer_pool_frames");
        if (!v.ok()) return v.status();
        buffer_pool_frames = static_cast<std::size_t>(v.value());
    }
    if (file.Has("checkpoint_interval_ms")) {
        auto v = file.GetUint("checkpoint_interval_ms");
        if (!v.ok()) return v.status();
        // Expressed in ms in the file because that is the unit an operator
        // reasons about, and held in ns internally because that is the
        // scheduler's. 0 keeps its documented meaning: no cadence.
        checkpoint_interval_ns = v.value() * 1'000'000ULL;
    }
    if (file.Has("durability")) {
        auto v = file.GetString("durability");
        if (!v.ok()) return v.status();
        auto parsed = wal::ParseDurabilityClass(v.value());
        if (!parsed.ok()) {
            return Status::InvalidArgument(file.origin() + ": " + parsed.status().message());
        }
        durability = parsed.value();
    }
    if (file.Has("peer_listeners")) {
        auto v = file.GetBool("peer_listeners");
        if (!v.ok()) return v.status();
        peer_listeners = v.value();
    }
    if (file.Has("tls")) {
        auto v = file.GetBool("tls");
        if (!v.ok()) return v.status();
        tls = v.value();
    }
    if (file.Has("tls_cert_file")) {
        auto v = file.GetString("tls_cert_file");
        if (!v.ok()) return v.status();
        tls_cert_file = std::move(v.value());
    }
    if (file.Has("tls_key_file")) {
        auto v = file.GetString("tls_key_file");
        if (!v.ok()) return v.status();
        tls_key_file = std::move(v.value());
    }
    // Checked here rather than at Serve(): a config that can never serve
    // should refuse at the same moment a typo'd key does. The *contents*
    // of the files are checked where they are read (Serve), which is as
    // early as an unreadable certificate can be detected.
    if (tls && (tls_cert_file.empty() || tls_key_file.empty())) {
        return Status::InvalidArgument(file.origin() +
                                        ": tls = on requires both tls_cert_file and "
                                        "tls_key_file");
    }
    if (file.Has("auth")) {
        auto v = file.GetString("auth");
        if (!v.ok()) return v.status();
        if (v.value() == "off") {
            auth_scram = false;
        } else if (v.value() == "scram") {
            auth_scram = true;
        } else {
            // Named values, not a boolean: a future method (mTLS-only,
            // say) must be a new word, and "on" would leave which method
            // unstated - exactly the kind of silence that lies later.
            return Status::InvalidArgument(file.origin() + ": auth must be 'off' or 'scram', got '" +
                                            v.value() + "'");
        }
    }
    if (file.Has("users_file")) {
        auto v = file.GetString("users_file");
        if (!v.ok()) return v.status();
        users_file = std::move(v.value());
    }
    if (auth_scram && users_file.empty()) {
        return Status::InvalidArgument(file.origin() +
                                        ": auth = scram requires users_file");
    }
    if (file.Has("waystone_recording")) {
        auto v = file.GetBool("waystone_recording");
        if (!v.ok()) return v.status();
        waystone_recording = v.value();
    }
    if (file.Has("isolation")) {
        auto v = file.GetString("isolation");
        if (!v.ok()) return v.status();
        auto parsed = txn::ParseIsolationLevel(v.value());
        if (!parsed.ok()) return parsed.status();
        isolation = parsed.value();
    }
    if (file.Has("default_key_mode")) {
        // Still *known*, so the refusal can say what happened rather than
        // "unknown key". The setting decided what a CREATE TABLE naming no
        // key-mode word meant; there is no longer a mode for it to decide
        // (docs/spec/heap-and-tuple.md section 4.1), and an instance that kept
        // the line would be reading a preference the engine cannot honor
        // either way - which is the misunderstanding this file's
        // unknown-key rule exists to prevent.
        return Status::Unsupported(
            "config key 'default_key_mode' no longer exists: every relation takes a "
            "caller-supplied primary key or issues one when INSERT omits it, so there is no "
            "default to set - remove the line");
    }
    if (file.Has("waystone_replay")) {
        auto v = file.GetBool("waystone_replay");
        if (!v.ok()) return v.status();
        waystone_replay = v.value();
    }
    if (file.Has("access_statistics")) {
        auto v = file.GetBool("access_statistics");
        if (!v.ok()) return v.status();
        access_statistics = v.value();
    }
    if (file.Has("cabins")) {
        auto v = file.GetBool("cabins");
        if (!v.ok()) return v.status();
        cabins = v.value();
    }
    if (file.Has("indexes")) {
        auto v = file.GetBool("indexes");
        if (!v.ok()) return v.status();
        indexes = v.value();
    }
    if (file.Has("cabin_max_values")) {
        auto v = file.GetUint("cabin_max_values");
        if (!v.ok()) return v.status();
        // No upper range check, and no zero check either: 0 means no value
        // may be observed, which is a coherent way to keep the catalog
        // objects while switching the behaviour off per instance.
        cabin_max_values = static_cast<std::size_t>(v.value());
    }
    if (file.Has("cabin_max_entries_per_value")) {
        auto v = file.GetUint("cabin_max_entries_per_value");
        if (!v.ok()) return v.status();
        cabin_max_entries_per_value = static_cast<std::size_t>(v.value());
    }
    if (file.Has("aggregate_max_groups")) {
        auto v = file.GetUint("aggregate_max_groups");
        if (!v.ok()) return v.status();
        // No zero check, for the reason `cabin_max_values` has none: 0
        // means no group may be founded, which refuses every aggregated
        // statement - a coherent way to switch the feature off per
        // instance while leaving the grammar in place.
        aggregate_max_groups = static_cast<std::size_t>(v.value());
    }
    if (file.Has("aggregate_max_distinct")) {
        auto v = file.GetUint("aggregate_max_distinct");
        if (!v.ok()) return v.status();
        aggregate_max_distinct = static_cast<std::size_t>(v.value());
    }
    if (file.Has("sort_max_rows")) {
        auto v = file.GetUint("sort_max_rows");
        if (!v.ok()) return v.status();
        // No zero check, for `aggregate_max_groups`'s reason: 0 means no
        // row may be held, which refuses every statement that needs a sort
        // while leaving the grammar in place.
        sort_max_rows = static_cast<std::size_t>(v.value());
    }
    if (file.Has("join_build_max_rows")) {
        auto v = file.GetUint("join_build_max_rows");
        if (!v.ok()) return v.status();
        // 0 is the off-switch, not a refusal: no statement is refused by
        // this knob at any value. The semantics have one home, at
        // `kDefaultJoinBuildMaxRows` (exec/budget.hpp).
        join_build_max_rows = static_cast<std::size_t>(v.value());
    }
    if (file.Has("physical_optimizer")) {
        auto v = file.GetString("physical_optimizer");
        if (!v.ok()) return v.status();
        std::string mode = v.value();
        for (char& c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (mode == "off") {
            physical_optimizer = PhysicalOptimizerMode::kOff;
        } else if (mode == "shadow") {
            physical_optimizer = PhysicalOptimizerMode::kShadow;
        } else if (mode == "on") {
            // Refused naming every gate, so the operator learns what is
            // missing rather than what word to try next
            // (docs/spec/physical-optimizer.md §6).
            return Status::InvalidArgument(
                file.origin() +
                ": physical_optimizer = on is not available: every relayout plan is blocked - "
                "compact on the reader horizon (readers are unregistered, txn.md §9), cluster "
                "on the ordered-between property kRange pruning reads, defrag on cross-relation "
                "page reuse breaking trail validation (docs/spec/physical-optimizer.md §6). "
                "Use 'shadow' for the report, 'off' to silence it.");
        } else {
            return Status::InvalidArgument(file.origin() + ": physical_optimizer '" + v.value() +
                                           "' is not off|shadow (on is refused, naming why)");
        }
    }
    if (file.Has("decay_half_life")) {
        auto v = file.GetUint("decay_half_life");
        if (!v.ok()) return v.status();
        // Positive, because a zero half-life has no meaning to round toward
        // (instant decay is "no score", which is not a configuration this
        // engine offers), and bounded so seconds-to-ns cannot wrap:
        // 2^64 ns is ~584 years, and a half-life near it is the same
        // request as "never decay", which is what a large finite value
        // already delivers.
        constexpr std::uint64_t kMaxHalfLifeSeconds = UINT64_MAX / 1'000'000'000ULL;
        if (v.value() == 0 || v.value() > kMaxHalfLifeSeconds) {
            return Status::InvalidArgument(
                file.origin() + ": decay_half_life " + std::to_string(v.value()) +
                " is outside 1.." + std::to_string(kMaxHalfLifeSeconds) +
                " (seconds; docs/spec/physical-optimizer.md R1)");
        }
        decay_half_life_ns = v.value() * 1'000'000'000ULL;
    }
    if (file.Has("cabin_optimizer")) {
        auto v = file.GetBool("cabin_optimizer");
        if (!v.ok()) return v.status();
        cabin_optimizer = v.value();
    }
    if (file.Has("cabin_optimizer_page_budget")) {
        auto v = file.GetUint("cabin_optimizer_page_budget");
        if (!v.ok()) return v.status();
        if (v.value() == 0) {
            return Status::InvalidArgument(
                file.origin() +
                ": cabin_optimizer_page_budget must be positive - a zero budget admits "
                "nothing, which is what cabin_optimizer = off already says");
        }
        cabin_optimizer_page_budget = v.value();
    }
    // The five thresholds, as percent integers (300 = θ 3.0): the config
    // file parses no decimals, and a percent is the coarsest unit at which
    // every proposed value stays expressible.
    const auto theta = [&](const char* key, std::uint32_t& out) -> Status {
        if (!file.Has(key)) return Status::OK();
        auto v = file.GetUint(key);
        if (!v.ok()) return v.status();
        if (v.value() == 0 || v.value() > 100'000) {
            return Status::InvalidArgument(file.origin() + ": " + key +
                                           " is outside 1..100000 (percent)");
        }
        out = static_cast<std::uint32_t>(v.value());
        return Status::OK();
    };
    if (Status s = theta("cabin_optimizer_theta_create_pct", cabin_optimizer_theta_create_pct);
        !s.ok()) {
        return s;
    }
    if (Status s = theta("cabin_optimizer_theta_drop_pct", cabin_optimizer_theta_drop_pct);
        !s.ok()) {
        return s;
    }
    if (Status s = theta("cabin_optimizer_theta_swap_pct", cabin_optimizer_theta_swap_pct);
        !s.ok()) {
        return s;
    }
    if (Status s = theta("cabin_optimizer_theta_extend_pct", cabin_optimizer_theta_extend_pct);
        !s.ok()) {
        return s;
    }
    if (Status s = theta("cabin_optimizer_theta_heal_pct", cabin_optimizer_theta_heal_pct);
        !s.ok()) {
        return s;
    }
    // The rule the anti-thrash hysteresis stands on (§II.4): the gap
    // θ_drop < 1 < θ_create must exist, whatever the numbers. Checked
    // whenever either key appears, against the pair's final values.
    if (cabin_optimizer_theta_drop_pct >= 100 || cabin_optimizer_theta_create_pct <= 100) {
        return Status::InvalidArgument(
            file.origin() +
            ": cabin_optimizer thetas must satisfy drop < 100 < create (the hysteresis "
            "gap); got drop=" +
            std::to_string(cabin_optimizer_theta_drop_pct) +
            " create=" + std::to_string(cabin_optimizer_theta_create_pct));
    }
    if (file.Has("cabin_optimizer_confirm_snapshots")) {
        auto v = file.GetUint("cabin_optimizer_confirm_snapshots");
        if (!v.ok()) return v.status();
        if (v.value() == 0 || v.value() > 1'000) {
            return Status::InvalidArgument(file.origin() +
                                           ": cabin_optimizer_confirm_snapshots is outside "
                                           "1..1000 - 0 would create on a single reading, "
                                           "which is what N_confirm exists to prevent");
        }
        cabin_optimizer_confirm_snapshots = static_cast<std::uint32_t>(v.value());
    }
    if (file.Has("cabin_optimizer_amort_windows")) {
        auto v = file.GetUint("cabin_optimizer_amort_windows");
        if (!v.ok()) return v.status();
        // 0 is refused rather than clamped: T_amort divides the build
        // cost, and amortizing over nothing prices every Cabin free -
        // create-everything wearing a configuration's clothes. The
        // ceiling is a sanity bound; a huge window that saturates the
        // cooldown arithmetic errs toward never dropping, which is the
        // direction such a configuration asked for.
        if (v.value() == 0 || v.value() > 100'000) {
            return Status::InvalidArgument(
                file.origin() + ": cabin_optimizer_amort_windows is outside 1..100000 - it "
                                "amortizes a Cabin's build cost over that many decay "
                                "half-lives, and over *zero* half-lives every Cabin is free");
        }
        cabin_optimizer_amort_windows = static_cast<std::uint32_t>(v.value());
    }
    if (file.Has("cabin_optimizer_cooldown_half_lives")) {
        auto v = file.GetUint("cabin_optimizer_cooldown_half_lives");
        if (!v.ok()) return v.status();
        // 0 is *accepted* where the amortization window's 0 is refused,
        // and the asymmetry is the point: a zero window prices every
        // Cabin free, which is nonsense, while zero time-patience is a
        // coherent (if brittle) choice - the score hysteresis remains.
        // The ceiling keeps `half_lives x half_life_ns` clear of u64.
        if (v.value() > 100'000) {
            return Status::InvalidArgument(
                file.origin() + ": cabin_optimizer_cooldown_half_lives is above 100000 - it "
                                "is the DECAYING dwell in decay half-lives, and a cooldown "
                                "that long is 'never drop' spelled the long way");
        }
        cabin_optimizer_cooldown_half_lives = static_cast<std::uint32_t>(v.value());
    }
    if (file.Has("cabin_optimizer_snapshot_interval_ms")) {
        auto v = file.GetUint("cabin_optimizer_snapshot_interval_ms");
        if (!v.ok()) return v.status();
        // 0 keeps its documented meaning: no cadence (the
        // checkpoint_interval_ms precedent).
        cabin_optimizer_snapshot_interval_ms = v.value();
    }
    if (file.Has("max_rows_touched")) {
        auto v = file.GetUint("max_rows_touched");
        if (!v.ok()) return v.status();
        // No range check beyond the parse: 0 is a documented value
        // (unlimited) and there is no upper bound worth inventing - a
        // ceiling too high to reach is the same as no ceiling, which the
        // operator has already asked for by setting it.
        max_rows_touched = v.value();
    }
    if (file.Has("kwp_port")) {
        auto v = file.GetUint("kwp_port");
        if (!v.ok()) return v.status();
        if (v.value() > 65535) {
            return Status::InvalidArgument("kwp_port must fit a TCP port");
        }
        kwp_port = static_cast<std::uint16_t>(v.value());
    }
    if (file.Has("max_insert_rows")) {
        auto v = file.GetUint("max_insert_rows");
        if (!v.ok()) return v.status();
        // A cap of 0 would refuse every INSERT, which no operator means;
        // 1 is the honest spelling of "single-row only".
        if (v.value() == 0) {
            return Status::InvalidArgument("max_insert_rows must be at least 1");
        }
        max_insert_rows = v.value();
    }
    if (file.Has("wal_drain_interval_us")) {
        auto v = file.GetUint("wal_drain_interval_us");
        if (!v.ok()) return v.status();
        // Microseconds in the file: a drain interval an operator cares
        // about is well under a millisecond, so ms would round it away.
        wal_drain_interval_ns = v.value() * 1'000ULL;
    }
    if (file.Has("relaxed_flush_interval_us")) {
        auto v = file.GetUint("relaxed_flush_interval_us");
        if (!v.ok()) return v.status();
        relaxed_flush_interval_ns = v.value() * 1'000ULL;
    }
    if (file.Has("inline_cell_width")) {
        auto v = file.GetUint("inline_cell_width");
        if (!v.ok()) return v.status();
        // Range-checked through the same function the superblock validates
        // with, so a config file and a data file can never disagree about
        // what a legal width is.
        if (v.value() > std::numeric_limits<std::uint32_t>::max()) {
            return Status::InvalidArgument(file.origin() + ": inline_cell_width " +
                                            std::to_string(v.value()) + " is not a u32");
        }
        auto width = static_cast<std::uint32_t>(v.value());
        if (Status s = storage::CheckInlineCellWidth(width); !s.ok()) {
            return Status::InvalidArgument(file.origin() + ": " + s.message());
        }
        inline_cell_width = width;
    }
    if (file.Has("placement")) {
        auto v = file.GetString("placement");
        if (!v.ok()) return v.status();
        if (v.value() == "creating") {
            placement = catalog::PlacementPolicy::kCreatingCore;
        } else if (v.value() == "rotate") {
            placement = catalog::PlacementPolicy::kRotate;
        } else {
            return Status::InvalidArgument(file.origin() + ": placement '" + v.value() +
                                            "' is not a policy; use creating or rotate");
        }
    }
    if (file.Has("cores")) {
        auto v = file.GetUint("cores");
        if (!v.ok()) return v.status();
        if (v.value() > std::numeric_limits<std::uint32_t>::max()) {
            return Status::InvalidArgument(file.origin() + ": cores " +
                                            std::to_string(v.value()) + " is not a u32");
        }
        auto count = static_cast<std::uint32_t>(v.value());
        // Range-checked through the same function the superblock validates
        // with, so a config file and a data file can never disagree about
        // what a legal core count is - the arrangement inline_cell_width
        // above already uses.
        if (Status s = CheckCoreCount(count); !s.ok()) {
            return Status::InvalidArgument(file.origin() + ": " + s.message());
        }
        cores = count;
    }
    if (file.Has("log_dir")) {
        auto v = file.GetString("log_dir");
        if (!v.ok()) return v.status();
        log_dir = std::move(v.value());
    }
    if (file.Has("log_file")) {
        auto v = file.GetString("log_file");
        if (!v.ok()) return v.status();
        log_file = std::move(v.value());
    }
    if (file.Has("log_level")) {
        auto v = file.GetString("log_level");
        if (!v.ok()) return v.status();
        auto level = ParseLogLevel(v.value());
        if (!level.ok()) return Status::InvalidArgument(file.origin() + ": " +
                                                        level.status().message());
        log_level = level.value();
    }

    // Cross-key validation, after every key has been read - a rule that
    // ran mid-overlay would judge keys the file had not yet spoken.
    // The KWP load endpoint has no auth stage, so opening it beside an
    // authenticated text port would put an ungated door on a guarded
    // server - every INSERT the gate protects, reachable without a
    // password. Refused, not warned (review 2026-08-13); the combination
    // becomes legal when KWP P07's handshake carries the same SCRAM
    // exchange.
    if (auth_scram && kwp_port != 0) {
        return Status::InvalidArgument(file.origin() +
                                        ": auth = scram cannot serve kwp_port yet - the KWP "
                                        "load endpoint has no auth stage until protocol-wp "
                                        "P07; set kwp_port = 0");
    }
    return Status::OK();
}

Expeditor::Expeditor(Config config, std::unique_ptr<storage::PageDevice> device,
                     std::unique_ptr<storage::DevicePageStore> store) noexcept
    : config_(std::move(config)), device_(std::move(device)), store_(std::move(store)) {}

StatusOr<std::unique_ptr<Expeditor>> Expeditor::Open(Config config,
                                                     std::uint64_t now_unix_seconds) {
    if (config.wal_dir.empty()) {
        config.wal_dir = config.data_file + ".wal";
    }

    // Checked here rather than in ApplyFile() because it is the one
    // validation that depends on the machine rather than on the value: a
    // config file is portable and a hardware core count is not, so the same
    // file must be able to fail on one host and pass on another. This is
    // the platform layer (rules.md #4), which is the only place allowed to
    // ask the hardware anything.
    //
    // Reactors are pinned and never block, so N of them on fewer than N
    // cores does not run slower - it runs one reactor's whole workload
    // behind another's, with no preemption to break the tie.
    if (Status s = CheckCoreCount(config.cores); !s.ok()) return s;
    if (Status s = CheckFrameBudget(config.buffer_pool_frames, config.cores); !s.ok()) return s;
    if (Status s = CheckPeerListenerConfig(config.peer_listeners, config.tls, config.auth_scram,
                                           config.cores, config.placement);
        !s.ok()) {
        return s;
    }
    const unsigned hardware_cores = std::thread::hardware_concurrency();
    // 0 means "not detectable" - not "no cores". Skipping the check is the
    // only honest response; refusing would make the server unstartable on a
    // platform that simply declines to answer.
    if (hardware_cores > 0 && config.cores > hardware_cores) {
        return Status::InvalidArgument(
            "cores " + std::to_string(config.cores) + " exceeds the " +
            std::to_string(hardware_cores) +
            " this machine reports; reactors are pinned one per core and never block, so "
            "overcommitting them serializes whole workloads behind each other");
    }

    auto device = storage::FilePageDevice::Open(config.data_file);
    if (!device.ok()) return device.status();

    auto store = storage::DevicePageStore::Open(*device.value(), kFirstUserPageId);
    if (!store.ok()) return store.status();
    // Only when the config asks for one. Zero means "unbounded", which is
    // already what Open() left unless the debug `KDS_TEST_FRAME_BUDGET`
    // override set a budget - and setting zero here would silently undo
    // that override on every server-path store, which is exactly the set
    // MG05's poisoner run needs under pressure.
    if (config.buffer_pool_frames != 0) {
        store.value()->SetFrameBudget(FrameBudgetShare(config.buffer_pool_frames, config.cores));
    }

    // Built here rather than in the initializer list because the members
    // below take references into it, which only become stable once the
    // Expeditor itself is on the heap and pinned.
    auto expeditor = std::unique_ptr<Expeditor>(new Expeditor(
        std::move(config), std::move(device.value()), std::move(store.value())));

    // Opened first, so everything after it can report. A log that cannot be
    // opened *is* fatal: the operator asked for a specific destination, and
    // starting anyway would run a server whose diagnostics silently go
    // nowhere - which is discovered at exactly the wrong moment.
    if (Status s = expeditor->OpenLog(); !s.ok()) return s;
    expeditor->logger_->Info("expeditor", "opening database '" + expeditor->config_.data_file +
                                              "', wal dir '" + expeditor->config_.wal_dir + "'");

    // The store is opened before the log exists (the log's own destination
    // is configuration, and reading it must not depend on a database), so
    // it is handed the logger here rather than at construction. Everything
    // it did during Open() is reported by the "database ready" line below.
    expeditor->store_->SetLogger(&*expeditor->logger_);

    auto database =
        bootstrap::BootstrapDatabase(*expeditor->store_, now_unix_seconds,
                                     expeditor->config_.inline_cell_width,
                                     expeditor->config_.cores, &*expeditor->logger_);
    if (!database.ok()) return database.status();
    expeditor->database_.emplace(std::move(database.value()));
    // The Catalog was moved out of the BootstrapResult; its logger came
    // along, but re-setting it keeps that fact local instead of depending
    // on a copy elsewhere staying a copy.
    expeditor->database_->catalog.SetLogger(&*expeditor->logger_);
    // The placement rule, before any DDL can run (workplan P6c). At
    // cores = 1 rotate degrades to the creating core by the formula, so no
    // validation couples the two keys.
    expeditor->database_->catalog.SetPlacementPolicy(expeditor->config_.placement);

    // The WAL stack, before the dispatcher: INSERT logs through it, so the
    // dispatcher cannot be built until it exists.
    auto log_device = wal::FileLogDevice::Open(expeditor->config_.wal_dir, /*core_id=*/0);
    if (!log_device.ok()) return log_device.status();
    expeditor->log_device_ = std::move(log_device.value());

    wal::WalManagerConfig wal_config;
    wal_config.relaxed_flush_interval_ns = expeditor->config_.relaxed_flush_interval_ns;
    auto wal = wal::WalManager::Open(expeditor->log_device_.get(), expeditor->clock_,
                                     /*core_id=*/0, wal_config);
    if (!wal.ok()) return wal.status();
    expeditor->wal_ = std::move(wal.value());
    expeditor->wal_->SetLogger(&*expeditor->logger_);

    // **The reactor stops touching the device here.** Every sync moves to
    // the WAL writer thread (wal/writer.hpp): a commit's, the D3
    // loss-window's, and the checkpoint gate's. The server starts one and
    // an in-process caller does not, because a test that drives a
    // WalManager on one thread wants its syncs to have happened by the time
    // the call returns.
    expeditor->wal_->StartWriter();

    // WAL-before-data, enforced by the store rather than asked of its
    // callers (device_page_store.hpp): from here on no dirty page reaches
    // the device ahead of the records describing it.
    expeditor->store_->SetWalGate(expeditor->wal_.get());

    // RV3: from here on every catalog mutation logs the ordinary record
    // types (workplan-rv3-catalog-recovery.md). Before recovery and the
    // mount sweeps below, so what they retire is logged too; bootstrap
    // above ran unlogged, which fresh-create is entitled to - it ends in
    // a flush and precedes every acknowledgement.
    expeditor->database_->catalog.SetWal(expeditor->wal_.get());

    if (expeditor->config_.cabins) {
        expeditor->cabin_store_.emplace(
            stats::CabinLimits{expeditor->config_.cabin_max_values,
                               expeditor->config_.cabin_max_entries_per_value});
    }
    if (expeditor->config_.waystone_recording) {
        expeditor->trail_recorder_.emplace(expeditor->database_->catalog, *expeditor->store_,
                                           &expeditor->clock_);
    }
    // The undo log, ahead of the rest of the transaction stack, because
    // recovery's undo phase writes through it and must run before anything
    // else in that stack exists (below).
    expeditor->undo_log_.emplace(*expeditor->store_, &*expeditor->wal_);

    // **Recovery, before anything can read a page or issue an id** (RV1,
    // server/mount_recovery.hpp). This is core 0's stream; every peer's is
    // recovered by its own CoreRuntime::Open (RV2 - no order between
    // streams).
    //
    // Where it sits is not a preference. Three things must already be true
    // and two must not yet be:
    //
    //   - the WAL manager is open, so its stream is positioned past the
    //     crash tail and undo's compensations append rather than overwrite
    //     (wal/stream.cpp's ScanTail);
    //   - the WAL gate is installed, so redo's page write-backs obey
    //     WAL-before-data like every other write;
    //   - the superblock is decoded, because the anchor recovery starts
    //     from lives in it;
    //   - **`TrxIdSequence` does not exist yet**, because it caches the
    //     ceiling at construction (txn/trx_id.hpp): raising `next_trx_id`
    //     after building it would change a field nothing reads again, and
    //     the sequence would hand out ids the log already names;
    //   - **no extent has been carved**, because the allocator's search hint
    //     must start above the floor recovery establishes (RC04's
    //     obligation 1, applied at StartPeers below).
    auto recovered = RecoverCoreAtMount(/*core_id=*/0,
                                        expeditor->database_->superblock.wal_anchor(0),
                                        *expeditor->log_device_, *expeditor->store_,
                                        *expeditor->undo_log_, &*expeditor->wal_,
                                        &*expeditor->logger_, &expeditor->clock_);
    if (!recovered.ok()) return recovered.status();
    expeditor->recovery_ = recovered.value();

    // RV3 D3a: redo just mutated catalog pages under a catalog constructed
    // above it. Nothing reads a catalog row between construction and here
    // (bootstrap on an existing database reads only the superblock), so
    // dropping whatever the cache holds is the whole ordering fix - the
    // stated rule is that nothing may start.
    expeditor->database_->catalog.InvalidateFromPeer();

    // RV3's audit (RC09): with catalog mutations logged (2026-08-19) this
    // asks whether the relations the *recovered* catalog describes can be
    // opened. O(relations), one page read each, and it reports rather than
    // refuses - an unopenable relation is the finding, not an error hit
    // while producing it. Zero from here on is RV3's proof.
    expeditor->recovery_ = AuditCatalogAfterRecovery(
        expeditor->database_->catalog, *expeditor->store_, expeditor->recovery_,
        &*expeditor->logger_);

    // DT10 (`ddl-transactional.md` §5c): retire the delete-marked
    // catalog rows a previous mount left behind, before anything can read
    // one. **Here and only here** - after recovery, so a mark this mount's
    // own log restored is included, and before the transaction stack
    // exists, so no live transaction can own a mark this retires.
    //
    // The system core's alone: a peer may not write a catalog page (P6),
    // and by the time a peer mounts, core 0 has already done this.
    auto finalized = expeditor->database_->catalog.FinalizeDeleteMarksAtMount();
    if (!finalized.ok()) return finalized.status();
    expeditor->recovery_.catalog_marks_finalized = finalized.value();

    // The transaction ceiling recovery computed, applied and made durable
    // before the sequence that caches it is built. `SetNextTrxId` refuses to
    // lower one, so the guard is what keeps a log that names nothing from
    // being an error.
    if (expeditor->recovery_.next_trx_id > expeditor->database_->superblock.next_trx_id()) {
        if (Status s =
                expeditor->database_->superblock.SetNextTrxId(expeditor->recovery_.next_trx_id);
            !s.ok()) {
            return s;
        }
        if (Status s = expeditor->PersistSuperBlock(); !s.ok()) return s;
        expeditor->logger_->Info("recovery",
                                 "transaction-id ceiling raised to " +
                                     std::to_string(expeditor->recovery_.next_trx_id) +
                                     " past what the log names");
    }

    // The rest of the transaction stack, before the dispatcher that reads
    // through it. The persist callback is what makes a reserved id block
    // durable: the superblock is unlogged, so a block is only safe once the
    // page has been written and synced (txn/trx_id.hpp records the exposure
    // that leaves).
    Expeditor* self = expeditor.get();
    expeditor->trx_ids_.emplace(expeditor->database_->superblock,
                                [self] { return self->PersistSuperBlock(); });
    expeditor->txn_manager_.emplace(*expeditor->trx_ids_, *expeditor->undo_log_,
                                    *expeditor->store_, &*expeditor->wal_);

    expeditor->dispatcher_.emplace(
        expeditor->database_->superblock, expeditor->database_->catalog, *expeditor->store_,
        &*expeditor->logger_, &expeditor->clock_, &*expeditor->wal_,
        expeditor->config_.durability, exec::Budget(expeditor->config_.max_rows_touched),
        expeditor->trail_recorder_ ? &*expeditor->trail_recorder_ : nullptr,
        expeditor->config_.waystone_replay, expeditor->config_.access_statistics,
        expeditor->cabin_store_ ? &*expeditor->cabin_store_ : nullptr,
        &*expeditor->txn_manager_, expeditor->config_.isolation, /*core_id=*/0,
        expeditor->config_.indexes, expeditor->config_.max_insert_rows);
    expeditor->dispatcher_->set_aggregate_limits(
        exec::AggregateLimits{expeditor->config_.aggregate_max_groups,
                              expeditor->config_.aggregate_max_distinct});
    expeditor->dispatcher_->set_sort_max_rows(expeditor->config_.sort_max_rows);
    expeditor->dispatcher_->set_join_build_max_rows(expeditor->config_.join_build_max_rows);
    // SHOW META's recovery block (RC09). A pointer into the member rather than
    // a copy, so the block reports the mount's own report and cannot drift from
    // it; `recovery_` is declared above the dispatcher and so outlives it.
    expeditor->dispatcher_->set_recovery(&expeditor->recovery_);

    // **Assertion enforcement, resumed** (RC07, AS6a). Here rather than beside
    // the recovery call above, because the registry it refills lives on the
    // dispatcher and the dispatcher did not exist yet - and still before
    // `Serve()`, so no statement is accepted against an unenforcing constraint.
    //
    // `checkpoint_lsn` from this core's anchor is the scan start: that is the
    // checkpoint whose snapshot is the base, which is what AS6a's "from the last
    // checkpoint" means. The anchor read here is the one recovery already used,
    // so the two cannot disagree about which checkpoint that was.
    expeditor->recovery_ = ResumeAssertionsAfterRecovery(
        expeditor->database_->catalog, *expeditor->store_, *expeditor->log_device_,
        /*core_id=*/0, expeditor->database_->superblock.wal_anchor(0).checkpoint_lsn,
        expeditor->dispatcher_->assertions(), expeditor->recovery_, &*expeditor->logger_);

    // **The completion checkpoint** (RC08), which is what makes the next
    // recovery cheap: it publishes an anchor past everything just replayed, so
    // a second crash scans from here instead of from wherever the last periodic
    // checkpoint left off - or, on a database that had never completed one,
    // from the head of the stream every single mount.
    //
    // The target and the anchor are built here rather than beside the
    // checkpointer below, because this call needs them and it must happen
    // before any statement can dirty a page. Both are members, so the
    // checkpointer built later borrows the same two objects and the anchor's
    // publish count spans the mount and the cadence.
    //
    // **Sequenced after the assertion resume above, not before it.** This
    // checkpoint becomes the anchor, so it is the record the *next* mount folds
    // its assertion directories from - and it can only carry their group
    // snapshots if the registry has already been refilled. Written the other way
    // round, enforcement survived exactly one restart: the mount after this one
    // would scan from a checkpoint holding no snapshot and find no base.
    expeditor->checkpoint_target_.emplace(*expeditor->store_);
    expeditor->checkpoint_anchor_.emplace(expeditor->database_->superblock, *expeditor->store_);
    expeditor->checkpoint_anchor_->SetLogger(&*expeditor->logger_);
    if (Status s = CheckpointAfterRecovery(/*core_id=*/0, *expeditor->wal_,
                                           *expeditor->checkpoint_target_,
                                           *expeditor->checkpoint_anchor_, &*expeditor->logger_,
                                           &expeditor->clock_,
                                           &expeditor->recovery_.checkpoint_ns,
                                           &expeditor->dispatcher_->assertions());
        !s.ok()) {
        return s;
    }


    expeditor->dispatcher_->set_relayout(expeditor->config_.physical_optimizer,
                                         expeditor->config_.decay_half_life_ns);
    // PHY01's collector, wired to both feeders: the dispatcher touches
    // S1/S2 per successful SELECT, the Cabin store forwards S3 from its
    // counting sites. One construction site, the config's clock and
    // half-life bound once.
    expeditor->optimizer_signals_.emplace(&expeditor->clock_,
                                          expeditor->config_.decay_half_life_ns);
    expeditor->dispatcher_->set_optimizer_signals(&*expeditor->optimizer_signals_);
    expeditor->dispatcher_->set_cabin_optimizer_enabled(expeditor->config_.cabin_optimizer);
    // PHY04: the decision core and its executor, only where a Cabin can
    // exist at all. The cadence task registers in Serve() beside the
    // checkpointer's.
    if (expeditor->cabin_store_) {
        expeditor->cabin_controller_.emplace(expeditor->config_.CabinOptimizerSettings());
        expeditor->cabin_executor_.emplace(
            expeditor->database_->catalog, *expeditor->store_, *expeditor->cabin_store_,
            *expeditor->cabin_controller_, &*expeditor->txn_manager_);
        expeditor->cabin_store_->set_signals(&*expeditor->optimizer_signals_);
        // PHY06: SHOW CABIN_OPTIMIZER reads both, read-only.
        expeditor->dispatcher_->set_cabin_optimizer_view(&*expeditor->cabin_controller_,
                                                         &*expeditor->cabin_executor_);
    }
    expeditor->logger_->Info("expeditor",
                             std::string("INSERT durability ") +
                                 wal::DurabilityClassName(expeditor->config_.durability) +
                                 ", isolation " +
                                 txn::IsolationLevelName(expeditor->config_.isolation));

    // The target and the anchor already exist - the completion checkpoint
    // above built them, and re-emplacing here would hand the cadence a fresh
    // anchor whose publish count starts at zero, hiding the mount's own.
    expeditor->checkpointer_.emplace(*expeditor->wal_, *expeditor->checkpoint_target_,
                                     *expeditor->txn_manager_,
                                     *expeditor->checkpoint_anchor_);
    // AS6a's snapshot source (RC07): every cadence checkpoint from here on writes
    // each live cabin's group headers, which is what the *next* mount folds onto.
    // The registry is the source because it owns the directories.
    expeditor->checkpointer_->SetAssertionSource(&expeditor->dispatcher_->assertions());
    expeditor->checkpointer_->SetLogger(&*expeditor->logger_);

    if (Status s = expeditor->Sync(); !s.ok()) return s;

    expeditor->logger_->Info("expeditor",
                             "database ready: " +
                                 std::to_string(expeditor->store_->allocated_pages()) +
                                 " pages, superblock version " +
                                 std::to_string(expeditor->database_->superblock.version()));
    return expeditor;
}

Status Expeditor::OpenLog() {
    const std::string path = config_.LogPath();
    if (path.empty()) {
        // No file configured. A Logger over a null sink still satisfies
        // every call site, so nothing downstream has to know.
        logger_.emplace(nullptr, wall_clock_, config_.log_level);
        return Status::OK();
    }

    auto sink = FileLogSink::Open(path);
    if (!sink.ok()) return sink.status();
    log_sink_ = std::move(sink.value());
    logger_.emplace(log_sink_.get(), wall_clock_, config_.log_level);
    return Status::OK();
}

Status Expeditor::PersistSuperBlock() {
    auto page = store_->Get(kSuperBlockPageId);
    if (!page.ok()) return page.status();
    database_->superblock.Encode(page.value().bytes());
    return Sync();
}

Status Expeditor::Checkpoint() {
    // CheckpointStats counters are cumulative over the process, so this
    // one's contribution is the delta. Logging the running total would
    // read as "this checkpoint flushed 5 pages" on every tick after the
    // first one that did.
    const std::uint64_t flushed_before = checkpointer_->stats().pages_flushed;

    Status s = checkpointer_->RunToCompletion();
    if (!s.ok()) {
        // The one place a checkpoint failure becomes visible. It runs on a
        // timer with no caller to return to, so without this it is a
        // silently widening loss window.
        logger_->Error("checkpoint", "checkpoint failed: " + s.message());
        return s;
    }

    const std::uint64_t flushed = checkpointer_->stats().pages_flushed - flushed_before;
    logger_->Debug("checkpoint", "checkpoint complete: redo_start=" +
                                     std::to_string(checkpointer_->redo_start_lsn()) +
                                     " pages_flushed=" + std::to_string(flushed));
    return Status::OK();
}

namespace {

// Pins `thread` to `core_id`. Best-effort by design: a container or a
// restricted cpuset can refuse, and a reactor that runs unpinned is slower
// rather than wrong - so this reports and continues instead of failing the
// server. The platform layer is allowed the syscall (rules.md #4).
void PinToCore(std::thread& thread, std::uint32_t core_id, Logger* log) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(core_id), &set);
    const int rc = pthread_setaffinity_np(thread.native_handle(), sizeof(set), &set);
    if (rc != 0 && log != nullptr && log->enabled(LogLevel::kWarn)) {
        log->Warn("expeditor", "could not pin core " + std::to_string(core_id) +
                                   " (errno " + std::to_string(rc) +
                                   "); it will run unpinned");
    }
}

}  // namespace

void Expeditor::BroadcastCatalogInvalidation(sched::Scheduler& core0_scheduler) {
    if (!transport_.has_value() || cores_.empty()) return;

    // **Flush before telling anyone.** Catalog writes are unlogged and
    // otherwise reach the device only at checkpoint or SYNC, so a peer told
    // to re-read now would read the state *before* this DDL - and conclude
    // the new relation does not exist, permanently, until something else
    // happened to flush. This is the ordering the whole scheme rests on.
    if (Status s = store_->FlushPages(catalog::kEveryCatalogPage); !s.ok()) {
        // Reported and not propagated: the DDL itself has already succeeded
        // and the caller is BumpVersion(), which returns void. The cost is
        // peers that keep a stale catalog until the next flush - stale, not
        // wrong, because a stale catalog answers "not found" and never a
        // wrong row.
        logger_->Error("catalog", "flushing catalog pages before invalidating peers failed: " +
                                      s.message());
        return;
    }

    for (const auto& core : cores_) {
        sched::MessageHeader header{};
        header.src_core = 0;
        header.dst_core = core->core_id();
        header.session_core = 0;
        header.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kCatalogInvalidate);
        header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
        core0_scheduler.Submit(sched::MakeSendRetryTask(*transport_, header, {}));
    }
}

void Expeditor::BroadcastShutdown(sched::Scheduler& core0_scheduler) {
    if (!transport_.has_value()) return;

    for (const auto& core : cores_) {
        sched::MessageHeader header{};
        header.src_core = 0;
        header.dst_core = core->core_id();
        header.session_core = 0;
        header.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kShutdown);
        header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
        core0_scheduler.Submit(sched::MakeSendRetryTask(*transport_, header, {}));
    }

    // Core 0's reactor has already left Run(), so nothing is draining its
    // ready queue - the sends above have to be pumped by hand. Bounded
    // rather than "until empty": a peer whose ring is full and whose reactor
    // has already stopped would otherwise hang the shutdown, and a core that
    // misses its message is joined below anyway once it notices its own
    // stop.
    for (int i = 0; i < 1000 && core0_scheduler.RunOnce(); ++i) {
    }
}

Status Expeditor::Serve() {
    auto io_backend = sched::EpollIoBackend::Create();
    if (!io_backend.ok()) return io_backend.status();

    // The TLS context, built before the port binds so a bad certificate
    // refuses to serve rather than serving briefly and failing per
    // connection. Declared before the listener - stricter than the
    // channel requires (a channel keeps its OpenSSL state alive on its
    // own, tls_channel.hpp), but keeping the context up for the server's
    // whole life makes the ownership story one sentence.
#if KDS_WITH_TLS
    std::optional<TlsContext> tls_context;
#endif
    if (config_.tls) {
#if KDS_WITH_TLS
        auto cert_pem = ReadWholeFile(config_.tls_cert_file, "tls_cert_file");
        if (!cert_pem.ok()) return cert_pem.status();
        auto key_pem = ReadWholeFile(config_.tls_key_file, "tls_key_file");
        if (!key_pem.ok()) return key_pem.status();
        auto ctx = TlsContext::NewServer(cert_pem.value(), key_pem.value());
        if (!ctx.ok()) return ctx.status();
        tls_context.emplace(std::move(ctx.value()));
        logger_->Info("server", "TLS on: every connection on port " +
                                    std::to_string(config_.port) +
                                    " must open with a TLS 1.3 handshake");
#else
        return Status::Unsupported(
            "tls = on, but this server was built without TLS (KDS_WITH_TLS=OFF); "
            "rebuild with OpenSSL or turn the key off");
#endif
    }

    // The credential store, same lifetime story as the TLS context: built
    // before the port binds so a bad users file refuses to serve, alive
    // until every connection's gate is gone.
#if KDS_WITH_TLS
    std::optional<FileCredentialStore> credentials;
#endif
    if (config_.auth_scram) {
#if KDS_WITH_TLS
        auto store = FileCredentialStore::Load(config_.users_file);
        if (!store.ok()) return store.status();
        if (store.value().size() == 0) {
            // An empty users file with auth on is a server nobody can
            // reach - almost certainly a provisioning step that never
            // ran, so it refuses loudly rather than starting uselessly.
            return Status::InvalidArgument("auth = scram, but users file '" +
                                           config_.users_file +
                                           "' holds no users; provision one with --add-user");
        }
        credentials.emplace(std::move(store.value()));
        logger_->Info("server", "auth on: SCRAM-SHA-256, " +
                                    std::to_string(credentials->size()) + " user(s) from " +
                                    config_.users_file);
#else
        return Status::Unsupported(
            "auth = scram, but this server was built without KDS_WITH_TLS; "
            "rebuild with OpenSSL or turn the key off");
#endif
    }

    // With peer listeners on, every core's socket - this one included -
    // must carry SO_REUSEPORT (tcp_server.hpp on why the first binder
    // matters).
    auto listener = TcpServer::Listen(config_.port, config_.peer_listeners);
    if (!listener.ok()) return listener.status();
#if KDS_WITH_TLS
    if (tls_context.has_value()) {
        listener.value().set_channel_factory(
            [ctx = &*tls_context] { return ctx->NewChannel(); });
    }
    if (credentials.has_value()) {
        listener.value().set_auth_gate_factory([store = &*credentials] {
            return std::make_unique<ScramAuthGate>(store);
        });
    }
#endif

    // KWP v0's load endpoint (docs/inflight/in-progress/workplan-kwp-load.md KW1): a second
    // listener, existing only when asked for - kwp_port 0 means no socket
    // is opened at all, so the default instance's surface is unchanged.
    std::optional<KwpLoadServer> kwp_listener;
    if (config_.kwp_port != 0) {
        auto kwp = KwpLoadServer::Listen(config_.kwp_port);
        if (!kwp.ok()) return kwp.status();
        kwp_listener.emplace(std::move(kwp.value()));
    }

    sched::Scheduler scheduler(clock_, io_backend.value());
    scheduler.SetLogger(&*logger_);
    // `SHOW META`'s group-accounting block on core 0 (sched.md §4). The
    // reactor is a local of this function and `dispatcher_` is a member that
    // outlives it, so the view has to be cleared on **every** exit - and
    // there are twenty early `return`s between here and the tail. A guard,
    // not a line at the bottom: a path that missed it would leave the
    // dispatcher pointing at a destroyed reactor, and `SHOW META` through
    // the public `dispatcher()` accessor would then read freed memory.
    // Declared after `scheduler`, so reverse order clears the view first.
    dispatcher_->set_scheduler_view(&scheduler);
    // The statement-shipping client armed below is the same shape of
    // borrow - a member holding this reactor - and is cleared by the same
    // guard, so a dispatch after `Serve` returns refuses as a single-core
    // one does instead of shipping through a destroyed reactor.
    //
    // `index_builds_` is the third of the same shape and is cleared here
    // too: it captures this function's `scheduler` exactly as the shipping
    // client does, and a `CREATE INDEX` through the public `dispatcher()`
    // accessor after `Serve` returned would send on a destroyed reactor.
    // It was installed and never withdrawn before this guard existed - the
    // hazard is the one the `set_scheduler_view` argument above states, and
    // withdrawing all three together is what makes this struct's name true.
    struct ClearReactorBorrows {
        CommandDispatcher* dispatcher;
        ~ClearReactorBorrows() {
            dispatcher->set_scheduler_view(nullptr);
            dispatcher->SetStatementShip(nullptr);
            dispatcher->SetShippedStatements(nullptr);
            dispatcher->SetIndexBuilds(nullptr);
            dispatcher->SetAssertionBuilds(nullptr);
        }
    } clear_reactor_borrows{&*dispatcher_};

    // Core-local, and installed before any statement runs: from here on a
    // coroutine that suspends while holding a page span - or, since P4d-3,
    // a page pin - is detected rather than merely forbidden in prose
    // (exec/step_vm.hpp, sched/coro.hpp). Nothing in the executor suspends
    // yet - this is in place *before* the first thing that can.
    exec::InstallSuspendAudit(store_.get());

    // ---- The fan-out (workplan-crosscore.md P2) -------------------------
    //
    // At `cores = 1` none of this runs: no transport is built, no thread is
    // spawned, and the reactor below is the same single one that has always
    // served. Guideline 2 asks for zero messages and zero allocations on the
    // single-core path, and the cheapest way to mean it is to build nothing.
    std::vector<std::thread> workers;
    if (config_.cores > 1) {
        auto transport = sched::RealRingTransport::Create(
            config_.cores, sched::kCoreRingSlots, sched::kCoreRingPayloadBytes);
        if (!transport.ok()) return transport.status();
        transport_.emplace(std::move(transport.value()));

        // Arms core 0's wake path too (sched/waker.hpp): core 0 is a
        // destination like any other, and a peer's reply to a statement
        // it shipped lands here.
        if (Status s = scheduler.AttachTransport(&*transport_, /*core_id=*/0); !s.ok()) {
            return s;
        }

        // The receiving half of a peer's STOP (CoreRuntime::ListenAndAttach
        // routes it here): stop core 0's reactor, after which Serve's tail
        // broadcasts kShutdown to every peer and joins - the same sequence
        // a STOP on core 0's own listener has always produced.
        sched::Scheduler* core0_sched = &scheduler;
        if (Status s = scheduler.RegisterMessageHandler(
                sched::RingMessageKind::kShutdown,
                [core0_sched, this](const sched::MessageHeader& header,
                                    std::span<const std::byte>) {
                    logger_->Info("expeditor", "stop routed from core " +
                                                   std::to_string(header.src_core) +
                                                   "; the shutdown tail follows");
                    core0_sched->Stop();
                });
            !s.ok()) {
            return s;
        }

        // The system core's half of the anchor path (M5): the superblock is
        // page 0 and belongs to core 0, so a peer's completed checkpoint
        // sends its anchor here and this writes it. The write itself goes
        // through the same SuperBlockCheckpointAnchor a local checkpoint
        // uses, so there is exactly one piece of code that knows how an
        // anchor reaches the page.
        if (Status s = scheduler.RegisterMessageHandler(
                sched::RingMessageKind::kAnchorWrite,
                [this](const sched::MessageHeader& header, std::span<const std::byte> payload) {
                    if (payload.size() != sizeof(AnchorWritePayload)) {
                        logger_->Error("checkpoint",
                                       "anchor write from core " +
                                           std::to_string(header.src_core) + " has " +
                                           std::to_string(payload.size()) +
                                           " bytes, not " +
                                           std::to_string(sizeof(AnchorWritePayload)));
                        return;
                    }
                    AnchorWritePayload fields{};
                    std::memcpy(&fields, payload.data(), sizeof(fields));

                    wal::CheckpointAnchorRecord record;
                    record.core_id = fields.core_id;
                    record.checkpoint_lsn = fields.checkpoint_lsn;
                    record.redo_start_lsn = fields.redo_start_lsn;
                    record.durable_lsn = fields.durable_lsn;
                    record.segment_no = fields.segment_no;

                    if (Status s = checkpoint_anchor_->Publish(record); !s.ok()) {
                        // Nowhere to return it: the sender is fire-and-forget
                        // by design, because a lost anchor costs a longer
                        // replay and never an answer (wal.md §8-3).
                        logger_->Error("checkpoint", "publishing core " +
                                                         std::to_string(fields.core_id) +
                                                         "'s anchor failed: " + s.message());
                    }
                });
            !s.ok()) {
            return s;
        }

        // Every peer's page-id lease is carved here, on the startup thread,
        // out of core 0's free map - which is the only writer of it (M5).
        //
        // The search starts above recovery's page floor, not at
        // `kFirstUserPageId` (RC04's obligation 1, `wal/high_water.hpp`).
        // The free map is unlogged, so a crash can revert it while the log
        // still names pages above it; recovery raises the *store's*
        // allocation floor past them, but an extent is carved from the map
        // rather than through that floor - so a lease could otherwise cover
        // exactly the pages redo just wrote. Same hazard, multicore shape.
        // `Reserve` never scans below its hint (extent_lease.cpp), so raising
        // it is a guarantee and not an optimization. Floored at
        // `kFirstUserPageId` because a log naming only system pages must not
        // *lower* where user extents start.
        const PageId extent_hint = recovery_.page_floor_raised
                                       ? std::max<PageId>(recovery_.page_floor, kFirstUserPageId)
                                       : kFirstUserPageId;
        // Over the store, not its bytes: a reservation must mark the map
        // dirty itself (extent_lease.hpp, PW3b's finding).
        extents_.emplace(*store_, extent_hint);

        for (std::uint32_t core_id = 1; core_id < config_.cores; ++core_id) {
            auto lease = extents_->Reserve(storage::kDefaultExtentPages);
            if (!lease.ok()) return lease.status();

            CoreRuntime::Config core_config;
            core_config.core_id = core_id;
            core_config.wal_dir = config_.wal_dir;
            core_config.checkpoint_interval_ns = config_.checkpoint_interval_ns;
            core_config.wal_drain_interval_ns = config_.wal_drain_interval_ns;
            core_config.inline_cell_width = database_->superblock.inline_cell_width();
            core_config.core_count = database_->superblock.core_count();
            core_config.durability = config_.durability;
            core_config.isolation = config_.isolation;
            core_config.budget = exec::Budget(config_.max_rows_touched);
            core_config.buffer_pool_frames =
                FrameBudgetShare(config_.buffer_pool_frames, config_.cores);
            core_config.lease = lease.value();
            // This peer's own anchor, copied out of the superblock core 0
            // decoded. A peer's `SuperBlock` member is a default-constructed
            // one whose anchor slots are all zero, and a peer's checkpointer
            // publishes through core 0 (remote_checkpoint_anchor.hpp) - so
            // without this copy every peer would recover from the head of its
            // stream while core 0 recovered from its checkpoint.
            core_config.anchor = database_->superblock.wal_anchor(core_id);
            // And the ceiling this peer's recovery must not sit above (PW1).
            // Same copy, same thread, same reason as the anchor.
            core_config.next_trx_id = database_->superblock.next_trx_id();

            auto core = CoreRuntime::Open(core_config, *device_, clock_, &*logger_);
            if (!core.ok()) return core.status();
            if (Status s = core.value()->AttachTransport(*transport_); !s.ok()) return s;
            if (config_.peer_listeners) {
                if (Status s = core.value()->ListenAndAttach(config_.port); !s.ok()) {
                    return s;
                }
            }
            cores_.push_back(std::move(core.value()));
        }

        // The reservations above set free-map bits that only exist in
        // memory until something writes the page. A peer's first allocation
        // must not be an id a restart would think free.
        if (Status s = Sync(); !s.ok()) return s;

        // Core 0's half of the page-id lease service (P5): a peer at its
        // low-water mark asks here, and this carves the next extent. The
        // reservation is synchronous because on this core it is a local
        // call - it is the *asking* that had to wait for coroutines.
        if (Status s = RegisterExtentGrantHandler(scheduler, *transport_, *extents_,
                                                   storage::kDefaultExtentPages, &*logger_);
            !s.ok()) {
            return s;
        }

        // The session side of remote reads (workplan P4c): core 0 ships an
        // eligible single-step read to the owning core and awaits its
        // batches. Registered before the dispatcher learns about it so a
        // reply can never beat its handlers.
        // One send for both pipeline endpoints on this core - the client
        // below and the step server after it. Written once so the two
        // cannot drift on src_core or scheduling group.
        auto step_send = [this, &scheduler](std::uint32_t dst, sched::RingMessageKind kind,
                                            std::vector<std::byte> payload) {
            sched::MessageHeader out{};
            out.src_core = 0;
            out.dst_core = dst;
            out.session_core = 0;
            out.kind = static_cast<std::uint16_t>(kind);
            out.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kForeground);
            scheduler.Submit(sched::MakeSendRetryTask(*transport_, out, payload));
            return Status::OK();
        };
        remote_reads_.emplace(/*core_id=*/0, step_send, &*logger_);
        // Core 0's own step server (workplan P4d-4b-3): a stage placed on
        // a relation core 0 owns is served here, like any peer serves its
        // own - the missing half that made "every stage's core serving"
        // true. Producers and consumers land on core 0's one reactor.
        remote_steps_.emplace(
            database_->catalog, *store_, /*core_id=*/0, step_send, &*logger_,
            kStepBatchTargetBytes,
            [&scheduler](std::unique_ptr<sched::Task> task) {
                scheduler.Submit(std::move(task));
            },
            txn_manager_.has_value() ? &*txn_manager_ : nullptr,
            // And the same row-touch ceiling every statement on this core
            // runs under (P4d-4c's review).
            exec::Budget(config_.max_rows_touched));
        if (Status s = scheduler.RegisterMessageHandler(
                sched::RingMessageKind::kStepOpen,
                [this](const sched::MessageHeader& header, std::span<const std::byte> payload) {
                    remote_steps_->OnStepOpen(header, payload);
                });
            !s.ok()) {
            return s;
        }
        if (Status s = scheduler.RegisterMessageHandler(
                sched::RingMessageKind::kStepCredit,
                [this](const sched::MessageHeader&, std::span<const std::byte> payload) {
                    remote_steps_->OnStepCredit(payload);
                });
            !s.ok()) {
            return s;
        }
        if (Status s = scheduler.RegisterMessageHandler(
                sched::RingMessageKind::kStepCancel,
                [this](const sched::MessageHeader&, std::span<const std::byte> payload) {
                    remote_steps_->OnStepCancel(payload);
                });
            !s.ok()) {
            return s;
        }
        // The two kinds both consumers hear: a scheduler holds exactly one
        // handler per kind (the map assigns), so core 0 - the one core
        // hosting a session client *and* a step server - fans each payload
        // to both. Safe because the tag is the demultiplexer: each
        // consumer discards a tag it does not hold, silently (§3).
        if (Status s = scheduler.RegisterMessageHandler(
                sched::RingMessageKind::kStepBatch,
                [this](const sched::MessageHeader&, std::span<const std::byte> payload) {
                    remote_reads_->OnStepBatch(payload);
                    remote_steps_->OnStepBatch(payload);
                });
            !s.ok()) {
            return s;
        }
        if (Status s = scheduler.RegisterMessageHandler(
                sched::RingMessageKind::kStepEof,
                [this](const sched::MessageHeader&, std::span<const std::byte> payload) {
                    remote_reads_->OnStepEof(payload);
                    remote_steps_->OnStepEof(payload);
                });
            !s.ok()) {
            return s;
        }
        if (Status s = scheduler.RegisterMessageHandler(
                sched::RingMessageKind::kStepError,
                [this](const sched::MessageHeader&, std::span<const std::byte> payload) {
                    remote_reads_->OnStepError(payload);
                });
            !s.ok()) {
            return s;
        }
        dispatcher_->SetRemoteReads(&*remote_reads_);

        // Core 0's index-build client (PW1c-6b-4): the foreign arm of
        // CREATE INDEX sends the owner a build request and parks on the
        // reply here. Registered before the dispatcher learns about it,
        // remote_reads_' rule, so a reply cannot beat its receiver.
        index_builds_.emplace(scheduler, *transport_, clock_, &*logger_);
        if (Status s = index_builds_->RegisterReplyReceiver(); !s.ok()) return s;
        dispatcher_->SetIndexBuilds(&*index_builds_);

        // Core 0's assertion-build client (PW1c-6c), the same shape and the
        // same ordering rule: a relation another core owns has its Bound
        // Cabin built there, because the owner's writes are what maintain
        // it.
        assertion_builds_.emplace(scheduler, *transport_, clock_, &*logger_);
        if (Status s = assertion_builds_->RegisterReplyReceiver(); !s.ok()) return s;
        dispatcher_->SetAssertionBuilds(&*assertion_builds_);

        // **Core 0's two halves of statement shipping** (SS1/SS3): the
        // owner's, because a peer ships core 0 every statement against a
        // relation core 0 owns, and the arrival core's, because core 0's
        // own clients name peer-owned relations. Registered before the
        // dispatcher is told about the client, for `index_builds_`' reason -
        // a reply must never beat its receiver.
        //
        // The executor is built first: the server holds its seam. Both
        // borrow this function's `scheduler`, exactly as `index_builds_`
        // does, and nothing pumps them after `Serve` returns.
        shipped_executor_.emplace(/*core_id=*/0, *dispatcher_, scheduler, clock_, &*logger_);
        statement_ship_server_.emplace(/*core_id=*/0, scheduler, *transport_,
                                       shipped_executor_->Seam(), &*logger_);
        if (Status s = scheduler.RegisterMessageHandler(
                sched::RingMessageKind::kShippedStatementRequest,
                [this](const sched::MessageHeader& header, std::span<const std::byte> payload) {
                    statement_ship_server_->OnRequest(header, payload);
                });
            !s.ok()) {
            return s;
        }
        statement_ship_client_.emplace(/*core_id=*/0, scheduler, *transport_, clock_,
                                       &*logger_);
        if (Status s = statement_ship_client_->RegisterReplyReceiver(); !s.ok()) return s;
        dispatcher_->SetStatementShip(&*statement_ship_client_);
        dispatcher_->SetShippedStatements(&*shipped_executor_);

        // The row-id lease's grant side (P5's shape): a peer's kRowIdLease
        // request is answered with a block carved by AllocateRowIdRange -
        // the bulk-INSERT primitive, already ceiling-checked. Core 0 is the
        // one core that may write the sequence page, which is the whole
        // reason this service exists.
        if (Status s = RegisterRowIdGrantHandler(scheduler, *transport_, database_->catalog,
                                                 &*logger_);
            !s.ok()) {
            return s;
        }

        // The transaction-id lease's grant side (PW1,
        // `docs/inflight/in-progress/workplan-peer-writer.md`): a peer's kTrxIdLease request is
        // answered with a block from core 0's **own** sequence, through the
        // same `Carve()` its own windows come from. Sharing that one carve
        // is what keeps two consumers of one ceiling from colliding, and it
        // persists the raise before replying - a grant whose ceiling a
        // crash could forget is the one thing `CoreRuntime::Open`'s
        // mount-time refusal cannot tell from a corrupt stream.
        if (Status s =
                RegisterTrxIdGrantHandler(scheduler, *transport_, *trx_ids_,
                                          kTrxIdLeasePerGrant, &*logger_);
            !s.ok()) {
            return s;
        }

        // Core 0's DDL choke point, wired to the broadcast. Installed after
        // the peers exist so the loop below always has somebody to tell.
        database_->catalog.SetInvalidationHook([this, &scheduler] {
            BroadcastCatalogInvalidation(scheduler);
        });

        // The send side of CC7's flush-then-grant handoff (workplan P6c):
        // a relation placed on a peer gets that peer fault rights over its
        // pages, flush strictly first - a grant to unflushed pages would
        // hand the owner stale bytes. Named rather than passed inline
        // because it has two callers since PW1c-7: CREATE TABLE's publish
        // and the owner's re-delivery request (below) run the identical
        // sequence, and the second exists precisely so that nothing else
        // ever does.
        const catalog::Catalog::RelationPublishHook publish =
            [this, &scheduler](catalog::Oid oid, std::uint32_t owner_core, PageId root,
                               PageId varheap_root, PageId anchor) {
                catalog::SysTableRow row{};
                row.desc_page_id = root;
                row.varheap_page_id = varheap_root;
                row.anchor_page_id = anchor;
                const storage::Extent range =
                    RelationFaultExtentOf(row, storage::kDefaultExtentPages);

                std::vector<PageId> pages;
                pages.reserve(range.count);
                for (PageId id = range.first; id < range.end(); ++id) pages.push_back(id);
                if (Status s = store_->FlushPages(pages); !s.ok()) {
                    // Reported, not propagated: the DDL succeeded, and an
                    // ungranted relation is refused retryably rather than
                    // served wrong - BroadcastCatalogInvalidation's stance.
                    logger_->Error("catalog", "flushing relation oid=" + std::to_string(oid) +
                                                  " before its fault grant failed: " +
                                                  s.message());
                    return;
                }

                // PW1c-4: flush (above) → durable handoff records → grants
                // (PL §9 rule 1; PrepareRelationHandoff owns the middle).
                // A failed prepare withholds the write grant - the
                // relation stays fault-readable, its writes refused
                // retryably, never served unsound.
                const PageId formatted_pages[] = {root, varheap_root, anchor};
                auto write_grant = PrepareRelationHandoff(&*wal_, owner_core, formatted_pages);
                if (!write_grant.ok()) {
                    logger_->Error("catalog", "relation oid=" + std::to_string(oid) +
                                                  ": " + write_grant.status().message());
                }

                const auto send = [&](sched::RingMessageKind kind, const auto& pod) {
                    std::byte payload[sizeof(pod)];
                    std::memcpy(payload, &pod, sizeof(pod));
                    sched::MessageHeader header{};
                    header.src_core = 0;
                    header.dst_core = owner_core;
                    header.session_core = 0;
                    header.kind = static_cast<std::uint16_t>(kind);
                    header.sched_group =
                        static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
                    // The task copies the payload (send_retry.hpp owns
                    // it), so a stack buffer is enough.
                    scheduler.Submit(sched::MakeSendRetryTask(*transport_, header, payload));
                };
                send(sched::RingMessageKind::kRelationFaultGrant,
                     ExtentGrantPayload{range.first, range.count});
                if (write_grant.ok()) {
                    send(sched::RingMessageKind::kRelationWriteGrant, write_grant.value());
                    // Complete the departure on this side (the 95b45e8
                    // review's C4): core 0 keeps clean frames of pages
                    // another core now owns; evicting them removes the
                    // stale-read window. Best-effort - a dirty frame
                    // refusal here would mean the flush above lied.
                    const std::span<const PageId> departed(
                        write_grant.value().page_ids, write_grant.value().count);
                    if (Status s = store_->EvictClean(departed); !s.ok()) {
                        logger_->Error("catalog",
                                       "evicting handed-off pages failed: " + s.message());
                    }
                }
            };
        database_->catalog.SetRelationPublishHook(publish);

        // PW1c-7 (relation_grant_service.hpp): an owner that finds itself
        // without a relation's write rights - after a restart, a crash
        // before its acquisition, or a message lost to the ring - asks, and
        // core 0 answers by running the same publish. Idempotent: a second
        // handoff record for a page is analysis's no-op, and the receive
        // side takes no second acquisition on a page already its own.
        if (Status s = RegisterRelationGrantHandler(scheduler, database_->catalog, publish,
                                                    &*logger_);
            !s.ok()) {
            return s;
        }

        // Spawned only after every core is built, so a failure above leaves
        // no thread to unwind.
        for (auto& core : cores_) {
            workers.emplace_back([&core] { core->Run(); });
            PinToCore(workers.back(), core->core_id(), &*logger_);
        }
        logger_->Info("expeditor", "running " + std::to_string(config_.cores) +
                                       " cores; core 0 serves every statement until the "
                                       "per-core catalog cache exists (workplan P6)");
    }
    // **The stop signal, as an ordinary readable fd** (`server/stop_signal.hpp`).
    // Registered here rather than polled, so a `systemctl stop` or a Ctrl-C takes
    // the same path a client's `STOP` does: the scheduler stops, the workers
    // join, and the shutdown tail below runs its final sync and checkpoint. Until
    // this, those signals killed the process mid-statement and the next mount
    // recovered as if from a crash.
    //
    // Absent for every in-process caller, which is what keeps a test from
    // inheriting signal behaviour it did not ask for.
    if (stop_signal_ != nullptr && stop_signal_->installed()) {
        StopSignal* stop = stop_signal_;
        Logger* log = &*logger_;
        sched::Scheduler* sched_ptr = &scheduler;
        if (Status s = scheduler.RegisterIoHandler(
                stop->fd(), sched::IoInterest::kReadable,
                [stop, log, sched_ptr](const sched::IoEvent&) {
                    // Drained, or a level-triggered epoll reports the same
                    // delivery forever and the reactor spins instead of stopping.
                    const std::uint32_t signo = stop->Consume();
                    log->Info("expeditor", "stopping on signal " + std::to_string(signo) +
                                               "; the shutdown sync and checkpoint follow");
                    sched_ptr->Stop();
                });
            !s.ok()) {
            return s;
        }
    }

    if (Status s = listener.value().Attach(scheduler, *dispatcher_, &*logger_); !s.ok()) {
        return s;
    }
    if (kwp_listener.has_value()) {
        if (Status s = kwp_listener->Attach(scheduler, *dispatcher_, &*logger_); !s.ok()) {
            return s;
        }
    }

    // The `system`-group cadence of wal.md section 11. A failed checkpoint
    // is not fatal and does not disarm the timer: the pages it did not
    // flush stay dirty and the next tick retries them, which is the same
    // recovery the paced Step() path already relies on. Reporting it needs
    // the observability path that does not exist yet - the failure is
    // visible in checkpoint_stats(), where started outruns completed.
    if (config_.checkpoint_interval_ns > 0) {
        scheduler.SubmitEvery(config_.checkpoint_interval_ns, [this] { (void)Checkpoint(); });
        logger_->Info("expeditor", "checkpoint cadence " +
                                       std::to_string(config_.checkpoint_interval_ns / 1'000'000) +
                                       "ms");
    } else {
        logger_->Warn("expeditor",
                      "checkpoint cadence disabled; durability is SYNC and shutdown only");
    }

    // PHY04's cadence: snapshot → Decide → Apply, PO8's switch read at
    // every boundary inside Tick. Registered only when the executor exists
    // and the interval is non-zero (0 = no cadence, the standing meaning);
    // with the boot switch off the tick is one predicate and a return.
    if (cabin_executor_ && config_.cabin_optimizer_snapshot_interval_ms > 0) {
        const sched::MonoTimeNs interval =
            config_.cabin_optimizer_snapshot_interval_ms * 1'000'000ULL;
        scheduler.SubmitEvery(interval, [this] {
            Status ticked = cabin_executor_->Tick(
                *optimizer_signals_, [this] { return dispatcher_->cabin_optimizer_enabled(); });
            if (!ticked.ok() && logger_->enabled(LogLevel::kWarn)) {
                logger_->Warn("expeditor", "cabin optimizer tick failed: " + ticked.message());
            }
        });
        logger_->Info("expeditor",
                      "cabin optimizer cadence " +
                          std::to_string(config_.cabin_optimizer_snapshot_interval_ms) +
                          "ms, switch " + (config_.cabin_optimizer ? "on" : "off"));
    }

    // EVT03's background writeback: drains spec-eviction §4's dirty queue -
    // pages a sweep found dirty at usage zero and queued instead of
    // reclaiming. One bounded batch per tick is the cooperative-yield
    // boundary. **Idle today by construction**: the queue only fills when
    // the sweep runs, and nothing calls the sweep until the PageRef
    // migration lands - so this registration is the task existing ahead of
    // its work, the same stance the sweep itself takes. The watermark loop
    // (MaintainFreeReserve) joins the body when EVT02's bounded pool gives
    // it real numbers; a cadence key follows with EVT04's protocol.
    constexpr sched::MonoTimeNs kWritebackIntervalNs = 50'000'000;  // 50 ms [PROPOSED]
    scheduler.SubmitEvery(kWritebackIntervalNs, [this] {
        auto drained = store_->DrainDirtyEvictionQueue();
        if (!drained.ok() && logger_->enabled(LogLevel::kWarn)) {
            logger_->Warn("expeditor", "writeback drain failed: " + drained.status().message());
        }
    });

    // The other `system`-group task of wal.md section 6-2/6-3. It is what
    // bounds a kRelaxed commit's loss window and what resolves a kGroup
    // batch no committer is waiting on; a tick with nothing staged does no
    // I/O, so the interval is chosen for the loss window, not for cost.
    auto drain = [this]() -> bool {
        // The bool is the post-task hook's answer to the idle policy: a
        // tick with a commit staged did work a parked statement is waiting
        // on (Scheduler::SetPostTaskHook). Read before the drain clears it.
        const bool had_staged_commits = wal_->HasPendingGroupCommits();
        if (Status s = wal_->DrainOnce(); !s.ok()) {
            // Same shape as the checkpoint timer: no caller to return
            // to, so the log is the only place this becomes visible.
            logger_->Error("wal", "drain failed: " + s.message());
        }
        return had_staged_commits;
    };

    // **The group committer.** Once per reactor iteration, after every
    // runnable statement has staged whatever it is going to stage, so one
    // device sync covers all of them. A committing statement parks instead
    // of syncing on its own stack (command_dispatcher.hpp's `pending_lsn`),
    // and this is what it parks *for*: without it the parked statement has
    // nothing to wake it until the timer below fires, and with the timer
    // alone every commit would pay that interval.
    scheduler.SetPostTaskHook(drain);

    if (config_.wal_drain_interval_ns > 0) {
        scheduler.SubmitEvery(config_.wal_drain_interval_ns, drain);
    } else {
        logger_->Warn("wal", "drain cadence disabled; relaxed commits stay unsynced "
                             "until checkpoint or shutdown");
    }

    logger_->Info("expeditor", "listening on 127.0.0.1:" + std::to_string(config_.port));
    scheduler.Run();
    // Same teardown rule as CoreRuntime::Run: the audit reads store_, and
    // the pointer must go before the store does.
    exec::UninstallSuspendAudit();
    logger_->Info("expeditor", "stopping");

    // Every peer is told to stop, then joined. The message is how a core is
    // stopped at all - `Scheduler::Stop()` writes a plain bool owned by that
    // core's own thread (ring_message.hpp's kShutdown says why).
    BroadcastShutdown(scheduler);
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    // After the join, so nothing is still appending to a stream being
    // synced. Each core's log is its own, so this is N independent syncs
    // and not a barrier.
    // The review's BUG 4: a peer's listener teardown rolls back open
    // sessions and can append CLRs, so it must run *before* this core's
    // final sync - detach-then-sync, the order core 0's own teardown
    // above already has.
    for (auto& core : cores_) {
        core->CloseListener();
    }
    for (auto& core : cores_) {
        if (Status s = core->Sync(); !s.ok()) {
            logger_->Error("expeditor", "core " + std::to_string(core->core_id()) +
                                            ": final log sync failed: " + s.message());
        }
        // **A peer's shutdown checkpoint** (PW3b, `core_runtime.hpp` carries
        // the contract): the third checkpoint point this core's tail below
        // has had since RC08 and a peer had not. Sound here for the reason
        // `Sync()` above is - after the join this thread owns the peer, and
        // it has owned core 0 all along - and not fatal on the same terms as
        // core 0's own below.
        if (Status s = core->ShutdownCheckpoint(*checkpoint_anchor_); !s.ok()) {
            logger_->Error("expeditor",
                           "core " + std::to_string(core->core_id()) +
                               ": the shutdown checkpoint failed, so its next mount will "
                               "replay from the previous anchor: " +
                               s.message());
        }
    }
    cores_.clear();
    transport_.reset();

    // Torn down before the scheduler leaves scope: both hold fds
    // registered with it.
    listener.value().Detach();

    // **A checkpoint on the way out, so the next mount does not re-read this
    // run's whole log.** A clean stop used to sync and stop there, which left
    // the anchor wherever the last cadence tick had put it - so the first mount
    // after a graceful shutdown rescanned every record written since, and redid
    // none of them. Measured: a cleanly stopped 2000-row instance re-read all
    // 10,883 of its own records (`bench/results-wal-recovery.md`).
    //
    // RC08 gave a *crash* a bounded next mount by checkpointing at the end of
    // recovery; this is the same call at the other end, and it is what makes the
    // bound hold for a stop as well. It also carries the assertion group
    // snapshots, because the cadence checkpointer already has that source
    // installed - so enforcement resumes from this record too.
    //
    // Not fatal if it fails, and it must not be: the data is already durable
    // through the syncs above, and refusing to exit over a slower *next* start
    // would trade a real shutdown for a bounded inconvenience. Reported and
    // continued, the cadence path's rule.
    Status s = Sync();
    if (!s.ok()) {
        logger_->Error("expeditor", "final sync failed: " + s.message());
    } else {
        logger_->Info("expeditor", "stopped cleanly; " +
                                       std::to_string(store_->allocated_pages()) +
                                       " pages persisted");
    }

    // **The checkpoint goes after the sync, and the order is the whole point.**
    //
    // A checkpoint's redo start is `min(recLSN)` over the dirty table it
    // snapshots at BEGIN (wal.md §11-3) - so checkpointing *before* the final
    // sync publishes an anchor pointing at the oldest page still dirty, which on
    // a busy run is near the start of the log. Measured: a 300-row instance
    // stopped that way still had its next mount read 1205 records. Synced first,
    // the dirty table is empty, the redo start is the CHECKPOINT_BEGIN LSN
    // itself, and the next mount reads only this checkpoint's own two records.
    //
    // This is the same trick RC08's mount checkpoint gets for free: it runs
    // before any statement has dirtied anything.
    if (Status ckpt = Checkpoint(); !ckpt.ok()) {
        logger_->Error("expeditor",
                       "the shutdown checkpoint failed, so the next mount will replay from the "
                       "previous anchor: " +
                           ckpt.message());
    }
    return s;
}

}  // namespace kds::server
