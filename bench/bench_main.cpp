// KDS microbenchmarks.
//
// What these are for: the numbers wal.md section 13 and page.md section 11
// say operators tune with - append throughput, group-commit batch
// efficiency, the cost of each durability class, and the flush path's
// per-page overhead. They are not a TPC anything; there is no executor,
// no transaction manager, and no disk-backed page store yet (see the gap
// list in the README section at the bottom of this file's output), so a
// whole-system number would be measuring a system that does not exist.
//
// Two backings are measured wherever durability is involved:
//
//   memory  - a MemoryLogDevice. Sync() is bookkeeping, so these numbers
//             are the engine's own CPU cost with the device removed. The
//             ceiling the code could reach on infinitely fast storage.
//   file    - a FileLogDevice under a temp dir, with real fsync. These
//             are the numbers that mean something for D1/D2, and the gap
//             between the two columns is the storage tax.
//
// Deliberately dependency-free (no Google Benchmark): the build has no
// third-party bench dep and adding one to print a table is not worth it.
// Timing uses std::chrono directly, which is allowed here because this is
// not engine logic - rules.md section 4's injected-clock rule is about the
// engine, and a benchmark that could not read a real clock could not
// measure anything.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kds/sched/clock.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_mgr/checkpoint_target.hpp"
#include "kds/storage/page_mgr/page_mgr.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/file_log_device.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// ---- Harness -------------------------------------------------------------

struct Result {
    std::string name;
    std::string backing;
    std::uint64_t ops = 0;
    double seconds = 0.0;
    double bytes = 0.0;
    std::string note;

    double ops_per_sec() const { return ops / seconds; }
    double ns_per_op() const { return seconds * 1e9 / static_cast<double>(ops); }
};

std::vector<Result> g_results;

// Runs `body(iterations)`, which must perform exactly `iterations`
// operations and return the number of bytes it moved (0 if not
// meaningful).
void Bench(const std::string& name, const std::string& backing, std::uint64_t iterations,
           const std::function<double(std::uint64_t)>& body, const std::string& note = "") {
    const auto start = Clock::now();
    const double bytes = body(iterations);
    const auto end = Clock::now();

    Result r;
    r.name = name;
    r.backing = backing;
    r.ops = iterations;
    r.seconds = std::chrono::duration<double>(end - start).count();
    r.bytes = bytes;
    r.note = note;
    std::printf("  %-44s %-7s %10.0f op/s %12.0f ns/op", r.name.c_str(), r.backing.c_str(),
                r.ops_per_sec(), r.ns_per_op());
    if (bytes > 0) {
        std::printf("  %7.1f MiB/s", bytes / r.seconds / (1024 * 1024));
    }
    if (!note.empty()) {
        std::printf("  [%s]", note.c_str());
    }
    std::printf("\n");
    g_results.push_back(std::move(r));
}

void Fatal(const kds::Status& status, const char* what) {
    std::fprintf(stderr, "bench: %s: %s\n", what, status.message().c_str());
    std::exit(1);
}

// ---- Fixtures ------------------------------------------------------------

// A scratch directory for the file-backed runs, removed on the way out.
class ScratchDir {
public:
    ScratchDir() {
        const char* base = std::getenv("KDS_BENCH_DIR");
        path_ = std::filesystem::path(base != nullptr ? base
                                                      : std::filesystem::temp_directory_path()) /
                ("kds-bench-" + std::to_string(::getpid()));
        std::filesystem::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    std::string Sub(const std::string& name) const {
        const auto p = path_ / name;
        std::filesystem::create_directories(p);
        return p.string();
    }

private:
    std::filesystem::path path_;
};

struct WalFixture {
    std::unique_ptr<kds::wal::LogDevice> device;
    std::unique_ptr<kds::wal::WalManager> wal;
    kds::sched::SystemClock clock;
};

// Segment big enough that a run does not spend its time rolling segments -
// rolls are measured separately.
constexpr std::uint64_t kBenchSegmentSize = 64ull * 1024 * 1024;

std::unique_ptr<WalFixture> MakeWal(bool file_backed, const ScratchDir& scratch,
                                    const std::string& tag) {
    auto fixture = std::make_unique<WalFixture>();
    if (file_backed) {
        auto device = kds::wal::FileLogDevice::Open(scratch.Sub(tag), 0, kBenchSegmentSize);
        if (!device.ok()) {
            Fatal(device.status(), "opening file log device");
        }
        fixture->device = std::move(device.value());
    } else {
        auto device = kds::wal::MemoryLogDevice::Create(kBenchSegmentSize);
        if (!device.ok()) {
            Fatal(device.status(), "creating memory log device");
        }
        fixture->device = std::move(device.value());
    }

    kds::wal::WalManagerConfig config;
    config.ring_capacity = 4 * 1024 * 1024;
    auto wal = kds::wal::WalManager::Open(fixture->device.get(), fixture->clock, 0, config);
    if (!wal.ok()) {
        Fatal(wal.status(), "opening wal manager");
    }
    fixture->wal = std::move(wal.value());
    return fixture;
}

std::vector<std::byte> Payload(std::size_t n) {
    std::vector<std::byte> bytes(n);
    for (std::size_t i = 0; i < n; ++i) {
        bytes[i] = static_cast<std::byte>(i & 0xFF);
    }
    return bytes;
}

// ---- WAL: the append path ------------------------------------------------

// Staging only: memcpy + cursor bump, no device involved. The ceiling on
// how fast a core can log.
void BenchAppend(std::size_t payload_size, std::uint64_t iterations) {
    ScratchDir scratch;
    auto fixture = MakeWal(/*file_backed=*/false, scratch, "append");
    const auto payload = Payload(payload_size);

    Bench("wal append (" + std::to_string(payload_size) + "B payload)", "memory", iterations,
          [&](std::uint64_t n) {
              double bytes = 0;
              for (std::uint64_t i = 0; i < n; ++i) {
                  auto lsn = fixture->wal->Append(
                      {kds::wal::RecordType::kHeapInsert, 1, 1, 0}, payload);
                  if (!lsn.ok()) {
                      Fatal(lsn.status(), "append");
                  }
                  bytes += static_cast<double>(kds::wal::EncodedRecordSize(payload.size()));
              }
              return bytes;
          },
          "ring staging + inline drains");
}

// ---- WAL: the durability classes ----------------------------------------

// D1: one sync per commit. This is the number a financial write pays.
void BenchStrictCommit(bool file_backed, std::uint64_t iterations) {
    ScratchDir scratch;
    auto fixture = MakeWal(file_backed, scratch, "d1");
    const auto payload = Payload(128);

    Bench("commit D1 strict (sync per commit)", file_backed ? "file" : "memory", iterations,
          [&](std::uint64_t n) {
              for (std::uint64_t i = 0; i < n; ++i) {
                  if (auto lsn = fixture->wal->Append(
                          {kds::wal::RecordType::kHeapInsert, i + 1, 1, 0}, payload);
                      !lsn.ok()) {
                      Fatal(lsn.status(), "append");
                  }
                  if (auto lsn = fixture->wal->Commit(i + 1, kds::wal::DurabilityClass::kStrict);
                      !lsn.ok()) {
                      Fatal(lsn.status(), "strict commit");
                  }
              }
              return 0.0;
          });
}

// D2: `batch` commits then one drain, which is what group commit buys.
// Sweeping the batch size shows the amortization curve directly.
void BenchGroupCommit(bool file_backed, std::uint64_t batch, std::uint64_t iterations) {
    ScratchDir scratch;
    auto fixture = MakeWal(file_backed, scratch, "d2-" + std::to_string(batch));
    const auto payload = Payload(128);

    Bench("commit D2 group (batch " + std::to_string(batch) + ")",
          file_backed ? "file" : "memory", iterations, [&](std::uint64_t n) {
              std::uint64_t txn = 0;
              for (std::uint64_t i = 0; i < n; i += batch) {
                  const std::uint64_t this_batch = std::min<std::uint64_t>(batch, n - i);
                  for (std::uint64_t j = 0; j < this_batch; ++j) {
                      ++txn;
                      if (auto lsn = fixture->wal->Append(
                              {kds::wal::RecordType::kHeapInsert, txn, 1, 0}, payload);
                          !lsn.ok()) {
                          Fatal(lsn.status(), "append");
                      }
                      if (auto lsn =
                              fixture->wal->Commit(txn, kds::wal::DurabilityClass::kGroup);
                          !lsn.ok()) {
                          Fatal(lsn.status(), "group commit");
                      }
                  }
                  // The system-group drain: one sync resolves the batch.
                  if (kds::Status s = fixture->wal->DrainOnce(); !s.ok()) {
                      Fatal(s, "drain");
                  }
              }
              return 0.0;
          },
          "one sync per batch");
}

// D3: no sync on the commit path at all - the bulk-load class.
void BenchRelaxedCommit(bool file_backed, std::uint64_t iterations) {
    ScratchDir scratch;
    auto fixture = MakeWal(file_backed, scratch, "d3");
    const auto payload = Payload(128);

    Bench("commit D3 relaxed (no sync on commit)", file_backed ? "file" : "memory", iterations,
          [&](std::uint64_t n) {
              for (std::uint64_t i = 0; i < n; ++i) {
                  if (auto lsn = fixture->wal->Append(
                          {kds::wal::RecordType::kHeapInsert, i + 1, 1, 0}, payload);
                      !lsn.ok()) {
                      Fatal(lsn.status(), "append");
                  }
                  if (auto lsn =
                          fixture->wal->Commit(i + 1, kds::wal::DurabilityClass::kRelaxed);
                      !lsn.ok()) {
                      Fatal(lsn.status(), "relaxed commit");
                  }
              }
              return 0.0;
          });
}

// The bare cost of the durability verb itself, with nothing else in the
// way: what one fsync costs on this box.
void BenchSync(bool file_backed, std::uint64_t iterations) {
    ScratchDir scratch;
    auto fixture = MakeWal(file_backed, scratch, "sync");
    const auto payload = Payload(64);

    Bench("wal sync (1 record staged)", file_backed ? "file" : "memory", iterations,
          [&](std::uint64_t n) {
              for (std::uint64_t i = 0; i < n; ++i) {
                  if (auto lsn = fixture->wal->Append(
                          {kds::wal::RecordType::kHeapInsert, 1, 1, 0}, payload);
                      !lsn.ok()) {
                      Fatal(lsn.status(), "append");
                  }
                  if (kds::Status s = fixture->wal->EnsureDurable(fixture->wal->appended_lsn() - 1);
                      !s.ok()) {
                      Fatal(s, "ensure durable");
                  }
              }
              return 0.0;
          });
}

// ---- Record codec --------------------------------------------------------

void BenchRecordCodec(std::uint64_t iterations) {
    const auto payload = Payload(256);
    std::vector<std::byte> buffer(kds::wal::EncodedRecordSize(payload.size()));

    Bench("record encode (256B payload, CRC32C)", "-", iterations, [&](std::uint64_t n) {
        double bytes = 0;
        for (std::uint64_t i = 0; i < n; ++i) {
            auto written = kds::wal::EncodeRecord(
                buffer, {kds::wal::RecordType::kHeapInsert, i, 1, 0}, 4096 + i * 8, payload);
            if (!written.ok()) {
                Fatal(written.status(), "encode");
            }
            bytes += static_cast<double>(written.value());
        }
        return bytes;
    });

    auto written = kds::wal::EncodeRecord(buffer, {kds::wal::RecordType::kHeapInsert, 1, 1, 0},
                                          4096, payload);
    if (!written.ok()) {
        Fatal(written.status(), "encode");
    }
    Bench("record decode + CRC verify", "-", iterations, [&](std::uint64_t n) {
        double bytes = 0;
        for (std::uint64_t i = 0; i < n; ++i) {
            auto decoded = kds::wal::DecodeRecord(buffer);
            if (!decoded.ok()) {
                Fatal(decoded.status(), "decode");
            }
            bytes += static_cast<double>(decoded.value().header.total_len);
        }
        return bytes;
    });
}

// ---- Heap page -----------------------------------------------------------

void BenchHeapInsert(std::size_t tuple_size, std::uint64_t iterations) {
    std::vector<std::byte> page_bytes(kds::kPageSize);
    const auto tuple = Payload(tuple_size);

    Bench("heap page insert (" + std::to_string(tuple_size) + "B tuple)", "-", iterations,
          [&](std::uint64_t n) {
              std::span<std::byte, kds::kPageSize> page(page_bytes.data(), kds::kPageSize);
              auto view = kds::heap::PageView::CreateEmpty(page, 0);
              if (!view.ok()) {
                  Fatal(view.status(), "create heap page");
              }
              double bytes = 0;
              for (std::uint64_t i = 0; i < n; ++i) {
                  auto slot = view.value().InsertTuple(tuple, i + 1);
                  if (!slot.ok()) {
                      // Page full: reformat and keep going. The reformat is
                      // counted in the time, which is honest - a real
                      // inserter pays for page allocation too, and at these
                      // tuple sizes it is one reformat per ~50 inserts.
                      view = kds::heap::PageView::CreateEmpty(page, 0);
                      if (!view.ok()) {
                          Fatal(view.status(), "recreate heap page");
                      }
                      slot = view.value().InsertTuple(tuple, i + 1);
                      if (!slot.ok()) {
                          Fatal(slot.status(), "insert");
                      }
                  }
                  bytes += static_cast<double>(tuple_size);
              }
              return bytes;
          });
}

void BenchPageChecksum(std::uint64_t iterations) {
    std::vector<std::byte> page_bytes(kds::kPageSize);
    std::span<std::byte, kds::kPageSize> page(page_bytes.data(), kds::kPageSize);
    kds::storage::FormatPage(page, kds::PageType::kHeap);

    Bench("page checksum (CRC32C over 8 KiB)", "-", iterations, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            kds::storage::StampPageChecksum(page);
        }
        return static_cast<double>(n) * kds::kPageSize;
    });
}

// ---- Buffer pool ---------------------------------------------------------

void BenchPoolHit(std::uint64_t iterations) {
    kds::storage::InMemoryPageStore backing;
    kds::storage::BufferPool pool(backing, 1024);
    for (kds::PageId id = 1; id <= 512; ++id) {
        auto frame = pool.AllocNew(id);
        if (!frame.ok()) {
            Fatal(frame.status(), "alloc");
        }
        pool.Unpin(*frame.value());
    }

    Bench("buffer pool hit (lookup + pin/unpin)", "-", iterations, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            auto frame = pool.Lookup(static_cast<kds::PageId>((i % 512) + 1));
            if (!frame.ok()) {
                Fatal(frame.status(), "lookup");
            }
            pool.Unpin(*frame.value());
        }
        return 0.0;
    });
}

// The flush path with the WAL gate in it, batched - what the checkpointer
// and background writer actually pay per page.
void BenchPoolFlush(bool file_backed, std::uint64_t pages, std::uint64_t rounds) {
    ScratchDir scratch;
    auto fixture = MakeWal(file_backed, scratch, "flush");
    kds::storage::InMemoryPageStore backing;
    kds::storage::BufferPool pool(backing, static_cast<std::uint32_t>(pages) + 16);
    pool.SetWalDurability(fixture->wal.get());

    std::vector<kds::PageId> ids;
    for (kds::PageId id = 1; id <= pages; ++id) {
        auto frame = pool.AllocNew(id);
        if (!frame.ok()) {
            Fatal(frame.status(), "alloc");
        }
        kds::storage::FormatPage(frame.value()->bytes(), kds::PageType::kHeap);
        pool.Unpin(*frame.value());
        ids.push_back(id);
    }

    Bench("pool flush batch (" + std::to_string(pages) + " pages, WAL-gated)",
          file_backed ? "file" : "memory", rounds * pages, [&](std::uint64_t n) {
              const std::uint64_t total_rounds = n / pages;
              for (std::uint64_t r = 0; r < total_rounds; ++r) {
                  for (kds::PageId id : ids) {
                      auto frame = pool.Lookup(id);
                      if (!frame.ok()) {
                          Fatal(frame.status(), "lookup");
                      }
                      auto lsn = fixture->wal->Append(
                          {kds::wal::RecordType::kHeapOverwrite, 1, id, 0});
                      if (!lsn.ok()) {
                          Fatal(lsn.status(), "append");
                      }
                      frame.value()->MarkDirty(lsn.value());
                      pool.Unpin(*frame.value());
                  }
                  if (kds::Status s = pool.FlushAll(); !s.ok()) {
                      Fatal(s, "flush all");
                  }
              }
              return static_cast<double>(n) * kds::kPageSize;
          },
          "per page; one log wait + one barrier per batch");
}

// ---- Checkpoint ----------------------------------------------------------

void BenchCheckpoint(bool file_backed, std::uint64_t pages, std::uint64_t checkpoints) {
    ScratchDir scratch;
    auto fixture = MakeWal(file_backed, scratch, "ckpt");
    kds::storage::InMemoryPageStore backing;
    kds::storage::BufferPool pool(backing, static_cast<std::uint32_t>(pages) + 16);
    pool.SetWalDurability(fixture->wal.get());
    kds::storage::BufferPoolCheckpointTarget target(pool);
    kds::wal::NoActiveTransactions txns;
    kds::wal::InMemoryCheckpointAnchor anchor;
    kds::wal::Checkpointer checkpointer(*fixture->wal, target, txns, anchor);

    std::vector<kds::PageId> ids;
    for (kds::PageId id = 1; id <= pages; ++id) {
        auto frame = pool.AllocNew(id);
        if (!frame.ok()) {
            Fatal(frame.status(), "alloc");
        }
        kds::storage::FormatPage(frame.value()->bytes(), kds::PageType::kHeap);
        pool.Unpin(*frame.value());
        ids.push_back(id);
    }

    Bench("checkpoint (" + std::to_string(pages) + " dirty pages)",
          file_backed ? "file" : "memory", checkpoints, [&](std::uint64_t n) {
              for (std::uint64_t i = 0; i < n; ++i) {
                  for (kds::PageId id : ids) {
                      auto frame = pool.Lookup(id);
                      if (!frame.ok()) {
                          Fatal(frame.status(), "lookup");
                      }
                      auto lsn = fixture->wal->Append(
                          {kds::wal::RecordType::kHeapOverwrite, 1, id, 0});
                      if (!lsn.ok()) {
                          Fatal(lsn.status(), "append");
                      }
                      frame.value()->MarkDirty(lsn.value());
                      pool.Unpin(*frame.value());
                  }
                  if (kds::Status s = checkpointer.RunToCompletion(); !s.ok()) {
                      Fatal(s, "checkpoint");
                  }
              }
              return 0.0;
          },
          "whole checkpoint, not per page");
}

// ---- Segment roll --------------------------------------------------------

void BenchSegmentRoll(std::uint64_t rolls) {
    ScratchDir scratch;
    // Small segments so a roll is reachable without writing 64 MiB.
    auto device = kds::wal::MemoryLogDevice::Create(64 * 1024);
    if (!device.ok()) {
        Fatal(device.status(), "creating device");
    }
    kds::sched::SystemClock clock;
    auto wal = kds::wal::WalManager::Open(device.value().get(), clock, 0, {});
    if (!wal.ok()) {
        Fatal(wal.status(), "opening wal");
    }

    Bench("segment roll (seal + create + header)", "memory", rolls, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            // Sealing forces the next append to roll.
            if (auto lsn = wal.value()->Append({kds::wal::RecordType::kHeapInsert, 1, 1, 0});
                !lsn.ok()) {
                Fatal(lsn.status(), "append");
            }
            if (kds::Status s = wal.value()->DrainOnce(); !s.ok()) {
                Fatal(s, "drain");
            }
        }
        return 0.0;
    },
          "amortized; segment size 64 KiB");
}

}  // namespace

int main(int argc, char** argv) {
    // A quick mode for CI, where the point is that the benchmarks still
    // run and not what they measure.
    const bool quick = argc > 1 && std::string(argv[1]) == "--quick";
    const std::uint64_t scale = quick ? 10 : 1;

    std::printf("KDS microbenchmarks%s\n", quick ? " (quick)" : "");
#ifndef NDEBUG
    // The engine lives in libkds, so a Debug tree means every number below
    // is measuring unoptimized engine code. Loud, because a Debug number
    // that escapes into a discussion is worse than no number.
    std::printf(
        "\n  *** DEBUG BUILD - these numbers are meaningless. Reconfigure with\n"
        "  *** -DCMAKE_BUILD_TYPE=Release and rebuild before quoting anything.\n\n");
#endif
    std::printf("  memory = MemoryLogDevice (no real sync): the engine's own cost\n");
    std::printf("  file   = FileLogDevice with fsync: what durability actually costs\n\n");

    std::printf("WAL append path\n");
    BenchAppend(64, 2'000'000 / scale);
    BenchAppend(1024, 500'000 / scale);
    BenchSegmentRoll(20'000 / scale);

    std::printf("\nRecord codec\n");
    BenchRecordCodec(2'000'000 / scale);

    std::printf("\nDurability classes (128B payload + commit record per txn)\n");
    BenchStrictCommit(false, 200'000 / scale);
    BenchStrictCommit(true, 2'000 / scale);
    BenchGroupCommit(false, 32, 200'000 / scale);
    BenchGroupCommit(true, 32, 20'000 / scale);
    BenchGroupCommit(true, 256, 50'000 / scale);
    BenchRelaxedCommit(false, 500'000 / scale);
    BenchRelaxedCommit(true, 500'000 / scale);
    BenchSync(false, 200'000 / scale);
    BenchSync(true, 2'000 / scale);

    std::printf("\nStorage\n");
    BenchHeapInsert(64, 1'000'000 / scale);
    BenchHeapInsert(512, 500'000 / scale);
    BenchPageChecksum(200'000 / scale);
    BenchPoolHit(2'000'000 / scale);
    BenchPoolFlush(false, 256, 200 / scale);
    BenchPoolFlush(true, 256, 20 / scale);

    std::printf("\nCheckpoint\n");
    BenchCheckpoint(false, 256, 200 / scale);
    BenchCheckpoint(true, 256, 20 / scale);

    std::printf("\n%zu benchmarks.\n", g_results.size());
    return 0;
}
