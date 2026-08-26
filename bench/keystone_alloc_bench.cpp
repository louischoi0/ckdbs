// What the Keystone id allocator costs today, and what bump-ahead would
// save (`docs/rules/keystoneid-invariant.md` §2, `docs/rules/keystoneid-k0-findings.md`).
//
// The question this answers is narrow and was asked in one sentence: could
// issue-once make inserts dramatically slower? The design that is *feared*
// to be slow - persisting an id per insert - is not the design in §2, and
// the design in §2 is not obviously faster than what exists either, because
// what exists already writes a catalog row per insert and is simply not
// durable about it. So four numbers are needed, in this order:
//
//   1. what `Catalog::AllocateRowId` costs per call, and what share of a
//      whole INSERT that is - the ceiling on what any allocator change can
//      win;
//   2. how that cost moves with the number of relations, since the
//      implementation **scans sys.tables** for the matching row - a cost §2
//      does not mention and nobody has measured;
//   3. what a chunked high-water mark costs instead, in CPU;
//   4. what it costs with a **real fsync** per chunk, which is the number
//      the "dramatically slower" question is actually about: crash-safe
//      issue-once means a durability point per id unless chunks amortize
//      it, and the gap between per-id and per-chunk fsync is the whole
//      argument for §2.
//
// The prototype in (3)/(4) lives here rather than in `catalog/` on purpose:
// K0 is an audit, K-M2 is the implementation, and a half-designed allocator
// merged to keep a measurement is how the two get confused. It models the
// same work the real one would do - read the catalog row once per chunk,
// issue from memory, write the row once per chunk - through the same page
// path, so the ratio it reports is the ratio K-M2 would inherit. What it
// deliberately does not model is recovery, which is what makes it a
// prototype and not a candidate.
//
// Timing is std::chrono directly, for the reason bench_main.cpp gives: the
// injected-clock rule (rules.md §4) is about engine logic, and a benchmark
// that cannot read a real clock cannot measure anything.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/file_page_device.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/keystone.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using kds::Status;
using kds::StatusOr;

double SecondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void Row(const std::string& what, std::uint64_t ops, double seconds, const std::string& note) {
    std::printf("  %-46s %12.0f op/s  %10.3f us/op  %s\n", what.c_str(), ops / seconds,
                (seconds / static_cast<double>(ops)) * 1e6, note.c_str());
}

// ---- The prototype: chunked bump-ahead -----------------------------------
//
// §2's allocator, minus recovery. `chunk` ids are reserved in the catalog
// row up front and handed out from memory; the row is touched once per
// chunk instead of once per id. `durable_per_chunk` additionally forces the
// reservation to the platter, which is what makes §2's crash-resume mean
// anything - and is the cost the "is this slow?" question is really about.
class BumpAheadAllocator {
public:
    BumpAheadAllocator(kds::storage::PageStore& store, kds::catalog::Oid table_oid,
                       std::uint64_t chunk, bool durable_per_chunk)
        : store_(store),
          table_oid_(table_oid),
          chunk_(chunk),
          durable_per_chunk_(durable_per_chunk) {}

    StatusOr<std::uint64_t> Next() {
        if (cursor_ >= ceiling_) {
            if (Status s = ReserveChunk(); !s.ok()) return s;
        }
        return cursor_++;
    }

    std::uint64_t reservations() const { return reservations_; }

private:
    // One pass over sys.tables per *chunk*: read `next_id`, publish
    // `next_id + chunk` as the durable ceiling, keep the interval in
    // memory. A crash loses the unissued remainder, which K3 makes legal.
    Status ReserveChunk() {
        auto bytes = store_.Get(kds::catalog::kCatalogPageTables);
        if (!bytes.ok()) return bytes.status();
        kds::heap::PageView page(bytes.value().bytes());
        for (std::uint16_t i = 0; i < page.slot_count(); ++i) {
            auto tuple = page.ReadTuple(i);
            if (!tuple.ok()) continue;
            auto row = kds::catalog::SysTableRow::Decode(tuple.value().payload);
            if (!row.ok()) return row.status();
            if (row.value().oid != table_oid_) continue;

            cursor_ = row.value().next_id;
            ceiling_ = cursor_ + chunk_;
            row.value().next_id = ceiling_;
            const auto encoded = row.value().Encode();
            if (Status s = page.OverwriteTuple(i, encoded, tuple.value().trx_id,
                                               tuple.value().undo_ptr);
                !s.ok()) {
                return s;
            }
            ++reservations_;
            if (durable_per_chunk_) {
                if (Status s = store_.Sync(); !s.ok()) return s;
            }
            return Status::OK();
        }
        return Status::NotFound("no sys.tables row for this oid");
    }

    kds::storage::PageStore& store_;
    kds::catalog::Oid table_oid_;
    std::uint64_t chunk_;
    bool durable_per_chunk_;
    std::uint64_t cursor_ = 0;
    std::uint64_t ceiling_ = 0;
    std::uint64_t reservations_ = 0;
};

// ---- Fixtures ------------------------------------------------------------

// A database on an InMemoryPageStore: no device in any number it produces,
// so what it measures is the engine's own CPU cost.
struct MemoryDatabase {
    kds::storage::InMemoryPageStore store{kds::server::kFirstUserPageId};
    kds::bootstrap::BootstrapResult boot;
    kds::server::CommandDispatcher dispatcher;

    MemoryDatabase()
        : boot(kds::bootstrap::BootstrapDatabase(store, 1000).value()),
          dispatcher(boot.superblock, boot.catalog, store) {}

    // `t` is created first so it sits early in the sys.tables scan - the
    // *optimistic* position. A relation created last would be found after
    // every other row and would flatter bump-ahead.
    //
    // Returns the number of user relations actually created: the catalog's
    // fixed pages do not chain, so past a few dozen tables CREATE TABLE
    // fails, and that ceiling is itself one of the findings.
    int Fill(int extra_relations) {
        if (dispatcher.Dispatch("CREATE TABLE t (id int64, v int64)").response.substr(0, 7) !=
            "CREATED") {
            return 0;
        }
        for (int i = 0; i < extra_relations; ++i) {
            const std::string sql =
                "CREATE TABLE pad" + std::to_string(i) + " (id int64, v int64)";
            if (dispatcher.Dispatch(sql).response.substr(0, 7) != "CREATED") return i + 1;
        }
        return extra_relations + 1;
    }

    kds::catalog::Oid TargetOid() {
        auto oid = boot.catalog.FindTableOidByName("t");
        return oid.ok() ? oid.value() : 0;
    }
};

// The same database on a file, so Sync() is a real fsync. This is the only
// fixture whose durability numbers mean anything.
struct FileDatabase {
    std::filesystem::path path;
    std::unique_ptr<kds::storage::FilePageDevice> device;
    std::unique_ptr<kds::storage::DevicePageStore> store;
    std::optional<kds::bootstrap::BootstrapResult> boot;
    std::optional<kds::server::CommandDispatcher> dispatcher;

    ~FileDatabase() {
        dispatcher.reset();
        boot.reset();
        store.reset();
        device.reset();
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    // Where the data file goes decides whether section 4 measures anything:
    // on this machine `/tmp` is tmpfs, where fsync costs about 4 us and the
    // durability question answers itself wrongly. So the default is the
    // working directory - the repo, on real storage - and `KDS_BENCH_DIR`
    // overrides it for a machine laid out differently.
    static std::filesystem::path Directory() {
        if (const char* dir = std::getenv("KDS_BENCH_DIR"); dir != nullptr && *dir != '\0') {
            return dir;
        }
        return std::filesystem::current_path();
    }

    bool Open() {
        path = Directory() /
               ("kds_keystone_bench_" + std::to_string(::getpid()) + ".dat");
        std::error_code ec;
        std::filesystem::remove(path, ec);

        auto dev = kds::storage::FilePageDevice::Open(path.string());
        if (!dev.ok()) return false;
        device = std::move(dev.value());

        auto st = kds::storage::DevicePageStore::Open(*device, kds::server::kFirstUserPageId);
        if (!st.ok()) return false;
        store = std::move(st.value());

        auto b = kds::bootstrap::BootstrapDatabase(*store, 1000);
        if (!b.ok()) return false;
        boot.emplace(std::move(b.value()));
        dispatcher.emplace(boot->superblock, boot->catalog, *store);

        return dispatcher->Dispatch("CREATE TABLE t (id int64, v int64)")
                   .response.substr(0, 7) == "CREATED";
    }

    kds::catalog::Oid TargetOid() {
        auto oid = boot->catalog.FindTableOidByName("t");
        return oid.ok() ? oid.value() : 0;
    }
};

// ---- Measurements --------------------------------------------------------

// (1a) The allocator alone, called the way an insert calls it.
double MeasureAllocator(kds::catalog::Catalog& catalog, kds::catalog::Oid oid, std::uint64_t n) {
    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < n; ++i) {
        auto id = catalog.AllocateRowId(oid);
        if (!id.ok()) return -1.0;
    }
    return SecondsSince(start);
}

// (1b) A whole INSERT through the dispatcher: parse, allocate, encode,
// append. Unlogged - no WalManager - so the number is the engine's CPU cost
// with the durability tax removed, which is the right baseline for asking
// what fraction of it the allocator is. Against a *logged* insert the
// allocator's share can only be smaller.
double MeasureInsert(kds::server::CommandDispatcher& d, std::uint64_t n) {
    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < n; ++i) {
        auto out = d.Dispatch("INSERT INTO t VALUES (" + std::to_string(i) + ")");
        if (out.response.substr(0, 8) != "INSERTED") return -1.0;
    }
    return SecondsSince(start);
}

// (3)/(4) The prototype at one chunk size.
double MeasureBumpAhead(kds::storage::PageStore& store, kds::catalog::Oid oid, std::uint64_t n,
                        std::uint64_t chunk, bool durable) {
    BumpAheadAllocator alloc(store, oid, chunk, durable);
    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < n; ++i) {
        auto id = alloc.Next();
        if (!id.ok()) return -1.0;
    }
    return SecondsSince(start);
}

// Today's allocator, but forced durable per id - the shape a crash-safe
// implementation reaches for if bump-ahead is skipped, and the one the
// "dramatically slower" worry is about.
double MeasureDurablePerId(kds::catalog::Catalog& catalog, kds::storage::PageStore& store,
                           kds::catalog::Oid oid, std::uint64_t n) {
    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < n; ++i) {
        auto id = catalog.AllocateRowId(oid);
        if (!id.ok()) return -1.0;
        if (Status s = store.Sync(); !s.ok()) return -1.0;
    }
    return SecondsSince(start);
}

}  // namespace

int main() {
    constexpr std::uint64_t kIds = 200000;
    constexpr std::uint64_t kInserts = 50000;
    constexpr std::uint64_t kDurableIds = 2000;  // fsync-bound; a smaller n

    std::printf("keystone id allocator - docs/rules/keystoneid-k0-findings.md\n\n");

    // ---- 1. The allocator's share of an INSERT ---------------------------
    std::printf("1. cost per issued id, and share of a whole INSERT (memory store)\n");
    {
        MemoryDatabase db;
        if (db.Fill(0) == 0) return 1;
        const kds::catalog::Oid oid = db.TargetOid();

        const double alloc_s = MeasureAllocator(db.boot.catalog, oid, kIds);
        if (alloc_s < 0) return 1;
        Row("AllocateRowId (today)", kIds, alloc_s, "");

        const double insert_s = MeasureInsert(db.dispatcher, kInserts);
        if (insert_s < 0) return 1;
        Row("INSERT end to end (unlogged)", kInserts, insert_s, "");

        const double alloc_us = (alloc_s / kIds) * 1e6;
        const double insert_us = (insert_s / kInserts) * 1e6;
        std::printf("  -> the allocator is %.1f%% of an unlogged INSERT: the ceiling on what\n"
                    "     any allocator change can win\n\n",
                    100.0 * alloc_us / insert_us);
    }

    // ---- 2. The scan is O(relations), but bounded -------------------------
    std::printf("2. cost per issued id as sys.tables grows (the scan §2 does not mention)\n");
    for (const int extra : {0, 9, 29, 499}) {
        MemoryDatabase db;
        const int made = db.Fill(extra);
        if (made == 0) return 1;
        const double s = MeasureAllocator(db.boot.catalog, db.TargetOid(), kIds);
        if (s < 0) return 1;
        Row("AllocateRowId, " + std::to_string(made) + " user relations", kIds, s,
            made < extra + 1 ? "*** ceiling: CREATE TABLE refused past here ***"
                             : "'t' is the first user row scanned");
    }
    std::printf("\n");

    // ---- 3. Bump-ahead, in CPU -------------------------------------------
    std::printf("3. chunked bump-ahead prototype (§2), memory store: CPU only\n");
    for (const std::uint64_t chunk : {std::uint64_t{1}, std::uint64_t{64}, std::uint64_t{4096}}) {
        MemoryDatabase db;
        if (db.Fill(0) == 0) return 1;
        const double s = MeasureBumpAhead(db.store, db.TargetOid(), kIds, chunk, /*durable=*/false);
        if (s < 0) return 1;
        Row("bump-ahead N=" + std::to_string(chunk), kIds, s,
            chunk == 1 ? "N=1 is today's cadence, and the control" : "");
    }
    std::printf("\n");

    // ---- 4. Durability, on a real file -----------------------------------
    std::printf("4. the durability question, on a file with real fsync (n=%llu)\n",
                static_cast<unsigned long long>(kDurableIds));
    {
        FileDatabase db;
        if (!db.Open()) {
            std::printf("  (could not open a file-backed database; skipped)\n");
        } else {
            const kds::catalog::Oid oid = db.TargetOid();

            // Today: the row is written per id but never forced, so this is
            // the same CPU cost as section 1 with a file underneath.
            const double s_today = MeasureAllocator(db.boot->catalog, oid, kDurableIds);
            if (s_today < 0) return 1;
            Row("AllocateRowId, no durability (today)", kDurableIds, s_today,
                "the sequence is not on the platter");

            // The feared design: crash-safe by forcing every id.
            const double s_per_id =
                MeasureDurablePerId(db.boot->catalog, *db.store, oid, kDurableIds);
            if (s_per_id < 0) return 1;
            Row("...forced durable per id", kDurableIds, s_per_id, "one fsync per issued id");

            // §2's design: crash-safe by forcing every chunk.
            double s_chunked = 0.0;
            for (const std::uint64_t chunk : {std::uint64_t{64}, std::uint64_t{4096}}) {
                const double s =
                    MeasureBumpAhead(*db.store, oid, kDurableIds, chunk, /*durable=*/true);
                if (s < 0) return 1;
                Row("bump-ahead N=" + std::to_string(chunk) + ", durable per chunk", kDurableIds,
                    s, "one fsync per reservation");
                if (chunk == 4096) s_chunked = s;
            }
            std::printf("  -> crash-safe issue-once costs %.0fx today's allocator if every id\n"
                        "     is forced, and %.2fx if chunks of 4096 are. The regression risk\n"
                        "     is per-id durability, not bump-ahead - which is *cheaper* than\n"
                        "     what runs today while also being crash-safe.\n",
                        s_per_id / s_today, s_chunked / s_today);
        }
    }
    std::printf("\nnote: sections 1-3 use an InMemoryPageStore, so no device is in them.\n");
    return 0;
}
