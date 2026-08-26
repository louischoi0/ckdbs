#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "kds/base/file_descriptor.hpp"
#include "kds/storage/page_device.hpp"

// The single-file, disk-backed PageDevice (docs/spec/page.md section 4,
// decision S5 CONFIRMED):
//
//   file_offset = page_id * kPageSize
//
// One data file per KDS instance. No segment table, no mapping structure,
// no indirection - the mapping is arithmetic, which is exactly what makes
// an id-sorted flush batch a sequential write (page.md section 13) and
// what bounds the design at 2^31 pages / 16 TiB (kMaxPageCount).
//
// Growth is extent-granular and sparse-friendly (page.md sections 5 and
// 14): EnsureCapacity() rounds up to a whole extent and reserves real
// blocks via posix_fallocate, so a later write cannot fail for lack of
// space after the ALLOC record was already logged. The extent size is
// still an open decision (page.md section 17), so it is a constructor
// parameter with a documented proposed default rather than a baked-in
// constant. Growth *batching* (growing several extents at once under
// pressure) is deliberately not here: that is allocation policy and
// belongs to the SpaceManager above this seam.
//
// mmap is not an option for this file - page.md section 15 rejects it for
// data and WAL on write-ordering, core-stalling, SIGBUS-error, and
// testability grounds.
//
// O_DIRECT is not enabled yet: it is bound up with the still-open I/O
// backend decision (CLAUDE.md), and it would impose alignment
// requirements on caller buffers that the buffer pool's aligned slab
// (page.md section 9) does not exist yet to satisfy. Nothing in this
// class's shape blocks it later.
//
// Concurrency: core-local, like every PageDevice. pread/pwrite are used
// rather than seek+read so there is no shared file offset to serialize on,
// which also keeps the class usable from a second core's device instance
// over a disjoint id range if that becomes the ownership model.

namespace kds::storage {

class FilePageDevice final : public PageDevice {
public:
    // Opens `path`, creating it if absent (mode 0600 - a data file is
    // never group- or world-readable). An existing file's size determines
    // the initial page_capacity(); a size that is not a whole number of
    // pages is reported as Corruption rather than silently rounded, since
    // neither ftruncate nor fallocate can produce one.
    //
    // `extent_pages` is the growth granularity and must be non-zero.
    static StatusOr<std::unique_ptr<FilePageDevice>> Open(
        const std::string& path, std::uint32_t extent_pages = kDefaultExtentPages);

    std::uint32_t page_capacity() const noexcept override { return page_capacity_; }
    std::uint32_t extent_pages() const noexcept { return extent_pages_; }
    const std::string& path() const noexcept { return path_; }

    Status ReadPage(PageId page_id, std::span<std::byte, kPageSize> out) override;
    Status WritePage(PageId page_id, std::span<const std::byte, kPageSize> in) override;

    // Overridden to move the whole run in one pread/pwrite: consecutive
    // page ids are consecutive file bytes, so a run needs no scatter/gather
    // at all. This is the concrete payoff of decision S5 (page.md section
    // 13) and the reason the run forms exist on the seam.
    Status ReadPageRun(PageId first_page_id, std::uint32_t nr_pages,
                       std::span<std::byte> out) override;
    Status WritePageRun(PageId first_page_id, std::uint32_t nr_pages,
                        std::span<const std::byte> in) override;

    Status EnsureCapacity(std::uint32_t nr_pages) override;

    // fsync, not fdatasync: after EnsureCapacity() the file's size and
    // block map have changed, and those are metadata. A data-only
    // durability verb would be a separate entry point, added with the I/O
    // backend decision that defines the metadata-durability contract
    // (page.md section 14).
    Status Sync() override;

private:
    FilePageDevice(FileDescriptor fd, std::string path, std::uint32_t extent_pages,
                   std::uint32_t page_capacity) noexcept;

    // Range-check against page_capacity(), then loop over the short
    // transfers pread/pwrite are permitted to return.
    Status ReadAt(PageId first_page_id, std::uint32_t nr_pages, std::byte* buffer);
    Status WriteAt(PageId first_page_id, std::uint32_t nr_pages, const std::byte* buffer);

    FileDescriptor fd_;
    std::string path_;
    std::uint32_t extent_pages_;
    std::uint32_t page_capacity_;
};

}  // namespace kds::storage
