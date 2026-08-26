// fdatasync overlap probe - standalone, deliberately outside the server.
//
// The question (docs/inflight/in-progress/workplan-peer-writer.md PW6 §7, left open): can two or
// more cores overlap `fdatasync` on one volume? If they cannot, the ceiling
// on multi-core ingest throughput belongs to the I/O backend decision and
// not to the cross-core architecture, and every ingest ratio in the
// multicore matrix would be misread as an architecture failure.
//
// Shape: N threads, each with its own open() fd on its own file on the
// target device, each looping one page-sized pwrite + fdatasync. Report
// syncs/second at N = 1, 2, 3, 4 and the N=4 : N=1 ratio.
//
// Scope caveat, because it decides how the ratio may be quoted: separate
// files means separate inodes. ext4/xfs serialize fdatasync on the inode
// lock, so N threads syncing *one* file is a different (worse) number than
// this one. This probe answers "N cores, N WAL streams", which is the
// engine's shape; it does not answer "N cores, one shared file".
//
// Files are pre-allocated and pre-synced so the steady-state loop measures
// data sync only - a size-extending write would drag metadata into every
// fdatasync and measure the journal instead of the overlap. The region is
// written with real pwrites rather than fallocate() on purpose: fallocate
// leaves unwritten extents, and the first overwrite of one is a metadata
// conversion, which is the very thing being excluded.
//
// Build (not part of the CMake targets, on purpose):
//   g++ -O2 -std=c++20 -pthread tools/fdatasync_probe.cpp -o build-release/fdatasync_probe

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <latch>
#include <string>
#include <thread>
#include <vector>

namespace {

// One 8192-byte page, matching hard invariant 1 - the unit the engine syncs.
constexpr std::size_t kPageBytes = 8192;
// Pre-allocated region per file; the loop rotates over it so no write ever
// extends the file, and so no single block stays hot enough for a device
// write cache to absorb the whole run. 256 pages = 2 MiB, comfortably past
// any plausible on-device cache line and still trivial to pre-write.
constexpr std::size_t kRegionPages = 256;
// Drained between "every thread is pre-allocated" and "start the clock".
// Not the barrier - the latch below is - but the previous repetition's
// unlink() defers its block frees to the filesystem's commit thread, and
// that work must not land inside a measured window.
constexpr auto kSettle = std::chrono::milliseconds(100);
// The sweep. N=4 vs N=1 is the deliverable; 2 and 3 show the shape between.
// The default sweep, kept at v2.1.0's four arms so that run's invocation
// reproduces exactly. A host with more cores needs more arms - the question
// the probe answers is "does the device overlap as many streams as this
// engine would open", and that bound moves with the machine - so the counts
// are overridable on the command line (see main).
constexpr int kDefaultThreadCounts[] = {1, 2, 3, 4};

struct ThreadResult {
    std::uint64_t syncs = 0;
    double seconds = 0.0;
};

[[noreturn]] void Fail(const char* what) {
    std::perror(what);
    // _Exit, not exit: this runs on a worker thread while its siblings and
    // main are still live, and running static destructors under that is
    // undefined behavior (and would deadlock main on the latch below).
    std::_Exit(1);
}

void RunThread(const std::string& path, std::latch& ready, std::latch& start,
               std::atomic<bool>& stop, ThreadResult& out) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) Fail("open");

    std::vector<char> page(kPageBytes, 'x');

    // Pre-allocate and sync the whole region so the measured loop never
    // extends the file and never triggers a metadata journal commit.
    for (std::size_t i = 0; i < kRegionPages; ++i) {
        if (::pwrite(fd, page.data(), kPageBytes,
                     static_cast<off_t>(i * kPageBytes)) !=
            static_cast<ssize_t>(kPageBytes)) {
            Fail("pwrite preallocate");
        }
    }
    if (::fsync(fd) != 0) Fail("fsync preallocate");

    // A real barrier, not a sleep. A thread still pre-allocating when the
    // clock started would both run a short window and drop its own 2 MiB of
    // journal traffic into everyone else's measurement - and it would do so
    // N times harder at N=4 than at N=1, biasing the one ratio this probe
    // exists to report.
    ready.count_down();
    start.wait();

    const auto begin = std::chrono::steady_clock::now();
    std::uint64_t syncs = 0;
    std::size_t slot = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        const off_t offset =
            static_cast<off_t>((slot % kRegionPages) * kPageBytes);
        ++slot;
        if (::pwrite(fd, page.data(), kPageBytes, offset) !=
            static_cast<ssize_t>(kPageBytes)) {
            Fail("pwrite");
        }
        if (::fdatasync(fd) != 0) Fail("fdatasync");
        ++syncs;
    }
    const auto end = std::chrono::steady_clock::now();

    ::close(fd);
    out.syncs = syncs;
    out.seconds = std::chrono::duration<double>(end - begin).count();
}

// Aggregate syncs/second across N threads for one repetition.
//
// Total syncs over the *longest* thread duration, deliberately: threads
// notice `stop` up to one fdatasync apart, and dividing by the longest
// window charges every thread for the full span. That errs low by at most
// one sync latency over the run (~0.3% at 10 ms and 3 s), and erring low is
// the safe direction for a gate that would otherwise overclaim overlap.
double RunOnce(const std::string& dir, int threads, double run_seconds) {
    const auto count = static_cast<std::size_t>(threads);
    std::latch ready{threads};
    std::latch start{1};
    std::atomic<bool> stop{false};
    std::vector<ThreadResult> results(count);
    std::vector<std::thread> workers;
    std::vector<std::string> paths;

    for (std::size_t i = 0; i < count; ++i) {
        paths.push_back(dir + "/fdatasync_probe_" + std::to_string(i) + ".dat");
    }
    for (std::size_t i = 0; i < count; ++i) {
        workers.emplace_back(RunThread, paths[i], std::ref(ready),
                             std::ref(start), std::ref(stop),
                             std::ref(results[i]));
    }

    ready.wait();
    std::this_thread::sleep_for(kSettle);
    start.count_down();
    std::this_thread::sleep_for(std::chrono::duration<double>(run_seconds));
    stop.store(true, std::memory_order_relaxed);

    for (auto& w : workers) w.join();

    std::uint64_t total_syncs = 0;
    double max_seconds = 0.0;
    for (const auto& r : results) {
        total_syncs += r.syncs;
        max_seconds = std::max(max_seconds, r.seconds);
    }
    for (const auto& p : paths) ::unlink(p.c_str());

    return max_seconds > 0.0 ? static_cast<double>(total_syncs) / max_seconds
                             : 0.0;
}

double Median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t mid = v.size() / 2;
    return (v.size() % 2 == 1) ? v[mid] : (v[mid - 1] + v[mid]) / 2.0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <dir-on-block-device> [reps] [seconds] "
                     "[thread-counts, e.g. 1,2,4,8]\n",
                     argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    const int reps = (argc > 2) ? std::atoi(argv[2]) : 5;
    const double run_seconds = (argc > 3) ? std::atof(argv[3]) : 3.0;
    // Guarded, not clamped: reps < 1 left the min/max scan below reading
    // runs[0] of an empty vector, and a non-positive window measured nothing.
    if (reps < 1 || run_seconds <= 0.0) {
        std::fprintf(stderr, "reps must be >= 1 and seconds must be > 0\n");
        return 2;
    }

    // Optional fourth argument: the thread counts, comma-separated. Every
    // arm must be >= 1 - a zero-thread arm would divide the ratio below by
    // a number nobody measured - and the first arm is the N=1 the ratios
    // are against, so it is required to be 1 rather than silently assumed.
    std::vector<int> thread_counts(std::begin(kDefaultThreadCounts),
                                   std::end(kDefaultThreadCounts));
    if (argc > 4) {
        thread_counts.clear();
        const std::string spec = argv[4];
        std::size_t pos = 0;
        while (pos <= spec.size()) {
            const std::size_t comma = spec.find(',', pos);
            const std::string tok =
                spec.substr(pos, comma == std::string::npos ? std::string::npos
                                                            : comma - pos);
            if (!tok.empty()) {
                const int n = std::atoi(tok.c_str());
                if (n < 1) {
                    std::fprintf(stderr, "thread count %s is not >= 1\n", tok.c_str());
                    return 2;
                }
                thread_counts.push_back(n);
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (thread_counts.empty() || thread_counts.front() != 1) {
            std::fprintf(stderr,
                         "the first thread count must be 1: every ratio printed "
                         "is against it\n");
            return 2;
        }
    }
    const std::size_t kArms = thread_counts.size();

    std::printf(
        "fdatasync overlap probe: dir=%s reps=%d seconds=%.1f page=%zu\n",
        dir.c_str(), reps, run_seconds, kPageBytes);
    std::fflush(stdout);

    // Interleaved, not blocked: running every repetition of N=1 and only then
    // every repetition of N=4 lets any drift across the run - an SLC cache
    // filling, thermal throttling, a noisy neighbour - land entirely on one
    // arm of the ratio and read as an absence of overlap.
    std::vector<std::vector<double>> runs(kArms);
    for (int r = 0; r < reps; ++r) {
        for (std::size_t a = 0; a < kArms; ++a) {
            runs[a].push_back(RunOnce(dir, thread_counts[a], run_seconds));
        }
    }

    std::printf("%-8s %-12s %-12s %-12s %-10s\n", "threads", "median_sps",
                "min_sps", "max_sps", "vs_N1");
    const double n1 = Median(runs[0]);
    for (std::size_t a = 0; a < kArms; ++a) {
        const double med = Median(runs[a]);
        const auto [lo, hi] =
            std::minmax_element(runs[a].begin(), runs[a].end());
        std::printf("%-8d %-12.1f %-12.1f %-12.1f %-10.3f\n", thread_counts[a],
                    med, *lo, *hi, n1 > 0.0 ? med / n1 : 0.0);
    }
    return 0;
}
