// ckdbs-sim — the standalone simulation harness binary (bench/workplan-
// teststrategy SIM01/SIM04). Deliberately not a gtest: a framework's
// fixture lifecycle fights crash-restart iteration, and CI wants one
// process, one verdict line, one exit code.
//
//   ckdbs-sim --seed N [--ops N] [--iterations N]
//             [--mode clean|sync-crash|crash]
//             [--profile uniform|zipfian|colliding]
//             [--faults none|io] [--fault-rate N]
//             [--toggles off|on|cabins|waystone] [--pair]
//             [--minimize [--out FILE]]
//   ckdbs-sim --replay FILE
//
// Prints the verdict line (and, on failure, the seed/iteration/op that
// reproduce it) and exits 0 on ok, 1 on failure, 2 on usage error.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "sim/loop.hpp"
#include "sim/minimize.hpp"

namespace {

int Usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --seed N [--ops N] [--iterations N]\n"
                 "          [--mode clean|sync-crash|crash]\n"
                 "          [--profile uniform|zipfian|colliding]\n"
                 "          [--faults none|io] [--fault-rate N]\n"
                 "          [--toggles off|on|cabins|waystone] [--pair]\n"
                 "          [--minimize [--out FILE] [--max-replays N]] | [--replay FILE]\n"
                 "          [--skip-recovery]\n",
                 argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    kds::sim::SimConfig config;
    bool have_seed = false;
    bool pair = false;
    bool minimize = false;
    std::string out_path = "case.sim";
    std::string replay_path;
    // A shrink is a few hundred whole instances; the bound keeps a
    // diagnostic from becoming an overnight job, and MinimizeFailure says
    // where it stopped rather than pretending it converged.
    std::size_t max_replays = 400;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (arg == "--seed" && value) {
            config.seed = std::strtoull(value, nullptr, 10);
            have_seed = true;
            ++i;
        } else if (arg == "--ops" && value) {
            config.ops = std::strtoull(value, nullptr, 10);
            ++i;
        } else if (arg == "--iterations" && value) {
            config.iterations = std::strtoull(value, nullptr, 10);
            ++i;
        } else if (arg == "--mode" && value) {
            const auto mode = kds::sim::ParseSimMode(value);
            if (!mode.has_value()) return Usage(argv[0]);
            config.mode = *mode;
            ++i;
        } else if (arg == "--profile" && value) {
            const auto profile = kds::sim::ParseProfile(value);
            if (!profile.has_value()) return Usage(argv[0]);
            config.profile = *profile;
            ++i;
        } else if (arg == "--faults" && value) {
            const auto faults = kds::sim::ParseFaultProfile(value);
            if (!faults.has_value()) return Usage(argv[0]);
            config.faults = *faults;
            ++i;
        } else if (arg == "--fault-rate" && value) {
            config.fault_rate = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            ++i;
        } else if (arg == "--toggles" && value) {
            // Pinning the advisory switches, which is what a repro needs:
            // the seeded draw is the production shape, but a divergence has
            // to be re-runnable with the feature named.
            const std::string toggles = value;
            kds::sim::FeatureToggles pinned;
            if (toggles == "off") {
                pinned.access_statistics = false;
            } else if (toggles == "on") {
                pinned.waystone = pinned.cabins = pinned.access_statistics = true;
            } else if (toggles == "cabins") {
                pinned.cabins = true;
                pinned.access_statistics = false;
            } else if (toggles == "waystone") {
                pinned.waystone = true;
                pinned.access_statistics = false;
            } else {
                return Usage(argv[0]);
            }
            config.toggles = pinned;
            ++i;
        } else if (arg == "--pair") {
            // The toggle pairing (SIM06): no crash, no faults, two
            // instances differing only in the advisory switches.
            pair = true;
        } else if (arg == "--minimize") {
            minimize = true;
        } else if (arg == "--out" && value) {
            out_path = value;
            ++i;
        } else if (arg == "--max-replays" && value) {
            max_replays = std::strtoull(value, nullptr, 10);
            ++i;
        } else if (arg == "--replay" && value) {
            replay_path = value;
            ++i;
        } else if (arg == "--skip-recovery") {
            // The fault injection, not a mode (sim/instance.hpp): boot over
            // the crashed devices without the recovery phase, which is the
            // engine as it stood before RV1. What it is for is showing that
            // the crash contract's assertion can fail - and what it costs,
            // measured, is the mount work recovery does.
            config.skip_recovery = true;
        } else {
            return Usage(argv[0]);
        }
    }
    // A replay needs no seed: the case file *is* the run (SIM07).
    if (!replay_path.empty()) {
        auto loaded = kds::sim::ReadCase(replay_path);
        if (!loaded.ok()) {
            std::fprintf(stderr, "%s\n", loaded.status().message().c_str());
            return 2;
        }
        const kds::sim::SimVerdict verdict =
            kds::sim::RunPlan(loaded.value().config, loaded.value().plan);
        std::printf("%s\n", verdict.Summary(loaded.value().config).c_str());
        return verdict.ok ? 0 : 1;
    }

    if (!have_seed || config.ops == 0 || config.iterations == 0) return Usage(argv[0]);

    if (minimize) {
        const kds::sim::MinimizeOutcome outcome = kds::sim::MinimizeFailure(config, max_replays);
        std::printf("%s\n", outcome.Summary().c_str());
        if (!outcome.found_failure) return 0;
        if (kds::Status s =
                kds::sim::WriteCase(out_path, config, outcome.plan, outcome.signature);
            !s.ok()) {
            std::fprintf(stderr, "%s\n", s.message().c_str());
            return 2;
        }
        std::printf("wrote %s\n", out_path.c_str());
        return 1;  // a failure was found: the exit code says so
    }

    const kds::sim::SimVerdict verdict =
        pair ? kds::sim::RunTogglePairing(config) : kds::sim::RunSimulation(config);
    std::printf("%s\n", verdict.Summary(config).c_str());
    return verdict.ok ? 0 : 1;
}
