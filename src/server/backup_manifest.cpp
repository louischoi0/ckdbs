#include "kds/server/backup_manifest.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kds/storage/crc32c.hpp"
#include "kds/wal/file_log_device.hpp"

namespace kds::server {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kCopyChunkBytes = 1u << 20;  // 1 MiB a read, a write

Status Errno(const std::string& what, int err) {
    return Status::IoError(what + ": " + std::strerror(err));
}

// Owns a descriptor; closes it at scope exit.
struct Fd {
    int fd = -1;
    explicit Fd(int f) : fd(f) {}
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};


std::string Hex32(std::uint32_t v) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", v);
    return buf;
}

std::string HexBytes(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", c);
        out += buf;
    }
    return out;
}

bool UnhexBytes(const std::string& hex, std::string* out) {
    if (hex.size() % 2 != 0) return false;
    out->clear();
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        char* end = nullptr;
        const std::string pair = hex.substr(i, 2);
        const unsigned long v = std::strtoul(pair.c_str(), &end, 16);
        if (end != pair.c_str() + 2) return false;
        out->push_back(static_cast<char>(v));
    }
    return true;
}

std::string Render(const BackupManifest& m) {
    std::ostringstream os;
    os << "kds-backup-manifest " << m.format_version << "\n"
       << "backup_id " << m.backup_id << "\n"
       // Hex, so a label carries any byte and never a line break.
       << "label " << (m.label.empty() ? "-" : HexBytes(m.label)) << "\n"
       << "path " << m.path << "\n"
       << "engine " << m.engine << "\n"
       << "superblock_version " << m.superblock_version << "\n"
       << "page_size " << m.page_size << "\n"
       << "segment_size " << m.segment_size << "\n"
       << "lineage " << m.lineage_id << "\n"
       << "start_lsn " << m.start_lsn << "\n"
       << "stop_lsn " << m.stop_lsn << "\n"
       << "start_segment " << m.start_segment << "\n"
       << "stop_segment " << m.stop_segment << "\n"
       << "created_unix " << m.created_unix << "\n";
    for (const BackupFile& f : m.files) {
        os << "file " << f.name << " " << f.size << " " << Hex32(f.crc32c) << "\n";
    }
    return os.str();
}

std::uint32_t CrcOf(const std::string& text) {
    return storage::Crc32cExtend(
        storage::kCrc32cInit,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

}  // namespace

Status PwriteAll(int fd, const std::byte* data, std::size_t len, std::uint64_t offset,
                 const std::string& what) {
    while (len > 0) {
        const ::ssize_t n = ::pwrite(fd, data, len, static_cast<::off_t>(offset));
        if (n < 0) {
            if (errno == EINTR) continue;
            return Errno("write " + what, errno);
        }
        data += n;
        offset += static_cast<std::uint64_t>(n);
        len -= static_cast<std::size_t>(n);
    }
    return Status::OK();
}

std::string SegmentFileName(std::uint64_t segment_no) {
    return wal::FileLogDevice::SegmentFileName(/*core_id=*/0, segment_no);  // the one stream
}

Status SyncDirectory(const std::string& dir) {
    Fd fd(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fd.fd < 0) return Errno("backup: open directory " + dir, errno);
    if (::fsync(fd.fd) != 0) return Errno("backup: fsync directory " + dir, errno);
    return Status::OK();
}

Status ChecksumFile(const std::string& path, std::uint64_t* size, std::uint32_t* crc32c) {
    Fd in(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (in.fd < 0) return Errno("backup: open " + path, errno);
    std::vector<std::byte> buf(kCopyChunkBytes);
    std::uint64_t total = 0;
    std::uint32_t crc = storage::kCrc32cInit;
    for (;;) {
        const ::ssize_t n = ::read(in.fd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return Errno("backup: read " + path, errno);
        }
        if (n == 0) break;
        crc = storage::Crc32cExtend(crc, std::span<const std::byte>(buf.data(),
                                                                    static_cast<std::size_t>(n)));
        total += static_cast<std::uint64_t>(n);
    }
    *size = total;
    *crc32c = crc;
    return Status::OK();
}

Status CopyFileDurably(const std::string& from, const std::string& to, std::uint64_t* size,
                       std::uint32_t* crc32c) {
    std::error_code ec;
    if (fs::exists(to, ec)) {
        return Status::AlreadyExists("backup: " + to + " already exists; a backup never "
                                     "overwrites a file");
    }
    const std::string tmp = to + ".tmp";
    fs::remove(tmp, ec);
    {
        Fd in(::open(from.c_str(), O_RDONLY | O_CLOEXEC));
        if (in.fd < 0) return Errno("backup: open " + from, errno);
        Fd out(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (out.fd < 0) return Errno("backup: create " + tmp, errno);
        std::vector<std::byte> buf(kCopyChunkBytes);
        std::uint64_t total = 0;
        std::uint32_t crc = storage::kCrc32cInit;
        for (;;) {
            const ::ssize_t n = ::read(in.fd, buf.data(), buf.size());
            if (n < 0) {
                if (errno == EINTR) continue;
                return Errno("backup: read " + from, errno);
            }
            if (n == 0) break;
            const std::span<const std::byte> chunk(buf.data(), static_cast<std::size_t>(n));
            if (Status s = PwriteAll(out.fd, chunk.data(), chunk.size(), total, tmp); !s.ok()) {
                return s;
            }
            crc = storage::Crc32cExtend(crc, chunk);
            total += static_cast<std::uint64_t>(n);
        }
        if (::fsync(out.fd) != 0) return Errno("backup: fsync " + tmp, errno);
        *size = total;
        *crc32c = crc;
    }
    if (::rename(tmp.c_str(), to.c_str()) != 0) return Errno("backup: rename " + tmp, errno);
    return SyncDirectory(fs::path(to).parent_path().string());
}

Status WriteManifest(const std::string& dir, const BackupManifest& manifest) {
    std::string text = Render(manifest);
    text += "end " + Hex32(CrcOf(text)) + "\n";
    const std::string path = (fs::path(dir) / kBackupManifestName).string();
    const std::string tmp = path + ".tmp";
    {
        Fd out(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
        if (out.fd < 0) return Errno("backup: create " + tmp, errno);
        if (Status s = PwriteAll(out.fd, reinterpret_cast<const std::byte*>(text.data()),
                                 text.size(), 0, tmp);
            !s.ok()) {
            return s;
        }
        if (::fsync(out.fd) != 0) return Errno("backup: fsync " + tmp, errno);
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) return Errno("backup: rename " + tmp, errno);
    return SyncDirectory(dir);
}

StatusOr<BackupManifest> ReadManifest(const std::string& dir) {
    const std::string path = (fs::path(dir) / kBackupManifestName).string();
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        return Status::NotFound("backup: " + dir + " has no " + kBackupManifestName +
                                "; a directory without a manifest is not a backup");
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t end_at = text.rfind("end ");
    if (end_at == std::string::npos || (end_at != 0 && text[end_at - 1] != '\n')) {
        return Status::Corruption("backup: " + path + " has no closing checksum line");
    }
    const std::string body = text.substr(0, end_at);
    std::string recorded = text.substr(end_at + 4);
    while (!recorded.empty() && (recorded.back() == '\n' || recorded.back() == '\r')) {
        recorded.pop_back();
    }
    if (recorded != Hex32(CrcOf(body))) {
        return Status::Corruption("backup: " + path + "'s checksum does not match its text");
    }

    BackupManifest m;
    std::map<std::string, std::string> fields;
    std::istringstream lines(body);
    std::string line;
    bool first = true;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string key;
        words >> key;
        if (first) {
            first = false;
            std::uint64_t version = 0;
            if (key != "kds-backup-manifest" || !(words >> version)) {
                return Status::Corruption("backup: " + path + " is not a kds backup manifest");
            }
            if (version != kBackupManifestVersion) {
                return Status::Unsupported("backup: manifest format " + std::to_string(version) +
                                           " is not this build's (" +
                                           std::to_string(kBackupManifestVersion) + ")");
            }
            m.format_version = static_cast<std::uint32_t>(version);
            continue;
        }
        if (key == "file") {
            BackupFile f;
            std::string crc;
            if (!(words >> f.name >> f.size >> crc)) {
                return Status::Corruption("backup: malformed file line in " + path);
            }
            f.crc32c = static_cast<std::uint32_t>(std::strtoul(crc.c_str(), nullptr, 16));
            m.files.push_back(std::move(f));
            continue;
        }
        std::string value;
        std::getline(words >> std::ws, value);
        fields[key] = value;
    }
    const char* required[] = {"backup_id",   "label",        "path",          "engine",
                              "superblock_version", "page_size", "segment_size", "lineage",
                              "start_lsn",   "stop_lsn",     "start_segment", "stop_segment",
                              "created_unix"};
    for (const char* key : required) {
        if (fields.find(key) == fields.end()) {
            return Status::Corruption("backup: " + path + " lacks `" + key + "`");
        }
    }
    auto u64 = [&](const char* key) { return std::strtoull(fields[key].c_str(), nullptr, 10); };
    m.backup_id = u64("backup_id");
    if (fields["label"] != "-" && !UnhexBytes(fields["label"], &m.label)) {
        return Status::Corruption("backup: " + path + "'s label is not hex");
    }
    m.path = fields["path"].empty() ? '?' : fields["path"][0];
    m.engine = fields["engine"];
    m.superblock_version = static_cast<std::uint32_t>(u64("superblock_version"));
    m.page_size = static_cast<std::uint32_t>(u64("page_size"));
    m.segment_size = u64("segment_size");
    m.lineage_id = u64("lineage");
    m.start_lsn = u64("start_lsn");
    m.stop_lsn = u64("stop_lsn");
    m.start_segment = u64("start_segment");
    m.stop_segment = u64("stop_segment");
    m.created_unix = u64("created_unix");
    if ((m.path != 'B' && m.path != 'D') || m.start_lsn == 0 || m.start_lsn >= m.stop_lsn ||
        m.start_segment > m.stop_segment || m.segment_size == 0) {
        return Status::Corruption("backup: " + path + " describes no backup window");
    }
    return m;
}

Status VerifyBackupFiles(const std::string& dir, const BackupManifest& manifest) {
    std::set<std::string> named;
    for (const BackupFile& f : manifest.files) named.insert(f.name);
    if (manifest.path == 'B' && named.count(kBackupDataFileName) == 0) {
        return Status::NotFound("backup " + std::to_string(manifest.backup_id) +
                                " is a base backup (path B) and names no data file");
    }
    const std::string wal = std::string(kBackupWalDirName) + "/";
    for (std::uint64_t seg = manifest.start_segment; seg <= manifest.stop_segment; ++seg) {
        if (named.count(wal + SegmentFileName(seg)) == 0) {
            return Status::NotFound(
                "backup " + std::to_string(manifest.backup_id) + ": segment " +
                std::to_string(seg) + " (lsn " + std::to_string(seg * manifest.segment_size) +
                " onward) is not in the backup, and the restore needs it to reach lsn " +
                std::to_string(manifest.stop_lsn));
        }
    }
    for (const BackupFile& f : manifest.files) {
        std::uint64_t size = 0;
        std::uint32_t crc = 0;
        const std::string path = (fs::path(dir) / f.name).string();
        if (Status s = ChecksumFile(path, &size, &crc); !s.ok()) {
            return s.WithMessage("backup " + std::to_string(manifest.backup_id) + ": " +
                                 f.name + " cannot be read: " + s.message());
        }
        if (size != f.size || crc != f.crc32c) {
            return Status::Corruption("backup " + std::to_string(manifest.backup_id) + ": " +
                                      f.name + " is " + std::to_string(size) + " bytes, crc " +
                                      Hex32(crc) + "; the manifest says " +
                                      std::to_string(f.size) + " bytes, crc " +
                                      Hex32(f.crc32c));
        }
    }
    return Status::OK();
}

}  // namespace kds::server
