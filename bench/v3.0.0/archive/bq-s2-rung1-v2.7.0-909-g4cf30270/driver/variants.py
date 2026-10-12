"""BQ-S2's measurement builds on an export of 4cf30270: patches one tree per
arm. usage: variants.py <src_dir> <arm>   arm in opt3, s16, s256, s1024"""
import sys

src, arm = sys.argv[1], sys.argv[2]


def edit(rel, old, new):
    p = f'{src}/{rel}'
    s = open(p).read()
    assert s.count(old) == 1, (rel, old[:60], s.count(old))
    s = s.replace(old, new)
    with open(p, 'w') as f:
        f.write(s)


if arm.startswith('s'):
    mib = int(arm[1:])
    edit('include/kds/wal/log_device.hpp',
         'inline constexpr std::uint64_t kDefaultSegmentSize = 64ull * 1024 * 1024;',
         f'inline constexpr std::uint64_t kDefaultSegmentSize = {mib}ull * 1024 * 1024;  // BQ-S2 H2 arm')
elif arm == 'opt3':
    # H4 option 3: skip the zero prewrite where posix_fallocate reserved the
    # extents; keep it (and its fsync) where Reserve fell back to a sparse
    # ftruncate, and keep the body's fsync in every case.
    edit('src/wal/file_log_device.cpp',
         'Status Reserve(int fd, std::uint64_t size, const std::string& path) {\n'
         '    const int rc = ::posix_fallocate(fd, 0, static_cast<::off_t>(size));\n'
         '    if (rc == 0) return Status::OK();',
         'Status Reserve(int fd, std::uint64_t size, const std::string& path, bool* sparse) {\n'
         '    *sparse = false;\n'
         '    const int rc = ::posix_fallocate(fd, 0, static_cast<::off_t>(size));\n'
         '    if (rc == 0) return Status::OK();\n'
         '    *sparse = true;')
    edit('src/wal/file_log_device.cpp',
         '    if (Status s = Reserve(fd.value().get(), segment_size_, temp); !s.ok()) return abandon(s);\n'
         '    if (Status s = Prewrite(fd.value().get(), segment_size_); !s.ok()) return abandon(s);',
         '    bool sparse = false;\n'
         '    if (Status s = Reserve(fd.value().get(), segment_size_, temp, &sparse); !s.ok()) return abandon(s);\n'
         '    if (sparse) {\n'
         '        if (Status s = Prewrite(fd.value().get(), segment_size_); !s.ok()) return abandon(s);\n'
         '    } else {\n'
         '        while (::fsync(fd.value().get()) != 0) {\n'
         '            if (errno == EINTR) continue;\n'
         '            return abandon(ErrnoStatus("FileLogDevice: fsync after reserve", errno));\n'
         '        }\n'
         '    }')
else:
    raise SystemExit('unknown arm ' + arm)
print('patched', arm)
