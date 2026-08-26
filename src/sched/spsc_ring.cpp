#include "kds/sched/spsc_ring.hpp"

#include <cstring>
#include <string>

namespace kds::sched {
namespace {

// Smallest power of two at or above `n`, for n >= 1. Used to round a
// requested capacity up so the index-to-slot map can be a mask.
std::size_t RoundUpToPowerOfTwo(std::size_t n) noexcept {
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

}  // namespace

const char* RingMessageKindName(RingMessageKind kind) noexcept {
    switch (kind) {
        case RingMessageKind::kUnset: return "unset";
        case RingMessageKind::kStepOpen: return "STEP_OPEN";
        case RingMessageKind::kStepBatch: return "STEP_BATCH";
        case RingMessageKind::kStepEof: return "STEP_EOF";
        case RingMessageKind::kStepCredit: return "STEP_CREDIT";
        case RingMessageKind::kStepCancel: return "STEP_CANCEL";
        case RingMessageKind::kStepError: return "STEP_ERROR";
        case RingMessageKind::kAnchorWrite: return "ANCHOR_WRITE";
        case RingMessageKind::kExtentLease: return "EXTENT_LEASE";
        case RingMessageKind::kTrxIdLease: return "TRXID_LEASE";
        case RingMessageKind::kCatalogInvalidate: return "CATALOG_INVALIDATE";
        case RingMessageKind::kShutdown: return "SHUTDOWN";
        case RingMessageKind::kRelationFaultGrant: return "RELATION_FAULT_GRANT";
        case RingMessageKind::kRowIdLease: return "ROWID_LEASE";
        case RingMessageKind::kRelationWriteGrant: return "RELATION_WRITE_GRANT";
        case RingMessageKind::kRelationGrantRequest: return "RELATION_GRANT_REQUEST";
        case RingMessageKind::kIndexBuildRequest: return "INDEX_BUILD_REQUEST";
        case RingMessageKind::kIndexBuildReply: return "INDEX_BUILD_REPLY";
        case RingMessageKind::kIndexBuildDone: return "INDEX_BUILD_DONE";
        case RingMessageKind::kShippedStatementRequest: return "SHIPPED_STATEMENT_REQUEST";
        case RingMessageKind::kShippedStatementReply: return "SHIPPED_STATEMENT_REPLY";
        case RingMessageKind::kAssertionBuildRequest: return "ASSERTION_BUILD_REQUEST";
        case RingMessageKind::kAssertionBuildReply: return "ASSERTION_BUILD_REPLY";
        case RingMessageKind::kAssertionBuildDone: return "ASSERTION_BUILD_DONE";
    }
    return "unknown";
}

SpscRing::SpscRing(std::size_t capacity, std::size_t max_payload)
    : capacity_(capacity),
      mask_(capacity - 1),
      max_payload_(max_payload),
      slot_stride_(sizeof(MessageHeader) + max_payload),
      storage_(capacity * (sizeof(MessageHeader) + max_payload)) {}

StatusOr<SpscRing> SpscRing::Create(std::size_t capacity_slots, std::size_t max_payload) {
    if (capacity_slots == 0) {
        return Status::InvalidArgument("spsc ring: capacity must be at least 1 slot");
    }
    if (max_payload == 0) {
        return Status::InvalidArgument("spsc ring: max_payload must be at least 1 byte");
    }
    return SpscRing(RoundUpToPowerOfTwo(capacity_slots), max_payload);
}

SpscRing::SpscRing(SpscRing&& other) noexcept
    : capacity_(other.capacity_),
      mask_(other.mask_),
      max_payload_(other.max_payload_),
      slot_stride_(other.slot_stride_),
      storage_(std::move(other.storage_)),
      write_pos_(other.write_pos_.load(std::memory_order_relaxed)),
      read_pos_(other.read_pos_.load(std::memory_order_relaxed)) {
    // Move is for construction only - Create() returns by value and the
    // transport builds its matrix. Moving a ring that two threads are
    // already using is not a supported operation and there is no way to
    // make it one; the transport constructs every ring before any reactor
    // starts, which is the whole reason this is allowed to exist.
}

Status SpscRing::TrySend(const MessageHeader& header, std::span<const std::byte> payload) {
    if (payload.size() > max_payload_) {
        return Status::InvalidArgument("spsc ring: payload of " +
                                       std::to_string(payload.size()) +
                                       " bytes exceeds the slot size of " +
                                       std::to_string(max_payload_));
    }

    // Own index relaxed (nobody else writes it); the consumer's acquired,
    // so that a slot this read frees is genuinely free - the acquire pairs
    // with the consumer's release in TryReceive().
    const std::uint64_t write = write_pos_.load(std::memory_order_relaxed);
    const std::uint64_t read = read_pos_.load(std::memory_order_acquire);
    if (write - read >= capacity_) {
        return Status::ResourceExhausted("spsc ring: full (" + std::to_string(capacity_) +
                                         " slots)");
    }

    std::byte* slot = storage_.data() + SlotOffset(write);
    MessageHeader stored = header;
    // The length copied and the length announced are the same number by
    // construction; a caller cannot make them disagree.
    stored.payload_len = static_cast<std::uint32_t>(payload.size());
    std::memcpy(slot, &stored, sizeof(stored));
    if (!payload.empty()) {
        std::memcpy(slot + sizeof(MessageHeader), payload.data(), payload.size());
    }

    // Release: everything written above is visible to the consumer that
    // acquires this index.
    write_pos_.store(write + 1, std::memory_order_release);
    return Status::OK();
}

bool SpscRing::TryReceive(MessageHeader& header, std::vector<std::byte>& payload) {
    const std::uint64_t read = read_pos_.load(std::memory_order_relaxed);
    const std::uint64_t write = write_pos_.load(std::memory_order_acquire);
    if (read == write) return false;

    const std::byte* slot = storage_.data() + SlotOffset(read);
    std::memcpy(&header, slot, sizeof(header));

    payload.resize(header.payload_len);
    if (header.payload_len > 0) {
        std::memcpy(payload.data(), slot + sizeof(MessageHeader), header.payload_len);
    }

    // Release: the copy above completes before the producer may reuse the
    // slot.
    read_pos_.store(read + 1, std::memory_order_release);
    return true;
}

std::size_t SpscRing::size() const noexcept {
    const std::uint64_t write = write_pos_.load(std::memory_order_acquire);
    const std::uint64_t read = read_pos_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(write - read);
}

}  // namespace kds::sched
