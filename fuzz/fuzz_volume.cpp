// BS-S5 target 3: volume and log images (instructions/v3.0.0/workorder-bs-test-hardening.md).
//
// A small volume is made once - a relation, rows, an index, what a crash
// leaves on its data file and log - and every input is a list of byte edits
// to a copy of it: 6-byte records of (file: data or log, offset, value). The
// copy is then mounted through production's `Expeditor::Open`, recovery and
// all, and read. The obligation: the mount refuses, or it mounts and every
// row it serves is one the volume held; never a crash, never a row nobody
// wrote.

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "kds/server/expeditor.hpp"

namespace {

namespace fs = std::filesystem;

constexpr const char* kRead = "SELECT * FROM t";

kds::server::Expeditor::Config ConfigAt(const fs::path& dir) {
    kds::server::Expeditor::Config config;
    config.data_file = (dir / "kds.db").string();
    config.wal_dir = (dir / "wal").string();
    config.log_file = {};
    config.buffer_pool_frames = 4096;  // required (BE-R3)
    config.cores = 1;
    config.debug_text_port = 0;
    return config;
}

// Each row line of a `SELECT *` reply: the reply's rows split on the debug
// text separator, header dropped.
std::vector<std::string> Rows(const std::string& reply) {
    std::vector<std::string> rows;
    std::size_t at = reply.find("\\n");
    while (at != std::string::npos) {
        const std::size_t next = reply.find("\\n", at + 2);
        rows.push_back(reply.substr(at + 2, next == std::string::npos ? std::string::npos
                                                                     : next - at - 2));
        at = next;
    }
    return rows;
}

// A file of the volume, held as its live prefix - up to its last nonzero
// byte, and a page past it - and its size: a log segment is 64 MiB of
// mostly zeroes, and copying it whole for every input left ~1 run a second.
// An edit lands inside the prefix; the rest of the file is zero.
struct File {
    std::string name;  // relative to the volume's directory
    std::vector<char> prefix;
    std::uintmax_t size = 0;
};

struct Template {
    std::vector<File> files;     // the data file, then each log segment
    std::set<std::string> rows;  // every version of every row the volume held
};

File Load(const fs::path& path, const fs::path& base) {
    File f;
    f.name = fs::relative(path, base).string();
    f.size = fs::file_size(path);
    f.prefix.resize(f.size);
    std::ifstream in(path, std::ios::binary);
    in.read(f.prefix.data(), static_cast<std::streamsize>(f.prefix.size()));
    std::size_t live = f.prefix.size();
    while (live > 0 && f.prefix[live - 1] == 0) --live;
    f.prefix.resize(std::min<std::uintmax_t>(f.size, live + 8192));
    return f;
}

void Store(const fs::path& base, const File& f) {
    const fs::path path = base / f.name;
    fs::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(f.prefix.data(), static_cast<std::streamsize>(f.prefix.size()));
    }
    fs::resize_file(path, f.size);  // the zero tail, sparse
}

const fs::path& WorkDir() {
    static const fs::path dir =
        fs::temp_directory_path() / ("kds_fuzz_volume_" + std::to_string(::getpid()));
    return dir;
}

const Template& Volume() {
    static const Template made = [] {
        Template t;
        const fs::path dir = WorkDir() / "template";
        fs::remove_all(WorkDir());
        fs::create_directories(dir);
        std::atexit([] {
            std::error_code ec;
            fs::remove_all(WorkDir(), ec);
        });
        {
            auto opened = kds::server::Expeditor::Open(ConfigAt(dir), /*now_unix_seconds=*/2000);
            if (!opened.ok()) __builtin_trap();
            kds::server::CommandDispatcher& d = opened.value()->dispatcher();
            for (const char* sql :
                 {"CREATE TABLE t (id int64, v int64, s varchar NULL) BTREE",
                  "CREATE INDEX tv ON t (v)",
                  "INSERT INTO t VALUES (1, 10, 'a'), (2, 20, 'b'), (3, 30, NULL)",
                  "INSERT INTO t VALUES (4, 40, 'a value long enough to leave the inline cell "
                  "for the var-heap, which is what makes the spill path part of the image')",
                  "UPDATE t SET v = 21 WHERE id = 2", "DELETE FROM t WHERE id = 3", "SYNC"}) {
                if (d.Dispatch(sql).response.rfind("ERR", 0) == 0) __builtin_trap();
                // Every version the relation ever held: an edited log may stop
                // recovery early, and an earlier state is a row someone wrote.
                if (std::string_view(sql).rfind("CREATE", 0) == 0) continue;
                for (const std::string& row : Rows(d.Dispatch(kRead).response)) t.rows.insert(row);
            }
            if (t.rows.size() != 5) __builtin_trap();  // 1, 2 twice, 3, 4
        }  // the Expeditor dies unstopped: the image is a crash's
        t.files.push_back(Load(dir / "kds.db", dir));
        // Sorted: a directory's iteration order is the filesystem's, and the
        // file an edit names must be the input's alone (BS-R3).
        std::vector<fs::path> logs;
        for (const auto& entry : fs::directory_iterator(dir / "wal")) {
            if (entry.is_regular_file()) logs.push_back(entry.path());
        }
        std::sort(logs.begin(), logs.end());
        for (const fs::path& log : logs) t.files.push_back(Load(log, dir));
        fs::remove_all(dir);
        return t;
    }();
    return made;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const Template& volume = Volume();
    std::vector<File> files = volume.files;
    constexpr std::size_t kEdit = 6;
    for (std::size_t at = 0; at + kEdit <= size && at < 32 * kEdit; at += kEdit) {
        const std::uint32_t offset = static_cast<std::uint32_t>(data[at + 1]) |
                                     static_cast<std::uint32_t>(data[at + 2]) << 8 |
                                     static_cast<std::uint32_t>(data[at + 3]) << 16 |
                                     static_cast<std::uint32_t>(data[at + 4]) << 24;
        // The first byte's low bit: the data file or the log.
        File& f = files[(data[at] & 1) != 0 && files.size() > 1 ? 1 : 0];
        if (!f.prefix.empty()) f.prefix[offset % f.prefix.size()] = static_cast<char>(data[at + 5]);
    }
    const fs::path dir = WorkDir() / "run";
    fs::remove_all(dir);
    for (const File& f : files) Store(dir, f);
    {
        auto mounted = kds::server::Expeditor::Open(ConfigAt(dir), /*now_unix_seconds=*/3000);
        if (mounted.ok()) {
            const std::string reply = mounted.value()->dispatcher().Dispatch(kRead).response;
            if (reply.rfind("ERR", 0) != 0) {
                for (const std::string& row : Rows(reply)) {
                    if (volume.rows.count(row) == 0) __builtin_trap();  // a row nobody wrote
                }
            }
        }
    }
    fs::remove_all(dir);
    return 0;
}
