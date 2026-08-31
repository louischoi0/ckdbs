// DA-b (`instructions/v2.7.0/ratification-da.md`) — prices the per-stage
// session-side state a fan-in's park predicate and its teardown pay, in
// isolation from the two-range ceiling this host's `CoreRuntime` count
// imposes. `bench/v2.7.0/results-*.md` carries the full argument for why a
// live 255-stage fan-in cannot be driven on a 2-CPU host: only cores
// `1..cores-1` get a `CoreRuntime`, so at `cores = 2` exactly one peer ever
// asks to open a range, and `OpenRangeOnSystemCore`'s top-owner suppression
// (`range_alloc.cpp`) then holds the relation at its first two ranges
// forever (confirmed empirically by `bench/spread_ceiling_probe.py`,
// archived beside the results file this bench feeds).
//
// What this file prices does **not** depend on the range count, and that
// is the whole reason it is runnable here: `SessionStepClient::reads_`
// (`session_step_client.hpp`) is a `std::vector<RemoteRead>` keyed by
// `PipelineTag{request_id, session_core, step_id}`, and a tag is minted
// once per `Open()` call regardless of how many distinct *ranges* the
// stages address. Opening N loopback reads against one relation with N
// distinct `request_id`s therefore exercises exactly the same vector a
// 255-stage fan-in would build, and lets the two O(N^2) candidates the
// order named be measured directly:
//
//   1. the park predicate (`command_dispatcher.cpp`'s `finished` lambda,
//      guarding the `WaitUntil` before `FinishRemoteReads`): one
//      `SessionStepClient::Find` per tag, per poll;
//   2. teardown (`FinishRemoteReads`'s `CloseAll` destructor): one
//      `SessionStepClient::Close` per tag, each a linear scan plus an
//      erase-shift.
//
// What it does **not** measure: the wire, the ring, or backpressure
// (`kInitialCreditsPerEdge` against `kCoreRingSlots`) — this is a loopback,
// synchronous delivery, no reactor, no transport, following the same
// isolation `bench/crosscore_pipeline_bench.cpp`'s header explains for the
// same reason (a peer-owned relation cannot be populated or fanned into
// over the wire on this host). The state-cost question is answered here;
// the wire question is out of reach on this host and is named as such in
// the results file.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/server/remote_step_service.hpp"
#include "kds/server/session_step_client.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/memory_page_device.hpp"

namespace {

using namespace kds;  // NOLINT - bench convenience, matches the sibling bench files

using Clock = std::chrono::steady_clock;

double UsSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

double Median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

// Nearest-rank, matching `bench_common.Phase.summary()`'s convention
// (rule 6): p0/p25/p50/p95/p99 over the reps. Rank counts, not sample
// counts, are thin here (kOpenCloseReps repetitions per N) - stated at
// the point of use rather than dressed up as more resolution than a
// microbenchmark of this shape can offer.
struct Percentiles {
    double p0 = 0, p25 = 0, p50 = 0, p95 = 0, p99 = 0;
};

Percentiles Summarize(std::vector<double> v) {
    Percentiles p;
    if (v.empty()) return p;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    auto at = [&](double q) {
        std::size_t i = static_cast<std::size_t>(q * static_cast<double>(n - 1));
        return v[i];
    };
    p.p0 = v.front();
    p.p25 = at(0.25);
    p.p50 = at(0.50);
    p.p95 = at(0.95);
    p.p99 = v.back();  // n < 100 in every cell here, so p99 collapses to max
    return p;
}

// One loopback rig: a single core-1-owned relation, a `RemoteStepServer`
// on core 1, a `SessionStepClient` on core 0, wired the same way
// `tests/session_step_client_test.cpp`'s fixture wires them - delivery is
// synchronous, so by the time `Open()` returns the scan has already run
// and EOF has already arrived.
struct Bed {
    std::unique_ptr<storage::MemoryPageDevice> device;
    std::unique_ptr<storage::DevicePageStore> store;
    std::optional<bootstrap::BootstrapResult> boot;
    catalog::Oid oid = 0;
    std::optional<server::RemoteStepServer> srv;
    std::optional<server::SessionStepClient> client;

    bool Build() {
        auto d = storage::MemoryPageDevice::Create(64);
        if (!d.ok()) return false;
        device = std::move(d.value());
        auto s = storage::DevicePageStore::Open(*device, server::kFirstUserPageId);
        if (!s.ok()) return false;
        store = std::move(s.value());
        auto b = bootstrap::BootstrapDatabase(*store, 1000);
        if (!b.ok()) return false;
        boot.emplace(std::move(b.value()));

        catalog::Schema schema;
        auto add = [&](const char* name, const char* type) {
            auto type_row = boot->catalog.ResolveTypeByName(type);
            catalog::SysColumnRow row{};
            row.pos = static_cast<std::uint16_t>(schema.columns.size());
            catalog::SetName(row.name, name);
            row.type_val = type_row.value().type_val;
            row.len = type_row.value().len;
            row.notnull = true;
            schema.columns.push_back(row);
        };
        add("id", "int64");
        add("qty", "int64");
        auto created = boot->catalog.CreateTable(catalog::kNamespacePublic, "t", schema,
                                                  catalog::ClusteredType::kHeap);
        if (!created.ok()) return false;
        oid = created.value();

        auto access = boot->catalog.InitTableAccess(oid);
        if (!access.ok()) return false;
        for (int i = 0; i < 4; ++i) {
            auto id = boot->catalog.AllocateRowId(oid);
            if (!id.ok()) return false;
            parser::AstValue qty;
            qty.type = parser::ValueType::kInt;
            qty.int_val = i * 10;
            qty.raw_int_text = std::to_string(i * 10);
            auto payload = exec::EncodeRow(access.value()->schema, access.value()->layout,
                                           id.value(), {qty});
            if (!payload.ok()) return false;
            auto placed = heap::ChainInsert(*store, access.value()->desc_page_id, id.value(),
                                            payload.value(), 1, access.value()->oid);
            if (!placed.ok()) return false;
        }

        srv.emplace(
            boot->catalog, *store, /*core_id=*/1,
            server::StepSendSeam{[this](std::uint32_t, sched::RingMessageKind kind,
                                        std::vector<std::byte> payload) {
                switch (kind) {
                    case sched::RingMessageKind::kStepBatch:
                        client->OnStepBatch(payload);
                        break;
                    case sched::RingMessageKind::kStepEof: client->OnStepEof(payload); break;
                    case sched::RingMessageKind::kStepError:
                        client->OnStepError(payload);
                        break;
                    default: break;
                }
                return Status::OK();
            }},
            nullptr, /*batch_target_bytes=*/64);
        client.emplace(/*core_id=*/0,
                       [this](std::uint32_t, sched::RingMessageKind kind,
                              std::vector<std::byte> payload) {
                           sched::MessageHeader from_session{};
                           from_session.src_core = 0;
                           from_session.dst_core = 1;
                           switch (kind) {
                               case sched::RingMessageKind::kStepOpen:
                                   srv->OnStepOpen(from_session, payload);
                                   break;
                               case sched::RingMessageKind::kStepCredit:
                                   srv->OnStepCredit(payload);
                                   break;
                               case sched::RingMessageKind::kStepCancel:
                                   srv->OnStepCancel(payload);
                                   break;
                               default: break;
                           }
                           return Status::OK();
                       });
        return true;
    }

    exec::Step ScanStep() const {
        exec::Step step;
        step.step_id = 0;
        step.rel_oid = oid;
        step.kind = exec::AccessKind::kScan;
        return step;
    }
};

struct Row {
    int n = 0;
    double open_us = 0;
    double open_us_per_tag = 0;
    double poll_us = 0;       // one full poll: N `Find` calls, tags-in-open-order
    double poll_us_per_tag = 0;
    double close_us = 0;      // teardown: N `Close` calls, tags-in-open-order
    double close_us_per_tag = 0;
    Percentiles poll_pct;     // over kOpenCloseReps independent rebuild+poll cycles
    Percentiles close_pct;    // over kOpenCloseReps independent rebuild+close cycles
};

}  // namespace

int main() {
#ifndef NDEBUG
    std::printf(
        "\n  *** DEBUG BUILD - these numbers are meaningless. Reconfigure with\n"
        "  *** -DCMAKE_BUILD_TYPE=Release and rebuild before quoting anything.\n\n");
#endif
    std::printf(
        "DA-b: SessionStepClient state cost, in isolation from the two-range "
        "ceiling (loopback, no wire).\n");
    std::printf(
        "N tags open in order 1..N; 'poll' = one WaitUntil-predicate pass over "
        "all N tags (park predicate shape); 'close' = CloseAll's teardown "
        "shape (Close per tag, open order).\n\n");

    const std::vector<int> ns = {1, 2, 4, 8, 16, 32, 64, 128, 200, 255};
    // 21 independent rebuild+open+poll+close cycles per N, so poll_us and
    // close_us each carry a distribution rather than one sample - rule 6's
    // p0/p25/p50/p95/p99 table below is over these reps. Thin by that
    // rule's own standard (a microbenchmark, not a server under load), and
    // said so at the table.
    const int kOpenCloseReps = 21;
    std::uint64_t req_id = 1;

    std::vector<Row> rows;

    std::printf("%5s %12s %14s %14s %14s %14s %16s\n", "N", "open_us",
               "open_us/tag", "poll_us", "poll_us/tag", "close_us", "close_us/tag");

    for (int n : ns) {
        std::vector<double> open_samples;
        std::vector<double> poll_samples;
        std::vector<double> close_samples;

        for (int rep = 0; rep < kOpenCloseReps; ++rep) {
            Bed bed;
            if (!bed.Build()) {
                std::printf("build failed at N=%d rep=%d\n", n, rep);
                return 1;
            }

            std::vector<server::PipelineTag> tags;
            tags.reserve(static_cast<std::size_t>(n));

            const Clock::time_point t_open0 = Clock::now();
            for (int i = 0; i < n; ++i) {
                auto tag = bed.client->Open(bed.ScanStep(), /*owner_core=*/1, req_id++);
                if (!tag.ok()) {
                    std::printf("open failed at N=%d i=%d rep=%d: %s\n", n, i, rep,
                               tag.status().message().c_str());
                    return 1;
                }
                tags.push_back(tag.value());
            }
            open_samples.push_back(UsSince(t_open0));

            // One poll pass, priced with enough repetitions to rise above
            // clock noise at small N without spending unreasonable time at
            // large N: target roughly a constant number of `Find`-visits
            // (~2,000,000 element comparisons) across N. `Find` is
            // non-mutating, so the reads vector is untouched between
            // repetitions - this measures the pure per-poll cost, not a
            // sequence of different polls.
            const long long target_visits = 2'000'000;
            const long long per_poll_visits =
                static_cast<long long>(n) * (n + 1) / 2 + 1;
            const int poll_reps =
                static_cast<int>(std::max<long long>(200, target_visits / per_poll_visits));

            const Clock::time_point t_poll0 = Clock::now();
            for (int p = 0; p < poll_reps; ++p) {
                for (const auto& tag : tags) {
                    auto* r = bed.client->Find(tag);
                    if (r == nullptr) {
                        std::printf("Find missed a tag it should hold, N=%d\n", n);
                        return 1;
                    }
                }
            }
            poll_samples.push_back(UsSince(t_poll0) / poll_reps);

            // Teardown, in the same order `FinishRemoteReads`'s `CloseAll`
            // walks `tags` - open order, which is also the order `Close`'s
            // own erase leaves the front of the vector at index 0 for the
            // next lookup (so the *find* half of each Close is O(1) here,
            // and the erase-shift is what remains O(N) per call).
            const Clock::time_point t_close0 = Clock::now();
            for (const auto& tag : tags) {
                bed.client->Close(tag);
            }
            close_samples.push_back(UsSince(t_close0));
        }

        Row row;
        row.n = n;
        row.open_us = Median(open_samples);
        row.open_us_per_tag = row.open_us / n;
        row.poll_us = Median(poll_samples);
        row.poll_us_per_tag = row.poll_us / n;
        row.close_us = Median(close_samples);
        row.close_us_per_tag = row.close_us / n;
        row.poll_pct = Summarize(poll_samples);
        row.close_pct = Summarize(close_samples);
        rows.push_back(row);

        std::printf("%5d %12.2f %14.4f %14.2f %14.4f %14.2f %16.4f\n", row.n, row.open_us,
                   row.open_us_per_tag, row.poll_us, row.poll_us_per_tag, row.close_us,
                   row.close_us_per_tag);
    }

    std::printf(
        "\nPercentiles over %d independent rebuild+open+poll+close cycles per N "
        "(rule 6; n=%d per cell, so p99 collapses to the max):\n",
        kOpenCloseReps, kOpenCloseReps);
    std::printf("%5s | %8s %8s %8s %8s %8s | %8s %8s %8s %8s %8s\n", "N", "poll_p0",
               "poll_p25", "poll_p50", "poll_p95", "poll_p99", "close_p0", "close_p25",
               "close_p50", "close_p95", "close_p99");
    for (const Row& row : rows) {
        std::printf("%5d | %8.3f %8.3f %8.3f %8.3f %8.3f | %8.2f %8.2f %8.2f %8.2f %8.2f\n",
                   row.n, row.poll_pct.p0, row.poll_pct.p25, row.poll_pct.p50,
                   row.poll_pct.p95, row.poll_pct.p99, row.close_pct.p0, row.close_pct.p25,
                   row.close_pct.p50, row.close_pct.p95, row.close_pct.p99);
    }

    std::printf(
        "\nScaling check (quadratic predicts poll_us(b)/poll_us(a) ~= "
        "(b/a)^2):\n");
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const Row& a = rows[i - 1];
        const Row& b = rows[i];
        const double n_ratio = static_cast<double>(b.n) / a.n;
        const double poll_ratio = (a.poll_us > 0) ? b.poll_us / a.poll_us : 0.0;
        const double close_ratio = (a.close_us > 0) ? b.close_us / a.close_us : 0.0;
        std::printf(
            "  N %4d -> %4d: n_ratio=%.2f  poll_ratio=%.2f (n^2=%.2f)  "
            "close_ratio=%.2f (n^2=%.2f)\n",
            a.n, b.n, n_ratio, poll_ratio, n_ratio * n_ratio, close_ratio,
            n_ratio * n_ratio);
    }
    return 0;
}
