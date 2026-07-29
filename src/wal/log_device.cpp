#include "kds/wal/log_device.hpp"

#include <string>

namespace kds::wal {

Status CheckSegmentRange(std::uint64_t segment_no, std::uint64_t offset, std::size_t length,
                         std::uint64_t segment_count, std::uint64_t segment_size) {
    if (segment_no >= segment_count) {
        return Status::OutOfRange("wal: segment " + std::to_string(segment_no) +
                                  " does not exist (have " + std::to_string(segment_count) + ")");
    }
    // Widened on purpose: offset + length can wrap near the top of u64,
    // and a wrapped range would silently address the start of the segment.
    if (offset > segment_size || length > segment_size - offset) {
        return Status::OutOfRange("wal: range at offset " + std::to_string(offset) + " + " +
                                  std::to_string(length) + " runs past the segment end (" +
                                  std::to_string(segment_size) + ")");
    }
    return Status::OK();
}

}  // namespace kds::wal
