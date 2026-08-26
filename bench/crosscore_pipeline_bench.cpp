// The cross-core step pipeline, priced against local execution
// (docs/inflight/in-progress/workplan-crosscore.md P4e).
//
// ---- Why this is a C++ benchmark and not a server driver ----------------
//
// Every other per-statement number in `bench/` is measured the way
// `docs/inflight/in-progress/workplan-aggregate-perf.md` prescribes: two servers, interleaved
// A/B over the wire. **That cannot reach this code path.** A pipeline runs
// only when a relation is owned by a core other than the session's, and a
// peer-owned relation cannot be populated over the wire at all: writes to
// it are refused by `CheckWriteAffinity` (crosscore.md CC3, cross-core
// writes are a retryable refusal), DML statement shipping is unbuilt, and
// only core 0 carries a listener. So there is no sequence of client
// statements that puts rows in a relation a pipeline would read.
//
// This harness therefore does what the equivalence test does
// (`CoreRuntimeTest.EveryShippableShapeAnswersExactlyWhatLocalExecution
// Answers`): builds the rows through `heap::ChainInsert` directly, then
// runs **one** dataset through two dispatchers that differ only in
// `core_id`. The relations are owned by core 1, so the core-1 dispatcher
// executes locally and the core-0 dispatcher ships - same catalog, same
// pages, same statement, one process. What separates the two timings is
// the pipeline and nothing else, which is a *stronger* isolation than an
// A/B across two builds can offer, because there is no second binary and
// no second process for drift to enter through.
//
// What it costs in exchange, stated so nobody quotes this as something it
// is not: there is no socket, no framing, no ring - the loopback delivers
// each message inline. **These numbers are the pipeline's CPU cost, not
// its latency under a real transport.** The ring's own cost is P1's to
// measure and is not here.
//
// ---- What it measures ---------------------------------------------------
//
// Two things, and the second is the one the workplan carries by name:
//
//   1. Per-statement: what a shipped join costs over the same join run
//      locally, at a fixed shape.
//   2. **Per-input-row**: the slope. Each row the leaf forwards costs the
//      consuming stage one `exec::ExecuteAsync` frame, a `Bind` and a
//      frame `Open` before it touches a tuple - the shape `95946c4`
//      removed from the local executor and P4d-4b reintroduced one level
//      up. Running the same statement over a growing outer relation and
//      taking the slope of (shipped - local) against forwarded rows is
//      what turns that from a described cost into a number, and that
//      number is what decides whether P4d-4c's per-batch runner handle is
//      required or merely tidy.
//
// The inner side is a **pk probe**, deliberately: one descent per input
// row, so the work per row is constant and the slope isolates the runner
// overhead rather than the join's own cost.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/remote_step_service.hpp"
#include "kds/server/session_step_client.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/memory_page_device.hpp"

namespace {

using namespace kds;

using Clock = std::chrono::steady_clock;

double Percentile(std::vector<double>& sorted_us, double q) {
    if (sorted_us.empty()) return 0.0;
    const std::size_t i = static_cast<std::size_t>(q * (sorted_us.size() - 1));
    return sorted_us[i];
}

struct Stats {
    double p0 = 0, p25 = 0, p50 = 0, p75 = 0, p95 = 0, p99 = 0, mean = 0;
};

Stats Summarize(std::vector<double> us) {
    std::sort(us.begin(), us.end());
    Stats s;
    s.p0 = Percentile(us, 0.0);
    s.p25 = Percentile(us, 0.25);
    s.p50 = Percentile(us, 0.50);
    s.p75 = Percentile(us, 0.75);
    s.p95 = Percentile(us, 0.95);
    s.p99 = Percentile(us, 0.99);
    double total = 0;
    for (double v : us) total += v;
    s.mean = us.empty() ? 0 : total / static_cast<double>(us.size());
    return s;
}

catalog::Schema TwoColumns(const char* second) {
    catalog::Schema schema;
    catalog::SysColumnRow id{};
    id.pos = 0;
    catalog::SetName(id.name, "id");
    id.type_val = catalog::kTypeValInt64;
    id.len = 8;
    id.notnull = true;
    catalog::SysColumnRow other{};
    other.pos = 1;
    catalog::SetName(other.name, second);
    other.type_val = catalog::kTypeValInt64;
    other.len = 8;
    other.notnull = true;
    schema.columns = {id, other};
    return schema;
}

// One measured configuration: a dataset, two dispatchers over it, and the
// loopback that stands in for the ring.
class Bed {
public:
    // `outer_rows` drives how many rows cross the edge; the inner relation
    // is fixed so each input row's probe does identical work.
    bool Build(int outer_rows, int inner_rows) {
        auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/4096);
        if (!device.ok()) return false;
        device_ = std::move(device.value());
        auto store = storage::DevicePageStore::Open(*device_, server::kFirstUserPageId);
        if (!store.ok()) return false;
        store_ = std::move(store.value());
        auto boot = bootstrap::BootstrapDatabase(*store_, 1000);
        if (!boot.ok()) return false;
        boot_.emplace(std::move(boot.value()));

        // Two cores and rotation: every relation lands on core 1, which is
        // what makes one dispatcher local and the other a shipper.
        catalog_.emplace(*store_, storage::kDefaultInlineCellWidth, /*core_count=*/2);
        catalog_->SetPlacementPolicy(catalog::PlacementPolicy::kRotate);

        auto outer = catalog_->CreateTable(catalog::kNamespacePublic, "ta",
                                           TwoColumns("b_id"), catalog::ClusteredType::kBtree);
        auto inner = catalog_->CreateTable(catalog::kNamespacePublic, "tb", TwoColumns("qty"),
                                           catalog::ClusteredType::kBtree);
        if (!outer.ok() || !inner.ok()) return false;

        auto row = catalog_->GetSysTableRow(outer.value());
        if (!row.ok() || row.value().owner_core != 1u) return false;  // rotation is the premise

        local_.emplace(boot_->superblock, *catalog_, *store_, nullptr, nullptr, nullptr,
                       wal::DurabilityClass::kGroup, exec::Budget(), nullptr, false,
                       /*access_statistics=*/false, nullptr, nullptr,
                       txn::IsolationLevel::kReadCommitted, /*core_id=*/1);

        // Populated **through the local dispatcher**, which is core 1 and
        // therefore owns these relations - so this is the engine's real
        // write path, btree descent and all, rather than a hand-built
        // chain that would have to know each clustered type's layout. It
        // is also the only writer available: the session dispatcher is
        // core 0, and a write from there is exactly what CC3 refuses.
        for (int i = 0; i < inner_rows; ++i) {
            if (!Insert(*local_, "tb", i * 10)) return false;
        }
        for (int i = 0; i < outer_rows; ++i) {
            if (!Insert(*local_, "ta", 1 + (i % inner_rows))) return false;
        }
        if (!store_->Sync().ok()) return false;

        session_.emplace(boot_->superblock, *catalog_, *store_, nullptr, nullptr, nullptr,
                         wal::DurabilityClass::kGroup, exec::Budget(), nullptr, false,
                         /*access_statistics=*/false, nullptr, nullptr,
                         txn::IsolationLevel::kReadCommitted, /*core_id=*/0);

        auto deliver = [this](std::uint32_t dst, sched::RingMessageKind kind,
                              std::vector<std::byte> payload) {
            if (dst == 1) {
                sched::MessageHeader h{};
                h.src_core = 1;
                h.dst_core = 1;
                switch (kind) {
                    case sched::RingMessageKind::kStepOpen: server_->OnStepOpen(h, payload); break;
                    case sched::RingMessageKind::kStepCredit: server_->OnStepCredit(payload); break;
                    case sched::RingMessageKind::kStepBatch: server_->OnStepBatch(payload); break;
                    case sched::RingMessageKind::kStepEof: server_->OnStepEof(payload); break;
                    case sched::RingMessageKind::kStepCancel:
                        server_->OnStepCancel(payload);
                        break;
                    default: break;
                }
                return Status::OK();
            }
            switch (kind) {
                case sched::RingMessageKind::kStepBatch: client_->OnStepBatch(payload); break;
                case sched::RingMessageKind::kStepEof: client_->OnStepEof(payload); break;
                case sched::RingMessageKind::kStepError: client_->OnStepError(payload); break;
                default: break;
            }
            return Status::OK();
        };
        server_.emplace(*catalog_, *store_, /*core_id=*/1, deliver, nullptr,
                        server::kStepBatchTargetBytes,
                        [this](std::unique_ptr<sched::Task> task) {
                            tasks_.push_back(std::move(task));
                        });
        client_.emplace(/*core_id=*/0, deliver);
        session_->SetRemoteReads(&*client_);
        return true;
    }

    std::string RunLocal(const std::string& sql) { return local_->Dispatch(sql).response; }

    // The shipped path, driven the way a reactor would drive it: the
    // statement coroutine and the stage tasks polled in one loop.
    std::string RunShipped(const std::string& sql) {
        server::DispatchOutcome out;
        auto statement = sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                             session_->DispatchAsync(sql, nullptr, &out));
        int rounds = 0;
        while (statement->Poll() != sched::PollResult::kDone) {
            for (auto& task : tasks_) {
                if (task != nullptr && task->Poll() == sched::PollResult::kDone) task.reset();
            }
            std::erase(tasks_, nullptr);
            if (++rounds > 100000) return "ERR the pipeline did not converge";
        }
        return out.response;
    }

private:
    static bool Insert(server::CommandDispatcher& into, const char* table,
                       std::int64_t second) {
        const std::string reply =
            into.Dispatch("INSERT INTO " + std::string(table) + " VALUES (" +
                          std::to_string(second) + ")")
                .response;
        if (reply.rfind("ERR ", 0) == 0) {
            std::printf("  ! insert refused: %s\n", reply.c_str());
            return false;
        }
        return true;
    }

    std::unique_ptr<storage::MemoryPageDevice> device_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<catalog::Catalog> catalog_;
    std::optional<server::CommandDispatcher> local_;
    std::optional<server::CommandDispatcher> session_;
    std::optional<server::RemoteStepServer> server_;
    std::optional<server::SessionStepClient> client_;
    std::vector<std::unique_ptr<sched::Task>> tasks_;
};

struct Point {
    int outer_rows = 0;
    Stats local;
    Stats shipped;
};

// One size: warm both paths, then alternate them rep by rep so any drift
// in the machine lands on both sides equally. Interleaving is the same
// discipline the server-side A/B files use, for the same reason.
bool Measure(int outer_rows, int inner_rows, int reps, Point& out) {
    Bed bed;
    if (!bed.Build(outer_rows, inner_rows)) {
        std::printf("  ! setup failed at outer=%d\n", outer_rows);
        return false;
    }
    const std::string sql =
        "SELECT a.id, b.qty FROM ta AS a JOIN tb AS b ON b.id = a.b_id";

    const std::string local_reply = bed.RunLocal(sql);
    const std::string shipped_reply = bed.RunShipped(sql);
    if (local_reply.rfind("ERR ", 0) == 0) {
        std::printf("  ! local refused: %s\n", local_reply.c_str());
        return false;
    }
    // The premise of every number below: the two paths answer identically.
    // A benchmark comparing two different answers measures nothing.
    if (local_reply != shipped_reply) {
        std::printf("  ! the two paths disagree at outer=%d; not a measurement\n", outer_rows);
        return false;
    }
    for (int i = 0; i < 20; ++i) {  // warm both
        bed.RunLocal(sql);
        bed.RunShipped(sql);
    }

    std::vector<double> local_us, shipped_us;
    local_us.reserve(reps);
    shipped_us.reserve(reps);
    for (int i = 0; i < reps; ++i) {
        const auto a0 = Clock::now();
        bed.RunLocal(sql);
        const auto a1 = Clock::now();
        bed.RunShipped(sql);
        const auto a2 = Clock::now();
        local_us.push_back(std::chrono::duration<double, std::micro>(a1 - a0).count());
        shipped_us.push_back(std::chrono::duration<double, std::micro>(a2 - a1).count());
    }
    out.outer_rows = outer_rows;
    out.local = Summarize(std::move(local_us));
    out.shipped = Summarize(std::move(shipped_us));
    return true;
}

void PrintRow(const char* label, int n, const Stats& s) {
    std::printf("  %-8s n=%-6d p0=%9.1f  p25=%9.1f  p50=%9.1f  p75=%9.1f  p95=%9.1f\n", label,
                n, s.p0, s.p25, s.p50, s.p75, s.p95);
}

}  // namespace

int main(int argc, char** argv) {
    int reps = 200;
    int inner_rows = 64;
    if (argc > 1) reps = std::atoi(argv[1]);
    if (argc > 2) inner_rows = std::atoi(argv[2]);

    std::printf(
        "\nCross-core pipeline vs local execution (workplan P4e)\n"
        "  one dataset, two dispatchers differing only in core_id; relations owned by core 1\n"
        "  statement: SELECT a.id, b.qty FROM ta AS a JOIN tb AS b ON b.id = a.b_id\n"
        "  inner relation: %d rows, pk-probed (one descent per input row)\n"
        "  reps: %d per size, local and shipped interleaved rep by rep\n"
        "  loopback transport: no socket, no ring - this is CPU cost, not wire latency\n",
        inner_rows, reps);

    const std::vector<int> sizes{8, 32, 128, 512, 2048};
    std::vector<Point> points;
    for (int n : sizes) {
        Point p;
        if (!Measure(n, inner_rows, reps, p)) return 1;
        std::printf("\n== outer = %d rows ==\n", n);
        PrintRow("local", n, p.local);
        PrintRow("shipped", n, p.shipped);
        std::printf("  delta   p50=%+9.1f us   mean=%+9.1f us   per input row (mean)=%+7.3f us\n",
                    p.shipped.p50 - p.local.p50, p.shipped.mean - p.local.mean,
                    (p.shipped.mean - p.local.mean) / n);
        points.push_back(p);
    }

    // The slope: least squares of (shipped - local) mean against outer
    // rows. The intercept is what a shipped statement costs before its
    // first row; the slope is the per-input-row runner cost the workplan
    // carries by name.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const double n = static_cast<double>(points.size());
    for (const Point& p : points) {
        const double x = p.outer_rows;
        const double y = p.shipped.mean - p.local.mean;
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    const double denom = n * sxx - sx * sx;
    if (denom != 0.0) {
        const double slope = (n * sxy - sx * sy) / denom;
        const double intercept = (sy - slope * sx) / n;
        std::printf(
            "\n== the fit ==\n"
            "  shipped - local  =  %.3f us  +  %.3f us per forwarded row\n"
            "  intercept: what a shipped statement pays before its first row\n"
            "  slope:     the per-input-row runner cost (one ExecuteAsync frame,\n"
            "             a Bind and a frame Open per row) - workplan P4d-4b's\n"
            "             carried debt, and the number P4d-4c's per-batch runner\n"
            "             handle would have to beat to be worth building\n",
            intercept, slope);
    }
    return 0;
}
