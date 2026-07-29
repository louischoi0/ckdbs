#include "kds/storage/page_device.hpp"

#include <string>

namespace kds::storage {

Status CheckPageRunRange(PageId first_page_id, std::uint32_t nr_pages, std::uint32_t capacity) {
    if (nr_pages == 0) {
        return Status::InvalidArgument("page run: nr_pages must be non-zero");
    }
    const std::uint64_t end = static_cast<std::uint64_t>(first_page_id) + nr_pages;
    if (end > capacity) {
        return Status::OutOfRange("page run: pages [" + std::to_string(first_page_id) + ", " +
                                  std::to_string(end) + ") exceed capacity " +
                                  std::to_string(capacity));
    }
    return Status::OK();
}

Status CheckPageRunBuffer(std::uint32_t nr_pages, std::size_t buffer_bytes) {
    const std::size_t expected = static_cast<std::size_t>(nr_pages) * kPageSize;
    if (buffer_bytes != expected) {
        return Status::InvalidArgument("page run: buffer is " + std::to_string(buffer_bytes) +
                                       " bytes, expected " + std::to_string(expected));
    }
    return Status::OK();
}

Status PageDevice::ReadPageRun(PageId first_page_id, std::uint32_t nr_pages,
                               std::span<std::byte> out) {
    Status status = CheckPageRunRange(first_page_id, nr_pages, page_capacity());
    if (!status.ok()) {
        return status;
    }
    status = CheckPageRunBuffer(nr_pages, out.size());
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < nr_pages; ++i) {
        auto page = out.subspan(static_cast<std::size_t>(i) * kPageSize, kPageSize);
        status = ReadPage(first_page_id + i, std::span<std::byte, kPageSize>(page));
        if (!status.ok()) {
            return status;
        }
    }
    return Status::OK();
}

Status PageDevice::WritePageRun(PageId first_page_id, std::uint32_t nr_pages,
                                std::span<const std::byte> in) {
    Status status = CheckPageRunRange(first_page_id, nr_pages, page_capacity());
    if (!status.ok()) {
        return status;
    }
    status = CheckPageRunBuffer(nr_pages, in.size());
    if (!status.ok()) {
        return status;
    }
    for (std::uint32_t i = 0; i < nr_pages; ++i) {
        auto page = in.subspan(static_cast<std::size_t>(i) * kPageSize, kPageSize);
        status = WritePage(first_page_id + i, std::span<const std::byte, kPageSize>(page));
        if (!status.ok()) {
            return status;
        }
    }
    return Status::OK();
}

}  // namespace kds::storage
