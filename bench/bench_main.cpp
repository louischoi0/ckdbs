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
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "kds/sched/clock.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/keystone.hpp"
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

// ---- Clustered storage: B+ tree vs heap chain ---------------------------
//
// The two organizations a relation can have (`sys.tables.clustered_type`):
// a chain of heap pages (heap_chain.hpp) or a clustered B+ tree over the
// same leaf format (btree.hpp). They store the identical bytes, so the
// only thing worth measuring is what the directory above the leaves buys
// and what it costs:
//
//   insert        what the descent + occasional split adds to a tail
//                 append that already knows where the tail is
//   point lookup  O(depth) page fetches against a chain walk that reads
//                 every page until it finds the id - the whole reason
//                 kBtree exists, and the number that has to be read with
//                 the relation's row count beside it, because only one of
//                 the two curves is flat
//   full scan     leaves are walked through the same sibling link the
//                 chain uses, so this should be a wash; it is here to
//                 show that the tree does not make a scan worse
//
// Backed by InMemoryPageStore with no WAL and no server: this is the
// storage layer's own cost. tools/benchmark.py --clustered btree measures
// the same two organizations through the socket, where a logged insert's
// fsync is the dominant term and these differences are invisible.

enum class Clustered { kHeap, kBtree };

const char* Label(Clustered c) { return c == Clustered::kBtree ? "btree" : "heap"; }

// A tuple whose leading Keystone word carries `id`, padded to `size` with
// filler standing in for body columns. Both organizations require the
// word (they read the id back out of it), so this is not tree-specific.
std::vector<std::byte> KeystoneTuple(std::uint64_t id, std::size_t size) {
    auto word = kds::Keystone::Encode(id, 0, 0);
    if (!word.ok()) {
        Fatal(word.status(), "encoding a keystone word");
    }
    std::vector<std::byte> tuple(std::max(size, kds::kKeystoneWordSize));
    std::memcpy(tuple.data(), &word.value(), kds::kKeystoneWordSize);
    for (std::size_t i = kds::kKeystoneWordSize; i < tuple.size(); ++i) {
        tuple[i] = static_cast<std::byte>(i & 0xFF);
    }
    return tuple;
}

// A relation of one organization or the other, root included. The root
// moves when a tree gains a level, so it is state, not a constant.
struct Relation {
    std::unique_ptr<kds::storage::InMemoryPageStore> store;
    kds::PageId root = kds::kInvalidPageId;
    Clustered clustered = Clustered::kHeap;
};

Relation MakeRelation(Clustered clustered) {
    Relation rel;
    rel.clustered = clustered;
    rel.store = std::make_unique<kds::storage::InMemoryPageStore>();

    auto created = rel.store->CreateNew();
    if (!created.ok()) {
        Fatal(created.status(), "allocating a relation root");
    }
    rel.root = created.value().first;

    // Exactly what Catalog::CreateTable() does for each clustered type:
    // one page either way, and a tree that is still a bare leaf.
    if (clustered == Clustered::kBtree) {
        // owner 0: these relations exist outside any catalog, so there is
        // no oid to stamp (page.md §2a's "unattributed").
        if (kds::Status s = kds::btree::FormatRoot(created.value().second.bytes(), 0); !s.ok()) {
            Fatal(s, "formatting a btree root");
        }
    } else {
        auto view = kds::heap::PageView::CreateEmpty(created.value().second.bytes(), 0);
        if (!view.ok()) {
            Fatal(view.status(), "formatting a heap root");
        }
    }
    return rel;
}

// One insert into whichever storage `rel` uses, mirroring
// CommandDispatcher::InsertIntoRelation() minus the WAL: a tree that grew
// a level reports a new root, which the dispatcher persists and this
// keeps in memory.
void InsertRow(Relation& rel, std::uint64_t id, std::span<const std::byte> payload) {
    if (rel.clustered == Clustered::kBtree) {
        auto placed = kds::btree::BtreeInsert(*rel.store, rel.root, id, payload, 1,
                                              /*owner_oid=*/0);
        if (!placed.ok()) {
            Fatal(placed.status(), "btree insert");
        }
        if (placed.value().new_root != kds::kInvalidPageId) {
            rel.root = placed.value().new_root;
        }
        return;
    }
    auto placed = kds::heap::ChainInsert(*rel.store, rel.root, id, payload, 1, /*owner_oid=*/0);
    if (!placed.ok()) {
        Fatal(placed.status(), "chain insert");
    }
}

// Ids are system-issued and monotonic (invariant 10), so a bulk load is
// 1..rows and every id is resident afterwards - which is what lets the
// lookup bench below probe without generating misses.
Relation BuildRelation(Clustered clustered, std::uint64_t rows, std::size_t tuple_size) {
    Relation rel = MakeRelation(clustered);
    for (std::uint64_t id = 1; id <= rows; ++id) {
        const auto tuple = KeystoneTuple(id, tuple_size);
        InsertRow(rel, id, tuple);
    }
    return rel;
}

// The heap chain's point lookup: there is no index, so this is the walk
// the dispatcher performs on a heap relation - every page, every slot,
// until the id turns up. The sentinel status stops ChainVisit early
// (a non-ok Status from the callback ends the walk), which is what makes
// this an average-case half-chain read rather than a full scan.
bool HeapScanLookup(Relation& rel, std::uint64_t id) {
    bool found = false;
    kds::Status s = kds::heap::ChainVisit(
        *rel.store, rel.root, kds::storage::PageAccess::kRead,
        [&](kds::PageId, kds::heap::PageView& view, std::uint16_t slot) {
            auto tuple = view.ReadTuple(slot);
            if (!tuple.ok() || tuple.value().payload.size() < kds::kKeystoneWordSize) {
                return kds::Status::OK();
            }
            std::uint64_t word = 0;
            std::memcpy(&word, tuple.value().payload.data(), kds::kKeystoneWordSize);
            if (kds::Keystone::Decode(word).id != id) {
                return kds::Status::OK();
            }
            found = true;
            return kds::Status::AlreadyExists("found");
        });
    if (!found && !s.ok()) {
        Fatal(s, "chain scan");
    }
    return found;
}

std::string RowsTag(std::uint64_t rows) {
    return rows >= 1000 ? std::to_string(rows / 1000) + "k rows" : std::to_string(rows) + " rows";
}

// Bulk load: the cost of getting `rows` tuples in, split by organization.
// Page allocation and (for the tree) splits and level growth are inside
// the timed region, because an insert pays for them.
void BenchClusteredInsert(Clustered clustered, std::uint64_t rows, std::size_t tuple_size) {
    // Payloads built up front: string/vector churn is not what is being
    // measured, and the heap-page bench above makes the same choice.
    std::vector<std::vector<std::byte>> tuples;
    tuples.reserve(rows);
    for (std::uint64_t id = 1; id <= rows; ++id) {
        tuples.push_back(KeystoneTuple(id, tuple_size));
    }

    Bench(std::string(Label(clustered)) + " insert (" + std::to_string(tuple_size) + "B tuple)",
          "-", rows, [&](std::uint64_t n) {
              Relation rel = MakeRelation(clustered);
              double bytes = 0;
              for (std::uint64_t i = 0; i < n; ++i) {
                  InsertRow(rel, i + 1, tuples[i]);
                  bytes += static_cast<double>(tuples[i].size());
              }
              return bytes;
          },
          "bulk load, ascending ids");
}

// The number the tree exists for. Same probe sequence for both
// organizations (same seed), so the only difference is how the id is
// found. Read the two side by side *and* across row counts: the tree's
// line is flat, the chain's is not.
void BenchClusteredLookup(Clustered clustered, std::uint64_t rows, std::uint64_t probes,
                          std::size_t tuple_size) {
    Relation rel = BuildRelation(clustered, rows, tuple_size);

    std::mt19937_64 rng(1);
    std::vector<std::uint64_t> ids(probes);
    for (std::uint64_t& id : ids) {
        id = (rng() % rows) + 1;
    }

    Bench(std::string(Label(clustered)) + " point lookup (" + RowsTag(rows) + ")", "-", probes,
          [&](std::uint64_t n) {
              for (std::uint64_t i = 0; i < n; ++i) {
                  if (clustered == Clustered::kBtree) {
                      auto found = kds::btree::BtreeLookup(*rel.store, rel.root, ids[i]);
                      if (!found.ok()) {
                          Fatal(found.status(), "btree lookup");
                      }
                  } else if (!HeapScanLookup(rel, ids[i])) {
                      Fatal(kds::Status::NotFound("id " + std::to_string(ids[i])), "chain scan");
                  }
              }
              return 0.0;
          },
          clustered == Clustered::kBtree ? "descent, every probe a hit"
                                         : "chain scan, every probe a hit");
}

// The scan both organizations do the same way: leaves are linked by the
// same next_page_id the chain uses. Counted per tuple visited, not per
// scan, so the two rows are directly comparable.
void BenchClusteredScan(Clustered clustered, std::uint64_t rows, std::uint64_t rounds,
                        std::size_t tuple_size) {
    Relation rel = BuildRelation(clustered, rows, tuple_size);

    Bench(std::string(Label(clustered)) + " full scan (" + RowsTag(rows) + ")", "-",
          rows * rounds, [&](std::uint64_t n) {
              const std::uint64_t total_rounds = n / rows;
              std::uint64_t seen = 0;
              for (std::uint64_t r = 0; r < total_rounds; ++r) {
                  auto visit = [&](kds::PageId, kds::heap::PageView& view, std::uint16_t slot) {
                      auto tuple = view.ReadTuple(slot);
                      if (tuple.ok()) {
                          ++seen;
                      }
                      return kds::Status::OK();
                  };
                  kds::Status walked =
                      clustered == Clustered::kBtree
                          ? kds::btree::BtreeVisit(*rel.store, rel.root,
                                                   kds::storage::PageAccess::kRead, visit)
                          : kds::heap::ChainVisit(*rel.store, rel.root,
                                                  kds::storage::PageAccess::kRead, visit);
                  if (!walked.ok()) {
                      Fatal(walked, "full scan");
                  }
              }
              return static_cast<double>(seen) * static_cast<double>(tuple_size);
          },
          "per tuple visited");
}

// How many pages the tree spends on the directory above its leaves - the
// space side of the same trade, printed rather than timed because it is
// not a rate.
void ReportBtreeShape(std::uint64_t rows, std::size_t tuple_size) {
    Relation tree = BuildRelation(Clustered::kBtree, rows, tuple_size);
    Relation chain = BuildRelation(Clustered::kHeap, rows, tuple_size);

    auto height = kds::btree::BtreeHeight(*tree.store, tree.root);
    auto leaves = kds::btree::BtreeLeafCount(*tree.store, tree.root);
    auto chain_pages = kds::heap::ChainLength(*chain.store, chain.root);
    if (!height.ok() || !leaves.ok() || !chain_pages.ok()) {
        Fatal(height.ok() ? (leaves.ok() ? chain_pages.status() : leaves.status())
                          : height.status(),
              "measuring relation shape");
    }

    std::printf("  %-44s %-7s height %u, %u leaves, %zu pages total"
                " (chain: %u pages)\n",
                ("btree shape (" + RowsTag(rows) + ")").c_str(), "-", height.value(),
                leaves.value(), tree.store->page_count(), chain_pages.value());
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

    // 64B tuples: ~100 to an 8 KB page, so the row counts below are ~10,
    // ~100 and ~1000 pages of relation - the range where the chain's walk
    // goes from cheap to the thing the tree replaces. Probe counts fall as
    // rows rise for the chain only, because its per-probe cost rises with
    // them; the tree's stays flat, which is the point.
    std::printf("\nClustered storage - B+ tree vs heap chain (in-memory store, no WAL)\n");
    constexpr std::size_t kRowSize = 64;
    // Quick mode shrinks the relations too, not just the op counts: a
    // lookup bench pays for its fixture before it measures anything, and
    // in CI the fixture is the whole cost.
    const std::uint64_t rows_big = 100'000 / scale;
    const std::uint64_t rows_mid = 10'000 / scale;
    const std::uint64_t rows_small = 1'000 / scale;
    BenchClusteredInsert(Clustered::kHeap, 200'000 / scale, kRowSize);
    BenchClusteredInsert(Clustered::kBtree, 200'000 / scale, kRowSize);
    BenchClusteredLookup(Clustered::kHeap, rows_small, 20'000 / scale, kRowSize);
    BenchClusteredLookup(Clustered::kBtree, rows_small, 200'000 / scale, kRowSize);
    BenchClusteredLookup(Clustered::kHeap, rows_mid, 2'000 / scale, kRowSize);
    BenchClusteredLookup(Clustered::kBtree, rows_mid, 200'000 / scale, kRowSize);
    BenchClusteredLookup(Clustered::kHeap, rows_big, 200 / scale, kRowSize);
    BenchClusteredLookup(Clustered::kBtree, rows_big, 200'000 / scale, kRowSize);
    BenchClusteredScan(Clustered::kHeap, rows_big, 20 / scale, kRowSize);
    BenchClusteredScan(Clustered::kBtree, rows_big, 20 / scale, kRowSize);
    ReportBtreeShape(rows_big, kRowSize);

    std::printf("\nCheckpoint\n");
    BenchCheckpoint(false, 256, 200 / scale);
    BenchCheckpoint(true, 256, 20 / scale);

    std::printf("\n%zu benchmarks.\n", g_results.size());
    return 0;
}
