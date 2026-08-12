// ckdbs-sim — the standalone simulation harness binary (bench/workplan-
// teststrategy SIM01/SIM04). Deliberately not a gtest: a framework's
// fixture lifecycle fights crash-restart iteration, and CI wants one
// process, one verdict line, one exit code.
//
//   ckdbs-sim --seed N [--ops N] [--iterations N]
//             [--mode clean|sync-crash|crash]
//             [--profile uniform|zipfian|colliding]
//
// Prints the verdict line (and, on failure, the seed/iteration/op that
// reproduce it) and exits 0 on ok, 1 on failure, 2 on usage error.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "sim/loop.hpp"

namespace {

int Usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --seed N [--ops N] [--iterations N]\n"
                 "          [--mode clean|sync-crash|crash]\n"
                 "          [--profile uniform|zipfian|colliding]\n"
                 "          [--skip-recovery]\n",
                 argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    kds::sim::SimConfig config;
    bool have_seed = false;

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
            const std::string mode = value;
            if (mode == "clean") config.mode = kds::sim::SimMode::kClean;
            else if (mode == "sync-crash") config.mode = kds::sim::SimMode::kSyncCrash;
            else if (mode == "crash") config.mode = kds::sim::SimMode::kCrash;
            else return Usage(argv[0]);
            ++i;
        } else if (arg == "--profile" && value) {
            const std::string profile = value;
            if (profile == "uniform") config.profile = kds::sim::Profile::kUniform;
            else if (profile == "zipfian") config.profile = kds::sim::Profile::kZipfian;
            else if (profile == "colliding") config.profile = kds::sim::Profile::kColliding;
            else return Usage(argv[0]);
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
    if (!have_seed || config.ops == 0 || config.iterations == 0) return Usage(argv[0]);

    const kds::sim::SimVerdict verdict = kds::sim::RunSimulation(config);
    std::printf("%s\n", verdict.Summary(config).c_str());
    return verdict.ok ? 0 : 1;
}
